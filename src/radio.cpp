#include "radio.h"

#include <chrono>
#include <thread>
#include <vector>

struct Radio {
	uint32_t center_hz = 0;
	uint32_t rate_hz = 0;
};

static constexpr uint32_t kFakeNativeRate = 20000000;

static bool g_plugged = false;
static std::vector<float> g_iq;

static const struct {
	uint16_t vid;
	uint16_t pid;
	const char* name;
} kKnown[] = {
	{0x0bda, 0x2838, "RTL-SDR"},
	{0x0bda, 0x2832, "RTL-SDR"},
	{0x1d50, 0x6089, "HackRF"},
	{0x1d50, 0x60a1, "Airspy"},
	{0x2cf0, 0x5250, "bladeRF"},
	{0x0403, 0x601f, "LimeSDR"},
	{0x0456, 0xb673, "Pluto"},
	{0x1df7, 0x3000, "SDRplay"},
};

void radio_fake_plug(bool present)
{
	g_plugged = present;
}

void radio_fake_queue(const float* interleaved_iq, size_t n_complex)
{
	if (!interleaved_iq || n_complex == 0) return;
	g_iq.insert(g_iq.end(), interleaved_iq, interleaved_iq + n_complex * 2);
}

RadioErr radio_open(Radio** radio, const RadioOpen& cfg)
{
	if (!radio) return RadioErr::refused;
	if (!g_plugged) return RadioErr::not_found;
	auto* r = new Radio;
	r->center_hz = cfg.center_hz;
	r->rate_hz = cfg.rate_hz ? cfg.rate_hz : kFakeNativeRate;
	*radio = r;
	return RadioErr::ok;
}

void radio_close(Radio* radio)
{
	delete radio;
}

RadioErr radio_set_center(Radio* radio, uint32_t center_hz)
{
	if (!radio) return RadioErr::refused;
	radio->center_hz = center_hz;
	g_iq.clear();
	return RadioErr::ok;
}

RadioErr radio_set_rate(Radio* radio, uint32_t rate_hz)
{
	if (!radio) return RadioErr::refused;
	radio->rate_hz = rate_hz;
	g_iq.clear();
	return RadioErr::ok;
}

uint32_t radio_max_rate(Radio* radio, uint32_t wanted)
{
	if (!radio) return 0;
	return wanted && kFakeNativeRate > wanted ? wanted : kFakeNativeRate;
}

int radio_read(Radio* radio, float* iq, int n_complex)
{
	if (!radio || !iq || n_complex <= 0) return 0;
	if (g_iq.empty())
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	if (g_iq.empty()) return 0;
	int n = (int)g_iq.size() / 2;
	if (n > n_complex) n = n_complex;
	for (int i = 0; i < n * 2; i++)
		iq[i] = g_iq[i];
	g_iq.erase(g_iq.begin(), g_iq.begin() + n * 2);
	return n;
}

const char* radio_error(RadioErr err)
{
	switch (err) {
	case RadioErr::ok: return "ok";
	case RadioErr::not_found: return "no SDR found";
	case RadioErr::not_recognized: return "SDR not recognized";
	case RadioErr::busy: return "busy";
	case RadioErr::refused: return "refused";
	case RadioErr::bad_index: return "bad index";
	}
	return "unknown";
}

RadioDetect radio_detect(size_t soapy_count, const UsbId* ids, size_t n)
{
	if (soapy_count > 0) return {RadioErr::ok, nullptr};
	if (!ids) return {RadioErr::not_found, nullptr};
	for (size_t i = 0; i < n; i++) {
		for (const auto& d : kKnown) {
			if (d.vid == ids[i].vid && d.pid == ids[i].pid)
				return {RadioErr::not_recognized, d.name};
		}
	}
	return {RadioErr::not_found, nullptr};
}
