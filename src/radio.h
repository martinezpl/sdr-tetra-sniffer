#pragma once
#include <cstddef>
#include <cstdint>

struct Radio;

struct RadioOpen {
	uint32_t center_hz = 0;
	uint32_t rate_hz = 0;
	int index = 0;
	int gain_tenth_db = -1;
	bool agc = false;
};

enum class RadioErr {
	ok = 0,
	not_found,
	not_recognized,
	busy,
	refused,
	bad_index,
	bad_rate,
	bad_tune,
	bad_gain,
};

constexpr int RADIO_READ_TIMEOUT_MS = 100;

struct UsbId {
	uint16_t vid;
	uint16_t pid;
};

struct RadioDetect {
	RadioErr err;
	const char* name;
};

RadioErr radio_open(Radio** radio, const RadioOpen& cfg);
void radio_close(Radio* radio);
void radio_stop(Radio* radio);
RadioErr radio_set_center(Radio* radio, uint32_t center_hz);
RadioErr radio_set_rate(Radio* radio, uint32_t rate_hz);
uint32_t radio_max_rate(Radio* radio, uint32_t wanted);
uint32_t radio_rate(const Radio* radio);
const char* radio_driver(const Radio* radio);
int radio_gain_tenth_db(const Radio* radio);
uint64_t radio_overflows(const Radio* radio);
int radio_read(Radio* radio, float* iq, int n_complex);
const char* radio_error(RadioErr err);
RadioDetect radio_detect(size_t soapy_count, const UsbId* ids, size_t n);

void radio_fake_plug(bool present);
void radio_fake_queue(const float* interleaved_iq, size_t n_complex);
