#include "radio.h"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef __linux__
#include <dirent.h>
#endif

enum class Fake { off, present, absent };

static Fake g_fake = Fake::off;
static Radio* g_fake_radio;
static constexpr uint32_t kFakeNativeRate = 20000000;
static char g_last_error[96];
static std::vector<UsbId> g_usb_fake;
static bool g_usb_fake_on = false;

struct Radio {
	bool fake = false;
	uint32_t center_hz = 0;
	uint32_t rate_hz = 0;
	int gain_tenth_db = -1;
	std::atomic<uint64_t> overflows{0};
	std::atomic<bool> stopped{false};
	std::string driver;
	std::vector<float> iq;
	std::mutex mu;
	std::condition_variable cv;
	SoapySDR::Device* dev = nullptr;
	SoapySDR::Stream* stream = nullptr;
};

struct Known {
	uint16_t vid, pid;
	const char* name;
	const char* deb;
	const char* brew;
};

static const Known kKnown[] = {
	{0x0bda, 0x2838, "RTL-SDR", "soapysdr-module-rtlsdr", "soapyrtlsdr"},
	{0x0bda, 0x2832, "RTL-SDR", "soapysdr-module-rtlsdr", "soapyrtlsdr"},
	{0x1d50, 0x6089, "HackRF", "soapysdr-module-hackrf", "soapyhackrf"},
	{0x1d50, 0x604b, "HackRF", "soapysdr-module-hackrf", "soapyhackrf"},
	{0x1d50, 0x60a1, "Airspy", "soapysdr-module-airspy", "soapyairspy"},
	{0x2cf0, 0x5250, "bladeRF", "soapysdr-module-bladerf", nullptr},
	{0x0403, 0x601f, "LimeSDR", "soapysdr-module-lms7", nullptr},
	{0x0456, 0xb673, "Pluto", "soapysdr-module-plutosdr", nullptr},
	{0x1df7, 0x3000, "SDRplay", "soapysdr-module-sdrplay", nullptr},
};

void radio_fake_plug(bool present)
{
	g_fake = present ? Fake::present : Fake::absent;
}

void radio_fake_usb(const UsbId* ids, size_t n)
{
	if (!ids && n == 0) {
		g_usb_fake_on = false;
		g_usb_fake.clear();
		return;
	}
	g_fake = Fake::off;
	g_usb_fake_on = true;
	g_usb_fake.clear();
	if (ids && n) g_usb_fake.assign(ids, ids + n);
}

#ifdef __APPLE__
static bool ioreg_int(const char* line, const char* key, uint32_t* out)
{
	char pat[32];
	snprintf(pat, sizeof pat, "\"%s\"", key);
	const char* p = strstr(line, pat);
	if (!p) return false;
	p = strchr(p, '=');
	if (!p) return false;
	p++;
	while (*p == ' ') p++;
	if (*p == '<') {
		const char* hex = p + 1;
		const char* end = hex;
		while (*end && *end != '>') end++;
		if (end - hex != 8) return false;
		unsigned b0 = 0, b1 = 0;
		if (sscanf(hex, "%2x%2x", &b0, &b1) != 2) return false;
		*out = b0 | (b1 << 8);
		return *out <= 0xffff;
	}
	char* end = nullptr;
	unsigned long v = strtoul(p, &end, 0);
	if (end == p || v > 0xffff) return false;
	*out = (uint32_t)v;
	return true;
}

static void scan_ioreg(const char* cmd, std::vector<UsbId>& out)
{
	FILE* pipe = popen(cmd, "r");
	if (!pipe) return;
	char line[1024];
	int vid = -1, pid = -1;
	while (fgets(line, sizeof line, pipe)) {
		if (strstr(line, "+-o ")) vid = pid = -1;
		uint32_t v = 0;
		if (ioreg_int(line, "idVendor", &v) || ioreg_int(line, "vendor-id", &v))
			vid = (int)v;
		if (ioreg_int(line, "idProduct", &v) || ioreg_int(line, "product-id", &v))
			pid = (int)v;
		if (vid >= 0 && pid >= 0) {
			out.push_back({(uint16_t)vid, (uint16_t)pid});
			vid = pid = -1;
		}
	}
	pclose(pipe);
}

static void scan_profiler(std::vector<UsbId>& out)
{
	FILE* pipe = popen("system_profiler SPUSBDataType", "r");
	if (!pipe) return;
	char line[1024];
	int vid = -1, pid = -1;
	while (fgets(line, sizeof line, pipe)) {
		unsigned v = 0;
		if (const char* p = strstr(line, "Vendor ID:")) {
			if (sscanf(p, "Vendor ID: 0x%x", &v) == 1) vid = (int)v;
		}
		if (const char* p = strstr(line, "Product ID:")) {
			if (sscanf(p, "Product ID: 0x%x", &v) == 1) pid = (int)v;
		}
		if (vid >= 0 && pid >= 0) {
			out.push_back({(uint16_t)vid, (uint16_t)pid});
			vid = pid = -1;
		}
	}
	pclose(pipe);
}
#endif

static std::vector<UsbId> list_os_usb()
{
	std::vector<UsbId> out;
#ifdef __linux__
	DIR* dir = opendir("/sys/bus/usb/devices");
	if (!dir) return out;
	while (dirent* e = readdir(dir)) {
		if (e->d_name[0] == '.') continue;
		char vpath[512], ppath[512];
		snprintf(vpath, sizeof vpath, "/sys/bus/usb/devices/%s/idVendor", e->d_name);
		snprintf(ppath, sizeof ppath, "/sys/bus/usb/devices/%s/idProduct", e->d_name);
		FILE* fv = fopen(vpath, "r");
		FILE* fp = fopen(ppath, "r");
		unsigned vid = 0, pid = 0;
		if (fv && fp && fscanf(fv, "%x", &vid) == 1 && fscanf(fp, "%x", &pid) == 1)
			out.push_back({(uint16_t)vid, (uint16_t)pid});
		if (fv) fclose(fv);
		if (fp) fclose(fp);
	}
	closedir(dir);
#elif defined(__APPLE__)
	scan_ioreg("ioreg -p IOUSB -l", out);
	if (out.empty()) scan_ioreg("ioreg -r -c IOUSBHostDevice -l", out);
	if (out.empty()) scan_profiler(out);
#endif
	return out;
}

static std::vector<UsbId> list_usb()
{
	if (g_usb_fake_on) return g_usb_fake;
	return list_os_usb();
}

static void set_not_recognized(const char* name)
{
	const char* pkg = "";
	for (const auto& d : kKnown) {
		if (strcmp(d.name, name) != 0) continue;
#ifdef __APPLE__
		pkg = d.brew ? d.brew : d.deb;
#else
		pkg = d.deb;
#endif
		break;
	}
	snprintf(g_last_error, sizeof g_last_error, "%s found, install %s", name, pkg);
}

void radio_fake_queue(const float* interleaved_iq, size_t n_complex)
{
	if (!g_fake_radio || !interleaved_iq || n_complex == 0) return;
	std::lock_guard<std::mutex> lock(g_fake_radio->mu);
	g_fake_radio->iq.insert(g_fake_radio->iq.end(), interleaved_iq,
				interleaved_iq + n_complex * 2);
	g_fake_radio->cv.notify_all();
}

static void unmake(Radio* r)
{
	if (!r || !r->dev) return;
	if (r->stream) {
		try { r->dev->deactivateStream(r->stream); } catch (...) {}
		try { r->dev->closeStream(r->stream); } catch (...) {}
		r->stream = nullptr;
	}
	try { SoapySDR::Device::unmake(r->dev); } catch (...) {}
	r->dev = nullptr;
}

static void discard(Radio* r)
{
	if (r->fake) {
		std::lock_guard<std::mutex> lock(r->mu);
		r->iq.clear();
		return;
	}
	if (!r->dev || !r->stream) return;
	try {
		r->dev->deactivateStream(r->stream);
		r->dev->activateStream(r->stream);
	} catch (...) {}
}

static RadioErr soapy_open(Radio* r, const RadioOpen& cfg)
{
	SoapySDR::KwargsList devs;
	try {
		devs = SoapySDR::Device::enumerate();
	} catch (...) {
		devs.clear();
	}
	if (devs.empty()) {
		if (cfg.index != 0) return RadioErr::bad_index;
		auto ids = list_usb();
		RadioDetect d = radio_detect(0, ids.empty() ? nullptr : ids.data(), ids.size());
		if (d.err == RadioErr::not_recognized && d.name) set_not_recognized(d.name);
		return d.err;
	}
	if (cfg.index < 0 || (size_t)cfg.index >= devs.size()) return RadioErr::bad_index;
	try {
		r->dev = SoapySDR::Device::make(devs[(size_t)cfg.index]);
	} catch (...) {
		return RadioErr::busy;
	}
	if (!r->dev) return RadioErr::busy;
	try {
		r->driver = r->dev->getDriverKey();
		for (char& c : r->driver) c = (char)std::tolower((unsigned char)c);
	} catch (...) {
		r->driver.clear();
	}
	try {
		r->stream = r->dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
		if (!r->stream || r->dev->activateStream(r->stream) != 0) {
			unmake(r);
			return RadioErr::busy;
		}
	} catch (...) {
		unmake(r);
		return RadioErr::busy;
	}
	if (cfg.rate_hz) {
		try {
			r->dev->setSampleRate(SOAPY_SDR_RX, 0, cfg.rate_hz);
			double got = r->dev->getSampleRate(SOAPY_SDR_RX, 0);
			r->rate_hz = got > 0 ? (uint32_t)got : cfg.rate_hz;
		} catch (...) {
			unmake(r);
			return RadioErr::bad_rate;
		}
	}
	try {
		r->dev->setFrequency(SOAPY_SDR_RX, 0, cfg.center_hz);
		r->center_hz = cfg.center_hz;
	} catch (...) {
		unmake(r);
		return RadioErr::bad_tune;
	}
	try {
		if (cfg.gain_tenth_db < 0) {
			r->dev->setGainMode(SOAPY_SDR_RX, 0, true);
			r->gain_tenth_db = -1;
		} else {
			r->dev->setGainMode(SOAPY_SDR_RX, 0, false);
			r->dev->setGain(SOAPY_SDR_RX, 0, cfg.gain_tenth_db / 10.0);
			r->gain_tenth_db = (int)llround(r->dev->getGain(SOAPY_SDR_RX, 0) * 10);
		}
	} catch (...) {
		unmake(r);
		return RadioErr::bad_gain;
	}
	if (r->driver == "rtlsdr") {
		try {
			for (const auto& info : r->dev->getSettingInfo()) {
				if (info.key != "digital_agc") continue;
				r->dev->writeSetting("digital_agc", cfg.agc ? "true" : "false");
				break;
			}
		} catch (...) {
		}
	}
	discard(r);
	return RadioErr::ok;
}

RadioErr radio_open(Radio** radio, const RadioOpen& cfg)
{
	if (!radio) return RadioErr::refused;
	g_last_error[0] = 0;
	if (g_fake == Fake::absent) return RadioErr::not_found;
	if (g_usb_fake_on) {
		RadioDetect d = radio_detect(0, g_usb_fake.empty() ? nullptr : g_usb_fake.data(),
					     g_usb_fake.size());
		if (d.err == RadioErr::not_recognized && d.name) set_not_recognized(d.name);
		return d.err;
	}
	if (g_fake == Fake::present) {
		auto* r = new Radio;
		r->fake = true;
		r->driver = "fake";
		r->center_hz = cfg.center_hz;
		r->rate_hz = cfg.rate_hz;
		r->gain_tenth_db = cfg.gain_tenth_db < 0 ? -1 : cfg.gain_tenth_db;
		g_fake_radio = r;
		*radio = r;
		return RadioErr::ok;
	}
	auto* r = new Radio;
	RadioErr err = soapy_open(r, cfg);
	if (err != RadioErr::ok) {
		delete r;
		return err;
	}
	*radio = r;
	return RadioErr::ok;
}

void radio_stop(Radio* radio)
{
	if (!radio) return;
	radio->stopped.store(true);
	radio->cv.notify_all();
	if (radio->dev && radio->stream) {
		try { radio->dev->deactivateStream(radio->stream); } catch (...) {}
	}
}

void radio_close(Radio* radio)
{
	if (!radio) return;
	radio_stop(radio);
	if (g_fake_radio == radio) g_fake_radio = nullptr;
	unmake(radio);
	delete radio;
}

RadioErr radio_set_center(Radio* radio, uint32_t center_hz)
{
	if (!radio) return RadioErr::refused;
	if (!radio->fake) {
		try {
			radio->dev->setFrequency(SOAPY_SDR_RX, 0, center_hz);
		} catch (...) {
			return RadioErr::bad_tune;
		}
	}
	radio->center_hz = center_hz;
	discard(radio);
	return RadioErr::ok;
}

RadioErr radio_set_rate(Radio* radio, uint32_t rate_hz)
{
	if (!radio) return RadioErr::refused;
	if (!radio->fake) {
		try {
			radio->dev->setSampleRate(SOAPY_SDR_RX, 0, rate_hz);
			double got = radio->dev->getSampleRate(SOAPY_SDR_RX, 0);
			radio->rate_hz = got > 0 ? (uint32_t)got : rate_hz;
		} catch (...) {
			return RadioErr::bad_rate;
		}
	} else {
		radio->rate_hz = rate_hz;
	}
	discard(radio);
	return RadioErr::ok;
}

uint32_t radio_max_rate(Radio* radio, uint32_t wanted)
{
	if (!radio) return 0;
	if (radio->fake)
		return wanted && kFakeNativeRate > wanted ? wanted : kFakeNativeRate;
	uint32_t best = 0;
	auto take = [&](double hz) {
		if (hz <= 0) return;
		uint32_t u = (uint32_t)hz;
		if (wanted && u > wanted) return;
		if (u > best) best = u;
	};
	try {
		auto listed = radio->dev->listSampleRates(SOAPY_SDR_RX, 0);
		if (!listed.empty()) {
			for (double r : listed) take(r);
			return best;
		}
		for (const auto& range : radio->dev->getSampleRateRange(SOAPY_SDR_RX, 0)) {
			double hi = range.maximum();
			if (wanted && hi > wanted) hi = wanted;
			if (hi >= range.minimum()) take(hi);
		}
	} catch (...) {
	}
	return best;
}

uint32_t radio_rate(const Radio* radio)
{
	return radio ? radio->rate_hz : 0;
}

const char* radio_driver(const Radio* radio)
{
	return radio ? radio->driver.c_str() : "";
}

int radio_gain_tenth_db(const Radio* radio)
{
	return radio ? radio->gain_tenth_db : -1;
}

uint64_t radio_overflows(const Radio* radio)
{
	return radio ? radio->overflows.load() : 0;
}

int radio_read(Radio* radio, float* iq, int n_complex)
{
	if (!radio || !iq || n_complex <= 0) return radio && radio->stopped.load() ? -1 : 0;
	if (radio->stopped.load()) return -1;
	if (radio->fake) {
		std::unique_lock<std::mutex> lock(radio->mu);
		if (radio->iq.empty())
			radio->cv.wait_for(lock, std::chrono::milliseconds(RADIO_READ_TIMEOUT_MS),
					   [&] { return radio->stopped.load() || !radio->iq.empty(); });
		if (radio->stopped.load()) return -1;
		if (radio->iq.empty()) return 0;
		int n = (int)radio->iq.size() / 2;
		if (n > n_complex) n = n_complex;
		for (int i = 0; i < n * 2; i++)
			iq[i] = radio->iq[i];
		radio->iq.erase(radio->iq.begin(), radio->iq.begin() + n * 2);
		return n;
	}
	if (!radio->dev || !radio->stream) return -1;
	void* buffs[] = { iq };
	int flags = 0;
	long long timeNs = 0;
	int ret = radio->dev->readStream(radio->stream, buffs, (size_t)n_complex, flags, timeNs,
					 (long)RADIO_READ_TIMEOUT_MS * 1000);
	if (radio->stopped.load()) return -1;
	if (ret == SOAPY_SDR_TIMEOUT) return 0;
	if (ret == SOAPY_SDR_OVERFLOW) {
		radio->overflows.fetch_add(1);
		return 0;
	}
	if (ret < 0) return -1;
	if (ret > n_complex) ret = n_complex;
	if (flags & SOAPY_SDR_END_ABRUPT) radio->overflows.fetch_add(1);
	return ret;
}

const char* radio_error(RadioErr err)
{
	switch (err) {
	case RadioErr::ok: return "ok";
	case RadioErr::not_found: return "no SDR found";
	case RadioErr::not_recognized:
		return g_last_error[0] ? g_last_error : "the SDR was not recognized";
	case RadioErr::busy:
		return "the receiver did not open (quit SDR++ if it holds the dongle)";
	case RadioErr::refused: return "the receiver refused the request";
	case RadioErr::bad_index: return "there is no receiver at that device index";
	case RadioErr::bad_rate: return "the receiver did not take the sample rate";
	case RadioErr::bad_tune: return "the receiver did not tune";
	case RadioErr::bad_gain: return "the receiver did not take the tuner gain";
	}
	return "the receiver failed for an unknown reason";
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
