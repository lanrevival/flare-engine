/*
Copyright © 2012 Piotr Rak
Copyright © 2014-2015 Justin Jacobs

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

#ifndef UTILS_MATH_H
#define UTILS_MATH_H 1

#include <cstdlib>
#include <algorithm> // for std::min()/std::max()
#include <math.h>

#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif

#ifndef M_PI
#define M_PI 3.1415926535898f
#endif

namespace Math {
	/**
	 * Returns sign of value.
	 */
	inline int signum(const int value) {
		return (0 < value) - (value < 0);
	}

	// randBetween(), randBetweenF(), percentChance() and percentChanceF() used to live here.
	// They were removed because they hid which random stream a caller was drawing from: all
	// four drew from the global C library generator, so two thirds of the engine's randomness
	// was invisible to a search for it. Their replacements are members of Rng (see Rng.h), which forces
	// every call site to name sim_rng or fx_rng.
	//
	//   randBetween(a, b)     -> rng->range(a, b)
	//   randBetweenF(a, b)    -> rng->rangeF(a, b)
	//   percentChance(p)      -> rng->percentChance(p)
	//   percentChanceF(p)     -> rng->percentChanceF(p)
	//
	// signum() above is kept: it is not a random function. It currently has no callers outside
	// this header, since randBetween() was its only user.
}
#endif // UTILS_MATH_H
