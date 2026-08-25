/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
Copyright © 2013 Kurt Rinnert
Copyright © 2014 Henrik Andersson
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
 * class MenuItemStorage
 */

#ifndef MENU_ITEM_STORAGE_H
#define MENU_ITEM_STORAGE_H

#include "CommonIncludes.h"
#include "ItemManager.h"
#include "ItemStorage.h"

class StatBlock;
class TooltipData;
class WidgetSlot;

/**
 * Holds an ItemStorage rather than being one (P1.3d-4c). Two ways to get data:
 *  - default-constructed, then initGrid()/initFromList(): allocates and owns its own ItemStorage,
 *    lazily, the first time one of those is called (MenuStash, MenuVendor -- their contents have
 *    no sim-side counterpart, this class is the only owner there is). data starts NULL rather than
 *    being allocated in the constructor, on purpose: MenuStashTab lives in a std::vector that
 *    grows via push_back, and a not-yet-initialized MenuItemStorage has to copy safely (NULL data,
 *    owns_data false) on every reallocation. Allocating eagerly would give each temporary a live
 *    owned ItemStorage before init, and the vector's shallow copy would leave two MenuItemStorages
 *    both believing they own, and will delete, the same one -- a double-free waiting for the first
 *    table with two tabs. This is the same reason ItemStorage's own `storage` field stays NULL
 *    until init().
 *  - bind(): stops owning, points at an externally-owned ItemStorage instead (MenuInventory,
 *    which points at PlayerInventory::inventory -- the simulation owns that data, this class is
 *    only a rendering/drag-and-drop view over it).
 * Either way, ItemStorage's own public methods are forwarded under the same names so existing
 * call sites (stock.add(...), stock[i].empty(), stock.count(...), ...) don't need to change.
 */
class MenuItemStorage {
protected:
	Rect grid_area;
	Point grid_pos;
	int nb_cols;

public:
	MenuItemStorage();
	~MenuItemStorage();

	void bind(ItemStorage* external);

	void initGrid(int _slot_number, const Rect& _area, int nb_cols);
	void initFromList(int _slot_number, const std::vector<Rect>& _area, const std::vector<size_t>& _slot_type);

	// forwarded to data -- same names/signatures as ItemStorage
	ItemStack& operator[](int slot);
	int getSlotNumber() const;
	void setItems(const std::string& s);
	void setQuantities(const std::string& s);
	void setForeign(bool is_foreign);
	std::string getItems();
	std::string getQuantities();
	ItemStack add(ItemStack stack, int slot);
	void subtract(int slot, int quantity);
	bool remove(ItemID item, int quantity);
	void sort(int mode);
	void sortNext();
	void clear();
	void clean();
	bool empty();
	bool full(ItemStack stack);
	int count(ItemID item);
	bool contain(ItemID item, int quantity);
	void refreshSortTooltip();

	// rendering
	void render();
	int slotOver(const Point& position);
	TooltipData checkTooltip(const Point& position, StatBlock *stats, int context, bool input_hint);
	ItemStack click(const Point& position);
	void itemReturn(ItemStack stack);
	void highlightMatching(ItemID item_id);
	void highlightClear();
	void setPos(int x, int y);
	ItemStack getItemStackAtPos(const Point& position);
	std::vector<size_t> slot_type;

	int drag_prev_slot;
	std::vector<WidgetSlot*> slots;
	WidgetSlot *current_slot;

	bool max_quantity_is_one;
	bool click_subtracts_item;

	// public rather than accessor-wrapped, matching ItemStorage's own storage/sort_tooltip --
	// the handful of call sites that reach past the forwarded API straight at ItemStorage's raw
	// fields (MenuInventory.cpp, MenuStash.cpp) go through data-> explicitly.
	ItemStorage* data;

private:
	bool owns_data;
};

#endif


