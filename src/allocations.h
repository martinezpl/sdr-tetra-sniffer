#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The table that the carrier children share with the parent.
//
// It holds one slot for each carrier that the program can follow. A slot that
// holds a frequency is followed by one child. A slot that holds zero is free,
// and the parent gives it to the first in-span carrier that a control carrier
// grants for a clear call. A slot is never taken back: a carrier that the
// network uses once it will use again, and a retune costs the head of a call.
//
// The table also carries the clear-call grants themselves, which is what lets
// a child on a traffic carrier name the talkgroup that a control carrier
// announced.
class Allocations {
public:
	struct Grant {
		uint32_t ssi;
		uint32_t issi;
		uint32_t control_hz;
	};

	// A child asks the parent for a slot on a frequency that no slot follows.
	struct Demand {
		uint32_t hz;
		uint32_t control_hz;
		uint32_t ssi;
	};

	// carriers seed the first slots. slots is the size of the whole pool, and
	// it never falls below the number of seed carriers.
	static Allocations create(const std::vector<uint32_t>& carriers, size_t slots);
	Allocations(Allocations&& other) noexcept;
	Allocations& operator=(Allocations&& other) noexcept;
	~Allocations();

	Allocations(const Allocations&) = delete;
	Allocations& operator=(const Allocations&) = delete;

	size_t slots() const;
	// The frequency that a slot follows, or 0 when the slot is free.
	uint32_t assigned(size_t slot) const;
	// Give the first free slot to hz. Returns the slot, or -1 when the pool is
	// full or another slot already follows that frequency.
	int assign(uint32_t hz);

	// A child wants a slot for d.hz. Returns false when the pool is full, so
	// the caller can report a call that this run cannot record.
	bool want(const Demand& d);
	// The parent takes one request. Returns false when there is none.
	bool take(Demand* d);

	bool publish(uint32_t carrier_hz, uint8_t tn_mask, int usage_marker, uint32_t ssi, uint32_t issi,
		     uint64_t sample, uint32_t control_hz = 0);
	bool invalidate(uint32_t carrier_hz, uint8_t tn, uint64_t sample);
	bool permits_clear(uint32_t carrier_hz, uint8_t tn, int local_encr, int dl_usage, uint64_t sample,
			   Grant* grant) const;

private:
	struct State;

	Allocations(State* state, size_t map_size, int lock_fd);
	bool set_lock(short type) const;

	State* state = nullptr;
	size_t map_size = 0;
	int lock_fd = -1;
};
