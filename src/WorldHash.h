/*
Copyright © 2026 Flare LAN Revival contributors

This file is part of FLARE.

FLARE is free software: you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

FLARE is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
FLARE.  If not, see http://www.gnu.org/licenses/
*/

#ifndef WORLDHASH_H
#define WORLDHASH_H

#include <stdint.h>
#include <stddef.h>
#include <string>

/** A 64-bit FNV-1a digest of the simulation state.
 *
 * This exists to turn "did that refactor change anything?" from a judgement call into a command
 * that exits 0 or 1. Phases 1 and 2 are almost entirely behaviour-preserving refactors, and
 * without a digest a reviewer looking at a 600-line change has nothing to check but vibes.
 *
 * TRAVERSAL ORDER is container order, deliberately, and NOT sorted by any id.
 *
 * Every container holding simulation state is a std::vector (insertion-ordered) or a std::map
 * (key-ordered). There is no unordered_map anywhere in the simulation, so container order is
 * already a deterministic function of the simulation itself. Sorting would therefore be strictly
 * WORSE than not sorting: two runs that spawn the same entities in a different order are already
 * desynced, and sorting would hide exactly that.
 *
 * (An earlier plan said to sort by Entity::net_id. No such field exists -- it belonged to an
 * abandoned prototype, not this tree.)
 *
 * FLOATS ARE HASHED BY BIT PATTERN, never formatted as text: printing at 6 significant figures
 * silently equates values that differ, which is the one thing this must never do. NaN is
 * canonicalised and -0.0 is normalised to +0.0 so that two bit patterns which compare equal
 * always hash equal.
 *
 * WHAT IS COVERED matters as much as how. A digest over positions alone would let every
 * inventory and quest refactor in Phase 2 sail through green. Player resources, inventory,
 * campaign statuses and loot are all included for that reason.
 */
class WorldHash {
public:
	/** Digest of the whole simulation. 'tick' is mixed in so that two runs which diverge and
	 * then reconverge still show a difference at the tick where they parted. */
	static uint64_t compute(unsigned long tick);

	/** Digest of only the fields the network already replicates: PlayerSnapshotEntry's set
	 * (id, pos, direction, animation, hp, hp_max, alive) for every player, nothing else. P3.7.
	 * Entities/hazards/loot/inventory/campaign are not wire-replicated yet (P3.9-P3.12), so
	 * compute() cannot be compared between a client and server process before then -- this can,
	 * once the two processes stop independently simulating (P3.8, D27). Before that, two
	 * processes fed the same scripted input can still disagree here by a tick or two of network
	 * latency; this is infrastructure for P3.8 onward, not an equality guarantee on its own.
	 *
	 * P3.8b: exclude_id, when >= 0, skips the player whose Avatar::id equals it -- used only by
	 * main_server.cpp's own --no-local-player runs, to leave the --load-slot clone template
	 * (never a real network player then) out of the digest a client's own computeReplicated() is
	 * compared against. -1 (the default) excludes nothing, matching every call site before this
	 * parameter existed. */
	static uint64_t computeReplicated(unsigned long tick, int exclude_id = -1);

	/** "0x%016llx" -- the form printed by --hash and stored in golden files. */
	static std::string toString(uint64_t h);

	// Incremental primitives. Public so the digest can be unit-tested without a game world.
	static uint64_t init();
	static uint64_t mixBytes(uint64_t h, const void* data, size_t len);
	static uint64_t mixU64(uint64_t h, uint64_t v);
	static uint64_t mixI32(uint64_t h, int32_t v);
	static uint64_t mixFloat(uint64_t h, float v);
	static uint64_t mixString(uint64_t h, const std::string& s);
};

#endif // WORLDHASH_H
