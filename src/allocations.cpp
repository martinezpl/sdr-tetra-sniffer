#include "allocations.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

struct Allocations::State {
	struct SharedGrant {
		uint64_t sample;
		int usage_marker;
		uint32_t ssi;
		uint32_t issi;
		uint32_t control_hz;
		uint8_t status;
	};
	struct Carrier {
		// 0 marks a free slot that no child follows yet.
		uint32_t hz;
		SharedGrant slots[4];
	};

	// A bounded queue of the frequencies that some child wants a slot for.
	// The oldest entry goes when it overflows, the same rule as the GSSI cap.
	static const size_t DEMANDS = 32;

	size_t count;
	Demand demands[DEMANDS];
	size_t demand_head;
	size_t demand_count;
	Carrier carriers[1];
};

Allocations::Allocations(State* state, size_t map_size, int lock_fd)
	: state(state), map_size(map_size), lock_fd(lock_fd)
{
}

Allocations Allocations::create(const std::vector<uint32_t>& carriers, size_t slots)
{
	if (carriers.empty())
		throw std::runtime_error("allocations need at least one carrier");
	if (slots < carriers.size()) slots = carriers.size();
	char path[] = "/tmp/tetra-analyze-allocations.XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0)
		throw std::runtime_error(std::string("mkstemp: ") + strerror(errno));
	unlink(path);
	size_t bytes = sizeof(State) + (slots - 1) * sizeof(State::Carrier);
	void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (p == MAP_FAILED) {
		int e = errno;
		close(fd);
		throw std::runtime_error(std::string("mmap: ") + strerror(e));
	}
	State* s = static_cast<State*>(p);
	s->count = slots;
	s->demand_head = 0;
	s->demand_count = 0;
	for (size_t i = 0; i < slots; i++)
		s->carriers[i].hz = i < carriers.size() ? carriers[i] : 0;
	return Allocations(s, bytes, fd);
}

Allocations::Allocations(Allocations&& other) noexcept
	: state(other.state), map_size(other.map_size), lock_fd(other.lock_fd)
{
	other.state = nullptr;
	other.map_size = 0;
	other.lock_fd = -1;
}

Allocations& Allocations::operator=(Allocations&& other) noexcept
{
	if (this == &other)
		return *this;
	if (state)
		munmap(state, map_size);
	if (lock_fd >= 0)
		close(lock_fd);
	state = other.state;
	map_size = other.map_size;
	lock_fd = other.lock_fd;
	other.state = nullptr;
	other.map_size = 0;
	other.lock_fd = -1;
	return *this;
}

Allocations::~Allocations()
{
	if (state)
		munmap(state, map_size);
	if (lock_fd >= 0)
		close(lock_fd);
}

bool Allocations::set_lock(short type) const
{
	struct flock lock = {};
	lock.l_type = type;
	lock.l_whence = SEEK_SET;
	while (fcntl(lock_fd, F_SETLKW, &lock) < 0) {
		if (errno != EINTR)
			return false;
	}
	return true;
}

size_t Allocations::slots() const
{
	return state ? state->count : 0;
}

uint32_t Allocations::assigned(size_t slot) const
{
	if (!state || slot >= state->count || !set_lock(F_RDLCK)) return 0;
	uint32_t hz = state->carriers[slot].hz;
	set_lock(F_UNLCK);
	return hz;
}

int Allocations::assign(uint32_t hz)
{
	if (!state || !hz || !set_lock(F_WRLCK)) return -1;
	int slot = -1;
	for (size_t i = 0; i < state->count; i++) {
		// Another slot already follows it, so this is not a new carrier.
		if (state->carriers[i].hz == hz) { set_lock(F_UNLCK); return -1; }
		if (!state->carriers[i].hz && slot < 0) slot = (int)i;
	}
	if (slot >= 0) {
		State::Carrier& c = state->carriers[slot];
		c.hz = hz;
		// The slot may hold grants of whatever it followed before it was freed.
		// It never is, today, but a stale grant would name the wrong talkgroup.
		for (unsigned tn = 0; tn < 4; tn++) c.slots[tn] = State::SharedGrant{};
	}
	set_lock(F_UNLCK);
	return slot;
}

bool Allocations::want(const Demand& d)
{
	if (!state || !d.hz || !set_lock(F_WRLCK)) return false;
	bool room = false;
	for (size_t i = 0; i < state->count; i++) {
		// Already followed, so there is nothing to ask for.
		if (state->carriers[i].hz == d.hz) { set_lock(F_UNLCK); return true; }
		if (!state->carriers[i].hz) room = true;
	}
	if (!room) { set_lock(F_UNLCK); return false; }
	for (size_t i = 0; i < state->demand_count; i++)
		if (state->demands[(state->demand_head + i) % State::DEMANDS].hz == d.hz) {
			set_lock(F_UNLCK);
			return true;
		}
	if (state->demand_count == State::DEMANDS) {
		state->demand_head = (state->demand_head + 1) % State::DEMANDS;
		state->demand_count--;
	}
	state->demands[(state->demand_head + state->demand_count) % State::DEMANDS] = d;
	state->demand_count++;
	set_lock(F_UNLCK);
	return true;
}

bool Allocations::take(Demand* d)
{
	if (!state || !set_lock(F_WRLCK)) return false;
	bool got = state->demand_count > 0;
	if (got) {
		*d = state->demands[state->demand_head];
		state->demand_head = (state->demand_head + 1) % State::DEMANDS;
		state->demand_count--;
	}
	set_lock(F_UNLCK);
	return got;
}

bool Allocations::publish(uint32_t carrier_hz, uint8_t tn_mask, int usage_marker, uint32_t ssi,
			  uint32_t issi, uint64_t sample, uint32_t control_hz)
{
	if (!state || !tn_mask || (tn_mask & 0xf0) || !set_lock(F_WRLCK))
		return false;
	bool found = false;
	for (size_t i = 0; i < state->count; i++) {
		if (!state->carriers[i].hz || state->carriers[i].hz != carrier_hz)
			continue;
		found = true;
		for (unsigned tn = 1; tn <= 4; tn++) {
			if (!(tn_mask & (8u >> (tn - 1))))
				continue;
			State::SharedGrant& grant = state->carriers[i].slots[tn - 1];
			if (!grant.status || sample > grant.sample) {
				grant.sample = sample;
				grant.usage_marker = usage_marker;
				grant.ssi = ssi;
				grant.issi = issi;
				grant.control_hz = control_hz;
				grant.status = 1;
			}
		}
		break;
	}
	set_lock(F_UNLCK);
	return found;
}

bool Allocations::invalidate(uint32_t carrier_hz, uint8_t tn, uint64_t sample)
{
	if (!state || tn < 1 || tn > 4 || !set_lock(F_WRLCK))
		return false;
	bool found = false;
	for (size_t i = 0; i < state->count; i++) {
		if (state->carriers[i].hz != carrier_hz)
			continue;
		found = true;
		State::SharedGrant& grant = state->carriers[i].slots[tn - 1];
		if (!grant.status || sample >= grant.sample) {
			grant.sample = sample;
			grant.status = 2;
		}
		break;
	}
	set_lock(F_UNLCK);
	return found;
}

bool Allocations::permits_clear(uint32_t carrier_hz, uint8_t tn, int local_encr, int dl_usage,
				uint64_t sample, Grant* out) const
{
	if (local_encr == 0)
		return true;
	if (!state || local_encr != -1 || tn < 1 || tn > 4 || dl_usage <= 3 ||
	    !set_lock(F_RDLCK))
		return false;
	bool permitted = false;
	for (size_t i = 0; i < state->count; i++) {
		if (state->carriers[i].hz != carrier_hz)
			continue;
		for (unsigned slot = 0; slot < 4; slot++) {
			const State::SharedGrant& grant = state->carriers[i].slots[slot];
			if (grant.status != 1 || grant.sample > sample ||
			    sample - grant.sample > 30 * 36000 ||
			    (grant.usage_marker >= 0 && grant.usage_marker != dl_usage))
				continue;
			permitted = true;
			if (out)
				*out = { grant.ssi, grant.issi, grant.control_hz };
			break;
		}
		break;
	}
	set_lock(F_UNLCK);
	return permitted;
}
