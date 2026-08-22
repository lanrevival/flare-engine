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

#include "Rng.h"

const uint64_t RNG_DEFAULT_SIM_SEED = static_cast<uint64_t>(0xF1A5EED5EED0F1A5ULL);

Rng* sim_rng = NULL;
Rng* fx_rng = NULL;

namespace {
	const uint64_t UINT64_ALL_ONES = ~static_cast<uint64_t>(0);

	// 2^-24, so unit() lands on exactly representable floats and can never reach 1.0
	const float UNIT_SCALE = 1.0f / 16777216.0f;

	/** splitmix64 -- used only to expand a seed into state, never as the stream itself. */
	uint64_t splitmix64(uint64_t& x) {
		x += static_cast<uint64_t>(0x9E3779B97F4A7C15ULL);
		uint64_t z = x;
		z = (z ^ (z >> 30)) * static_cast<uint64_t>(0xBF58476D1CE4E5B9ULL);
		z = (z ^ (z >> 27)) * static_cast<uint64_t>(0x94D049BB133111EBULL);
		return z ^ (z >> 31);
	}
}

Rng::Rng()
	: last_seed(0)
{
	seed(0);
}

void Rng::seed(uint64_t s) {
	last_seed = s;

	uint64_t x = s;
	state[0] = splitmix64(x);
	state[1] = splitmix64(x);

	// xorshift128+ is stuck forever on all-zero state. splitmix64 makes this astronomically
	// unlikely rather than impossible, so handle it rather than reason about it.
	if (state[0] == 0 && state[1] == 0) {
		state[0] = static_cast<uint64_t>(0x9E3779B97F4A7C15ULL);
		state[1] = static_cast<uint64_t>(0xBF58476D1CE4E5B9ULL);
	}
}

uint64_t Rng::getSeed() const {
	return last_seed;
}

uint64_t Rng::next() {
	uint64_t x = state[0];
	const uint64_t y = state[1];

	state[0] = y;
	x ^= x << 23;
	state[1] = x ^ y ^ (x >> 17) ^ (y >> 26);

	return state[1] + y;
}

int Rng::range(int min_val, int max_val) {
	// Draws nothing. See the note in Rng.h -- the draw COUNT is part of determinism.
	if (min_val >= max_val)
		return min_val;

	// Widen before subtracting: range(INT_MIN, INT_MAX) overflows int arithmetic.
	const uint64_t span = static_cast<uint64_t>(
		static_cast<int64_t>(max_val) - static_cast<int64_t>(min_val)) + 1;

	// Rejection sampling, NOT modulo. Modulo over-represents the low (2^64 % span) values.
	// Accept only from the largest multiple of span that fits, so every value is equally likely.
	const uint64_t rem = ((UINT64_ALL_ONES % span) + 1) % span;
	const uint64_t limit = UINT64_ALL_ONES - rem;

	uint64_t r = next();
	while (r > limit)
		r = next();

	return min_val + static_cast<int>(r % span);
}

float Rng::rangeF(float min_val, float max_val) {
	if (min_val >= max_val)
		return min_val;

	return min_val + (unit() * (max_val - min_val));
}

size_t Rng::index(size_t count) {
	// Draws nothing ONLY for an empty container. count == 1 still draws, matching the old
	// modulo-by-size idiom; range() is the opposite because the old randBetween() no-drew on equal
	// bounds. Both rules are arbitrary in isolation -- what matters is that every peer follows
	// the same one, so they are documented rather than left to fall out of the implementation.
	if (count == 0)
		return 0;

	const uint64_t span = static_cast<uint64_t>(count);
	const uint64_t rem = ((UINT64_ALL_ONES % span) + 1) % span;
	const uint64_t limit = UINT64_ALL_ONES - rem;

	uint64_t r = next();
	while (r > limit)
		r = next();

	return static_cast<size_t>(r % span);
}

bool Rng::percentChance(int percent) {
	return range(0, 99) < percent;
}

bool Rng::percentChanceF(float percent) {
	return rangeF(0.0f, 100.0f) < percent;
}

float Rng::unit() {
	// Top 24 bits: float has a 24-bit mantissa, so every result is exact and < 1.0.
	return static_cast<float>(next() >> 40) * UNIT_SCALE;
}
