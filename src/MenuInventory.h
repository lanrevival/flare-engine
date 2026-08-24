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
 * class MenuInventory
 */

#ifndef MENU_INVENTORY_H
#define MENU_INVENTORY_H

#include "CommonIncludes.h"
#include "Menu.h"
#include "MenuItemStorage.h"
#include "Utils.h"
#include "WidgetLabel.h"

class BonusData;
class GameSlotPreview;
class StatBlock;
class WidgetButton;

class MenuInventory : public Menu {
private:
	void loadGraphics();
	void updateEquipment(int slot);
	void updateEquipmentSetWidgets();

	WidgetLabel label_inventory;
	WidgetLabel label_currency;
	WidgetButton *button_close;
	WidgetButton *button_sort;

	// equipment swap buttons
	std::vector<WidgetButton*> equipmentSetButton;
	WidgetButton* equipmentSetPrevious;
	WidgetButton* equipmentSetNext;
	WidgetLabel* equipmentSetLabel;

	// label and widget positions
	Rect help_pos;
	int carried_cols;
	int carried_rows;
	std::vector<Point> equipped_pos;
	Point carried_pos;

	Timer tap_to_activate_timer;

	int activated_slot;
	ItemID activated_item;

	GameSlotPreview* preview;
	bool preview_enabled;
	Point preview_pos;

	bool sort_enabled;
	Point sort_pos;

public:
	enum {
		CTRL_NONE = 0,
		CTRL_VENDOR = 1,
		CTRL_STASH = 2
	};

	static const int NO_AREA = -1;
	static const int EQUIPMENT = 0;
	static const int CARRIED = 1;

	static const bool ADD_PLAY_SOUND = true;
	static const bool ADD_AUTO_EQUIP = true;
	static const bool IS_DRAGGING = true;

	explicit MenuInventory();
	~MenuInventory();
	void align();

	void applyDeathPenalty();

	/** Steps the active equipment set: +1 next, -1 previous, 0 nothing.
	 *
	 * Driven from GameStatePlay's tick, not from this class's logic(), which is where it used to
	 * live as a direct read of inpt->pressing[Input::EQUIPMENT_SWAP]. See PlayerCommand.h. The
	 * guard on max_equipment_set stays here because it is inventory state -- the caller should
	 * not have to know whether this character's mod defines more than one set.
	 *
	 * Returns whether anything happened, so the caller can claim the input lock on exactly the
	 * ticks the old code claimed it: a mod that defines no equipment sets left the key unclaimed
	 * for the rest of the UI, and still does.
	 */
	bool applyEquipmentSetDelta(int delta);
	void logic();
	void render();
	void renderTooltips(const Point& position);
	int areaOver(const Point& position);

	ItemStack click(const Point& position);
	void itemReturn(ItemStack stack);
	bool drop(const Point& position, ItemStack stack);
	void activate(const Point& position);

	bool add(ItemStack stack, int area, int slot, bool play_sound, bool auto_equip);
	bool remove(ItemID item, int quantity);
	void removeFromPrevSlot(int quantity);
	void addCurrency(int count);
	bool buy(ItemStack stack, int tab, bool dragging);
	bool sell(ItemStack stack);

	bool requirementsMet(ItemID item);

	void applyEquipment();
	void applyItemStats();
	void applyItemSetBonuses(std::vector<ItemSetID> &active_sets, std::vector<int> &active_set_quantities);
	void applyBonus(const BonusData* bdata);
	void applyEquipmentSet(unsigned set);
	void applyNextEquipmentSet();
	void applyPreviousEquipmentSet();

	int getEquippedCount();
	int getTotalSlotCount();

	void clearHighlight();

	void fillEquipmentSlots();

	int getMaxPurchasable(ItemStack item, int vendor_tab);

	void disableEquipmentSlot(size_t slot_type);

	bool canEquipItem(const Point& position);
	bool canUseItem(const Point& position);
	bool canPlaceItemOnActionbar(const Point& position);

	Rect carried_area;
	std::vector<Rect> equipped_area;

	/** Points at PlayerInventory::inventory. NOT a second array -- see PlayerInventory.h.
	 *
	 * A pointer rather than a reference so the declaration stays a one-liner and every
	 * inventory[EQUIPMENT] in this class and its ~60 callers keeps compiling untouched. That is
	 * deliberate: it keeps this commit a pure ownership move. P1.3d-4b deletes this member and
	 * points the simulation-side callers at pinv directly.
	 */
	MenuItemStorage* inventory;
	int drag_prev_src;

	bool changed_equipment;

	short inv_ctrl;

	std::string show_book;

	std::queue<ItemStack> drop_stack;
};

#endif

