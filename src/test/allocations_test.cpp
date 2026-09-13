#include "allocations.h"

#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

static void check(bool ok, const char* name)
{
	if (!ok) {
		fails++;
		std::cerr << "FAIL " << name << "\n";
	}
}

static int child_status(pid_t pid)
{
	int status = 0;
	return waitpid(pid, &status, 0) == pid && WIFEXITED(status) ? WEXITSTATUS(status) : 255;
}

int main()
{
	const uint32_t control = 419962500;
	const uint32_t traffic = 420762500;
	Allocations allocations = Allocations::create({ control, traffic }, 2);
	Allocations::Grant grant = {};

	check(!allocations.permits_clear(traffic, 1, -1, 10, 100, &grant), "absent grant denied");
	pid_t publisher = fork();
	if (publisher == 0)
		_exit(allocations.publish(traffic, 0x0a, 10, 1005, 3005001, 100) ? 0 : 1);
	check(publisher > 0 && child_status(publisher) == 0, "child published grant");
	check(!allocations.permits_clear(traffic, 1, -1, 10, 99, &grant), "future grant denied");
	check(allocations.permits_clear(traffic, 1, -1, 10, 100, &grant), "TN1 propagated");
	check(grant.ssi == 1005 && grant.issi == 3005001, "identities propagated");
	check(!allocations.permits_clear(traffic, 1, -1, 11, 100, &grant), "usage mismatch denied");
	check(!allocations.permits_clear(traffic, 1, -1, 3, 100, &grant), "nontraffic denied");
	check(allocations.permits_clear(traffic, 2, -1, 10, 100, &grant), "usage marker permits TN2");
	check(allocations.permits_clear(traffic, 3, -1, 10, 100, &grant), "TN3 propagated");
	check(allocations.permits_clear(traffic, 4, -1, 10, 100, &grant), "usage marker permits TN4");
	check(!allocations.permits_clear(traffic, 1, -1, 10, 100 + 30 * 36000 + 1, &grant),
	      "unused grant expired");

	for (int encr = 1; encr <= 3; encr++)
		check(!allocations.permits_clear(traffic, 1, encr, 10, 100, &grant),
		      "local encrypted state wins");
	check(allocations.permits_clear(traffic, 1, 0, 10, 100, &grant), "local clear state allowed");
	check(!allocations.publish(traffic, 0, 10, 0, 0, 110), "empty mask denied");
	check(!allocations.publish(traffic, 0x10, 10, 0, 0, 110), "malformed mask denied");
	check(!allocations.publish(415000000, 0x08, 10, 0, 0, 110), "unknown carrier denied");

	check(allocations.invalidate(traffic, 1, 200), "traffic exit invalidated");
	check(allocations.permits_clear(traffic, 1, -1, 10, 200, &grant), "other marked slot preserves grant");
	pid_t stale = fork();
	if (stale == 0)
		_exit(allocations.publish(traffic, 0x08, 10, 1005, 0, 150) ? 0 : 1);
	check(stale > 0 && child_status(stale) == 0, "delayed publisher completed");
	check(allocations.permits_clear(traffic, 1, -1, 10, 250, &grant),
	      "stale slot cannot replace the other marked grant");
	check(allocations.permits_clear(traffic, 3, -1, 10, 250, &grant), "other slot preserved");

	// The carrier pool. A free slot takes a frequency that a grant named, and
	// it is never taken back.
	{
		const uint32_t known = 419562500, fresh = 420562500, other = 420362500;
		Allocations pool = Allocations::create({ known }, 2);
		check(pool.slots() == 2, "the pool holds one free slot");
		check(pool.assigned(0) == known, "the seed carrier sits in slot 0");
		check(pool.assigned(1) == 0, "the second slot starts free");
		check(!pool.publish(0, 0x08, 10, 1, 1, 100), "a free slot never matches frequency 0");

		check(pool.want({ fresh, known, 1234 }), "a free pool takes a request");
		check(pool.want({ fresh, known, 1234 }), "the same request twice is not an error");
		Allocations::Demand d = {};
		check(pool.take(&d) && d.hz == fresh && d.ssi == 1234, "the request round trips");
		check(!pool.take(&d), "the queue holds each frequency once");

		check(pool.assign(fresh) == 1, "the free slot takes the carrier");
		check(pool.assigned(1) == fresh, "the slot follows it");
		check(pool.assign(fresh) < 0, "the same carrier is never assigned twice");
		check(pool.assign(other) < 0, "a full pool assigns nothing");
		check(!pool.want({ other, known, 1 }), "a full pool refuses a request");
		check(pool.want({ known, known, 1 }), "a carrier already followed needs no slot");

		// A slot must answer for the frequency it holds now, and for no other.
		Allocations::Grant g = {};
		check(!pool.permits_clear(fresh, 1, -1, 10, 100, &g), "a new slot holds no grant");
		check(pool.publish(fresh, 0x08, 10, 42, 43, 100), "the new slot takes a grant");
		check(pool.permits_clear(fresh, 1, -1, 10, 100, &g) && g.ssi == 42,
		      "the new slot serves its own grant");
		check(!pool.publish(other, 0x08, 10, 1, 1, 100), "a carrier outside the pool is denied");
	}

	if (!fails)
		std::cout << "PASS allocations\n";
	return fails != 0;
}
