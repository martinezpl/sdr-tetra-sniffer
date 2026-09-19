// "sweep" finds the carriers of a network, so that "run" can be given a real
// carrier list. It works in two stages.
//
// A power scan measures every channel of the raster. That finds a carrier only
// while it transmits, which is always true of a control carrier and false of an
// idle traffic carrier, so a peak is a candidate and never a conclusion.
//
// A decode stage then puts a real demodulator on each candidate. A candidate
// that reaches frame lock is a TETRA carrier. Its system information also names
// the main carrier of its cell: a carrier that names itself is a control
// carrier, and one that names another frequency is a traffic carrier of that
// cell. "run" must always hold the control carriers, because every channel
// grant arrives on them.

#include "demod_chain.h"
#include "radio.h"
#include "sweep.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <dsp/channel/rx_vfo.h>

extern "C" {
#include "tetra_common.h"
#include "crypto/tetra_crypto.h"
#include <phy/tetra_burst_sync.h>
}

namespace {

struct Channel {
	uint32_t hz;
	double power = 0;   // sum of the squared magnitude
	uint64_t n = 0;     // samples behind that sum
	int span = -1;
	double snr = 0;     // dB above the noise floor of its own span
	bool tested = false;
	bool locked = false;
	// The frequency error the demodulator converged on, in Hz, and how many
	// samples of it were taken while this carrier held frame lock.
	double error_sum = 0;
	int error_n = 0;
	uint32_t main_hz = 0;   // the main carrier of the cell, from system information
};

// One candidate under decode: a VFO, a demodulator and a TETRA decoder.
struct Probe {
	dsp::channel::RxVFO vfo;
	DemodChain chain;
	tetra_mac_state* tms = nullptr;
	tetra_rx_state* trs = nullptr;
	size_t chan = 0;
};

// A carrier this strong has settled, so its frequency estimate is worth more
// than one that barely held lock.
static const double STRONG_DB = 12.0;

// A carrier whose own frequency error is worth believing: the median of the
// samples taken while it held lock. The median, because an FLL that slips for
// one block puts a wild value in the list and a mean would carry it.
double median_of(std::vector<double> v)
{
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

int read_live(Radio* r, dsp::complex_t* in, int n)
{
	for (;;) {
		int got = radio_read(r, (float*)in, n);
		if (got < 0) return -1;
		if (got > 0) return got;
	}
}

// A carrier is kept SWEEP_EDGE_HZ clear of the edge, so the scan does too.
double usable_half(double rate) { return rate / 2 - SWEEP_EDGE_HZ; }

// The dongle rolls off well before the edge of its span. Past this point a
// carrier still decodes, but it loses SNR, so a suggested centre avoids it.
double good_half(double rate) { return 0.8 * (rate / 2); }

double median(std::vector<double> v)
{
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

void probe_init(Probe& p, uint32_t hz)
{
	p.chain.init();
	auto* tms = (tetra_mac_state*)calloc(1, sizeof(tetra_mac_state));
	tetra_mac_state_init(tms);
	tms->tcs = (tetra_crypto_state*)calloc(1, sizeof(tetra_crypto_state));
	tms->t_display_st = (tetra_display_state*)calloc(1, sizeof(tetra_display_state));
	tetra_crypto_state_init(tms->tcs);
	tms->fragslots = (fragslot*)calloc(FRAGSLOT_NR_SLOTS, sizeof(fragslot));
	for (int i = 0; i < 4; i++) {
		tms->t_display_st->slot_encr[i] = -1;
		tms->t_display_st->slot_speech[i] = -1;
		tms->t_display_st->slot_e2ee[i] = -1;
		tms->t_display_st->slot_ssi[i] = 0;
	}
	tms->clear_only = true;
	// The sweep identifies carriers. It decodes no speech, so the voice
	// callbacks stay null. tetra_lower_mac.c returns before it reaches them.
	tms->skip_voice_decode = true;
	tms->own_carrier_hz = hz;
	p.tms = tms;
	p.trs = (tetra_rx_state*)calloc(1, sizeof(tetra_rx_state));
	p.trs->burst_cb_priv = tms;
}

void probe_free(Probe& p)
{
	if (p.tms) {
		free(p.tms->fragslots);
		free(p.tms->t_display_st);
		free(p.tms->tcs);
		free(p.tms);
		p.tms = nullptr;
	}
	free(p.trs);
	p.trs = nullptr;
}

// The centre that holds every frequency in the list, if one exists.
// Otherwise the centre that holds the most of them.
double choose_center(const std::vector<uint32_t>& hz, const std::vector<double>& weight,
		     double tune_offset, double good, size_t* covered)
{
	double best = 0, best_weight = -1;
	size_t best_count = 0;
	for (size_t i = 0; i < hz.size(); i++)
		for (size_t j = i; j < hz.size(); j++) {
			double c = ((double)hz[i] + (double)hz[j]) / 2 + tune_offset;
			size_t count = 0;
			double sum = 0;
			for (size_t k = 0; k < hz.size(); k++)
				if (std::fabs((double)hz[k] + tune_offset - c) <= good) {
					count++;
					sum += weight[k];
				}
			if (count > best_count || (count == best_count && sum > best_weight)) {
				best_count = count;
				best_weight = sum;
				best = c;
			}
		}
	*covered = best_count;
	return std::round(best / 10000) * 10000;
}

} // namespace

int sweep_main(const SweepArgs& a)
{
	if (a.band_hi <= a.band_lo) {
		fprintf(stderr, "tetra-sniff: --band needs LOW:HIGH with HIGH above LOW\n");
		return 2;
	}

	// Every channel of the raster inside the band.
	std::vector<Channel> chans;
	for (double hz = std::ceil(a.band_lo / a.step) * a.step; hz <= a.band_hi; hz += a.step)
		chans.push_back(Channel{ (uint32_t)llround(hz) });
	if (chans.empty()) {
		fprintf(stderr, "tetra-sniff: the band holds no channel at a step of %.0f Hz\n", a.step);
		return 2;
	}

	// The receiver is opened before the spans are planned, because the rate it
	// takes decides how wide a span is and so how many there are.
	RadioOpen cfg{};
	cfg.center_hz = (uint32_t)((a.band_lo + a.band_hi) / 2);
	cfg.rate_hz = (uint32_t)a.rate;
	double width = a.band_hi - a.band_lo;
	uint32_t one_span_cap = 0;
	if (!a.rate) {
		double span = width > TETRA_SPAN_HZ ? TETRA_SPAN_HZ : width;
		one_span_cap = (uint32_t)llround((span + 2 * SWEEP_EDGE_HZ) * SWEEP_ONE_SPAN_MARGIN);
		cfg.max_rate_hz = one_span_cap;
	}
	cfg.index = a.device;
	cfg.channel = a.rx;
	cfg.antenna = a.antenna;
	cfg.gain_tenth_db = a.gain_db < 0 ? -1 : (int)llround(a.gain_db * 10);
	Radio* radio = nullptr;
	RadioErr rc = radio_open(&radio, cfg);
	if (rc != RadioErr::ok) {
		fprintf(stderr, "tetra-sniff: %s\n", radio_error(rc));
		return 2;
	}

	// Rate 0 asked for a window that holds the TETRA allocation (or a
	// narrower --band) in one span. The IQ block size, not the rate, is
	// what has to stay inside the SDR++ work buffers.
	double rate = radio_rate(radio);
	if (!rate) {
		fprintf(stderr, "tetra-sniff: %s\n", radio_error(RadioErr::bad_rate));
		radio_close(radio);
		return 2;
	}
	// The IQ window is `rate` hertz wide. If that covers the search band,
	// one tune is enough. usable_half is tighter (15 kHz off each edge);
	// a stick that snapped to exactly the band width would otherwise
	// miss by 30 kHz and the 95% overlap loop would add a second span.
	if (!a.rate && width > rate && one_span_cap) {
		uint32_t wider = radio_max_rate(radio, one_span_cap);
		if (wider > rate && width <= wider &&
		    radio_set_rate(radio, wider) == RadioErr::ok) {
			double got = radio_rate(radio);
			rate = got ? got : wider;
		}
	}
	if (!a.rate)
		printf("tetra-sniff: the receiver takes %.3f MS/s, so a span is %.3f MHz\n",
		       rate / 1e6, (2 * usable_half(rate)) / 1e6);

	double half = usable_half(rate);
	std::vector<double> centers;
	if (width <= rate) {
		centers.push_back((a.band_lo + a.band_hi) / 2);
	} else {
		for (double c = a.band_lo + half; c - half < a.band_hi; c += 2 * half * 0.95)
			centers.push_back(c);
	}

	const char* ant = radio_antenna(radio);
	printf("tetra-sniff: sweep %.3f-%.3f MHz, %.1f kHz raster, %.1f MS/s, %zu span(s), rx %d%s%s\n",
	       a.band_lo / 1e6, a.band_hi / 1e6, a.step / 1e3, rate / 1e6, centers.size(),
	       a.rx, (ant && *ant) ? " " : "", (ant && *ant) ? ant : "");

	int block = iq_block(rate);
	auto* in = dsp::buffer::alloc<dsp::complex_t>(block);
	auto* out = dsp::buffer::alloc<dsp::complex_t>(block);
	auto* syms = dsp::buffer::alloc<dsp::complex_t>(block);
	auto* dibits = dsp::buffer::alloc<uint8_t>(block);
	auto* bits = dsp::buffer::alloc<uint8_t>(2 * block);
	int scan_blocks = std::max(1, (int)llround(a.scan * rate / block));
	// A group holds every VFO that one pass of the input feeds. The VFOs are
	// built once: setOffset moves the rotator, and the filter taps stay.
	const size_t GROUP = 32;
	int status = 0;
	int saved = quiet_begin();
	std::vector<dsp::channel::RxVFO> vfos(GROUP);
	for (auto& v : vfos) v.init(nullptr, rate, VFO_RATE, VFO_BW, 0);
	quiet_end(saved);

	// Stage 1: measure the power of every channel.
	for (size_t s = 0; s < centers.size() && !status; s++) {
		if (radio_set_center(radio, (uint32_t)centers[s]) != RadioErr::ok) {
			fprintf(stderr, "tetra-sniff: %s\n", radio_error(RadioErr::bad_tune));
			status = 2;
			break;
		}
		std::vector<size_t> idx;
		for (size_t i = 0; i < chans.size(); i++)
			if (chans[i].span < 0 &&
			    std::fabs((double)chans[i].hz + a.tune_offset - centers[s]) <= half)
				idx.push_back(i);
		printf("tetra-sniff: span %zu at %.3f MHz, %zu channels\n", s + 1, centers[s] / 1e6,
		       idx.size());
		for (size_t g = 0; g < idx.size() && !status; g += GROUP) {
			size_t n = std::min(GROUP, idx.size() - g);
			for (size_t j = 0; j < n; j++)
				vfos[j].setOffset((double)chans[idx[g + j]].hz + a.tune_offset -
						  centers[s]);
			for (int b = 0; b < scan_blocks; b++) {
				int cnt = read_live(radio, in, block);
				if (cnt < 0) {
					fprintf(stderr, "tetra-sniff: the receiver stopped\n");
					status = 2;
					break;
				}
				for (size_t j = 0; j < n; j++) {
					int m = vfos[j].process(cnt, in, out);
					double p = 0;
					for (int k = 0; k < m; k++)
						p += (double)out[k].re * out[k].re +
						     (double)out[k].im * out[k].im;
					Channel& c = chans[idx[g + j]];
					c.power += p;
					c.n += m;
					c.span = (int)s;
				}
			}
		}
	}

	// An SNR is only comparable inside one span, because the automatic gain of
	// the tuner moves between spans. Take the noise floor of each span alone.
	for (size_t s = 0; s < centers.size(); s++) {
		std::vector<double> db;
		for (const Channel& c : chans)
			if (c.span == (int)s && c.n) db.push_back(10 * std::log10(c.power / c.n));
		double floor_db = median(db);
		for (Channel& c : chans)
			if (c.span == (int)s && c.n) c.snr = 10 * std::log10(c.power / c.n) - floor_db;
	}

	// A carrier is 25 kHz wide, and the raster can be finer, so one carrier can
	// light several channels. Keep the strongest of each neighbourhood.
	// ponytail: a pairwise scan, because the raster holds a few hundred channels
	std::vector<size_t> peaks;
	for (size_t i = 0; i < chans.size(); i++) {
		if (!chans[i].n || chans[i].snr < a.threshold_db) continue;
		bool best = true;
		for (size_t j = 0; j < chans.size() && best; j++) {
			if (i == j || !chans[j].n) continue;
			if (std::fabs((double)chans[j].hz - (double)chans[i].hz) > 12500) continue;
			if (chans[j].snr > chans[i].snr || (chans[j].snr == chans[i].snr && j < i))
				best = false;
		}
		if (best) peaks.push_back(i);
	}
	printf("tetra-sniff: %zu peak(s) at or above %.1f dB over the noise floor\n", peaks.size(),
	       a.threshold_db);

	// Group the peaks into spans to decode in. A band wider than one span needs
	// several, and every one that holds a peak is worth a dwell, because a
	// control carrier anywhere in the band decides what its own cell does.
	double good = good_half(rate);
	std::vector<std::vector<size_t>> groups;
	for (size_t k : peaks) {
		// Peaks come out in frequency order, so a greedy pass is enough: keep
		// adding to the open group while the whole group still fits one span.
		if (!groups.empty()) {
			double lo = (double)chans[groups.back().front()].hz;
			if ((double)chans[k].hz - lo <= 2 * good) {
				groups.back().push_back(k);
				continue;
			}
		}
		groups.push_back({ k });
	}
	printf("tetra-sniff: %zu span(s) to decode\n", groups.size());

	// Stage 2: decode the candidates of each group in turn.
	for (size_t g = 0; g < groups.size() && !status; g++) {
		std::vector<size_t>& cand = groups[g];
		double lo = (double)chans[cand.front()].hz;
		double hi = (double)chans[cand.back()].hz;
		double decode_center = std::round(((lo + hi) / 2 + a.tune_offset) / 10000) * 10000;
		std::sort(cand.begin(), cand.end(),
			  [&](size_t x, size_t y) { return chans[x].snr > chans[y].snr; });
		if (cand.size() > a.max_carriers) {
			printf("tetra-sniff: --max-carriers is %zu, so %zu weaker peak(s) of this"
			       " span go undecoded\n", a.max_carriers, cand.size() - a.max_carriers);
			cand.resize(a.max_carriers);
		}
		printf("tetra-sniff: span %zu of %zu, decode %zu candidate(s) at %.3f MHz for %.0f s\n",
		       g + 1, groups.size(), cand.size(), decode_center / 1e6, a.dwell);
		if (radio_set_center(radio, (uint32_t)decode_center) != RadioErr::ok) {
			fprintf(stderr, "tetra-sniff: %s\n", radio_error(RadioErr::bad_tune));
			status = 2;
			break;
		}
		std::vector<Probe> probes(cand.size());
		int saved2 = quiet_begin();
		for (size_t i = 0; i < cand.size(); i++) {
			probes[i].chan = cand[i];
			probe_init(probes[i], chans[cand[i]].hz);
			probes[i].vfo.init(nullptr, rate, VFO_RATE, VFO_BW,
					   (double)chans[cand[i]].hz + a.tune_offset - decode_center);
		}
		quiet_end(saved2);
		for (size_t k : cand) chans[k].tested = true;
		int dwell_blocks = std::max(1, (int)llround(a.dwell * rate / block));
		// One list of estimates for each candidate, keyed by channel.
		std::map<size_t, std::vector<double>> errors;
		for (int b2 = 0; b2 < dwell_blocks; b2++) {
			int cnt = read_live(radio, in, block);
			if (cnt < 0) {
				fprintf(stderr, "tetra-sniff: the receiver stopped\n");
				status = 2;
				break;
			}
			for (Probe& p : probes) {
				int m = p.vfo.process(cnt, in, out);
				m = p.chain.process(m, out, syms, dibits, bits);
				for (int off = 0; off < m; off += 2048)
					tetra_burst_sync_in(p.trs, bits + off, std::min(2048, m - off));
				// Lock comes and goes, so one moment of it is enough.
				if (p.trs->state != RX_S_LOCKED) continue;
				chans[p.chan].locked = true;
				// Only while locked does the FLL hold a real estimate of
				// the error, and only then is this carrier certainly TETRA.
				errors[p.chan].push_back(p.chain.error_hz());
			}
		}
		for (Probe& p : probes) {
			// System information names the main carrier of the cell, which
			// is not always the carrier that carries it.
			chans[p.chan].main_hz = p.tms->last_logged_dl;
			chans[p.chan].error_n = (int)errors[p.chan].size();
			chans[p.chan].error_sum = median_of(errors[p.chan]);
			probe_free(p);
		}
	}

	dsp::buffer::free(in);
	dsp::buffer::free(out);
	dsp::buffer::free(syms);
	dsp::buffer::free(dibits);
	dsp::buffer::free(bits);
	radio_close(radio);
	if (status) return status;

	// The report.
	printf("\n  %-12s %7s  %-24s %-18s %s\n", "channel", "SNR dB", "role",
	       "cell main carrier", "error Hz");
	for (size_t k : peaks) {
		const Channel& c = chans[k];
		const char* role = "not tested, out of span";
		if (c.tested && !c.locked) role = "no lock, not TETRA";
		else if (c.tested && c.main_hz == c.hz) role = "TETRA control carrier";
		else if (c.tested && c.main_hz) role = "TETRA traffic carrier";
		else if (c.tested) role = "TETRA, cell unknown";
		char mainhz[32] = "-";
		if (c.main_hz) snprintf(mainhz, sizeof mainhz, "%u", c.main_hz);
		char err[32] = "-";
		if (c.error_n) snprintf(err, sizeof err, "%+.0f", c.error_sum);
		printf("  %-12u %7.1f  %-24s %-18s %s\n", c.hz, c.snr, role, mainhz, err);
	}

	// Everything that proved to be TETRA, in frequency order.
	std::vector<size_t> found;
	size_t controls = 0;
	for (size_t k : peaks)
		if (chans[k].locked) {
			found.push_back(k);
			if (chans[k].main_hz == chans[k].hz) controls++;
		}

	printf("\n");
	if (found.empty()) {
		printf("No candidate reached frame lock. Either the band holds no TETRA, or the\n"
		       "peaks are too weak. Try a longer --dwell, a fixed --gain, or another --band.\n");
		return 1;
	}

	// The frequency error of the receiver, in parts per million rather than Hz.
	// The error of an oscillator scales with the frequency it is tuned to, so
	// Hz measured at one carrier does not carry to a carrier 30 MHz away, and a
	// sweep of the whole band spans far more than that.
	//
	// A carrier that barely held lock has barely settled, so prefer the strong
	// ones and fall back only if too few are strong.
	std::vector<double> ppm, ppm_all;
	for (size_t k : found) {
		if (!chans[k].error_n) continue;
		double p = chans[k].error_sum / (chans[k].hz / 1e6);
		ppm_all.push_back(p);
		if (chans[k].snr >= STRONG_DB) ppm.push_back(p);
	}
	if (ppm.size() < 3) ppm = ppm_all;
	double error_ppm = median_of(ppm);
	// The VFO looks at hz + tune_offset - centre, so raising the offset moves
	// the search up and the reported error down by the same amount. Measured on
	// air: at offset 0 the error was -146 Hz, and at -2500 it was +2234. So the
	// offset that drives the error to zero is the one already in use plus the
	// error still being reported, taken at the centre each run will use.
	auto offset_at = [&](double center_hz) {
		return lround(a.tune_offset + error_ppm * (center_hz / 1e6));
	};

	printf("%zu TETRA carrier(s), of which %zu are a control carrier.\n",
	       found.size(), controls);
	if (!controls)
		printf("\nNone of them is a control carrier. Widen --band or raise --dwell, because\n"
		       "\"run\" learns the traffic carriers from the grants on a control carrier.\n");

	if (!ppm.empty()) {
		double plo = *std::min_element(ppm.begin(), ppm.end());
		double phi = *std::max_element(ppm.begin(), ppm.end());
		printf("\nThe receiver is off by about %+.2f ppm, from %zu carrier(s)%s that\n"
		       "held lock, spread %+.2f to %+.2f ppm. Each run below carries the\n"
		       "offset in Hz at its own centre, because the error scales with it.\n",
		       error_ppm, ppm.size(), ppm.size() == ppm_all.size() ? "" : " above the noise",
		       plo, phi);
		if (phi - plo > 1.0)
			printf("That spread is wide for one oscillator. A carrier of its own may\n"
			       "sit off frequency, or a weak one may not have settled. A longer\n"
			       "--dwell tightens it.\n");
	}

	// One receiver hears one span at a time, so the carriers are grouped into
	// spans and each group becomes its own "run". A group is opened by a control
	// carrier, because a run without one learns nothing: it would sit on traffic
	// carriers and never hear a grant. A carrier that no group reaches needs
	// another receiver.
	struct Group {
		std::vector<uint32_t> hz;
		size_t controls = 0;
	};
	std::vector<Group> runs;
	std::vector<uint32_t> orphans;
	// A group must be placed against the whole of itself, not against the
	// carrier that opened it, so take the extremes each time.
	auto fits = [&](const Group& g, uint32_t hz) {
		if (g.hz.size() >= a.max_carriers) return false;
		double lo = std::min<double>(*std::min_element(g.hz.begin(), g.hz.end()), hz);
		double hi = std::max<double>(*std::max_element(g.hz.begin(), g.hz.end()), hz);
		return hi - lo <= 2 * good;
	};
	// The control carriers first, because only they can open a group. In one
	// pass a traffic carrier below its own control carrier would be orphaned
	// before that control carrier had opened anything.
	for (size_t k : found) {
		if (chans[k].main_hz != chans[k].hz) continue;
		bool placed = false;
		for (Group& g : runs)
			if (fits(g, chans[k].hz)) {
				g.hz.push_back(chans[k].hz);
				g.controls++;
				placed = true;
				break;
			}
		if (!placed) runs.push_back(Group{ { chans[k].hz }, 1 });
	}
	// Then the traffic carriers, into whichever group reaches them.
	for (size_t k : found) {
		if (chans[k].main_hz == chans[k].hz) continue;
		bool placed = false;
		for (Group& g : runs)
			if (fits(g, chans[k].hz)) {
				g.hz.push_back(chans[k].hz);
				placed = true;
				break;
			}
		// No group reaches it, and it grants nothing itself.
		if (!placed) orphans.push_back(chans[k].hz);
	}

	if (runs.size() > 1)
		printf("\nThose carriers do not fit one span at %.1f MS/s, so they need %zu runs,\n"
		       "one for each span. A receiver hears one span at a time, so running them\n"
		       "at once needs one receiver for each, named with --device.\n",
		       rate / 1e6, runs.size());

	for (size_t i = 0; i < runs.size(); i++) {
		Group& g = runs[i];
		std::sort(g.hz.begin(), g.hz.end());
		double center = std::round((((double)g.hz.front() + g.hz.back()) / 2) / 10000) * 10000;
		if (runs.size() > 1)
			printf("\n  # span %zu of %zu, %zu carrier(s), %zu control\n", i + 1,
			       runs.size(), g.hz.size(), g.controls);
		printf("\n  ./tetra-sniff run --center %.0f \\\n", center);
		// The rate is what the receiver was measured to take, and the carriers
		// were grouped into spans that wide. A run at any other rate has a
		// different span, so the group it is handed may no longer fit.
		printf("      --rate %.0f \\\n", rate);
		if (a.rx) printf("      --rx %d \\\n", a.rx);
		if (a.antenna && *a.antenna) printf("      --antenna %s \\\n", a.antenna);
		if (!ppm.empty()) printf("      --tune-offset %ld \\\n", offset_at(center));
		printf("      --carriers ");
		for (size_t j = 0; j < g.hz.size(); j++) printf("%s%u", j ? "," : "", g.hz[j]);
		printf("\n");
	}
	printf("\n");

	if (!orphans.empty()) {
		printf("%zu carrier(s) sit too far from any control carrier to share a span with\n"
		       "one, so nothing would grant them:", orphans.size());
		for (uint32_t hz : orphans) printf(" %u", hz);
		printf("\nThey are traffic carriers of a cell whose control carrier the sweep did\n"
		       "not find. Widen --band or raise --dwell to look for it.\n\n");
	}

	printf("A power scan sees a carrier only while it transmits, so an idle traffic\n"
	       "carrier is missing from that list. \"run\" reports each grant that names a\n"
	       "carrier the list does not hold, so watch its log and add what it names.\n");
	return 0;
}
