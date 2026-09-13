#include "rtl.h"

#include <cstdlib>
#include <vector>

extern "C" {
typedef struct rtlsdr_dev rtlsdr_dev_t;
// Each declaration matches rtl-sdr.h. rtlsdr_get_device_count returns uint32_t there.
uint32_t rtlsdr_get_device_count(void);
int rtlsdr_open(rtlsdr_dev_t** dev, uint32_t index);
int rtlsdr_close(rtlsdr_dev_t* dev);
int rtlsdr_set_sample_rate(rtlsdr_dev_t* dev, uint32_t rate);
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t* dev);
int rtlsdr_set_center_freq(rtlsdr_dev_t* dev, uint32_t freq);
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t* dev, int manual);
int rtlsdr_set_tuner_gain(rtlsdr_dev_t* dev, int gain);
int rtlsdr_get_tuner_gains(rtlsdr_dev_t* dev, int* gains);
int rtlsdr_set_agc_mode(rtlsdr_dev_t* dev, int on);
int rtlsdr_reset_buffer(rtlsdr_dev_t* dev);
int rtlsdr_read_sync(rtlsdr_dev_t* dev, void* buf, int len, int* n_read);
int rtlsdr_read_async(rtlsdr_dev_t* dev, void (*cb)(unsigned char*, uint32_t, void*),
		      void* ctx, uint32_t buf_num, uint32_t buf_len);
int rtlsdr_cancel_async(rtlsdr_dev_t* dev);
}

// The tuner takes only the gains of its own table. Pick the nearest one.
static int nearest_gain(rtlsdr_dev_t* d, int want)
{
	int count = rtlsdr_get_tuner_gains(d, nullptr);
	if (count < 1) return want;
	std::vector<int> gains(count);
	if (rtlsdr_get_tuner_gains(d, gains.data()) < 1) return want;
	int best = gains[0];
	for (int g : gains)
		if (std::abs(g - want) < std::abs(best - want)) best = g;
	return best;
}

int rtl_open(rtlsdr_dev** dev, uint32_t center_hz, uint32_t rate_hz, int index, int gain_tenth_db,
	     bool agc, int* applied_gain_tenth_db)
{
	auto** d = (rtlsdr_dev_t**)dev;
	uint32_t count = rtlsdr_get_device_count();
	if (count < 1) return -1;
	if (index < 0 || (uint32_t)index >= count) return -5;
	if (rtlsdr_open(d, (uint32_t)index) < 0) return -2;
	if (rtlsdr_set_sample_rate(*d, rate_hz) < 0) { rtlsdr_close(*d); return -3; }
	if (rtlsdr_set_center_freq(*d, center_hz) < 0) { rtlsdr_close(*d); return -4; }
	if (applied_gain_tenth_db) *applied_gain_tenth_db = -1;
	if (gain_tenth_db < 0) {
		// The automatic gain of the tuner. A failure here leaves the gain of the
		// last run, which records the wrong level in silence, so it ends the run.
		if (rtlsdr_set_tuner_gain_mode(*d, 0) < 0) { rtlsdr_close(*d); return -6; }
	} else {
		if (rtlsdr_set_tuner_gain_mode(*d, 1) < 0) { rtlsdr_close(*d); return -6; }
		int gain = nearest_gain(*d, gain_tenth_db);
		if (rtlsdr_set_tuner_gain(*d, gain) < 0) { rtlsdr_close(*d); return -6; }
		if (applied_gain_tenth_db) *applied_gain_tenth_db = gain;
	}
	if (rtlsdr_set_agc_mode(*d, agc ? 1 : 0) < 0) { rtlsdr_close(*d); return -7; }
	rtlsdr_reset_buffer(*d);
	return 0;
}

// The highest rate this device really takes, which is the widest span it can
// give. librtlsdr documents two ranges and refuses the rest, but a device can
// round a rate or refuse one the header allows, so ask it rather than assume:
// set a candidate, read back what it settled on, and keep the best that stuck.
uint32_t rtl_max_rate(rtlsdr_dev* dev, uint32_t wanted)
{
	static const uint32_t CANDIDATES[] = { 3200000, 2880000, 2560000, 2400000,
					       2048000, 1800000, 1024000, 900001 };
	auto* d = (rtlsdr_dev_t*)dev;
	uint32_t best = 0;
	for (uint32_t rate : CANDIDATES) {
		if (wanted && rate > wanted) continue;
		if (rtlsdr_set_sample_rate(d, rate) < 0) continue;
		uint32_t got = rtlsdr_get_sample_rate(d);
		// A device may round. Take what it reports, not what was asked.
		if (got && got > best) best = got;
		if (best) break;   // the list runs high to low, so the first is the best
	}
	return best;
}

int rtl_set_rate(rtlsdr_dev* dev, uint32_t rate_hz)
{
	if (rtlsdr_set_sample_rate((rtlsdr_dev_t*)dev, rate_hz) < 0) return -3;
	rtlsdr_reset_buffer((rtlsdr_dev_t*)dev);
	return 0;
}

int rtl_set_center(rtlsdr_dev* dev, uint32_t center_hz)
{
	auto* d = (rtlsdr_dev_t*)dev;
	if (rtlsdr_set_center_freq(d, center_hz) < 0) return -4;
	// Whatever the buffer still holds came from the old frequency.
	rtlsdr_reset_buffer(d);
	return 0;
}

const char* rtl_error(int rc)
{
	switch (rc) {
	case -1: return "no RTL-SDR found";
	case -2: return "the RTL-SDR did not open (quit SDR++ if it holds the dongle)";
	case -3: return "the RTL-SDR did not take the sample rate";
	case -4: return "the RTL-SDR did not tune";
	case -5: return "there is no RTL-SDR at that device index";
	case -6: return "the RTL-SDR did not take the tuner gain";
	case -7: return "the RTL-SDR did not take the AGC setting";
	}
	return "the RTL-SDR failed for an unknown reason";
}

int rtl_read(rtlsdr_dev* dev, void* buf, int len)
{
	int n = 0;
	if (rtlsdr_read_sync((rtlsdr_dev_t*)dev, buf, len, &n) < 0) return -1;
	return n;
}

struct AsyncCtx {
	rtl_async_cb cb;
	void* ctx;
};

static void async_cb(unsigned char* data, uint32_t len, void* p)
{
	auto* ctx = (AsyncCtx*)p;
	ctx->cb(data, len, ctx->ctx);
}

int rtl_read_async(rtlsdr_dev* dev, rtl_async_cb cb, void* ctx, uint32_t block_bytes)
{
	AsyncCtx a = { cb, ctx };
	return rtlsdr_read_async((rtlsdr_dev_t*)dev, async_cb, &a, 0, block_bytes);
}

void rtl_cancel_async(rtlsdr_dev* dev)
{
	rtlsdr_cancel_async((rtlsdr_dev_t*)dev);
}

void rtl_close(rtlsdr_dev* dev)
{
	if (dev) rtlsdr_close((rtlsdr_dev_t*)dev);
}
