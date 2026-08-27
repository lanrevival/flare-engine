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

class Avatar;
class GameSlotPreview;
class PlayerInventory;
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

	// Duplicated on PlayerInventory now (P1.3d-4b-3), not aliased -- add()'s real home. Kept here
	// too, unlike the mod-select constant P1.3d-4b-2 deleted outright, because this class's own
	// drop()/activate()/buy()/itemReturn() still call add() (the thin wrapper below) with these as
	// readable argument names, and qualifying every one of those call sites with
	// PlayerInventory:: for a constant that is still exactly `true` on both classes would be
	// churn with no behaviour to show for it.
	static const bool ADD_PLAY_SOUND = true;
	static const bool ADD_AUTO_EQUIP = true;
	static const bool IS_DRAGGING = true;

	/** inventory[] is bound (MenuItemStorage::bind(), not a C++ reference) to player_inventory's
	 * own storage in the constructor -- unlike MenuActionBar's reference members, this binding
	 * uses a real pointer underneath and could in principle be re-bound later, but P2.3 only wires
	 * up construction time; see MenuManager::setPlayer(). */
	explicit MenuInventory(Avatar* _player, PlayerInventory* _player_inventory);
	~MenuInventory();
	void align();

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

	/** add(), remove() and applyEquipment() are thin wrappers now (P1.3d-4b-3) -- the mutators
	 * moved to PlayerInventory, which every genuinely sim-side caller now calls directly. What's
	 * left here is exactly the UI-only behaviour those bodies used to have bundled in: add()'s
	 * equipment-slot redraw flag and the drag-state reset, remove()'s activated-item special case,
	 * applyEquipment()'s GameSlotPreview refresh. See PlayerInventory.h for the full accounting of
	 * what moved and why each of these three didn't move whole. Every existing caller of these
	 * three -- this class's own drop()/activate()/buy()/itemReturn(), MenuManager, MenuPowers --
	 * keeps calling them exactly as before; only CampaignManager/PowerManager/EventManager/
	 * GameStatePlay/SaveLoad were repointed to PlayerInventory's own object directly (P1.3d-4b-3;
	 * P2.3 later gave that object an explicit name at every call site instead of a global).
	 */
	bool add(ItemStack stack, int area, int slot, bool play_sound, bool auto_equip);
	bool remove(ItemID item, int quantity);
	void removeFromPrevSlot(int quantity);
	bool buy(ItemStack stack, int tab, bool dragging);
	bool sell(ItemStack stack);

	bool requirementsMet(ItemID item);

	void applyEquipment();
	void applyEquipmentSet(unsigned set);
	void applyNextEquipmentSet();
	void applyPreviousEquipmentSet();

	int getEquippedCount();
	int getTotalSlotCount();

	void clearHighlight();

	int getMaxPurchasable(ItemStack item, int vendor_tab);

	bool canEquipItem(const Point& position);
	bool canUseItem(const Point& position);
	bool canPlaceItemOnActionbar(const Point& position);

	Rect carried_area;
	std::vector<Rect> equipped_area;

	/** Widget-bearing views onto PlayerInventory::inventory, bound (not copied) in the constructor
	 * via MenuItemStorage::bind() -- see PlayerInventory.h and P1.3d-4c. Every inventory[EQUIPMENT]
	 * / inventory[CARRIED] call site in this class keeps compiling and reading/writing the same
	 * data as before; only the type of the member changed, from a pointer into pinv's array to an
	 * owned array of views bound to it.
	 */
	MenuItemStorage inventory[2];
	int drag_prev_src;

	bool changed_equipment;

	short inv_ctrl;

	std::string show_book;

	std::queue<ItemStack> drop_stack;

	Avatar* player;
	PlayerInventory* player_inventory;
};

#endif

