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
 */

#include "ActionBarState.h"
#include "PowerManager.h"
#include "SharedGameResources.h"

ActionBarState::ActionBarState()
	: slots_count(0)
	, updated(false) {
	// MENU_COUNT is a compile-time constant, unlike slots_count, so this can size itself here
	// rather than waiting for initSlots().
	requires_attention.resize(MENU_COUNT, false);
}

void ActionBarState::initSlots(unsigned _slots_count) {
	slots_count = _slots_count;
	hotkeys.resize(slots_count);
	hotkeys_temp.resize(slots_count);
	hotkeys_mod.resize(slots_count);
	locked.resize(slots_count);
}

void ActionBarState::clearSlot(size_t slot) {
	hotkeys[slot] = 0;
	hotkeys_temp[slot] = 0;
	hotkeys_mod[slot] = 0;
	locked[slot] = false;
}

void ActionBarState::addPower(const PowerID id, const PowerID target_id) {
	if (!powers->isValid(id))
		return;

	// some powers are explicitly prevented from being placed on the actionbar
	if (powers->powers[id]->no_actionbar)
		return;

	// can't put passive powers on the action bar
	if (powers->powers[id]->passive)
		return;

	// if we're not replacing an existing power, avoid placing duplicate powers
	if (target_id == 0) {
		for (unsigned i = 0; i < 12; ++i) {
			if (hotkeys[i] == id)
				return;
		}
	}

	// MAIN slots have priority. 10, 11 and 12 are MenuActionBar::SLOT_MAIN1/SLOT_MAIN2/SLOT_MAX --
	// this class does not include MenuActionBar.h, so the literals are a duplicate of those values,
	// the same call PlayerInventory::EQUIPMENT/CARRIED already made. Matches the original loop
	// bounds exactly, including relying on menus/actionbar.txt always defining slot_M1 and slot_M2
	// so slots_count is always >= 12 -- moved, not fixed; that assumption predates this class.
	for (unsigned i = 10; i < 12; ++i) {
		if (hotkeys[i] == target_id) {
			if (target_id == 0 && prevent_changing[i]) {
				continue;
			}
			hotkeys[i] = id;
			updated = true;
			if (target_id == 0)
				return;
		}
	}

	// now try 0-9 slots
	for (unsigned i = 0; i < 10; ++i) {
		if (hotkeys[i] == target_id) {
			if (target_id == 0 && prevent_changing[i]) {
				continue;
			}
			hotkeys[i] = id;
			updated = true;
			if (target_id == 0)
				return;
		}
	}
}

void ActionBarState::set(std::vector<PowerID> power_id, bool skip_empty) {
	for (unsigned i = 0; i < slots_count; i++) {
		if (!powers->isValid(power_id[i]))
			continue;

		if (!powers->powers[power_id[i]] || powers->powers[power_id[i]]->no_actionbar)
			continue;

		if (!skip_empty || hotkeys[i] == 0)
			hotkeys[i] = power_id[i];
	}
	updated = true;
}
