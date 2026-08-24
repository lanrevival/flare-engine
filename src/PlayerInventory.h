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
 *
 * What the character is carrying and wearing. This used to be fields on MenuInventory, which meant
 * a player's possessions were owned by a window -- fine for one local player, wrong the moment
 * there are eight and only one of them is looking at a screen.
 *
 * There is exactly ONE copy of this data. MenuInventory::inventory is a pointer INTO the array
 * below, not a second array, and that is the single most important property of this class: two
 * copies desynchronise and the symptom is items disappearing.
 *
 * Two things here are honestly out of place and are scheduled, not overlooked:
 *
 *   - MenuItemStorage carries a std::vector<WidgetSlot*>. So the simulation currently owns
 *     widgets. That is a wart. P1.3d-4c is what removes it, by making MenuItemStorage HOLD an
 *     ItemStorage instead of BEING one; the simulation keeps the ItemStorage and the menu keeps
 *     the widgets. Doing it in this step as well would have made one unreviewable commit out of
 *     two reviewable ones.
 *   - init() takes screen rectangles, because how many equipment slots a character has is
 *     currently the number of rectangles somebody drew in menus/inventory.txt. That is D1 in
 *     plans/phase1/P1.3-VERIFICATION.md and it is P1.3d-4d's problem. It is not a resolution
 *     dependency -- icon_size and the grid dimensions are mod constants, measured -- but it does
 *     mean this class cannot yet be built without a menu layout file.
 */

#ifndef PLAYER_INVENTORY_H
#define PLAYER_INVENTORY_H

#include "CommonIncludes.h"
#include "ItemManager.h"
#include "MenuItemStorage.h"
#include "Utils.h"

class PlayerInventory {
public:
	// Indices into inventory[] below. MenuInventory::EQUIPMENT and ::CARRIED still exist and still
	// have these values; they are kept because ~60 call sites outside this class spell them that
	// way and P1.3d-4b is where those move.
	static const int EQUIPMENT = 0;
	static const int CARRIED = 1;

	PlayerInventory();
	~PlayerInventory();

	/** Sizes and builds the two storages. Called once, from MenuInventory's constructor, after it
	 * has parsed menus/inventory.txt -- see the note about D1 above.
	 */
	void init(const std::vector<Rect>& equipped_area, const std::vector<size_t>& _slot_type,
			  const std::vector<unsigned int>& _equipment_set,
			  const Rect& carried_area, int carried_cols, int carried_rows);

	/** Whether an equipment slot may hold an item.
	 *
	 * This was WidgetSlot::enabled until 17a15ffa -- a widget flag the simulation read to decide
	 * whether an item could be equipped. It became a vector on MenuInventory then, and it belongs
	 * here now. Written only through setEquipSlotEnabled(), which also updates the widget, so the
	 * dependency runs state -> presentation and never back.
	 */
	bool isEquipSlotEnabled(int slot) const;
	void setEquipSlotEnabled(int slot, bool enabled);

	MenuItemStorage inventory[2];

	// Which item type each equipment slot accepts, and which equipment set it belongs to (0 means
	// shared across all sets). Parallel to inventory[EQUIPMENT], one entry per slot.
	std::vector<size_t> slot_type;
	std::vector<unsigned int> equipment_set;

	unsigned active_equipment_set;
	unsigned max_equipment_set;

	int currency;

	int MAX_EQUIPPED;
	int MAX_CARRIED;

private:
	std::vector<bool> equip_slot_enabled;
};

#endif
