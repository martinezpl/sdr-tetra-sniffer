#include "pfb.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

static int fails;

static void check(bool ok, const char* name)
{
	if (!ok) {
		fails++;
		std::cerr << "FAIL " << name << "\n";
	}
}

static void pass(const char* name)
{
	if (fails) std::exit(1);
	std::cout << "PASS " << name << "\n";
}

// |H(f)| of the prototype, in dB against DC.
static double response_db(const std::vector<float>& h, double f, double rate)
{
	std::complex<double> s = 0, dc = 0;
	for (size_t i = 0; i < h.size(); i++) {
		s += (double)h[i] * std::polar(1.0, -2 * M_PI * f * (double)i / rate);
		dc += h[i];
	}
	return 20 * std::log10(std::abs(s) / std::abs(dc));
}

int main()
{
	check(pfb_channels(61440000) == 128, "61.44 MS/s gives 128 sub-bands");
	check(pfb_channels(3200000) == 1, "3.2 MS/s gives no bank");
	check(pfb_channels(62537500) == 1, "a rate with few factors of 2 gives no bank");
	check(pfb_channels(20000000) == 64, "20 MS/s gives 64 sub-bands");
	pass("pfb_channels");

	const double rate = 61440000;
	const int block = 4096;
	Channelizer bank;
	bank.init(rate, block);
	const int m = bank.channels(), d = m / 2;
	const double spacing = rate / m;
	check(bank.out_rate() == 960000, "out_rate");

	double rest;
	check(bank.channel_of(-200000, &rest) == 0 && std::fabs(rest + 200000) < 1e-6, "channel_of near the centre");
	check(bank.channel_of(25e6, &rest) == 52 && std::fabs(rest - 40000) < 1e-3, "channel_of above");
	check(bank.channel_of(-25e6, &rest) == m - 52 && std::fabs(rest + 40000) < 1e-3, "channel_of below");
	pass("channel_of");

	// A carrier 15 kHz wide at the worst place in its sub-band is flat, and
	// nothing that folds onto it gets through.
	check(response_db(bank.proto, 0.5 * spacing + 15000, rate) > -0.5, "passband");
	double worst = -1000, worst_f = 0;
	for (double f = 1.5 * spacing - 15000; f <= rate / 2; f += spacing / 50) {
		double db = response_db(bank.proto, f, rate);
		if (db > worst) worst = db, worst_f = f;
	}
	if (worst >= -70) std::cerr << "worst alias " << worst << " dB at " << worst_f << " Hz\n";
	check(worst < -70, "alias rejection");
	pass("prototype filter");

	// Random input in uneven pieces, against the definition of sub-band c:
	// sum h[i] x[g-i] e^(-j 2 pi c (g-i) / m) at every d-th sample g.
	std::vector<dsp::complex_t> x(3 * block);
	uint32_t seed = 1;
	for (auto& v : x) {
		seed = seed * 1664525u + 1013904223u;
		v.re = (float)(seed >> 8) / (1 << 24) - 0.5f;
		seed = seed * 1664525u + 1013904223u;
		v.im = (float)(seed >> 8) / (1 << 24) - 0.5f;
	}
	const int chans[] = { 0, 1, 5, 63, 64, 65, 127 };
	for (int c : chans) bank.use(c);
	std::vector<std::vector<dsp::complex_t>> got(m);
	const int pieces[] = { 1000, 4096, 17, 3000, 1, 60, 4000 };
	size_t at = 0;
	for (int n : pieces) {
		if (at + n > x.size()) n = (int)(x.size() - at);
		int k = bank.process(n, x.data() + at);
		for (int c : chans) got[c].insert(got[c].end(), bank.out(c), bank.out(c) + k);
		at += n;
	}
	check(got[0].size() == at / d, "one output for every d inputs");
	double err = 0, peak = 0;
	for (int c : chans)
		for (size_t s = 0; s < got[c].size(); s++) {
			long g = (long)(s + 1) * d - 1;
			std::complex<double> want = 0;
			for (long i = 0; i < (long)bank.proto.size() && g - i >= 0; i++) {
				std::complex<double> v(x[g - i].re, x[g - i].im);
				want += (double)bank.proto[i] * v * std::polar(1.0, -2 * M_PI * c * (double)((g - i) % m) / m);
			}
			std::complex<double> have(got[c][s].re, got[c][s].im);
			err = std::max(err, std::abs(have - want));
			peak = std::max(peak, std::abs(want));
		}
	check(err < 1e-4 * peak, "matches the definition");
	pass("sub-bands");

	Channelizer thru;
	thru.init(3200000, block);
	check(thru.channels() == 1 && thru.out_rate() == 3200000, "no bank at 3.2 MS/s");
	check(thru.process(123, x.data()) == 123 && thru.out(0) == x.data(), "passes the input through");
	pass("pass-through");
	return 0;
}
