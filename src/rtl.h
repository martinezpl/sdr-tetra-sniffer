#pragma once
#include <cstdint>

struct rtlsdr_dev;
using rtl_async_cb = void (*)(const uint8_t*, uint32_t, void*);

// gain_tenth_db below zero selects the automatic gain of the tuner.
// applied_gain_tenth_db takes the gain that the tuner took, or -1 for automatic.
int rtl_open(rtlsdr_dev** dev, uint32_t center_hz, uint32_t rate_hz, int index, int gain_tenth_db,
	     bool agc, int* applied_gain_tenth_db);
// The highest sample rate the device accepts, which is the widest span it can
// give. wanted caps the search; 0 means no cap. Returns 0 if none stuck.
uint32_t rtl_max_rate(rtlsdr_dev* dev, uint32_t wanted);
// Change the rate of an open device.
int rtl_set_rate(rtlsdr_dev* dev, uint32_t rate_hz);
// Retune an open device. It also drops whatever the buffer still holds.
int rtl_set_center(rtlsdr_dev* dev, uint32_t center_hz);
// A readable form of the codes that rtl_open returns.
const char* rtl_error(int rc);
int rtl_read(rtlsdr_dev* dev, void* buf, int len);
int rtl_read_async(rtlsdr_dev* dev, rtl_async_cb cb, void* ctx, uint32_t block_bytes);
void rtl_cancel_async(rtlsdr_dev* dev);
void rtl_close(rtlsdr_dev* dev);
