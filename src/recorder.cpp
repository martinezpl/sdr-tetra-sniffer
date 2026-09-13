#include "recorder.h"
#include "allocations.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

// A TETRA network keeps every carrier inside a few MHz of the others. A grant
// beyond this is a decode error, not a carrier that a wider span would reach.
static const double PLAUSIBLE_BAND_HZ = 10e6;

static void write_all(int fd, const void* p, size_t n, off_t off = -1)
{
	const char* c = (const char*)p;
	while (n) {
		ssize_t w = off < 0 ? write(fd, c, n) : pwrite(fd, c, n, off);
		if (w < 0) {
			if (errno == EINTR) continue;
			throw std::runtime_error(std::string("write: ") + strerror(errno));
		}
		c += w; n -= w;
		if (off >= 0) off += w;
	}
}

static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

void WavWriter::open(const std::string& path)
{
	// No O_TRUNC and no O_APPEND. On Linux O_APPEND makes pwrite ignore its offset,
	// and O_TRUNC would erase the samples this function is about to seed nsamples from.
	fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0644);
	if (fd < 0) throw std::runtime_error(path + ": " + strerror(errno));
	// A new file starts at zero samples. An old file keeps the samples that it holds.
	off_t end = lseek(fd, 0, SEEK_END);
	nsamples = end > 44 ? (end - 44) / 2 : 0;
	write_header();
}

void WavWriter::write_header()
{
	uint8_t h[44];
	uint32_t data = nsamples * 2;
	memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
	put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 1);
	put32(h + 24, 8000); put32(h + 28, 16000); put16(h + 32, 2); put16(h + 34, 16);
	memcpy(h + 36, "data", 4); put32(h + 40, data);
	write_all(fd, h, sizeof h, 0);
}

void WavWriter::append(const int16_t* s, int n)
{
	write_all(fd, s, n * 2, 44 + nsamples * 2);
	nsamples += n;
	write_header();
}

void WavWriter::pad_to(uint64_t n)
{
	int16_t zero[8000] = {};
	while (nsamples < n)
		append(zero, std::min<uint64_t>(8000, n - nsamples));
}

void WavWriter::close()
{
	if (fd >= 0) ::close(fd);
	fd = -1;
}

Recorder::Recorder(uint32_t hz, const std::string& dir, const std::string& start_utc_iso, double iq_rate,
		   Allocations* allocations, bool output_files, double span_center_hz)
	: hz(hz), dir(dir), allocations(allocations), iq_rate(iq_rate), span_center_hz(span_center_hz)
{
	if (!output_files) return;
	std::string base = dir + "/" + std::to_string(hz);
	wav.open(base + ".wav");
	log = fopen((base + ".log").c_str(), "w");
	if (!log) throw std::runtime_error(base + ".log: " + strerror(errno));
	fprintf(log, "# tetra-sniff hz=%u start_utc=%s iq_rate=%.0f wav=%u.wav\n", hz, start_utc_iso.c_str(), iq_rate, hz);
	fprintf(log, "# offset_s ISSI GSSI event hz TN usage_marker encr dl_usage chanalloc_hz chanalloc_tn iq_s\n");
	fflush(log);
}

static std::string col(long long v)
{
	return v < 0 ? "-" : std::to_string(v);
}

void Recorder::line(const char* event, long long tn, long long gssi, long long issi, long long usage_marker,
		    long long encr, long long dl_usage, long long ca_hz, long long ca_tn)
{
	if (!log) return;
	fprintf(log, "%.3f %s %s %s %u %s %s %s %s %s %s %.3f\n", wav.samples() / 8000.0,
		col(issi).c_str(), col(gssi).c_str(), event, hz, col(tn).c_str(),
		col(usage_marker).c_str(), col(encr).c_str(), col(dl_usage).c_str(),
		col(ca_hz).c_str(), col(ca_tn).c_str(), iq_s);
	fflush(log);
}

static long long marker_of(int dl_usage) { return dl_usage > 3 ? dl_usage : -1; }

static long long z(uint32_t v) { return v ? (long long)v : -1; }

void Recorder::open_talkspurt(const tetra_mac_event& ev)
{
	cur = Talkspurt{ ev.tn, ev.ssi, ev.issi, ev.encr, iq_s, iq_s, wav.samples() };
	int dl = slot_dl_usage[ev.tn - 1];
	line("PLAY", ev.tn, z(ev.ssi), z(ev.issi), marker_of(dl), ev.encr, dl, -1, -1);
}

void Recorder::end_talkspurt(int dl_usage)
{
	line("END", cur->tn, z(cur->gssi), z(cur->issi), marker_of(slot_dl_usage[cur->tn - 1]), cur->encr,
	     dl_usage, -1, -1);
	cur.reset();
}

void Recorder::retune(uint32_t new_hz)
{
	// A talkspurt belongs to the old frequency. Close it before the identity
	// of this slot changes, or its END line would name the new carrier.
	if (cur) end_talkspurt(-1);
	hz = new_hz;
	refused.clear();
	for (int i = 0; i < 4; i++) slot_dl_usage[i] = -1;
}

void Recorder::advance(double seconds_of_input)
{
	iq_s += seconds_of_input;
	if (cur && iq_s - cur->last_voice_iq_s > 1.0) end_talkspurt(-1);
}

void Recorder::advance(double seconds_of_input, uint64_t samples)
{
	processed_samples = samples;
	advance(seconds_of_input);
}

void Recorder::on_event(const tetra_mac_event& ev)
{
	switch (ev.kind) {
	case TETRA_EV_VOICE:
		if (cur && cur->tn == ev.tn && cur->gssi == ev.ssi) {
			if (ev.issi && cur->issi && ev.issi != cur->issi) {
				end_talkspurt(-1);
				open_talkspurt(ev);
				return;
			}
			if (ev.issi && !cur->issi) {
				cur->issi = ev.issi;
				int dl = slot_dl_usage[ev.tn - 1];
				line("TALKER", ev.tn, z(cur->gssi), ev.issi, marker_of(dl), cur->encr,
				     dl, -1, -1);
			}
			cur->last_voice_iq_s = iq_s;
			return;
		}
		if (cur) end_talkspurt(-1);
		open_talkspurt(ev);
		return;
	case TETRA_EV_SLOT:
		slot_dl_usage[ev.tn - 1] = ev.dl_usage;
		return;
	case TETRA_EV_RESOURCE:
		if (ev.alloc_type < 0) return;
		if (allocations && ev.encr == 0 && ev.speech == 1 && ev.e2ee == 0 &&
		    ev.alloc_hz && ev.alloc_hz != hz &&
		    !allocations->publish(ev.alloc_hz, ev.alloc_tn_mask, ev.usage_marker, ev.ssi,
					  ev.issi, processed_samples, hz) &&
		    // publish() also gives false for tn_mask 0, which means "go to MCCH", and
		    // for a corrupt mask. Only a carrier that the table misses counts here.
		    ev.alloc_tn_mask && !(ev.alloc_tn_mask & 0xf0)) {
			// No slot follows this carrier, so this clear call is going out on
			// air unrecorded. Ask the parent for a slot, so the next call on it
			// is recorded, and report the one that is lost now.
			// A grant can name a corrupt frequency, so the span test is also
			// the guard that keeps a wild value out of the pool.
			double off = std::fabs((double)ev.alloc_hz - span_center_hz);
			const char* status;
			const char* note;
			if (off >= PLAUSIBLE_BAND_HZ) {
				// Nothing reaches this, and no wider span would, so it must
				// not read as advice to move the centre.
				status = "BADFREQ";
				note = "is too far from the span to be a carrier, so that grant decoded wrong";
			} else if (off + 15e3 >= iq_rate / 2) {
				status = "OUTSIDE";
				note = "is outside the captured span. Move --center, or add a second dongle";
			} else if (allocations->want({ ev.alloc_hz, hz, ev.ssi })) {
				status = "UNTUNED";
				note = "is inside the span, so a free carrier slot takes it";
			} else {
				status = "NOSLOT";
				note = "is inside the span, but the carrier pool is full";
			}
			missed_call(status, ev);
			// One stdout line for each frequency, not for each call.
			if (refused.insert(ev.alloc_hz).second)
				fprintf(stdout, "%u: grant %u Hz GSSI %u %s\n", hz, ev.alloc_hz, ev.ssi,
					note);
		}
		line(ev.alloc_type == 1 || ev.alloc_type == 2 ? "ALLOC" : "REPLACE", ev.tn, z(ev.ssi),
		     z(ev.issi), ev.usage_marker, ev.encr, -1, ev.alloc_hz, ev.alloc_tn);
		return;
	case TETRA_EV_SYSINFO:
		line("SYSINFO", ev.tn, -1, -1, -1, -1, -1, -1, -1);
		return;
	}
}

// calls.log belongs to the stitch process, which writes the header. A child
// appends one short line, and O_APPEND keeps whole lines from several writers.
void Recorder::missed_call(const char* status, const tetra_mac_event& ev)
{
	if (processed_samples == last_missed_sample && ev.alloc_hz == last_missed_hz &&
	    ev.ssi == last_missed_gssi)
		return;
	last_missed_sample = processed_samples;
	last_missed_hz = ev.alloc_hz;
	last_missed_gssi = ev.ssi;
	if (calls_fd < 0) {
		calls_fd = ::open((dir + "/calls.log").c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
		if (calls_fd < 0) return;
	}
	// The columns of calls.log: capture_sample tdma_frame ISSI GSSI carrier_hz
	// control_hz TN usage status. A grant gives no frame number and no usage.
	dprintf(calls_fd, "%llu - %s %s %u %u %u - %s\n", (unsigned long long)processed_samples,
		col(z(ev.issi)).c_str(), col(z(ev.ssi)).c_str(), ev.alloc_hz, hz, ev.alloc_tn,
		status);
}

bool Recorder::permits_clear(uint8_t tn, int local_encr, uint32_t* ssi, uint32_t* issi,
			     uint32_t* control_hz) const
{
	Allocations::Grant grant = {};
	if (!allocations || tn < 1 || tn > 4 ||
	    !allocations->permits_clear(hz, tn, local_encr, slot_dl_usage[tn - 1],
					processed_samples, &grant))
		return false;
	*ssi = grant.ssi;
	*issi = grant.issi;
	*control_hz = grant.control_hz;
	return true;
}

void Recorder::on_voice(const int16_t* pcm, int n)
{
	if (!log) return;
	if (cur) {
		wav.pad_to(cur->wav_start + (uint64_t)llround((iq_s - cur->iq_start) * 8000));
		int dl = slot_dl_usage[cur->tn - 1];
		line("FRAME", cur->tn, z(cur->gssi), z(cur->issi), marker_of(dl), cur->encr,
		     dl, -1, -1);
	}
	wav.append(pcm, n);
}

void Recorder::finish()
{
	if (calls_fd >= 0) {
		::close(calls_fd);
		calls_fd = -1;
	}
	if (!log) return;
	if (cur) end_talkspurt(-1);
	wav.close();
	fclose(log);
	log = nullptr;
}
