#include "pfb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <dsp/taps/windowed_sinc.h>
#include <dsp/window/nuttall.h>

// Sub-bands at most this far apart. A narrower sub-band makes each slot
// cheaper, and the FFT only grows with log2 M. Measured on a Pi 5 at 61.44
// MS/s with 24 slots: 64 sub-bands cost 1.28 cores, 128 cost 1.05, 256 cost
// 0.99, but 256 leave the filter less margin.
static const double MAX_SPACING = 0.5e6;
// The filter cuts off at the spacing. A carrier needs flat response to 0.5
// spacing + 15 kHz, and what folds onto it starts at 1.5 spacing - 15 kHz.
// The main lobe of the Nuttall window is about 8 * rate / taps wide. With 8
// taps for each branch that fold is at -98 dB for 128 sub-bands at 61.44
// MS/s, and at -79 dB at the narrowest spacing that MAX_SPACING allows.
static const int TAPS_PER_BRANCH = 8;
// Below this the span is narrow, and plain slot filters cost little: 15
// slots at 3.2 MS/s take about a quarter of a core. So a narrow span keeps
// the path that has no bank.
static const int MIN_CHANNELS = 16;
// The parent, the carrier processes and the driver share the cores.
static const unsigned MAX_WORKERS = 4;

int pfb_channels(double rate)
{
	int m = 1;
	while (rate / m > MAX_SPACING) m *= 2;
	// An integer sub-band rate gives the resampler of each slot a whole input
	// rate. The power-of-2 stage inside that SDR++ resampler can still round:
	// 875 kHz / 16 is 54687.5 Hz, which puts those slots 9 ppm off 36 kS/s.
	while (m >= MIN_CHANNELS && std::fmod(rate, m / 2) != 0) m /= 2;
	return m >= MIN_CHANNELS ? m : 1;
}

Channelizer::~Channelizer()
{
	if (plan) fftwf_destroy_plan(plan);
	for (auto* p : acc) fftwf_free(p);
	for (auto* p : spec) fftwf_free(p);
}

void Channelizer::init(double r, int max_block)
{
	rate = r;
	m = pfb_channels(r);
	if (m == 1) return;
	d = m / 2;
	nw = (int)std::min(MAX_WORKERS, std::max(1u, std::thread::hardware_concurrency()));
	int len = m * TAPS_PER_BRANCH;
	auto h = dsp::taps::windowedSinc<float>(len, rate / m, rate, dsp::window::nuttall);
	proto.assign(h.taps, h.taps + len);
	dsp::taps::free(h);
	// Each tap twice, once for re and once for im, so that the branch sum is
	// a plain float multiply-add that the compiler can vectorize.
	taps.resize(2 * len);
	for (int p = 0; p < TAPS_PER_BRANCH; p++)
		for (int j = 0; j < m; j++)
			taps[2 * (p * m + j)] = taps[2 * (p * m + j) + 1] = proto[p * m + m - 1 - j];
	hist = len - 1;
	seam.assign(2 * hist, dsp::complex_t{0, 0});
	rot.resize(m);
	for (int k = 0; k < m; k++)
		rot[k] = { (float)std::cos(-2 * M_PI * k / m), (float)std::sin(-2 * M_PI * k / m) };
	max_out = max_block / d + 1;
	outs.assign(m, {});
	used.assign(m, false);
	for (int w = 0; w < nw; w++) {
		acc.push_back(fftwf_alloc_complex(m));
		spec.push_back(fftwf_alloc_complex(m));
	}
	// One plan serves every worker: the new-array execute is thread safe.
	plan = fftwf_plan_dft_1d(m, acc[0], spec[0], FFTW_FORWARD, FFTW_MEASURE);
}

int Channelizer::channel_of(double offset_hz, double* rest_hz) const
{
	if (m == 1) {
		*rest_hz = offset_hz;
		return 0;
	}
	double spacing = rate / m;
	long k = lround(offset_hz / spacing);
	*rest_hz = offset_hz - k * spacing;
	return (int)(((k % m) + m) % m);
}

void Channelizer::use(int c)
{
	if (m == 1 || used[c]) return;
	used[c] = true;
	used_list.push_back(c);
	outs[c].resize(max_out);
}

void Channelizer::clear_use()
{
	if (m == 1) return;
	used.assign(m, false);
	used_list.clear();
}

const dsp::complex_t* Channelizer::out(int c) const
{
	return m == 1 ? last_in : outs[c].data();
}

int Channelizer::process(int count, const dsp::complex_t* in)
{
	if (m == 1) {
		last_in = in;
		return count;
	}
	// Output k falls on sample first + k d of this block. `since` samples of
	// the first one arrived in the block before.
	int first = d - 1 - since;
	int n = first < count ? (count - 1 - first) / d + 1 : 0;
	// An output reads its newest sample and hist older ones. Near the start
	// of the block some of those are in the block before, so those outputs
	// read the seam: the old tail, then the head of this block.
	int head = std::min(count, hist);
	memcpy(seam.data() + hist, in, head * sizeof(dsp::complex_t));
	int nt = std::max(1, std::min(nw, n));
	run_parallel(nt, [&](int w) {
		int lo = (int)((long)n * w / nt), hi = (int)((long)n * (w + 1) / nt);
		for (int k = lo; k < hi; k++) {
			int t = first + k * d;
			emit(w, t < hist ? seam.data() + hist + t : in + t, k, steps + k);
		}
	});
	// The new tail is the last hist samples of the old tail and this block.
	if (count >= hist)
		memcpy(seam.data(), in + count - hist, hist * sizeof(dsp::complex_t));
	else
		memmove(seam.data(), seam.data() + count, hist * sizeof(dsp::complex_t));
	since = (since + count) % d;
	steps += n;
	return n;
}

// All branches for one output, as floats: n is 2m, and the sum over the
// branches stays in a register. TAPS_PER_BRANCH is a constant, so the inner
// loop unrolls and the outer loop vectorizes.
static void branches(float* __restrict a, const float* __restrict h, const float* __restrict x, int n)
{
	for (int k = 0; k < n; k++) {
		float s = 0;
		for (int p = 0; p < TAPS_PER_BRANCH; p++)
			s += h[p * n + k] * x[k - p * n];
		a[k] = s;
	}
}

// Sub-band c at the newest sample g is sum h[i] x[g-i] e^(-j 2 pi c (g-i) / m).
// Put i = p m + k. The sum over p is the branch k, and the sum over k is a DFT
// of the branches. The branches go into acc in reverse, so that both arrays
// run forward in the inner loop. That reverse and the time g together cost
// the factor e^(-j 2 pi c (g+1) / m) after a forward FFT.
void Channelizer::emit(int w, const dsp::complex_t* newest, int k, unsigned long long step)
{
	branches((float*)acc[w], taps.data(), (const float*)(newest - (m - 1)), 2 * m);
	fftwf_execute_dft(plan, acc[w], spec[w]);
	int g1 = (int)(((step + 1) * (unsigned long long)d) % m);
	for (int c : used_list) {
		const dsp::complex_t& r = rot[(c * g1) % m];
		float re = spec[w][c][0], im = spec[w][c][1];
		outs[c][k] = { re * r.re - im * r.im, re * r.im + im * r.re };
	}
}
