#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static const char* USAGE =
	"usage: gen_iq --rate HZ --seconds S --out FILE|- OFFSET_HZ...\n"
	"Writes raw cf32 IQ with one synthetic TETRA downlink carrier per OFFSET_HZ (relative to centre).\n"
	"Each carrier is pi/4-DQPSK at 18 ksym/s, RRC 0.35, frames of one sync burst plus three normal\n"
	"bursts with real training sequences and random payload. HZ must be a multiple of 18000.\n";

static const int SYM_RATE = 18000;
static const int BURST_BITS = 510;
static const uint8_t y_bits[38] = { 1,1, 0,0, 0,0, 0,1, 1,0, 0,1, 1,1, 0,0, 1,1, 1,0, 1,0, 0,1, 1,1, 0,0, 0,0, 0,1, 1,0, 0,1, 1,1 };
static const uint8_t n_bits[22] = { 1,1, 0,1, 0,0, 0,0, 1,1, 1,0, 1,0, 0,1, 1,1, 0,1, 0,0 };
static const int SYNC_TRAIN_POS = 214;
static const int NORM_TRAIN_POS = 244;

static double rrc(double t, double b)
{
	if (std::fabs(t) < 1e-9) return 1 - b + 4 * b / M_PI;
	if (std::fabs(std::fabs(t) - 1 / (4 * b)) < 1e-9)
		return b / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * b)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * b)));
	return (std::sin(M_PI * t * (1 - b)) + 4 * b * t * std::cos(M_PI * t * (1 + b))) /
	       (M_PI * t * (1 - std::pow(4 * b * t, 2)));
}

static std::vector<uint8_t> frame_bits(std::mt19937& rng)
{
	std::vector<uint8_t> f(4 * BURST_BITS);
	for (auto& b : f) b = rng() & 1;
	memcpy(&f[SYNC_TRAIN_POS], y_bits, sizeof y_bits);
	for (int tn = 1; tn < 4; tn++) memcpy(&f[tn * BURST_BITS + NORM_TRAIN_POS], n_bits, sizeof n_bits);
	return f;
}

int main(int argc, char** argv)
{
	double rate = 0, seconds = 0;
	std::string out;
	std::vector<double> offsets;
	for (int i = 1; i < argc; i++) {
		std::string s = argv[i];
		if (s == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
		else if (s == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
		else if (s == "--out" && i + 1 < argc) out = argv[++i];
		else if (s == "--help") { fputs(USAGE, stdout); return 0; }
		else offsets.push_back(atof(s.c_str()));
	}
	if (rate <= 0 || (long)rate % SYM_RATE || seconds <= 0 || out.empty()) { fputs(USAGE, stderr); return 2; }
	FILE* f = out == "-" ? stdout : fopen(out.c_str(), "wb");
	if (!f) { perror(out.c_str()); return 2; }

	int L = (int)rate / SYM_RATE;
	int span = 8;
	std::vector<double> pulse(span * L + 1);
	for (size_t i = 0; i < pulse.size(); i++) pulse[i] = rrc(((double)i - span * L / 2) / L, 0.35);

	size_t nsyms = (size_t)(seconds * SYM_RATE);
	size_t nsamp = nsyms * L;
	std::vector<std::complex<float>> sum(nsamp + pulse.size());
	std::mt19937 rng(1);
	std::normal_distribution<float> noise(0, 0.003f);
	for (auto& s : sum) s = { noise(rng), noise(rng) };

	static const double step[4] = { M_PI / 4, 3 * M_PI / 4, -M_PI / 4, -3 * M_PI / 4 };
	std::vector<std::complex<double>> base(nsamp + pulse.size());
	for (double off : offsets) {
		std::fill(base.begin(), base.end(), 0.0);
		double phi = 0;
		std::vector<uint8_t> bits;
		const size_t frame_syms = 4 * BURST_BITS / 2;
		for (size_t k = 0; k < nsyms; k++) {
			if (k % frame_syms == 0) bits = frame_bits(rng);
			size_t bi = 2 * (k % frame_syms);
			phi += step[bits[bi] << 1 | bits[bi + 1]];
			std::complex<double> sym = std::polar(1.0, phi);
			for (size_t i = 0; i < pulse.size(); i++) base[k * L + i] += sym * pulse[i];
		}
		double w = 2 * M_PI * off / rate;
		for (size_t n = 0; n < sum.size(); n++)
			sum[n] += std::complex<float>(base[n] * std::polar(0.3, w * (double)n));
	}
	fwrite(sum.data(), sizeof sum[0], nsamp, f);
	if (f != stdout) fclose(f);
	return 0;
}
