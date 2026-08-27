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

#include "ActionBarState.h"
#include "Avatar.h"
#include "EffectManager.h"
#include "EngineSettings.h"
#include "FileParser.h"
#include "ItemManager.h"
#include "MessageEngine.h"
#include "PlayerInventory.h"
#include "PowerBonusState.h"
#include "PowerManager.h"
#include "Rng.h"
#include "Settings.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "StatBlock.h"
#include "UtilsParsing.h"

PlayerInventory::PlayerInventory()
	: owner(NULL)
	, actionbar(NULL)
	, powerbonus(NULL)
	, active_equipment_set(0)
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
						   int carried_cols, int carried_rows) {
	slot_type = _slot_type;
	equipment_set = _equipment_set;

	MAX_EQUIPPED = static_cast<int>(equipped_area.size());
	MAX_CARRIED = carried_cols * carried_rows;

	equip_slot_enabled.resize(MAX_EQUIPPED, true);

	inventory[EQUIPMENT].init(MAX_EQUIPPED);
	inventory[CARRIED].init(MAX_CARRIED);

	for (size_t i = 0; i < equipment_set.size(); i++) {
		if (equipment_set[i] > max_equipment_set) {
			max_equipment_set = equipment_set[i];
		}
	}
}

// P1.3d-4d. engine/equipment.txt is optional -- most mods don't have it yet, including every
// third-party mod that predates this change, and that is by design: if it's absent, this returns
// false and the caller (MenuInventory's constructor) falls back to deriving equipment shape from
// menus/inventory.txt exactly as it always has, unchanged. If it's present, it is authoritative:
// this class's shape no longer depends on any screen rectangle. See
// plans/phase1/P1.3d-4d-equipment-data.md for why the file exists and what still lives in
// menus/inventory.txt (screen positions only, matched back to these slots by file order).
bool PlayerInventory::loadEquipmentData() {
	FileParser infile;
	if (!infile.open("engine/equipment.txt", FileParser::MOD_FILE, FileParser::ERROR_NONE))
		return false;

	std::vector<size_t> new_slot_type;
	std::vector<unsigned int> new_equipment_set;
	int new_carried = 0;

	while (infile.next()) {
		if (infile.key == "equip_slot") {
			new_slot_type.push_back(items->getItemTypeIndexByString(Parse::popFirstString(infile.val)));
			new_equipment_set.push_back(static_cast<unsigned int>(Parse::popFirstInt(infile.val)));
		}
		else if (infile.key == "carried_slots") {
			new_carried = Parse::toInt(infile.val);
		}
		else {
			infile.error("PlayerInventory: '%s' is not a valid key.", infile.key.c_str());
		}
	}
	infile.close();

	slot_type = new_slot_type;
	equipment_set = new_equipment_set;

	MAX_EQUIPPED = static_cast<int>(slot_type.size());
	MAX_CARRIED = new_carried;

	equip_slot_enabled.resize(MAX_EQUIPPED, true);

	inventory[EQUIPMENT].init(MAX_EQUIPPED);
	inventory[CARRIED].init(MAX_CARRIED);

	max_equipment_set = 0;
	for (size_t i = 0; i < equipment_set.size(); i++) {
		if (equipment_set[i] > max_equipment_set) {
			max_equipment_set = equipment_set[i];
		}
	}

	return true;
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
	if (!items->isValid(item) || !items->requirementsMet(&owner->stats, item))
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
	recomputeCurrency();
}

// F4 in plans/phase1/P1.3-VERIFICATION.md: inventory[CARRIED] was always the single owner of how
// much currency exists; currency and owner->stats.currency are both caches recomputed together. Until
// P1.3d-4b-3 the only place that ran this recompute was MenuInventory::logic(), once a frame -- so
// it silently stopped updating the moment nothing constructs a MenuManager. Every mutator that can
// change how much currency is carried calls this now, so the cache is never gated on a live UI. The
// MenuInventory::logic() line still runs too, redundantly, which is harmless: same read, same write.
void PlayerInventory::recomputeCurrency() {
	currency = inventory[CARRIED].count(eset->misc.currency_id);
	owner->stats.currency = currency;
}

// Moved from MenuInventory::add() in P1.3d-4b-3. Two things deliberately did NOT come, both
// MenuInventory-owned UI state with no simulation meaning -- see PlayerInventory.h:
//   - the EQUIPMENT branch's updateEquipment(slot) call, a redraw dirty flag
//   - the trailing drag_prev_src = -1, which ends a mouse drag that a sim-triggered add() never
//     started
// menu_act->addPower() became actionbar->addPower() directly -- this call is always id != 0 (power is
// checked > 0 immediately above), which is provably always the pure-state half of that split (see
// ActionBarState.h), so this is not a behaviour choice, just removing a layer that already forwarded
// here.
bool PlayerInventory::add(ItemStack stack, int area, int slot, bool play_sound, bool auto_equip) {
	if (stack.empty())
		return true;

	if (!items->isValid(stack.item))
		return false;

	bool success = true;

	if (play_sound)
		items->playSound(stack.item);

	if (auto_equip && settings->auto_equip) {
		int equip_slot = getEquipSlotFromItem(stack.item, ONLY_EMPTY_SLOTS);
		bool disabled_slots_empty = true;

		// if this item would disable non-empty slots, don't auto-equip it
		for (size_t i = 0; i < items->items[stack.item]->disable_slots.size(); ++i) {
			for (int j = 0; j < MAX_EQUIPPED; ++j) {
				if (!inventory[EQUIPMENT].storage[j].empty() && slot_type[j] == items->items[stack.item]->disable_slots[i]) {
					disabled_slots_empty = false;
				}
			}
		}

		if (equip_slot >= 0 && isEquipSlotEnabled(equip_slot) && disabled_slots_empty) {
			area = EQUIPMENT;
			slot = equip_slot;
		}

	}

	if (area == CARRIED) {
		ItemStack leftover = inventory[CARRIED].add(stack, slot);
		if (!leftover.empty()) {
			if (items->items[stack.item]->quest_item) {
				// quest items can't be dropped, so find a non-quest item in the inventory to drop
				const int max_q = items->items[stack.item]->max_quantity;
				int slots_to_clear = 1;
				if (max_q > 0)
					slots_to_clear = leftover.quantity + (leftover.quantity % max_q) / max_q;

				for (int i = MAX_CARRIED-1; i >=0; --i) {
					if (items->items[inventory[CARRIED].storage[i].item]->quest_item)
						continue;

					drop_stack.push(inventory[CARRIED].storage[i]);
					inventory[CARRIED].storage[i].clear();

					slots_to_clear--;
					if (slots_to_clear <= 0)
						break;
				}

				if (slots_to_clear > 0) {
					// inventory is full of quest items! we have to drop this now...
					drop_stack.push(leftover);
				}
				else {
					add(leftover, CARRIED, slot, !ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
				}
			}
			else {
				drop_stack.push(leftover);
			}
			owner->logMsg(msg->get("Inventory is full."), Avatar::MSG_NORMAL);
			success = false;
		}
	}
	else if (area == EQUIPMENT) {
		ItemStack &dest = inventory[EQUIPMENT].storage[slot];
		ItemStack leftover;
		leftover.item = stack.item;

		if (!dest.empty() && dest.item != stack.item) {
			// items don't match, so just add the stack to the carried area
			leftover.quantity = stack.quantity;
		}
		else if (dest.quantity + stack.quantity > items->items[stack.item]->max_quantity) {
			// items match, so attempt to merge the stacks. Any leftover will be added to the carried area
			leftover.quantity = dest.quantity + stack.quantity - items->items[stack.item]->max_quantity;
			stack.quantity = items->items[stack.item]->max_quantity - dest.quantity;
			if (stack.quantity > 0) {
				add(stack, EQUIPMENT, slot, !ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
			}
		}
		else {
			// put the item in the appropriate equipment slot
			inventory[EQUIPMENT].add(stack, slot);
			leftover.clear();
		}

		if (!leftover.empty()) {
			add(leftover, CARRIED, ItemStorage::NO_SLOT, !ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
		}

		applyEquipment();
	}

	// if this item has a power, place it on the action bar if possible
	if (success && items->getItemType(items->items[stack.item]->type).auto_actionbar && items->items[stack.item]->power > 0) {
		actionbar->addPower(items->items[stack.item]->power, 0);
	}

	recomputeCurrency();

	return success;
}

// Moved from MenuInventory::remove() in P1.3d-4b-3. The activated_item/activated_slot special case
// did NOT come -- it is MenuInventory's record of which exact carried slot a right-click activation
// targeted, UI interaction state with no meaning outside a live click. MenuInventory::remove() keeps
// that check and falls through to this for the general case, unchanged in behaviour either way.
bool PlayerInventory::remove(ItemID item, int quantity) {
	if (!inventory[CARRIED].remove(item, quantity)) {
		if (!inventory[EQUIPMENT].remove(item, quantity)) {
			return false;
		}
		else {
			applyEquipment();
		}
	}

	recomputeCurrency();

	return true;
}

// Moved from MenuInventory::addCurrency() in P1.3d-4b-3 unchanged -- it only ever calls add() with
// CARRIED, which never touches the one widget call add() used to make, so there was nothing to
// split here and no wrapper is needed on MenuInventory for it.
void PlayerInventory::addCurrency(int count) {
	if (count > 0) {
		ItemStack stack;
		stack.item = eset->misc.currency_id;
		stack.quantity = count;
		add(stack, CARRIED, ItemStorage::NO_SLOT, !ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
	}
}

// Moved from MenuInventory::applyEquipment() in P1.3d-4b-3. Only the last two lines did not come --
// preview->loadGraphicsFromInventory(this), the new-game/continue screen's GameSlotPreview widget.
// MenuInventory keeps a thin applyEquipment() that calls this and then does that one line; every
// caller that had a reason to want it (a live UI) still goes through that wrapper unchanged, since
// nothing here required them to stop. Only genuinely sim-side callers were repointed to call this
// directly. clearBonusLevels() was menu->pow->clearBonusLevels() before P1.3g; it is powerbonus->
// unconditionally now, same as every other caller in the tree.
void PlayerInventory::applyEquipment() {
	if (items->items.empty())
		return;

	ItemID item_id;
	std::vector<ItemSetID> active_sets;
	std::vector<int> active_set_quantities;

	// calculate bonuses to basic stats, added by items
	bool checkRequired = true;
	while(checkRequired) {
		checkRequired = false;
		active_sets.clear();
		active_set_quantities.clear();

		for (size_t j = 0; j < eset->primary_stats.list.size(); ++j) {
			owner->stats.primary_additional[j] = 0;
		}

		for (int i = 0; i < MAX_EQUIPPED; i++) {
			if (isEquipSlotActive(i)) {
				item_id = inventory[EQUIPMENT].storage[i].item;
				if (!items->isValid(item_id))
					continue;

				Item* item = items->items[item_id];
				unsigned bonus_counter = 0;
				while (bonus_counter < item->bonus.size()) {
					for (size_t j = 0; j < eset->primary_stats.list.size(); ++j) {
						if (item->bonus[bonus_counter].type == BonusData::PRIMARY_STAT && item->bonus[bonus_counter].index == j)
							owner->stats.primary_additional[j] += static_cast<int>(item->bonus[bonus_counter].value.get());
					}

					bonus_counter++;
				}
			}
		}

		// determine which item sets are active and count the number of items for each active set
		std::vector<ItemSetID>::iterator it;
		for (int i=0; i<MAX_EQUIPPED; i++) {
			ItemStack& stack = inventory[EQUIPMENT].storage[i];

			if (items->isValid(stack.item) && isEquipSlotActive(i) && items->items[stack.item]->set > 0) {
				it = std::find(active_sets.begin(), active_sets.end(), items->items[stack.item]->set);
				if (it != active_sets.end()) {
					active_set_quantities[std::distance(active_sets.begin(), it)] += 1;
				}
				else {
					active_sets.push_back(items->items[stack.item]->set);
					active_set_quantities.push_back(1);
				}
			}
		}

		// calculate bonuses to basic stats, added by item sets
		for (size_t k = 0; k < active_sets.size(); ++k) {
			if (!items->isValidSet(active_sets[k]))
				continue;

			ItemSet* item_set = items->item_sets[active_sets[k]];
			for (size_t bonus_counter = 0; bonus_counter < item_set->bonus.size(); ++bonus_counter) {
				if (item_set->bonus[bonus_counter].requirement != active_set_quantities[k])
					continue;

				for (size_t j = 0; j < eset->primary_stats.list.size(); ++j) {
					if (item_set->bonus[bonus_counter].type == BonusData::PRIMARY_STAT && item_set->bonus[bonus_counter].index == j)
						owner->stats.primary_additional[j] += static_cast<int>(item_set->bonus[bonus_counter].value.get());
				}
			}
		}

		// check that each equipped item fit requirements and is in the proper type of slot
		for (int i = 0; i < MAX_EQUIPPED; i++) {
			ItemStack& stack = inventory[EQUIPMENT].storage[i];

			if (items->isValid(stack.item)) {
				if ((isEquipSlotActive(i) && !items->requirementsMet(&owner->stats, stack.item)) || (!stack.empty() && slot_type[i] != items->items[stack.item]->type)) {
					add(stack, CARRIED, ItemStorage::NO_SLOT, ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
					stack.clear();
					checkRequired = true;
				}
			}
		}
	}

	// defaults
	for (unsigned i=0; i<owner->stats.powers_list_items.size(); ++i) {
		PowerID id = owner->stats.powers_list_items[i];
		// owner->stats.hp > 0 is hack to keep on_death revive passives working
		if (powers->powers[id]->passive && owner->stats.hp > 0 && !powers->powers[id]->passive_effects_persist) {
			owner->stats.effects.removeEffectPassive(id);
		}
	}
	owner->stats.powers_list_items.clear();

	// reset wielding vars
	owner->stats.equip_flags.clear();

	// remove all effects and bonuses added by items
	owner->stats.effects.clearItemEffects();

	// reset power level bonuses
	powerbonus->clearBonusLevels();

	applyItemStats();
	applyItemSetBonuses(active_sets, active_set_quantities);

	// enable all slots by default
	for (int i = 0; i < MAX_EQUIPPED; ++i) {
		setEquipSlotEnabled(i, true);
	}
	// disable any incompatible slots, unequipping items if neccessary
	for (int i = 0; i < MAX_EQUIPPED; ++i) {
		item_id = inventory[EQUIPMENT][i].item;

		if (items->isValid(item_id) && isEquipSlotActive(i)) {
			for (size_t j = 0; j < items->items[item_id]->disable_slots.size(); ++j) {
				disableEquipmentSlot(items->items[item_id]->disable_slots[j]);
			}
		}
	}

	// disable equipment slots via passive powers
	for (size_t i = 0; i < owner->stats.powers_passive.size(); ++i) {
		PowerID id = owner->stats.powers_passive[i];
		if (!powers->powers[id]->passive)
			continue;

		for (size_t j = 0; j < powers->powers[id]->disable_equip_slots.size(); ++j) {
			disableEquipmentSlot(powers->powers[id]->disable_equip_slots[j]);
		}
	}
	for (size_t i = 0; i < owner->stats.powers_list_items.size(); ++i) {
		PowerID id = owner->stats.powers_list_items[i];
		if (!powers->powers[id]->passive)
			continue;

		for (size_t j = 0; j < powers->powers[id]->disable_equip_slots.size(); ++j) {
			disableEquipmentSlot(powers->powers[id]->disable_equip_slots[j]);
		}
	}

	// update stat display
	owner->stats.refresh_stats = true;

	if (owner->stats.cur_state == StatBlock::ENTITY_POWER) {
		owner->stats.cur_state = StatBlock::ENTITY_STANCE;
	}

	recomputeCurrency();
}

// The state-mutation half of MenuInventory::applyNextEquipmentSet()/applyPreviousEquipmentSet() --
// see P1.4c. Widget refresh (updateEquipmentSetWidgets(), the hudlog message) stays on
// MenuInventory, which still wraps this. Wraparound matches those two functions exactly: past the
// top, wrap to 1; past the bottom, wrap to max_equipment_set.
bool PlayerInventory::applyEquipmentSetDelta(int delta) {
	if (delta == 0 || max_equipment_set == 0)
		return false;

	if (delta > 0) {
		if (active_equipment_set < max_equipment_set)
			active_equipment_set++;
		else
			active_equipment_set = 1;
	}
	else {
		if (active_equipment_set > 1)
			active_equipment_set--;
		else
			active_equipment_set = max_equipment_set;
	}

	applyEquipment();
	return true;
}

void PlayerInventory::applyItemStats() {
	if (items->items.empty())
		return;

	// reset additional values
	for (size_t i = 0; i < eset->damage_types.list.size(); ++i) {
		owner->stats.item_base_dmg[i].min = owner->stats.item_base_dmg[i].max = 0;
	}
	owner->stats.item_base_abs.min = owner->stats.item_base_abs.max = 0;

	// apply stats from all items
	for (int i=0; i<MAX_EQUIPPED; i++) {
		if (isEquipSlotActive(i)) {
			ItemID item_id = inventory[EQUIPMENT].storage[i].item;
			if (!items->isValid(item_id))
				continue;

			Item* item = items->items[item_id];

			// apply base stats
			for (size_t j = 0; j < eset->damage_types.list.size(); ++j) {
				owner->stats.item_base_dmg[j].min += item->base_dmg[j].min.get();
				owner->stats.item_base_dmg[j].max += item->base_dmg[j].max.get();
			}

			// set equip flags
			for (unsigned j=0; j<item->equip_flags.size(); ++j) {
				owner->stats.equip_flags.insert(item->equip_flags[j]);
			}

			// apply absorb bonus
			owner->stats.item_base_abs.min += item->base_abs.min.get();
			owner->stats.item_base_abs.max += item->base_abs.max.get();

			// apply various bonuses
			unsigned bonus_counter = 0;
			while (bonus_counter < item->bonus.size()) {
				applyBonus(&item->bonus[bonus_counter]);
				bonus_counter++;
			}

			// add item powers
			if (item->power > 0) {
				owner->stats.powers_list_items.push_back(item->power);
				if (owner->stats.effects.triggered_others)
					powers->activateSinglePassive(&owner->stats, item->power);
			}
		}
	}
}

void PlayerInventory::applyItemSetBonuses(std::vector<ItemSetID> &active_sets, std::vector<int> &active_set_quantities) {
	// apply item set bonuses
	for (size_t i = 0; i < active_sets.size(); ++i) {
		if (!items->isValidSet(active_sets[i]))
			continue;

		ItemSet* item_set = items->item_sets[active_sets[i]];

		for (size_t j = 0; j < item_set->bonus.size(); ++j) {
			if (item_set->bonus[j].requirement > active_set_quantities[i])
				continue;

			applyBonus(&(item_set->bonus[j]));
		}
	}
}

void PlayerInventory::applyBonus(const BonusData* bdata) {
	EffectDef ed;

	if (bdata->type == BonusData::SPEED) {
		ed.id = "speed";
	}
	else if (bdata->type == BonusData::ATTACK_SPEED) {
		ed.id = "attack_speed";
	}
	else if (bdata->type == BonusData::STAT) {
		ed.id = Stats::KEY[bdata->index];
	}
	else if (bdata->type == BonusData::DAMAGE_MIN) {
		ed.id = eset->damage_types.list[bdata->index].min;
	}
	else if (bdata->type == BonusData::DAMAGE_MAX) {
		ed.id = eset->damage_types.list[bdata->index].max;
	}
	else if (bdata->type == BonusData::RESIST_ELEMENT) {
		ed.id = eset->damage_types.list[bdata->index].resist;
	}
	else if (bdata->type == BonusData::PRIMARY_STAT) {
		ed.id = eset->primary_stats.list[bdata->index].id;
	}
	else if (bdata->power_id > 0) {
		powerbonus->addBonusLevels(bdata->power_id, static_cast<int>(bdata->value.get()));
		return; // don't add item effect
	}
	else if (bdata->type == BonusData::RESOURCE_STAT) {
		ed.id = eset->resource_stats.list[bdata->index].ids[bdata->sub_index];
	}

	ed.type = Effect::getTypeFromString(ed.id);

	EffectParams ep;
	ep.magnitude = bdata->value.get();
	ep.is_multiplier = bdata->is_multiplier;
	ep.source_type = Power::SOURCE_TYPE_HERO;
	ep.is_from_item = true;

	owner->stats.effects.addEffect(&owner->stats, ed, ep);
}

// Moved from MenuInventory::disableEquipmentSlot() in P1.3d-4b-3. Confirmed by grep before moving:
// its only callers anywhere in the tree are the three sites inside applyEquipment() above, so this
// is a private helper, not part of the public interface. The updateEquipment(i) call did not come
// -- same dirty-flag reasoning as add()'s, see PlayerInventory.h.
void PlayerInventory::disableEquipmentSlot(size_t disable_slot_type) {
	for (int i=0; i<MAX_EQUIPPED; ++i) {
		if (isEquipSlotActive(i) && slot_type[i] == disable_slot_type) {
			if (!inventory[EQUIPMENT].storage[i].empty()) {
				add(inventory[EQUIPMENT].storage[i], CARRIED, ItemStorage::NO_SLOT, ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
				inventory[EQUIPMENT].storage[i].clear();
				applyEquipment();
			}
			setEquipSlotEnabled(i, false);
		}
	}
}

// Moved from MenuInventory::fillEquipmentSlots() in P1.3d-4b-3 unchanged -- no internal caller
// anywhere in the tree except SaveLoad, and no widget touch in its body at all.
void PlayerInventory::fillEquipmentSlots() {
	// create temporary array
	ItemStack *equip_stack = new ItemStack[MAX_EQUIPPED];

	for (int i = 0; i < MAX_EQUIPPED; ++i) {
		// initialize array
		// if an item is set, ensure the quantity is >= 1
		equip_stack[i] = inventory[EQUIPMENT].storage[i];
		if (equip_stack[i].item > 0)
			equip_stack[i].quantity = std::max(1, equip_stack[i].quantity);
		else
			equip_stack[i].clear();

		// clean up storage[]
		inventory[EQUIPMENT].storage[i].clear();

		// if items were in the correct slot, put them back
		if (!equip_stack[i].empty() && inventory[EQUIPMENT].storage[i].empty() && items->isValid(equip_stack[i].item) && items->items[equip_stack[i].item]->type == slot_type[i]) {
			inventory[EQUIPMENT].storage[i] = equip_stack[i];
			equip_stack[i].clear();
		}
	}

	// for items that weren't in a matching slot, try to find one
	// if all else fails, add them to the inventory
	for (int i = 0; i < MAX_EQUIPPED; ++i) {
		if (equip_stack[i].empty() || !items->isValid(equip_stack[i].item))
			continue;

		bool found_slot = false;
		for (int j = 0; j < MAX_EQUIPPED; ++j) {
			// search for empty slot with needed type
			if (inventory[EQUIPMENT].storage[j].empty()) {
				if (items->items[equip_stack[i].item]->type == slot_type[j]) {
					inventory[EQUIPMENT].storage[j] = equip_stack[i];
					found_slot = true;
					break;
				}
			}
		}

		// couldn't find a slot, adding to inventory
		if (!found_slot) {
			add(equip_stack[i], CARRIED, ItemStorage::NO_SLOT, !ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
		}
	}

	delete [] equip_stack;
}

// Moved from MenuInventory::applyDeathPenalty() in P1.3d-4b-3 unchanged -- its own header comment
// (still above, in the same relative place this docstring now sits) already explained why the CALL
// SITE moved to GameStatePlay's tick back in P1.3b, ahead of the code itself: "the CODE still lives
// here because it needs the inventory, and the inventory is still menu-owned... this method moves
// with the rest of the storage operations, its call site becoming owner->inventory.applyDeathPenalty()."
// That call site is inventoryFor(id)->applyDeathPenalty() now; PlayerInventory ended up as the
// owner it predicted.
void PlayerInventory::applyDeathPenalty() {
	if (owner->stats.death_penalty && eset->death_penalty.enabled) {
		std::string death_message = "";

		// remove a % of currency
		if (eset->death_penalty.currency > 0) {
			if (currency > 0)
				removeCurrency(static_cast<int>((static_cast<float>(currency) * eset->death_penalty.currency) / 100.f));
			death_message += msg->getv("Lost %s%% of %s.", Utils::floatToString(eset->death_penalty.currency, eset->number_format.death_penalty).c_str(), eset->loot.currency.c_str()) + ' ';
		}

		// remove a % of either total xp or xp since the last level
		if (eset->death_penalty.xp > 0) {
			if (owner->stats.xp > 0)
				owner->stats.xp -= static_cast<int>((static_cast<float>(owner->stats.xp) * eset->death_penalty.xp) / 100.f);
			death_message += msg->getv("Lost %s%% of total XP.", Utils::floatToString(eset->death_penalty.xp, eset->number_format.death_penalty).c_str()) + ' ';
		}
		else if (eset->death_penalty.xp_current > 0) {
			if (owner->stats.xp - eset->xp.getLevelXP(owner->stats.level) > 0)
				owner->stats.xp -= static_cast<int>((static_cast<float>(owner->stats.xp - eset->xp.getLevelXP(owner->stats.level)) * eset->death_penalty.xp_current) / 100.f);
			death_message += msg->getv("Lost %s%% of current level XP.", Utils::floatToString(eset->death_penalty.xp_current, eset->number_format.death_penalty).c_str()) + ' ';
		}

		// prevent down-leveling from removing too much xp
		if (owner->stats.xp < eset->xp.getLevelXP(owner->stats.level))
			owner->stats.xp = eset->xp.getLevelXP(owner->stats.level);

		// remove a random carried item
		if (eset->death_penalty.item) {
			std::vector<ItemID> removable_items;
			removable_items.clear();
			for (int i=0; i < MAX_EQUIPPED; i++) {
				if (!inventory[EQUIPMENT][i].empty() && items->isValid(inventory[EQUIPMENT][i].item)) {
					if (!items->items[inventory[EQUIPMENT][i].item]->quest_item)
						removable_items.push_back(inventory[EQUIPMENT][i].item);
				}
			}
			for (int i=0; i < MAX_CARRIED; i++) {
				if (!inventory[CARRIED][i].empty() && items->isValid(inventory[CARRIED][i].item)) {
					if (!items->items[inventory[CARRIED][i].item]->quest_item)
						removable_items.push_back(inventory[CARRIED][i].item);
				}
			}
			if (!removable_items.empty()) {
				size_t random_item = sim_rng->index(removable_items.size());
				remove(removable_items[random_item], 1);
				death_message += msg->getv("Lost %s.",items->getItemName(removable_items[random_item]).c_str());
			}
		}

		owner->logMsg(death_message, Avatar::MSG_NORMAL);

		owner->stats.death_penalty = false;
	}
}
