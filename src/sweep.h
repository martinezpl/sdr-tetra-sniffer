#pragma once
#include <cstddef>

// The TETRA allocation below 470 MHz. A sweep with no --band searches all of
// it. A sweep with no --rate will not open a span wider than this, because
// nothing in that allocation sits outside it.
static const double TETRA_BAND_LO = 380000000;
static const double TETRA_BAND_HI = 430000000;
static const double TETRA_SPAN_HZ = TETRA_BAND_HI - TETRA_BAND_LO;
static const double SWEEP_EDGE_HZ = 15000;

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
};

int sweep_main(const SweepArgs& a);
