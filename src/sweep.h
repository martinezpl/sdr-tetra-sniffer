#pragma once
#include <cstddef>

// The TETRA allocation below 470 MHz. A sweep with no --band searches all of
// it. A sweep with no --rate asks for a window that holds that allocation
// in one span. The search band does not grow; only the sample window may,
// so a stick that snaps or lists a coarse rate (Lime 61.44 MS/s) still
// covers. SWEEP_ONE_SPAN_MARGIN is that extra, 20% plus a little for the
// next listed rate above 20%.
static const double TETRA_BAND_LO = 380000000;
static const double TETRA_BAND_HI = 430000000;
static const double TETRA_SPAN_HZ = TETRA_BAND_HI - TETRA_BAND_LO;
static const double SWEEP_EDGE_HZ = 15000;
static const double SWEEP_ONE_SPAN_MARGIN = 1.25;

struct SweepArgs {
	double band_lo, band_hi;   // the band to search, in Hz
	double rate;               // dongle sample rate, and so the width of one span
	double step;               // channel raster of the power scan
	double tune_offset;        // correction for the frequency error of the dongle
	double gain_db;            // below zero means the automatic gain of the tuner
	int device;
	double scan;               // seconds of power measurement for each group
	double dwell;              // seconds of decode on the candidates
	double threshold_db;       // how far above the noise floor a peak must stand
	size_t max_carriers;       // how many candidates the decode stage takes
	int rx;                    // Soapy RX channel, default 0
	const char* antenna;       // Soapy RX antenna; null keeps the driver default
	const char* device_args;   // Soapy device arguments, KEY=VALUE,...; null adds none
};

int sweep_main(const SweepArgs& a);
