/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012-2016 Justin Jacobs

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
 * class PlayerInventory
 */

#include "PlayerInventory.h"
#include "WidgetSlot.h"

PlayerInventory::PlayerInventory()
	: active_equipment_set(0)
	, max_equipment_set(0)
	, currency(0)
	// The same defaults MenuInventory's constructor used to carry. They are only ever seen if
	// menus/inventory.txt is missing or defines no slots, in which case init() below overwrites
	// MAX_EQUIPPED with 0 and the character has nowhere to put anything -- which is what the old
	// code did too.
	, MAX_EQUIPPED(4)
	, MAX_CARRIED(64) {
}

void PlayerInventory::init(const std::vector<Rect>& equipped_area, const std::vector<size_t>& _slot_type,
						   const std::vector<unsigned int>& _equipment_set,
						   const Rect& carried_area, int carried_cols, int carried_rows) {
	slot_type = _slot_type;
	equipment_set = _equipment_set;

	MAX_EQUIPPED = static_cast<int>(equipped_area.size());
	MAX_CARRIED = carried_cols * carried_rows;

	equip_slot_enabled.resize(MAX_EQUIPPED, true);

	inventory[EQUIPMENT].initFromList(MAX_EQUIPPED, equipped_area, slot_type);
	inventory[CARRIED].initGrid(MAX_CARRIED, carried_area, carried_cols);

	for (size_t i = 0; i < equipment_set.size(); i++) {
		if (equipment_set[i] > max_equipment_set) {
			max_equipment_set = equipment_set[i];
		}
	}
}

bool PlayerInventory::isEquipSlotEnabled(int slot) const {
	if (slot < 0 || slot >= static_cast<int>(equip_slot_enabled.size()))
		return false;

	return equip_slot_enabled[slot];
}

void PlayerInventory::setEquipSlotEnabled(int slot, bool enabled) {
	if (slot < 0 || slot >= static_cast<int>(equip_slot_enabled.size()))
		return;

	equip_slot_enabled[slot] = enabled;

	// The widget still needs it: a disabled WidgetSlot draws differently and refuses clicks.
	// Presentation follows the state, never the other way round. This is the one place the
	// simulation touches a widget, and P1.3d-4c is what deletes it.
	if (slot < static_cast<int>(inventory[EQUIPMENT].slots.size()))
		inventory[EQUIPMENT].slots[slot]->enabled = enabled;
}

PlayerInventory::~PlayerInventory() {
}
