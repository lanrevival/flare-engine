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

#ifndef RNG_H
#define RNG_H

#include <stdint.h>
#include <stddef.h>

/** An explicit pseudo-random stream.
 *
 * ALGORITHM: xorshift128+, seeded by splitmix64.
 *
 *   Vigna, "Further scramblings of Marsaglia's xorshift generators" (2016).
 *   Steele, Lea & Flood, "Fast splittable pseudorandom number generators" (2014).
 *
 * The algorithm is fixed on purpose. Two peers must produce byte-identical sequences from the
 * same seed regardless of compiler, standard library, or platform, so the generator cannot be
 * anything whose sequence an implementation is free to choose.
 *
 * In particular this is NOT std::mt19937 + std::uniform_int_distribution. mt19937's raw output
 * is specified, but uniform_int_distribution's mapping is explicitly implementation-defined --
 * two peers on different standard libraries would draw different numbers from identical state.
 * (It is also C++11, which this project does not use. See Codingstyle.txt and plans/P0.1.)
 *
 * There are two global streams and the distinction is load-bearing:
 *
 *   sim_rng  Simulation. Seeded, reproducible, and drawn ONLY from the authority's tick.
 *   fx_rng   Presentation and client-local UI. Unseeded and never reproducible.
 *
 * A stream is safe only if every peer draws from it the same number of times in the same order.
 * That is why "when in doubt use sim_rng" is wrong: a client-local draw on sim_rng (a player
 * clicking 'randomise' on the character creation screen) puts that client permanently out of
 * step. The question is not "does this matter?" but "who executes this draw, and is it part of
 * the authoritative tick?"
 */
class Rng {
public:
	Rng();

	/** Expand 's' through splitmix64 into the two state words. */
	void seed(uint64_t s);

	/** The value last passed to seed(). Needed by save games and the replay harness. */
	uint64_t getSeed() const;

	/** One raw draw. Every other member is built on this. */
	uint64_t next();

	/** Uniform in [min_val, max_val], BOTH ENDS INCLUSIVE. Unbiased (rejection sampling).
	 *
	 * Draws nothing when min_val >= max_val, returning min_val. Matching the old
	 * Math::randBetween() here is deliberate: it kept the draw COUNT identical for the common
	 * randBetween(n, n) case, and determinism depends on the count, not just the values.
	 */
	int range(int min_val, int max_val);

	/** Uniform in [min_val, max_val). Draws nothing when min_val >= max_val. */
	float rangeF(float min_val, float max_val);

	/** Uniform in [0, count). Draws nothing and returns 0 when count == 0; count == 1 draws.
	 *
	 * Several call sites take a modulo by a container's size with no empty check. Returning
	 * 0 preserves that behaviour without the undefined division. It does NOT make those call
	 * sites correct -- they still index an empty container. See plans/phase0/P0.3b.
	 */
	size_t index(size_t count);

	/** True with 'percent' chance out of 100. Always draws. */
	bool percentChance(int percent);
	bool percentChanceF(float percent);

	/** Uniform in [0.0, 1.0). Never returns 1.0. */
	float unit();

private:
	uint64_t state[2];
	uint64_t last_seed;
};

/** Placeholder simulation seed. Phase 3 replaces this with a value received from the host,
 * at which point this constant should have no callers left.
 */
extern const uint64_t RNG_DEFAULT_SIM_SEED;

extern Rng* sim_rng;
extern Rng* fx_rng;

#endif // RNG_H
