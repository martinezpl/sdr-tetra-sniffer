#pragma once
// A polyphase filter bank. It splits the whole span into M sub-bands in one
// pass, so a carrier slot filters one narrow sub-band and not the full rate.
// The cost of the bank does not grow with the number of slots.
//
// The bank is oversampled by 2. Sub-band c has its centre at c * rate / M and
// comes out at 2 * rate / M. Every frequency is within rate / 2M of a centre,
// so a 30 kHz carrier always lies flat inside one sub-band, with no alias on
// it. Below 16 sub-bands the bank saves little, so it passes the input through.
//
// A wide span is too much for one core of a Raspberry Pi, so the bank splits
// each block among worker threads. Each output depends only on input samples,
// so the workers need no order between them.

#include <functional>
#include <thread>
#include <vector>

#include <fftw3.h>

#include <dsp/types.h>

// The number of sub-bands for this rate. 1 means no bank.
int pfb_channels(double rate);

// Calls f(w) for w = 0 .. workers-1, each on its own thread, and waits.
// ponytail: a thread for each call, 60 calls a second at 61.44 MS/s; a pool
// if that ever shows in a profile.
inline void run_parallel(int workers, const std::function<void(int)>& f)
{
	std::vector<std::thread> t;
	for (int w = 1; w < workers; w++) t.emplace_back(f, w);
	f(0);
	for (auto& x : t) x.join();
}

struct Channelizer {
	Channelizer() = default;
	Channelizer(const Channelizer&) = delete;
	Channelizer& operator=(const Channelizer&) = delete;
	~Channelizer();

	// max_block is the largest count that process() gets.
	void init(double rate, int max_block);
	int channels() const { return m; }
	// Threads that process() uses, and that a caller can use for its slots.
	int workers() const { return nw; }
	double out_rate() const { return m == 1 ? rate : 2 * rate / m; }
	// The sub-band that holds offset_hz from the centre of the span, and the
	// offset that is left inside that sub-band.
	int channel_of(double offset_hz, double* rest_hz) const;
	// Only a used sub-band keeps its samples.
	void use(int c);
	void clear_use();
	// Every sub-band gets the same number of samples, and that is the result.
	int process(int count, const dsp::complex_t* in);
	const dsp::complex_t* out(int c) const;

	// The prototype low-pass filter, in the order of time. The test reads it.
	std::vector<float> proto;

private:
	void emit(int w, const dsp::complex_t* newest, int k, unsigned long long step);

	double rate = 0;
	int m = 1, d = 1, hist = 0, since = 0, max_out = 0, nw = 1;
	unsigned long long steps = 0;
	std::vector<float> taps;           // proto, reversed inside each branch, each tap twice
	std::vector<dsp::complex_t> seam;  // the last hist samples, then the first hist new ones
	std::vector<dsp::complex_t> rot;   // e^(-j 2 pi k / m)
	std::vector<std::vector<dsp::complex_t>> outs;
	std::vector<bool> used;
	std::vector<int> used_list;
	const dsp::complex_t* last_in = nullptr;
	std::vector<fftwf_complex*> acc, spec;  // one pair for each worker
	fftwf_plan plan = nullptr;
};
