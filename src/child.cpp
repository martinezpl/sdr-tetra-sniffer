#include "recorder.h"
#include "allocations.h"
#include "voice_frame.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

#include "demod_chain.h"

extern "C" {
#include "crypto/tetra_crypto.h"
#include <phy/tetra_burst_sync.h>
}

static const int BLOCK = 3600;

struct VoiceSink {
	int fd;
	uint64_t sample;
};

// Everything that belongs to one frequency. A slot that changes frequency
// throws all of it away and builds it again. The state of this decoder reaches
// into the display state, the crypto state and the fragment slots, and one
// stale field is enough to suppress playback for the rest of the run, so a
// rebuild is the only reset that cannot miss one.
struct Decoder {
	DemodChain chain;
	tetra_mac_state* tms = nullptr;
	tetra_rx_state* trs = nullptr;
};

static void decoder_init(Decoder& d, uint32_t hz, Recorder* rec, VoiceSink* voice)
{
	d.chain.init();
	auto* tms = (struct tetra_mac_state*)calloc(1, sizeof(struct tetra_mac_state));
	tetra_mac_state_init(tms);
	tms->tcs = (struct tetra_crypto_state*)calloc(1, sizeof(struct tetra_crypto_state));
	tms->t_display_st = (struct tetra_display_state*)calloc(1, sizeof(struct tetra_display_state));
	tetra_crypto_state_init(tms->tcs);
	tms->fragslots = (struct fragslot*)calloc(FRAGSLOT_NR_SLOTS, sizeof(struct fragslot));
	for (int i = 0; i < 4; i++) {
		tms->t_display_st->slot_encr[i] = -1;
		tms->t_display_st->slot_speech[i] = -1;
		tms->t_display_st->slot_e2ee[i] = -1;
		tms->t_display_st->slot_ssi[i] = 0;
	}
	tms->clear_only = true;
	/* Without --per-carrier, Recorder::on_voice is a no-op. Do not decode the discarded speech. */
	tms->skip_voice_decode = !rec->writes_files();
	tms->own_carrier_hz = hz;

	tms->put_voice_data_ctx = rec;
	tms->put_voice_data = [](void* p, int n, int16_t* pcm) { ((Recorder*)p)->on_voice(pcm, n); };
	tms->put_voice_bits_ctx = voice;
	tms->put_voice_bits = [](void* p, uint32_t frame, uint32_t hz, uint32_t gssi, uint32_t issi,
				 uint32_t control_hz, uint8_t tn, uint8_t usage, int8_t encr,
				 const uint8_t* bits) {
		auto* sink = (VoiceSink*)p;
		VoiceFrame v = { sink->sample, frame, hz, control_hz, gssi, issi, tn, usage, encr };
		memcpy(v.bits, bits, sizeof v.bits);
		while (write(sink->fd, &v, sizeof v) < 0) {
			if (errno == EINTR) continue;
			// The stitch process is gone. This carrier records nothing more.
			std::cout << hz << ": voice pipe: " << strerror(errno) << "\n";
			_exit(3);
		}
	};
	tms->event_ctx = rec;
	tms->event_cb = [](void* p, const struct tetra_mac_event* ev) { ((Recorder*)p)->on_event(*ev); };
	tms->clear_slot_ctx = rec;
	tms->clear_slot_cb = [](void* p, uint8_t tn, int encr, uint32_t* ssi, uint32_t* issi,
				uint32_t* control_hz) {
		return ((Recorder*)p)->permits_clear(tn, encr, ssi, issi, control_hz);
	};
	d.tms = tms;
	d.trs = (struct tetra_rx_state*)calloc(1, sizeof(struct tetra_rx_state));
	d.trs->burst_cb_priv = tms;
}

static void decoder_free(Decoder& d)
{
	if (d.tms) {
		free(d.tms->fragslots);
		free(d.tms->t_display_st);
		free(d.tms->tcs);
		free(d.tms);
		d.tms = nullptr;
	}
	free(d.trs);
	d.trs = nullptr;
}

static int read_block(int fd, void* buf, size_t n)
{
	char* c = (char*)buf;
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, c + got, n - got);
		if (r < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (r == 0) break;
		got += r;
	}
	return got;
}

int child_main(int fd, uint32_t hz, const std::string& dir, const std::string& start_utc, double iq_rate,
	       Allocations* allocations, int voice_fd, double span_center_hz, bool per_carrier,
	       size_t slot)
{
	signal(SIGINT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	Recorder rec(hz, dir, start_utc, iq_rate, allocations, per_carrier, span_center_hz);
	tetra_codec_init_once();

	VoiceSink voice = { voice_fd, 0 };
	Decoder dec;
	uint32_t following = 0;

	auto* in = dsp::buffer::alloc<dsp::complex_t>(BLOCK);
	auto* syms = dsp::buffer::alloc<dsp::complex_t>(BLOCK);
	auto* dibits = dsp::buffer::alloc<uint8_t>(BLOCK);
	auto* bits = dsp::buffer::alloc<uint8_t>(2 * BLOCK);

	uint64_t processed_samples = 0;
	for (;;) {
		int got = read_block(fd, in, BLOCK * sizeof(dsp::complex_t));
		if (got < 0) {
			std::cout << hz << ": read: " << strerror(errno) << "\n";
			decoder_free(dec);
			return 4;
		}
		int n = got / sizeof(dsp::complex_t);
		if (!n) break;
		// The parent feeds every slot, free or not, so each child counts the
		// same samples. The grants in the shared table are stamped with that
		// count, so a slot filled late still reads them on the same timebase.
		processed_samples += n;
		voice.sample = processed_samples;

		uint32_t want = allocations ? allocations->assigned(slot) : hz;
		if (want != following) {
			if (following) decoder_free(dec);
			following = want;
			// The parent reports an assignment, and it reports the seed list at
			// the start, so there is nothing to say here.
			if (following) {
				decoder_init(dec, following, &rec, &voice);
				rec.retune(following);
			}
		}
		if (!following) continue;

		rec.advance((double)n / VFO_RATE, processed_samples);
		int m = dec.chain.process(n, in, syms, dibits, bits);
		for (int off = 0; off < m; off += 2048)
			tetra_burst_sync_in(dec.trs, bits + off, std::min(2048, m - off));
	}
	rec.finish();
	decoder_free(dec);
	dsp::buffer::free(in);
	dsp::buffer::free(syms);
	dsp::buffer::free(dibits);
	dsp::buffer::free(bits);
	return 0;
}
