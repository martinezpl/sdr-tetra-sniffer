#pragma once
#include <cstdint>

struct VoiceFrame {
	uint64_t sample;
	uint32_t frame;
	uint32_t hz;
	uint32_t control_hz;
	uint32_t gssi;
	uint32_t issi;
	uint8_t tn;
	uint8_t usage;
	int8_t encr;
	uint8_t bits[432];
};

// Each carrier child writes one frame into one shared pipe.
// A write of PIPE_BUF bytes or less stays atomic. macOS gives 512. Linux gives 4096.
// The size is 464 bytes: 463 bytes of fields and 1 byte of padding at the end.
static_assert(sizeof(VoiceFrame) <= 512, "VoiceFrame must fit in the macOS PIPE_BUF of 512");
