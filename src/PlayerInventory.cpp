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

#include "Avatar.h"
#include "EngineSettings.h"
#include "ItemManager.h"
#include "PlayerInventory.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "StatBlock.h"
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

bool PlayerInventory::isEquipSlotActive(size_t equipped) const {
	// equipment_set 0 means "in every set", so such a slot is active whichever set is selected.
	return equipment_set[equipped] == 0 || equipment_set[equipped] == active_equipment_set;
}

bool PlayerInventory::equipmentContain(ItemID item, int quantity) const {
	int total_quantity = 0;
	for (int i = 0; i < MAX_EQUIPPED; ++i) {
		if (!isEquipSlotActive(i))
			continue;

		if (inventory[EQUIPMENT].storage[i].item == item)
			total_quantity += inventory[EQUIPMENT].storage[i].quantity;

		if (total_quantity >= quantity)
			return true;
	}
	return false;
}

int PlayerInventory::getEquippedSetCount(size_t set_id) const {
	int quantity = 0;
	for (int i = 0; i < MAX_EQUIPPED; i++) {
		ItemID item_id = inventory[EQUIPMENT].storage[i].item;
		if (items->isValid(item_id) && isEquipSlotActive(i)) {
			if (items->items[item_id]->set == set_id) {
				quantity++;
			}
		}
	}
	return quantity;
}

int PlayerInventory::getEquipSlotFromItem(ItemID item, bool only_empty_slots) const {
	// -2 and -1 are different answers and callers rely on it: -2 is "this character may not wear
	// that", -1 is "may, but has no free slot of the right type".
	if (!items->isValid(item) || !items->requirementsMet(&pc->stats, item))
		return -2;

	int equip_slot = -1;

	// find first empty (or just first) slot for item to equip
	for (int i = 0; i < MAX_EQUIPPED; i++) {
		if (!isEquipSlotActive(i))
			continue;

		if (slot_type[i] == items->items[item]->type) {
			if (inventory[EQUIPMENT].storage[i].empty()) {
				// empty and matching, no need to search more
				equip_slot = i;
				break;
			}
			else if (!only_empty_slots && equip_slot == -1) {
				// non-empty and matching
				equip_slot = i;
			}
		}
	}

	return equip_slot;
}

bool PlayerInventory::canActivateItem(ItemID item) const {
	if (!items->isValid(item))
		return false;

	if (!items->items[item]->script.empty())
		return true;
	if (!items->items[item]->book.empty())
		return true;
	if (items->items[item]->power > 0 && getEquipSlotFromItem(item, !ONLY_EMPTY_SLOTS) == -1)
		return true;

	return false;
}

PowerID PlayerInventory::getPowerMod(PowerID meta_power) const {
	for (int i = 0; i < inventory[EQUIPMENT].getSlotNumber(); ++i) {
		if (!isEquipSlotActive(i))
			continue;

		ItemID id = inventory[EQUIPMENT].storage[i].item;
		if (!items->isValid(id))
			continue;

		for (size_t j = 0; j < items->items[id]->replace_power.size(); j++) {
			if (items->items[id]->replace_power[j].first == meta_power && items->items[id]->replace_power[j].second != meta_power) {
				return items->items[id]->replace_power[j].second;
			}
		}
	}

	return 0;
}

void PlayerInventory::removeCurrency(int count) {
	inventory[CARRIED].remove(eset->misc.currency_id, count);
}
