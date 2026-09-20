#pragma once
// The demodulator chain of one TETRA carrier. "run" builds one for each
// carrier and "sweep" builds one for each candidate, so the constants live
// here and cannot drift apart.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include <dsp/buffer/buffer.h>
#include <dsp/stream.h>
#include "dsp/pi4dqpsk.h"
#include "dsp/dqpsk_sym_extr.h"
#include "dsp/bit_unpacker.h"

// The TETRA symbol rate fixes both of these. They are not options.
static const double VFO_RATE = 36000;
static const double VFO_BW = 30000;

// One IQ block is 100 ms, unless that many samples would overflow the SDR++
// resampler/FIR work buffer. Then more, shorter, blocks cover the same time.
// The receiver's rate is not capped: a wide-span stick still gives its span.
static inline int iq_block(double rate)
{
	int n = (int)(rate / 10);
	if (n > STREAM_BUFFER_SIZE) n = STREAM_BUFFER_SIZE;
	return n > 0 ? n : 1;
}

// The resampler of SDR++ and the driver of the dongle both print to stdout
// when they start. The log of a run is for the operator, so that chatter goes
// to /dev/null. Wrap only the setup calls, never the log of this program.
inline int quiet_begin()
{
	fflush(stdout);
	int saved = dup(1);
	int null = open("/dev/null", O_WRONLY);
	if (null >= 0) {
		dup2(null, 1);
		close(null);
	}
	return saved;
}

inline void quiet_end(int saved)
{
	fflush(stdout);
	if (saved >= 0) {
		dup2(saved, 1);
		close(saved);
	}
}

// The frequency error of the receiver, read out of the demodulator.
//
// The FLL of the demodulator tracks the residual carrier offset and holds it
// in the frequency of its phase control loop, which advances the phase by that
// much for each input sample. So the unit is radians per sample at the VFO
// rate, and Hz is freq * VFO_RATE / 2pi. Both members are protected in their
// own classes, so a derived class is the way to read them without patching a
// submodule. Nothing here writes to them.
class FllProbe : public dsp::demod::PI4DQPSK {
public:
	// Radians per sample, or 0 before the loop has seen anything.
	float loop_freq() { return static_cast<FllReader&>(fll).freq(); }

private:
	struct FllReader : public dsp::loop::FLL {
		float freq() { return pcl.freq; }
	};
};

struct DemodChain {
	FllProbe demod;
	dsp::DQPSKSymbolExtractor sym;
	dsp::BitUnpacker unpack;

	void init()
	{
		int saved = quiet_begin();
		float bw = 0.00628f, damp = 0.707f;
		float den = 1.0f + 2.0f * damp * bw + bw * bw;
		demod.init(nullptr, 18000, VFO_RATE, 65, 0.35f, 0.02f, 0.01f, 0.006f,
			   (4.0f * bw * bw) / den, (4.0f * damp * bw) / den, 0.02f);
		sym.init(nullptr);
		unpack.init(nullptr);
		quiet_end(saved);
	}

	// The frequency error the FLL has converged on, in Hz at the carrier. It
	// means nothing until the demodulator locks, so read it only then.
	double error_hz() { return demod.loop_freq() * VFO_RATE / (2.0 * M_PI); }

	// Complex samples in, unpacked bits out. The three buffers are scratch.
	// Returns the number of bits.
	int process(int n, dsp::complex_t* in, dsp::complex_t* syms, uint8_t* dibits, uint8_t* bits)
	{
		int m = demod.process(n, in, syms);
		m = sym.process(m, syms, dibits);
		return unpack.process(m, dibits, bits);
	}
};
