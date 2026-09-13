#pragma once
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>

extern "C" {
#include "tetra_common.h"
}

class Allocations;

class WavWriter {
public:
	void open(const std::string& path);
	void append(const int16_t* s, int n);
	void pad_to(uint64_t n);
	uint64_t samples() const { return nsamples; }
	void close();

private:
	void write_header();
	int fd = -1;
	uint64_t nsamples = 0;
};

struct Talkspurt {
	int tn;
	uint32_t gssi;
	uint32_t issi;
	int encr;
	double last_voice_iq_s;
	double iq_start;
	uint64_t wav_start;
};

class Recorder {
public:
	Recorder(uint32_t hz, const std::string& dir, const std::string& start_utc_iso, double iq_rate,
		 Allocations* allocations = nullptr, bool output_files = true,
		 double span_center_hz = 0);
	~Recorder() { finish(); }
	void advance(double seconds_of_input);
	void advance(double seconds_of_input, uint64_t processed_samples);
	void on_event(const tetra_mac_event& ev);
	void on_voice(const int16_t* pcm, int n);
	// True when --per-carrier asked for a WAV and a log for this carrier.
	bool writes_files() const { return log != nullptr; }
	// The slot moved to another frequency. The caller rebuilds the decoder.
	void retune(uint32_t new_hz);
	bool permits_clear(uint8_t tn, int local_encr, uint32_t* ssi, uint32_t* issi,
			   uint32_t* control_hz) const;
	void finish();

private:
	void line(const char* event, long long tn, long long gssi, long long issi, long long usage_marker,
		  long long encr, long long dl_usage, long long ca_hz, long long ca_tn);
	// A clear call that this run cannot record, written into the shared
	// calls.log so a talkgroup's history shows the gap.
	void missed_call(const char* status, const tetra_mac_event& ev);
	void open_talkspurt(const tetra_mac_event& ev);
	void end_talkspurt(int dl_usage);

	uint32_t hz;
	std::string dir;
	int calls_fd = -1;
	WavWriter wav;
	FILE* log = nullptr;
	double iq_s = 0;
	uint64_t processed_samples = 0;
	Allocations* allocations;
	// The span holds a carrier if |carrier_hz - span_center_hz| + 15 kHz < iq_rate / 2.
	double iq_rate = 0, span_center_hz = 0;
	// A refused grant names a carrier that the table does not hold. Report each one time.
	std::set<uint32_t> refused;
	// A control carrier repeats a grant, so the same missed call arrives several
	// times at one sample. Report it once.
	uint64_t last_missed_sample = 0;
	uint32_t last_missed_hz = 0, last_missed_gssi = 0;
	int slot_dl_usage[4] = { -1, -1, -1, -1 };
	std::optional<Talkspurt> cur;
};
