/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
Copyright © 2013 Kurt Rinnert
Copyright © 2014 Henrik Andersson
Copyright © 2012-2016 Justin Jacobs
Copyright © 2026 Flare LAN Co-op contributors

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

/**
 * class ActionBarState
 *
 * What powers a character has bound to which hotkey. This used to be fields on MenuActionBar,
 * which meant a player's bindings were owned by a window -- see PlayerInventory.h for why that's
 * the wrong owner the moment there is more than one player and only one of them has a screen.
 *
 * There is exactly ONE copy of this data. MenuActionBar's `hotkeys`, `hotkeys_temp`, `hotkeys_mod`,
 * `locked`, `requires_attention`, `updated` and `slots_count` are REFERENCES bound to the members
 * below, not copies of them -- MenuActionBar.cpp's ~90 uses of e.g. `hotkeys[i]` still compile and
 * still mean the same storage, unchanged. That is deliberate and it is this class's whole point:
 * two copies desynchronise and the symptom is a hotkey silently reverting.
 *
 * `requires_attention` and `updated` are presentation, not simulation, and they move here anyway:
 * `requires_attention[MENU_LOG]` is serialised as `questlog_dismissed` (SaveLoad.cpp), and the save
 * format must not change, so SaveLoad needs an owner for it that exists without a live MenuManager.
 * See P1.3e-actionbar-state.md's "open question" -- this was a judgment call, not a measurement,
 * and is recorded as one.
 *
 * What is NOT here, on purpose: `addPower()`, `clear()`, `set()`, `clearSlot()`, `checkAction()`.
 * `checkAction()` reads widget click state and never runs on a headless server at all -- it stays
 * on MenuActionBar permanently, not as a shortcut. The mutators are split later, in P1.3e-c, on the
 * same precedent PlayerInventory::setEquipSlotEnabled() already set: `addPower()`'s common path
 * (id != 0, target_id == 0) is pure array writes, but its other branch touches a widget through
 * clearSlot(), so it cannot simply move today without either dragging that widget touch along or
 * silently dropping it.
 */

#ifndef ACTION_BAR_STATE_H
#define ACTION_BAR_STATE_H

#include "CommonIncludes.h"
#include "Utils.h"

class ActionBarState {
public:
	// How many menu buttons requires_attention tracks (character/inventory/powers/log). A
	// duplicate of MenuActionBar::MENU_COUNT's value, not a reference to it -- this class does not
	// include MenuActionBar.h, the same reasoning PlayerInventory::EQUIPMENT/CARRIED already gives.
	static const int MENU_COUNT = 4;

	ActionBarState();

	/** Sizes the four per-slot arrays. Called once, from MenuActionBar's constructor, after it has
	 * parsed menus/actionbar.txt and knows how many slots exist -- see PlayerInventory::init()'s
	 * header comment for the same shape of dependency and why it hasn't moved yet (D1).
	 */
	void initSlots(unsigned _slots_count);

	unsigned slots_count;
	std::vector<PowerID> hotkeys;       // refers to power_index in PowerManager
	std::vector<PowerID> hotkeys_temp;  // saved here during shapeshifting, restored after
	std::vector<PowerID> hotkeys_mod;   // hotkeys, with item/bonus modifications applied
	std::vector<bool> locked;           // slot can't be dragged out from under a transform

	// Sized to MENU_COUNT, not slots_count -- one flag per menu button, not per hotkey slot.
	std::vector<bool> requires_attention;
	bool updated;
};

#endif
