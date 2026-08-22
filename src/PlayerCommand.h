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

#ifndef PLAYERCOMMAND_H
#define PLAYERCOMMAND_H

#include "CommonIncludes.h"
#include "InputState.h"
#include "ActionData.h"
#include "Utils.h"

class InputState;

/** One player's intent for one tick.
 *
 * This is the boundary that makes multiplayer possible. A player's intent has to be a value you
 * can hold, copy, queue and eventually put on a wire -- not a global the simulation reaches into.
 * Phase 2 needs eight players' worth of intent per tick, which is impossible while intent *is*
 * the keyboard.
 *
 * Screen-to-map conversion happens when this is built, never inside the simulation: the camera is
 * presentation, and the simulation must not need to know where it is pointing.
 *
 * Deliberately holds no pointer to anything and reaches no global.
 */
class PlayerCommand {
public:
	PlayerCommand();

	// movement intent, already resolved through the engine's pressing/lock pair
	bool move_up, move_down, move_left, move_right;
	bool aim_up, aim_down, aim_left, aim_right;

	// mouse-move
	bool mm_pressed;
	FPoint mm_map_target;   // MAP coordinates, converted at the boundary

	// attacks and modifiers
	bool main1_pressed, main2_pressed;
	bool main1_active, main2_active;   // pressed and not consumed by the UI
	bool shift;
	bool using_mouse;

	// Screen coordinates. Present ONLY because Avatar still asks the action bar whether the
	// cursor is over a menu (Avatar.cpp:464), which is a menu dependency rather than an input
	// one. P1.3 removes those call sites and this field should go with them -- the simulation
	// has no business knowing about screen space.
	Point mouse_screen;

	// power activations queued this tick
	std::vector<ActionData> actions;
};

/** Mouse-click arbitration, borrowed by the simulation for one tick and handed back.
 *
 * `inpt->lock[MAIN1]` is not really input: it is a shared "this click has been used" channel
 * between the player and roughly twenty UI files -- MenuManager, the action bar, every widget,
 * cutscenes. Avatar both reads and WRITES it, and three of its four writes are conditional on
 * simulation state (is the target position walkable, is the cursor over an enemy, is the death
 * animation running), so they cannot be hoisted to the boundary the way a plain "just pressed"
 * edge could.
 *
 * So the array is passed in, mutated in place exactly where the simulation used to mutate the
 * global, and copied back afterwards. That keeps click arbitration working with the UI while
 * removing Avatar's reach into a global.
 *
 * This is an intermediate state and it is worth being blunt about that: the coupling is not gone,
 * it is only made explicit and passed by hand. **P1.3** takes the menus out of the simulation,
 * which is what actually dissolves it. A replay test cannot catch a regression here, because
 * replays do not exercise menus -- click arbitration must be checked by hand.
 */
class PlayerInputLocks {
public:
	PlayerInputLocks();

	void copyFrom(const InputState& in);
	void copyTo(InputState& in) const;

	/** Mirrors InputState::unlockActionBar() on the borrowed array. Transform powers lock the
	 * action bar and the simulation unlocks it again when the transform ends. */
	void unlockActionBar();

	bool lock[InputState::KEY_COUNT];
};

namespace PlayerCommandBuilder {
	/** The only place player intent is read out of the global input state. */
	void build(PlayerCommand& cmd, const InputState& in, const FPoint& cam_pos);
}

#endif // PLAYERCOMMAND_H
