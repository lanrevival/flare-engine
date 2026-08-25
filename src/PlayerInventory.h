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
 * There is exactly ONE copy of this data. MenuInventory::inventory (P1.3d-4c) is a MenuItemStorage
 * array bound, via MenuItemStorage::bind(), to point AT the array below -- not a second array, and
 * that is the single most important property of this class: two copies desynchronise and the
 * symptom is items disappearing. inventory[] itself is a plain ItemStorage, not a MenuItemStorage:
 * P1.3d-4c is what removed the std::vector<WidgetSlot*> that used to ride along on it, by making
 * MenuItemStorage HOLD an ItemStorage instead of BEING one. The simulation keeps the ItemStorage,
 * the menu keeps the widgets.
 *
 * D1 (plans/phase1/P1.3-VERIFICATION.md) -- how many equipment slots a character has used to be
 * however many rectangles a mod author drew in menus/inventory.txt -- is resolved by P1.3d-4d for
 * any mod that defines engine/equipment.txt: loadEquipmentData() reads slot type/set and carrying
 * capacity from it directly, no screen rectangle involved, and this class can be built with no
 * menu layout file at all. init() still exists, and still takes screen rectangles, as the fallback
 * for a mod chain that has no engine/equipment.txt -- every mod that predates this change, so the
 * old behaviour has to keep working exactly as it did. See plans/phase1/P1.3d-4d-equipment-data.md.
 */

#ifndef PLAYER_INVENTORY_H
#define PLAYER_INVENTORY_H

#include "CommonIncludes.h"
#include "ItemManager.h"
#include "ItemStorage.h"
#include "Utils.h"

class PlayerInventory {
public:
	// Indices into inventory[] below. MenuInventory::EQUIPMENT and ::CARRIED still exist and still
	// have these values; they are kept because ~60 call sites outside this class spell them that
	// way and P1.3d-4b is where those move.
	static const int EQUIPMENT = 0;
	static const int CARRIED = 1;

	// Argument to getEquipSlotFromItem(). MenuInventory used to declare this; it does not any
	// more, and there is deliberately no alias left behind -- a second spelling of a constant that
	// selects between two opposite search behaviours is exactly the thing that drifts.
	static const bool ONLY_EMPTY_SLOTS = true;

	PlayerInventory();
	~PlayerInventory();

	/** Sizes the two storages. Called once, from MenuInventory's constructor, after it has parsed
	 * menus/inventory.txt -- see the note about D1 above. equipped_area is still a vector of
	 * screen Rects rather than a plain count for the same reason: D1 is what number of equipment
	 * slots means before P1.3d-4d, and it is "however many rects the mod drew." carried_area
	 * dropped out of this signature in P1.3d-4c -- MenuItemStorage's own initGrid() is what needed
	 * it, for widget positions this class no longer allocates.
	 */
	void init(const std::vector<Rect>& equipped_area, const std::vector<size_t>& _slot_type,
			  const std::vector<unsigned int>& _equipment_set,
			  int carried_cols, int carried_rows);

	/** P1.3d-4d. Reads engine/equipment.txt, if the mod chain has one, as the authoritative source
	 * for equipment slots/sets and carrying capacity -- no screen rectangle involved. Returns
	 * whether the file existed; MenuInventory's constructor calls this before init() and only
	 * falls back to init()'s menus/inventory.txt-derived path if it returns false. See
	 * plans/phase1/P1.3d-4d-equipment-data.md.
	 */
	bool loadEquipmentData();

	/** Whether an equipment slot may hold an item.
	 *
	 * This was WidgetSlot::enabled until 17a15ffa -- a widget flag the simulation read to decide
	 * whether an item could be equipped. It became a vector on MenuInventory then, and it belongs
	 * here now, as data only (P1.3d-4c removed the WidgetSlot push this method used to do).
	 * MenuInventory::applyEquipment() pulls this into the equipment slots' widgets after every
	 * call to PlayerInventory::applyEquipment(), the only place setEquipSlotEnabled() is called
	 * from -- so the dependency still runs state -> presentation, just pulled instead of pushed.
	 */
	bool isEquipSlotEnabled(int slot) const;
	void setEquipSlotEnabled(int slot, bool enabled);

	/** Questions a character can be asked about what it is carrying.
	 *
	 * All read-only, all previously methods on MenuInventory, all moved here unchanged by
	 * P1.3d-4b-2. The mutators -- add(), remove(), applyEquipment(), the equipment-set switches --
	 * did NOT come with them, and the reason is worth stating so nobody "finishes the job" by
	 * dragging them across: add() calls menu_act->addPower() and applyEquipment() calls
	 * menu->pow->clearBonusLevels(). Those are not redraws that could be dropped; the action bar's
	 * contents and MenuPowers' bonus levels are real state that two other classes still own.
	 * Moving the mutators now would only trade "inventory reaches through a menu" for
	 * "PlayerInventory reaches through a menu", which is not progress. They move when
	 * P1.3e extracts ActionBarState and MenuPowers gives up its bonus levels.
	 *
	 * isEquipSlotActive() is the one to read carefully: an equipment_set of 0 means the slot is
	 * shared by every set, so it is active always. Drop that clause and a character with no
	 * equipment sets defined at all is wearing nothing.
	 */
	bool isEquipSlotActive(size_t equipped) const;
	bool equipmentContain(ItemID item, int quantity) const;
	int getEquippedSetCount(size_t set_id) const;
	int getEquipSlotFromItem(ItemID item, bool only_empty_slots) const;
	bool canActivateItem(ItemID item) const;
	PowerID getPowerMod(PowerID meta_power) const;

	/** The one mutator that came across in P1.3d-4b-2, because it has no UI in it at all: it is a
	 * single call into the carried storage. addCurrency() stayed behind then -- it goes through
	 * add(), which hadn't moved yet.
	 */
	void removeCurrency(int count);

	/** The rest of the mutators, moved in P1.3d-4b-3 once P1.3e (ActionBarState) and P1.3g
	 * (PowerBonusState) had removed the two real reasons they couldn't come with 4b-2: add() used
	 * to call menu_act->addPower() and applyEquipment() used to call menu->pow->clearBonusLevels().
	 * Both now go through pab/pbs, neither of which needs a menu to exist.
	 *
	 * What did NOT move, and why -- each of these is a MenuInventory-owned UI concern entangled in
	 * the original body, not a simulation concern that got left behind by mistake:
	 *
	 *   - add()'s EQUIPMENT branch used to call updateEquipment(slot), a one-line dirty flag
	 *     MenuManager reads to decide whether to redraw a slot icon. Dropped from this version.
	 *     Every UI caller that changes equipment already separately calls updateEquipment() itself
	 *     at the same call site (see MenuInventory::drop()/activate()), so nothing currently relies
	 *     on add() setting it internally.
	 *   - applyEquipment()'s last two lines used to call preview->loadGraphicsFromInventory(this)
	 *     -- a GameSlotPreview widget used by the new-game/continue screen. Dropped. MenuInventory
	 *     keeps a thin applyEquipment() that calls this version and then does that one line, and
	 *     every existing caller (UI or otherwise) still goes through it unchanged -- see below.
	 *   - remove()'s FIRST branch checked activated_item/activated_slot, MenuInventory's record of
	 *     which exact carried slot a right-click activation targeted, so the right stack (not just
	 *     any stack of the same item) gets consumed. That is UI interaction state with no
	 *     simulation meaning outside a live click, so it is not here -- this version is only the
	 *     general-case fallback. MenuInventory::remove() keeps the special case and falls through
	 *     to this for everything else.
	 *
	 * MenuInventory::add(), ::remove() and ::applyEquipment() all still exist, as thin wrappers
	 * around these, for exactly the reason MenuActionBar::addPower()'s id==0 branch and
	 * MenuActionBar::clear() still exist after P1.3e: a caller that IS a live UI (MenuManager.cpp,
	 * MenuPowers.cpp, and MenuInventory's own drop()/activate()/buy()/itemReturn()) has no reason
	 * to skip the widget bookkeeping, and forcing it to would be trading one kind of correctness
	 * risk for another. Only genuinely sim-side callers (CampaignManager, PowerManager,
	 * EventManager, GameStatePlay, SaveLoad) were repointed to call these directly.
	 *
	 * drop_stack is new here, not moved: overflow items (inventory full) still have to go
	 * somewhere when add() is called with no menu around to own a queue for them.
	 * GameStatePlay::checkLootDrop() drains this alongside menu->drop_stack/camp->drop_stack/
	 * menu->inv->drop_stack, which still exist and are still drained separately -- UI-triggered
	 * overflow (drag-and-drop) still goes through MenuInventory's own queue via the wrapper.
	 */
	bool add(ItemStack stack, int area, int slot, bool play_sound, bool auto_equip);
	bool remove(ItemID item, int quantity);
	void addCurrency(int count);
	void applyEquipment();
	void applyDeathPenalty();
	void fillEquipmentSlots();

	// Argument names for add(), moved from MenuInventory alongside it. Deleted there, not aliased
	// -- same reasoning as ONLY_EMPTY_SLOTS above.
	static const bool ADD_PLAY_SOUND = true;
	static const bool ADD_AUTO_EQUIP = true;

	std::queue<ItemStack> drop_stack;

	ItemStorage inventory[2];

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

	// applyEquipment()'s own helpers, moved with it. Only ever called from within it (or, for
	// disableEquipmentSlot(), from within applyEquipment() and nowhere else in the whole tree --
	// verified by grep before moving, not assumed).
	void applyItemStats();
	void applyItemSetBonuses(std::vector<ItemSetID>& active_sets, std::vector<int>& active_set_quantities);
	void applyBonus(const BonusData* bdata);
	void disableEquipmentSlot(size_t disable_slot_type);

	// currency is a cache of inventory[CARRIED].count(currency_id), recomputed here so it stays
	// correct with no MenuInventory::logic() around to do it once a frame. That line still exists
	// there too, redundantly -- see MenuInventory.cpp -- which is harmless, not a second owner.
	void recomputeCurrency();
};

#endif
