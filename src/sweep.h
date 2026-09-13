#pragma once
#include <cstddef>

struct SweepArgs {
	double band_lo, band_hi;   // the band to search, in Hz
	double rate;               // dongle sample rate, and so the width of one span
	double step;               // channel raster of the power scan
	double tune_offset;        // correction for the frequency error of the dongle
	double gain_db;            // below zero means the automatic gain of the tuner
	bool agc;
	int device;
	double scan;               // seconds of power measurement for each group
	double dwell;              // seconds of decode on the candidates
	double threshold_db;       // how far above the noise floor a peak must stand
	size_t max_carriers;       // how many candidates the decode stage takes
};

int sweep_main(const SweepArgs& a);
