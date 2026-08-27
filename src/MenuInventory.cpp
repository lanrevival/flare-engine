/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
Copyright © 2012 Stefan Beller
Copyright © 2013-2014 Henrik Andersson
Copyright © 2013 Kurt Rinnert
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

#include "Avatar.h"
#include "CommonIncludes.h"
#include "EffectManager.h"
#include "EngineSettings.h"
#include "EventManager.h"
#include "FileParser.h"
#include "FontEngine.h"
#include "GameSlotPreview.h"
#include "Hazard.h"
#include "ItemManager.h"
#include "Menu.h"
#include "MenuActionBar.h"
#include "MenuHUDLog.h"
#include "MenuInventory.h"
#include "MenuManager.h"
#include "MenuPowers.h"
#include "MessageEngine.h"
#include "PlayerInventory.h"
#include "PowerBonusState.h"
#include "PowerManager.h"
#include "Rng.h"
#include "Settings.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "SoundManager.h"
#include "StatBlock.h"
#include "TooltipManager.h"
#include "Utils.h"
#include "UtilsParsing.h"
#include "WidgetButton.h"
#include "WidgetSlot.h"

MenuInventory::MenuInventory(Avatar* _player, PlayerInventory* _player_inventory)
	: button_close(new WidgetButton(WidgetButton::CLOSE_FILE))
	, button_sort(NULL)
	, equipmentSetPrevious(NULL)
	, equipmentSetNext(NULL)
	, equipmentSetLabel(NULL)
	, carried_cols(4)
	, carried_rows(4)
	, tap_to_activate_timer(Settings::SIM_TICK_HZ / 3)
	, activated_slot(-1)
	, activated_item(0)
	, preview(NULL)
	, preview_enabled(false)
	, sort_enabled(false)
	, drag_prev_src(-1)
	, changed_equipment(true)
	, inv_ctrl(CTRL_NONE)
	, show_book("")
	, player(_player)
	, player_inventory(_player_inventory)
{
	visible = false;

	// raw data for equipment swap buttons
	std::map<unsigned, std::string> raw_set_button;
	std::string raw_previous;
	std::string raw_next;
	std::string raw_label;

	// Which item type each equipment slot takes, and which equipment set it belongs to. Parsed
	// here because that is where menus/inventory.txt is read, but handed straight to
	// PlayerInventory below -- they are properties of a character, not of a window. See D1 in
	// plans/phase1/P1.3-VERIFICATION.md for why the parse has not moved with them.
	std::vector<size_t> parsed_slot_type;
	std::vector<unsigned int> parsed_equipment_set;

	// Load config settings
	FileParser infile;
	// @CLASS MenuInventory|Description of menus/inventory.txt
	if (infile.open("menus/inventory.txt", FileParser::MOD_FILE, FileParser::ERROR_NORMAL)) {
		while(infile.next()) {
			if (parseMenuKey(infile.key, infile.val))
				continue;

			// @ATTR close|point|Position of the close button.
			if(infile.key == "close") {
				Point pos = Parse::toPoint(infile.val);
				button_close->setBasePos(pos.x, pos.y, Utils::ALIGN_TOPLEFT);
			}
			// @ATTR set_button|int, int, int, filename : ID, Widget X, Widget Y, Image file|Set number, position and image filename for an equipment swap set button.
			else if(infile.key == "set_button") {
				unsigned id = static_cast<unsigned>(Parse::popFirstInt(infile.val));
				std::pair<unsigned, std::string> set_button(id, infile.val);
				raw_set_button.insert(set_button);
				raw_set_button[id] = infile.val;
			}
			// @ATTR set_previous|int, int, filename : Widget X, Widget Y, Image file|Position and image filename for an equipment swap set previous button.
			else if(infile.key == "set_previous") {
				raw_previous = infile.val;
			}
			// @ATTR set_next|int, int, filename : Widget X, Widget Y, Image file|Position and image filename for an equipment swap set next button.
			else if(infile.key == "set_next") {
				raw_next = infile.val;
			}
			// @ATTR label_equipment_set|label|Label showing the active equipment set.
			else if(infile.key == "label_equipment_set") {
				raw_label = infile.val;
			}
			// @ATTR equipment_slot|repeatable(int, int, string, int) : X, Y, Slot Type, Equipment set|Position, item type and equipment set number of an equipment slot. Equipment set number is "0" for shared items."
			else if(infile.key == "equipment_slot") {
				Rect area;
				Point pos;
				size_t slt_type;
				int eq_set;

				pos.x = area.x = Parse::popFirstInt(infile.val);
				pos.y = area.y = Parse::popFirstInt(infile.val);
				slt_type = items->getItemTypeIndexByString(Parse::popFirstString(infile.val));
				eq_set = Parse::popFirstInt(infile.val);
				area.w = area.h = eset->resolutions.icon_size;

				equipped_area.push_back(area);
				equipped_pos.push_back(pos);
				parsed_slot_type.push_back(slt_type);
				parsed_equipment_set.push_back(eq_set);
			}
			// @ATTR carried_area|point|Position of the first normal inventory slot.
			else if(infile.key == "carried_area") {
				Point pos;
				carried_pos.x = carried_area.x = Parse::popFirstInt(infile.val);
				carried_pos.y = carried_area.y = Parse::popFirstInt(infile.val);
			}
			// @ATTR carried_cols|int|The number of columns for the normal inventory.
			else if (infile.key == "carried_cols") carried_cols = std::max(1, Parse::toInt(infile.val));
			// @ATTR carried_rows|int|The number of rows for the normal inventory.
			else if (infile.key == "carried_rows") carried_rows = std::max(1, Parse::toInt(infile.val));
			// @ATTR label_title|label|Position of the "Inventory" label.
			else if (infile.key == "label_title") {
				label_inventory.setFromLabelInfo(Parse::popLabelInfo(infile.val));
			}
			// @ATTR currency|label|Position of the label that displays the total currency being carried.
			else if (infile.key == "currency") {
				label_currency.setFromLabelInfo(Parse::popLabelInfo(infile.val));
			}
			// @ATTR help|rectangle|A mouse-over area that displays some help text for inventory shortcuts.
			else if (infile.key == "help") help_pos = Parse::toRect(infile.val);

			// @ATTR preview_enabled|bool|When enabled, the player is drawn in the inventory menu as they appear in the game world. Disabled by default.
			else if (infile.key == "preview_enabled") preview_enabled = Parse::toBool(infile.val);
			// @ATTR preview_pos|point|Position of the preview image. The character is drawn so that this point should lie between their feet, or thereabouts.
			else if (infile.key == "preview_pos") preview_pos = Parse::toPoint(infile.val);

			// @ATTR sort_enabled|bool|Enables the button for sorting carried items. Defaults to false. The button image is expected to be located at 'images/menus/buttons/button_sort.png'.
			else if (infile.key == "sort_enabled") sort_enabled = Parse::toBool(infile.val);
			// @ATTR sort_pos|point|Position of the sort button.
			else if (infile.key == "sort_pos") sort_pos = Parse::toPoint(infile.val);

			else infile.error("MenuInventory: '%s' is not a valid key.", infile.key.c_str());
		}
		infile.close();
	}

	carried_area.w = carried_cols * eset->resolutions.icon_size;
	carried_area.h = carried_rows * eset->resolutions.icon_size;

	label_inventory.setText(msg->get("Inventory"));
	label_inventory.setColor(font->getColor(FontEngine::COLOR_MENU_NORMAL));

	label_currency.setColor(font->getColor(FontEngine::COLOR_MENU_NORMAL));

	// Hand the parsed shape to the character's inventory -- it sizes its own two ItemStorages --
	// then bind this menu's widget-bearing views to point AT that data (P1.3d-4c). `inventory` is
	// NOT a copy: MenuItemStorage::bind() makes each element read and write the same storage
	// player_inventory->inventory[...] owns. Everything below in this file, and every menu->inv-> caller
	// outside it, is reading and writing the same two storages the simulation owns; only the
	// widget slots themselves (created by initFromList()/initGrid() below) belong to this menu.
	//
	// P1.3d-4d: player_inventory->loadEquipmentData() tries engine/equipment.txt first -- if the mod chain has
	// one, it is authoritative for slot count/type/set and carrying capacity, and the
	// equipment_slot=/carried_cols/carried_rows values just parsed above supply screen position
	// ONLY, matched to it by file order (see PlayerInventory::loadEquipmentData()). If it doesn't
	// exist, player_inventory->init() below falls back to exactly today's behaviour, unchanged.
	bool has_equipment_data = player_inventory->loadEquipmentData();
	if (has_equipment_data) {
		if (equipped_area.size() != player_inventory->slot_type.size()) {
			Utils::logError("MenuInventory: menus/inventory.txt has %d equipment_slot lines, but engine/equipment.txt defines %d slots. Screen positions were matched by line order up to the shorter of the two; extra slots on either side have no counterpart.",
							 static_cast<int>(equipped_area.size()), static_cast<int>(player_inventory->slot_type.size()));
		}
		for (size_t i = 0; i < equipped_area.size() && i < player_inventory->slot_type.size(); ++i) {
			if (parsed_slot_type[i] != player_inventory->slot_type[i] || static_cast<unsigned int>(parsed_equipment_set[i]) != player_inventory->equipment_set[i]) {
				Utils::logError("MenuInventory: equipment_slot line %d in menus/inventory.txt doesn't match engine/equipment.txt's equip_slot line %d. The two files must list equipment slots in the same order.",
								 static_cast<int>(i + 1), static_cast<int>(i + 1));
			}
		}
		equipped_area.resize(player_inventory->MAX_EQUIPPED);
		equipped_pos.resize(player_inventory->MAX_EQUIPPED);

		if (player_inventory->MAX_CARRIED != carried_cols * carried_rows) {
			Utils::logError("MenuInventory: engine/equipment.txt declares %d carried_slots, but menus/inventory.txt's carried_cols x carried_rows is %d. The character's real carrying capacity is the former; the grid drawn on screen may not match it.",
							 player_inventory->MAX_CARRIED, carried_cols * carried_rows);
		}
	}
	else {
		player_inventory->init(equipped_area, parsed_slot_type, parsed_equipment_set, carried_cols, carried_rows);
	}
	inventory[EQUIPMENT].bind(&player_inventory->inventory[EQUIPMENT]);
	inventory[CARRIED].bind(&player_inventory->inventory[CARRIED]);
	inventory[EQUIPMENT].initFromList(player_inventory->MAX_EQUIPPED, equipped_area, parsed_slot_type);
	inventory[CARRIED].initGrid(player_inventory->MAX_CARRIED, carried_area, carried_cols);

	for (int i = 0; i < player_inventory->MAX_EQUIPPED; i++) {
		tablist.add(inventory[EQUIPMENT].slots[i]);
	}
	for (int i = 0; i < player_inventory->MAX_CARRIED; i++) {
		tablist.add(inventory[CARRIED].slots[i]);
	}

	// create equipment swap buttons
	std::map<unsigned, std::string>::iterator it;
	for (it = raw_set_button.begin(); it != raw_set_button.end(); ++it) {
		int px = Parse::popFirstInt(it->second);
		int py = Parse::popFirstInt(it->second);
		std::string icon = Parse::popFirstString(it->second);
		equipmentSetButton.push_back(new WidgetButton(icon));
		equipmentSetButton.back()->setBasePos(px, py, Utils::ALIGN_TOPLEFT);
		tablist.add(equipmentSetButton.back());
	}
	raw_set_button.clear();

	if (!raw_previous.empty()) {
		int px = Parse::popFirstInt(raw_previous);
		int py = Parse::popFirstInt(raw_previous);
		std::string icon = Parse::popFirstString(raw_previous);
		equipmentSetPrevious = new WidgetButton(icon);
		equipmentSetPrevious->setBasePos(px, py, Utils::ALIGN_TOPLEFT);
		tablist.add(equipmentSetPrevious);
	}

	if (!raw_next.empty()) {
		int px = Parse::popFirstInt(raw_next);
		int py = Parse::popFirstInt(raw_next);
		std::string icon = Parse::popFirstString(raw_next);
		equipmentSetNext = new WidgetButton(icon);
		equipmentSetNext->setBasePos(px, py, Utils::ALIGN_TOPLEFT);
		tablist.add(equipmentSetNext);
	}

	if (!raw_label.empty()) {
		equipmentSetLabel = new WidgetLabel;
		equipmentSetLabel->setFromLabelInfo(Parse::popLabelInfo(raw_label));

		std::stringstream label;
		label << player_inventory->active_equipment_set << "/" << player_inventory->max_equipment_set;
		equipmentSetLabel->setText(label.str());
		equipmentSetLabel->setColor(font->getColor(FontEngine::COLOR_MENU_NORMAL));
	}

	if (player_inventory->max_equipment_set > 0) {
		applyEquipmentSet(1);
	}

	if (!background)
		setBackground("images/menus/inventory.png");

	if (preview_enabled) {
		preview = new GameSlotPreview();
		preview->setStatBlock(&player->stats);
		preview->setDirection(6); // face forward
		preview->loadDefaultGraphics();
		preview->loadGraphicsFromInventory(this);
	}

	if (sort_enabled) {
		button_sort = new WidgetButton(WidgetButton::SORT_ITEMS_FILE);

		if (button_sort) {
			button_sort->setBasePos(sort_pos.x, sort_pos.y, Utils::ALIGN_TOPLEFT);
			tablist.add(button_sort);

			inventory[CARRIED].data->sort_tooltip = &button_sort->tooltip;
			inventory[CARRIED].refreshSortTooltip();
		}
	}

	align();
}

void MenuInventory::align() {
	Menu::align();

	for (int i=0; i<player_inventory->MAX_EQUIPPED; i++) {
		equipped_area[i].x = equipped_pos[i].x + window_area.x;
		equipped_area[i].y = equipped_pos[i].y + window_area.y;
	}

	carried_area.x = carried_pos.x + window_area.x;
	carried_area.y = carried_pos.y + window_area.y;

	inventory[EQUIPMENT].setPos(window_area.x, window_area.y);
	inventory[CARRIED].setPos(window_area.x, window_area.y);

	button_close->setPos(window_area.x, window_area.y);

	if (!equipmentSetButton.empty()) {
		for (size_t i=0; i<equipmentSetButton.size(); i++) {
			equipmentSetButton[i]->setPos(window_area.x, window_area.y);
		}
	}

	if (equipmentSetPrevious) equipmentSetPrevious->setPos(window_area.x, window_area.y);
	if (equipmentSetNext) equipmentSetNext->setPos(window_area.x, window_area.y);
	if (equipmentSetLabel) equipmentSetLabel->setPos(window_area.x, window_area.y);

	label_inventory.setPos(window_area.x, window_area.y);
	label_currency.setPos(window_area.x, window_area.y);

	if (preview)
		preview->setPos(Point(window_area.x + preview_pos.x, window_area.y + preview_pos.y));

	if (button_sort)
		button_sort->setPos(window_area.x, window_area.y);
}

// applyDeathPenalty() moved to PlayerInventory in P1.3d-4b-3 -- see its header comment there for
// why the call site moved to GameStatePlay's tick first, back in P1.3b, ahead of the code itself.

void MenuInventory::logic() {

	// a copy of currency is kept in stats, to help with various situations
	//
	// Also simulation state written by a menu, and also still here for the reason above: the
	// count comes from inventory[CARRIED], which nothing outside this class owns yet. P1.3d.
	player->stats.currency = player_inventory->currency = inventory[CARRIED].count(eset->misc.currency_id);

	if (visible) {
		tablist.logic();

		// check close button
		if (button_close->checkClick()) {
			visible = false;
			snd->play(sfx_close, snd->DEFAULT_CHANNEL, snd->NO_POS, !snd->LOOP);
		}

		if (drag_prev_src == -1) {
			clearHighlight();
		}

		//check equipment set buttons
		if (!equipmentSetButton.empty()) {
			for (size_t i=0; i<equipmentSetButton.size(); i++) {
				if(equipmentSetButton[i]->checkClick()) {
					applyEquipmentSet(static_cast<unsigned>(i)+1);
					applyEquipment();
				}
			}
		}

		if (equipmentSetNext) {
			if (equipmentSetNext->checkClick()) {
				applyNextEquipmentSet();
				applyEquipment();
			}
		}

		if (equipmentSetPrevious) {
			if (equipmentSetPrevious->checkClick()) {
				applyPreviousEquipmentSet();
				applyEquipment();
			}
		}

		if (button_sort && button_sort->checkClick()) {
			inventory[CARRIED].sortNext();
		}
	}

	// The equipment-set keys used to be read here, out of inpt->pressing, and used here to change
	// which half of this character's gear is being worn. GameStatePlay drives that now, from
	// PlayerCommand::equip_set_delta; see applyEquipmentSetDelta().

	tap_to_activate_timer.tick();

	if (preview)
		preview->logic();
}

void MenuInventory::render() {
	if (!visible) return;

	// background
	Menu::render();

	// close button
	button_close->render();

	// equipment set buttons
	if (!equipmentSetButton.empty()) {
		for (size_t i=0; i<equipmentSetButton.size(); i++) {
			equipmentSetButton[i]->render();
		}
	}
	if (equipmentSetPrevious) equipmentSetPrevious->render();
	if (equipmentSetNext) equipmentSetNext->render();
	if (equipmentSetLabel) equipmentSetLabel->render();

	// text overlay
	label_inventory.render();

	if (!label_currency.isHidden()) {
		label_currency.setText(msg->getv("%d %s", player_inventory->currency, eset->loot.currency.c_str()));
		label_currency.render();
	}

	inventory[EQUIPMENT].render();
	inventory[CARRIED].render();

	if (preview)
		preview->render();

	if (button_sort)
		button_sort->render();
}

int MenuInventory::areaOver(const Point& position) {
	if (Utils::isWithinRect(carried_area, position)) {
		return CARRIED;
	}
	else {
		for (unsigned i=0; i<equipped_area.size(); i++) {
			if (Utils::isWithinRect(equipped_area[i], position)) {
				return EQUIPMENT;
			}
		}
	}

	// point is inside the inventory menu, but not over a slot
	if (Utils::isWithinRect(window_area, position)) {
		return NO_AREA;
	}

	return -2;
}

/**
 * If mousing-over an item with a tooltip, return that tooltip data.
 *
 * @param mouse The x,y screen coordinates of the mouse cursor
 */
void MenuInventory::renderTooltips(const Point& position) {
	if (!visible || !Utils::isWithinRect(window_area, position))
		return;

	int area = areaOver(position);
	int slot = -1;
	TooltipData tip_data;

	if (area < 0) {
		if (position.x >= window_area.x + help_pos.x && position.y >= window_area.y+help_pos.y && position.x < window_area.x+help_pos.x+help_pos.w && position.y < window_area.y+help_pos.y+help_pos.h) {
			tip_data.addText(msg->get("Pick up item(s):") + " " + inpt->getBindingString(Input::MAIN1));
			tip_data.addText(msg->get("Use or equip item:") + " " + inpt->getBindingString(Input::MAIN2) + "\n");
			tip_data.addText(msg->getv("%s modifiers", inpt->getBindingString(Input::MAIN1).c_str()));
			tip_data.addText(msg->get("Select a quantity of item:") + " " + inpt->getBindingString(Input::SHIFT));

			if (inv_ctrl == CTRL_STASH)
				tip_data.addText(msg->get("Stash item stack:") + " " + inpt->getBindingString(Input::CTRL));
			else if (inv_ctrl == CTRL_VENDOR || eset->misc.sell_without_vendor)
				tip_data.addText(msg->get("Sell item stack:") + " " + inpt->getBindingString(Input::CTRL));
		}
		tooltipm->push(tip_data, position, TooltipData::STYLE_FLOAT);
	}
	else {
		slot = inventory[area].slotOver(position);
	}

	if (slot == -1)
		return;

	tip_data.clear();

	if (area == EQUIPMENT)
		if (!player_inventory->isEquipSlotActive(slot))
			return;

	if (inventory[area][slot].item > 0) {
		tip_data = inventory[area].checkTooltip(position, &player->stats, ItemManager::PLAYER_INV, ItemManager::TOOLTIP_INPUT_HINT);
	}
	else if (area == EQUIPMENT && inventory[area][slot].empty()) {
		tip_data.addText(msg->get(items->getItemType(player_inventory->slot_type[slot]).name));
	}

	tooltipm->push(tip_data, position, TooltipData::STYLE_FLOAT);
}

/**
 * Click-start dragging in the inventory
 */
ItemStack MenuInventory::click(const Point& position) {
	ItemStack item;

	drag_prev_src = areaOver(position);
	if (drag_prev_src > -1) {
		item = inventory[drag_prev_src].click(position);

		if (inpt->usingTouchscreen()) {
			tablist.setCurrent(inventory[drag_prev_src].current_slot);
			tap_to_activate_timer.reset(Timer::BEGIN);
		}

		if (item.empty()) {
			drag_prev_src = -1;
			return item;
		}

		// if dragging equipment, prepare to change stats/sprites
		if (drag_prev_src == EQUIPMENT) {
			if (player->stats.humanoid) {
				updateEquipment(inventory[EQUIPMENT].drag_prev_slot);
			}
			else {
				itemReturn(item);
				item.clear();
			}
		}
	}

	return item;
}

/**
 * Return dragged item to previous slot
 */
void MenuInventory::itemReturn(ItemStack stack) {
	if (drag_prev_src == -1) {
		add(stack, CARRIED, ItemStorage::NO_SLOT, !ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
	}
	else {
		int prev_slot = inventory[drag_prev_src].drag_prev_slot;
		inventory[drag_prev_src].itemReturn(stack);
		// if returning equipment, prepare to change stats/sprites
		if (drag_prev_src == EQUIPMENT) {
			updateEquipment(prev_slot);
		}
	}
	drag_prev_src = -1;
}

/**
 * Dragging and dropping an item can be used to rearrange the inventory
 * and equip items
 */
bool MenuInventory::drop(const Point& position, ItemStack stack) {
	items->playSound(stack.item);

	bool success = true;

	int area = areaOver(position);
	if (area < 0) {
		if (drag_prev_src == -1) {
			success = add(stack, CARRIED, ItemStorage::NO_SLOT, !ADD_PLAY_SOUND, ADD_AUTO_EQUIP);
		}
		else {
			// not dropped into a slot. Just return it to the previous slot.
			itemReturn(stack);
		}
		return success;
	}

	int slot = inventory[area].slotOver(position);
	if (slot == -1) {
		if (drag_prev_src == -1) {
			success = add(stack, CARRIED, ItemStorage::NO_SLOT, !ADD_PLAY_SOUND, ADD_AUTO_EQUIP);
		}
		else {
			// not dropped into a slot. Just return it to the previous slot.
			itemReturn(stack);
		}
		return success;
	}

	int drag_prev_slot = -1;
	if (drag_prev_src != -1)
		drag_prev_slot = inventory[drag_prev_src].drag_prev_slot;

	if (area == EQUIPMENT) { // dropped onto equipped item

		// make sure the item is going to the correct slot
		// we match slot_type to stack.item's type to place items in the proper slots
		// also check to see if the hero meets the requirements
		if (items->isValid(stack.item) && player_inventory->slot_type[slot] == items->items[stack.item]->type && items->requirementsMet(&player->stats, stack.item) && player->stats.humanoid && player_inventory->isEquipSlotEnabled(slot)) {
			if (inventory[area][slot].item == stack.item) {
				// Merge the stacks
				success = add(stack, area, slot, !ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
			}
			else {
				// Swap the two stacks
				if (!inventory[area][slot].empty())
					itemReturn(inventory[area][slot]);
				inventory[area][slot] = stack;
				updateEquipment(slot);
				applyEquipment();

				// if this item has a power, place it on the action bar if possible
				if (items->items[stack.item]->power > 0) {
					menu_act->addPower(items->items[stack.item]->power, 0);
				}
			}
		}
		else {
			// equippable items only belong to one slot, for the moment
			itemReturn(stack); // cancel
			updateEquipment(slot);
			applyEquipment();
		}
	}
	else if (area == CARRIED) {
		// dropped onto carried item

		if (drag_prev_src == CARRIED) {
			if (slot != drag_prev_slot) {
				if (inventory[area][slot].item == stack.item) {
					// Merge the stacks
					success = add(stack, area, slot, !ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
				}
				else if (inventory[area][slot].empty()) {
					// Drop the stack
					inventory[area][slot] = stack;
				}
				else if (drag_prev_slot != -1 && inventory[drag_prev_src][drag_prev_slot].empty()) {
					// Check if the previous slot is free (could still be used if SHIFT was used).
					// Swap the two stacks
					itemReturn( inventory[area][slot]);
					inventory[area][slot] = stack;
				}
				else {
					itemReturn( stack);
				}
			}
			else {
				itemReturn( stack); // cancel

				// allow reading books on touchscreen devices
				// since touch screens don't have right-click, we use a "tap" (drop on same slot quickly) to activate
				// NOTE: the quantity must be 1, since the number picker appears when tapping on a stack of more than 1 item
				// NOTE: we only support activating books since equipment activation doesn't work for some reason
				// NOTE: Consumables are usually in stacks > 1, so we ignore those as well for consistency
				if (inpt->usingTouchscreen() && !tap_to_activate_timer.isEnd() && stack.quantity == 1 && items->isValid(stack.item) && !items->items[stack.item]->book.empty()) {
					activate(position);
				}
			}
		}
		else {
			if (inventory[area][slot].item == stack.item || drag_prev_src == -1) {
				// Merge the stacks
				success = add(stack, area, slot, !ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
			}
			else if (inventory[area][slot].empty()) {
				// Drop the stack
				inventory[area][slot] = stack;
			}
			else if (
				inventory[EQUIPMENT][drag_prev_slot].empty()
				&& inventory[CARRIED][slot].item != stack.item
				&& items->isValid(inventory[CARRIED][slot].item)
				&& items->items[inventory[CARRIED][slot].item]->type == player_inventory->slot_type[drag_prev_slot]
				&& items->requirementsMet(&player->stats, inventory[CARRIED][slot].item)
			)
			{
				// The whole equipped stack is dropped on an empty carried slot or on a wearable item
				// Swap the two stacks
				itemReturn(inventory[area][slot]);
				updateEquipment(drag_prev_slot);

				// if this item has a power, place it on the action bar if possible
				if (items->items[inventory[EQUIPMENT][drag_prev_slot].item]->power > 0) {
					menu_act->addPower(items->items[inventory[EQUIPMENT][drag_prev_slot].item]->power, 0);
				}

				inventory[area][slot] = stack;

				applyEquipment();
			}
			else {
				itemReturn(stack); // cancel
			}
		}
	}

	drag_prev_src = -1;

	return success;
}

/**
 * Right-clicking on a usable item in the inventory causes it to activate.
 * e.g. drink a potion
 * e.g. equip an item
 */
void MenuInventory::activate(const Point& position) {
	FPoint nullpt;
	nullpt.x = nullpt.y = 0;

	// clicked a carried item
	int slot = inventory[CARRIED].slotOver(position);
	if (slot == -1)
		return;

	ItemStack& stack = inventory[CARRIED][slot];

	// empty slot
	if (stack.empty() || !items->isValid(stack.item))
		return;

	// run the item's script if it has one
	if (!items->items[stack.item]->script.empty()) {
		eventm->executeScript(items->items[stack.item]->script, player->stats.pos.x, player->stats.pos.y);
	}
	// if the item is a book, open it
	else if (!items->items[stack.item]->book.empty()) {
		show_book = items->items[stack.item]->book;
	}
	// use a power attached to a non-equipment item
	else if (powers->isValid(items->items[stack.item]->power) && player_inventory->getEquipSlotFromItem(stack.item, !PlayerInventory::ONLY_EMPTY_SLOTS) == -1) {
		PowerID power_id = items->items[stack.item]->power;
		Power* item_power = powers->powers[power_id];

		// equipment might want to replace powers, so do it here
		for (int i = 0; i < inventory[EQUIPMENT].getSlotNumber(); ++i) {
			ItemID id = inventory[EQUIPMENT][i].item;
			if (id == 0 || !items->items[id])
				continue;

			for (size_t j = 0; j < items->items[id]->replace_power.size(); ++j) {
				if (power_id == items->items[id]->replace_power[j].first) {
					power_id = items->items[id]->replace_power[j].second;
					break;
				}
			}
		}

		// if the power consumes items, make sure we have enough
		for (size_t i = 0; i < item_power->required_items.size(); ++i) {
			if (item_power->required_items[i].id > 0 &&
			    item_power->required_items[i].quantity > inventory[CARRIED].count(item_power->required_items[i].id))
			{
				player->logMsg(msg->get("You don't have enough of the required item."), Avatar::MSG_NORMAL);
				return;
			}

			if (item_power->required_items[i].id == stack.item) {
				activated_slot = slot;
				activated_item = stack.item;
			}
		}

		// check power & item requirements
		if (!player->stats.canUsePower(power_id, !StatBlock::CAN_USE_PASSIVE) || !player->power_cooldown_timers[power_id]->isEnd()) {
			player->logMsg(msg->get("You can't use this item right now."), Avatar::MSG_NORMAL);
			return;
		}

		// if this item requires targeting it can't be used this way
		if (!item_power->requires_targeting) {
			ActionData action_data;
			action_data.power = power_id;
			action_data.activated_from_inventory = true;

			action_data.target = Utils::calcVector(player->stats.pos, player->stats.direction, player->stats.melee_range);

			if (item_power->new_state == Power::STATE_INSTANT) {
				for (size_t j = 0; j < item_power->required_items.size(); ++j) {
					if (item_power->required_items[j].id > 0 && !item_power->required_items[j].equipped) {
						action_data.instant_item = true;
						break;
					}
				}
			}

			player->action_queue.push_back(action_data);
		}
		else {
			// let player know this can only be used from the action bar
			player->logMsg(msg->get("This item can only be used from the action bar."), Avatar::MSG_NORMAL);
		}

	}
	// equip an item
	else if (player->stats.humanoid && !items->getItemType(items->items[stack.item]->type).name.empty()) {
		int equip_slot = player_inventory->getEquipSlotFromItem(inventory[CARRIED].data->storage[slot].item, !PlayerInventory::ONLY_EMPTY_SLOTS);

		if (equip_slot >= 0) {
			ItemStack active_stack = click(position);

			if (inventory[EQUIPMENT][equip_slot].item == active_stack.item) {
				// Merge the stacks
				add(active_stack, EQUIPMENT, equip_slot, !ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
			}
			else if (inventory[EQUIPMENT][equip_slot].empty()) {
				// Drop the stack
				inventory[EQUIPMENT][equip_slot] = active_stack;
			}
			else {
				if (stack.empty()) { // Don't forget this slot may have been emptied by the click()
					// Swap the two stacks
					itemReturn(inventory[EQUIPMENT][equip_slot]);
				}
				else {
					// Drop the equipped item anywhere
					add(inventory[EQUIPMENT][equip_slot], CARRIED, ItemStorage::NO_SLOT, ADD_PLAY_SOUND, !ADD_AUTO_EQUIP);
				}
				inventory[EQUIPMENT][equip_slot] = active_stack;
			}

			updateEquipment(equip_slot);
			items->playSound(inventory[EQUIPMENT][equip_slot].item);

			// if this item has a power, place it on the action bar if possible
			if (items->items[active_stack.item]->power > 0) {
				menu_act->addPower(items->items[active_stack.item]->power, 0);
			}

			applyEquipment();
		}
		else if (equip_slot == -1) {
			Utils::logError("MenuInventory: Can't find equip slot, corresponding to type %s", items->getItemType(items->items[stack.item]->type).id.c_str());
		}
	}

	drag_prev_src = -1;
}

/**
 * Insert item into first available carried slot, preferably in the optionnal specified slot
 *
 * @param ItemStack Stack of items
 * @param area Area number where it will try to store the item
 * @param slot Slot number where it will try to store the item
 */
// Thin wrapper since P1.3d-4b-3 -- the real logic is PlayerInventory::add() now. This preserves the
// two things that were genuinely MenuInventory's: the equipment-slot redraw flag (only relevant
// when area == EQUIPMENT, matching the one place the original body called it) and ending whatever
// mouse drag was in progress. See PlayerInventory.h for the full accounting.
bool MenuInventory::add(ItemStack stack, int area, int slot, bool play_sound, bool auto_equip) {
	bool success = player_inventory->add(stack, area, slot, play_sound, auto_equip);
	if (area == EQUIPMENT)
		updateEquipment(slot);
	drag_prev_src = -1;
	return success;
}

/**
 * Remove one given item from the player's inventory.
 */
// Thin wrapper since P1.3d-4b-3, keeping only the activated_item special case that was genuinely
// MenuInventory's -- see PlayerInventory.h. Everything else falls through to player_inventory->remove().
bool MenuInventory::remove(ItemID item, int quantity) {
	if (activated_item != 0 && activated_slot != -1 && item == activated_item) {
		inventory[CARRIED].subtract(activated_slot, 1);
		activated_item = 0;
		activated_slot = -1;
		return true;
	}

	return player_inventory->remove(item, quantity);
}

void MenuInventory::removeFromPrevSlot(int quantity) {
	if (drag_prev_src > -1 && inventory[drag_prev_src].drag_prev_slot > -1) {
		int drag_prev_slot = inventory[drag_prev_src].drag_prev_slot;
		inventory[drag_prev_src].subtract(drag_prev_slot, quantity);
		if (inventory[drag_prev_src].data->storage[drag_prev_slot].empty()) {
			if (drag_prev_src == EQUIPMENT)
				updateEquipment(inventory[EQUIPMENT].drag_prev_slot);
		}
	}
}

// addCurrency() moved to PlayerInventory in P1.3d-4b-3 -- see PlayerInventory.h.

/**
 * Check if there is enough currency to buy the given stack, and if so remove it from the current total and add the stack.
 * (Handle the drop into the equipment area, but add() don't handle it well in all circonstances. MenuManager::logic() allow only into the carried area.)
 */
bool MenuInventory::buy(ItemStack stack, int tab, bool dragging) {
	if (stack.empty()) {
		return true;
	}

	if (!items->isValid(stack.item))
		return false;

	Item* item = items->items[stack.item];

	bool can_afford = false;
	int count = 0;

	if (tab == ItemManager::VENDOR_CRAFT) {
		count = item->getCraftCount();
		can_afford = (count > 0);
	}
	else {
		int value_each = 0;
		if (tab == ItemManager::VENDOR_BUY)
			value_each = item->getPrice(ItemManager::USE_VENDOR_RATIO);
		else if (tab == ItemManager::VENDOR_SELL)
			value_each = item->getSellPrice(stack.can_buyback);

		count = value_each * stack.quantity;
		can_afford = (inventory[CARRIED].count(eset->misc.currency_id) >= count);
	}

	if (can_afford) {
		stack.can_buyback = false;

		if (dragging) {
			drop(inpt->mouse, stack);
		}
		else {
			add(stack, CARRIED, ItemStorage::NO_SLOT, ADD_PLAY_SOUND, ADD_AUTO_EQUIP);
		}

		if (tab == ItemManager::VENDOR_CRAFT) {
			for (size_t i = 0; i < item->crafting_items.size(); ++i) {
				remove(item->crafting_items[i].item, item->crafting_items[i].quantity * stack.quantity);
			}
		}
		else {
			player_inventory->removeCurrency(count);
			items->playSound(eset->misc.currency_id);
		}

		return true;
	}
	else {
		if (tab == ItemManager::VENDOR_CRAFT)
			player->logMsg(msg->get("You do not have the required items to craft."), Avatar::MSG_NORMAL);
		else
			player->logMsg(msg->getv("Not enough %s.", eset->loot.currency.c_str()), Avatar::MSG_NORMAL);

		drop_stack.push(stack);
		return false;
	}
}

/**
 * Sell a specific stack of items
 */
bool MenuInventory::sell(ItemStack stack) {
	if (stack.empty() || !items->isValid(stack.item)) {
		return false;
	}

	// can't sell currency
	if (stack.item == eset->misc.currency_id) return false;

	// items that have no price cannot be sold
	if (items->items[stack.item]->getPrice(ItemManager::USE_VENDOR_RATIO) == 0) {
		items->playSound(stack.item);
		player->logMsg(msg->get("This item can not be sold."), Avatar::MSG_NORMAL);
		return false;
	}

	// quest items can not be sold
	if (items->items[stack.item]->quest_item) {
		items->playSound(stack.item);
		player->logMsg(msg->get("This item can not be sold."), Avatar::MSG_NORMAL);
		return false;
	}

	int value_each = items->items[stack.item]->getSellPrice(ItemManager::DEFAULT_SELL_PRICE);
	int value = value_each * stack.quantity;
	player_inventory->addCurrency(value);
	items->playSound(eset->misc.currency_id);
	drag_prev_src = -1;
	return true;
}

void MenuInventory::updateEquipment(int slot) {

	if (slot == -1) {
		// This should never happen, but ignore it if it does
		return;
	}
	else {
		changed_equipment = true;
	}
}

/**
 * Given the equipped items, calculate the hero's stats
 */
// Thin wrapper since P1.3d-4b-3 -- the real logic is PlayerInventory::applyEquipment() now. This
// preserves two things that were genuinely MenuInventory's: pulling the per-slot enabled state
// PlayerInventory::applyEquipment() just recomputed into the equipment widgets (P1.3d-4c --
// PlayerInventory::setEquipSlotEnabled() used to push this directly, see PlayerInventory.h), and
// refreshing the new-game/continue screen's character preview, if one exists.
void MenuInventory::applyEquipment() {
	player_inventory->applyEquipment();

	for (int i = 0; i < player_inventory->MAX_EQUIPPED; i++) {
		inventory[EQUIPMENT].slots[i]->enabled = player_inventory->isEquipSlotEnabled(i);
	}

	if (preview)
		preview->loadGraphicsFromInventory(this);
}

// applyItemStats()/applyItemSetBonuses()/applyBonus() moved to PlayerInventory in P1.3d-4b-3,
// unchanged -- see PlayerInventory.h. They were only ever called from applyEquipment() above.


void MenuInventory::applyEquipmentSet(unsigned set) {
	unsigned prev_equipment_set = player_inventory->active_equipment_set;

	if (set > 0 && set <= player_inventory->max_equipment_set) {
		player_inventory->active_equipment_set = set;
		updateEquipmentSetWidgets();
	}

	if (player_inventory->active_equipment_set > 0 && prev_equipment_set != player_inventory->active_equipment_set) {
		if (!visible && menu && menu->hudlog) {
			menu->hudlog->add(msg->getv("Equipped set %d.", static_cast<int>(player_inventory->active_equipment_set)), MenuHUDLog::MSG_NORMAL);
		}
	}
}

void MenuInventory::applyNextEquipmentSet() {
	if (player_inventory->active_equipment_set < player_inventory->max_equipment_set) {
		applyEquipmentSet(player_inventory->active_equipment_set+1);
	}
	else {
		applyEquipmentSet(1);
	}
}

void MenuInventory::applyPreviousEquipmentSet() {
	if (player_inventory->active_equipment_set > 1) {
		applyEquipmentSet(player_inventory->active_equipment_set-1);
	}
	else {
		applyEquipmentSet(player_inventory->max_equipment_set);
	}
}

void MenuInventory::updateEquipmentSetWidgets() {
	Widget* first_active_slot = NULL;
	Widget* current_tablist_widget = tablist.getWidgetByIndex(tablist.getCurrent());
	bool reset_tablist_cursor = false;

	for (int i=0; i<player_inventory->MAX_EQUIPPED; i++) {
		if (player_inventory->isEquipSlotActive(i)) {
			if (!first_active_slot) {
				first_active_slot = inventory[EQUIPMENT].slots[i];
			}
			inventory[EQUIPMENT].slots[i]->visible = true;
			inventory[EQUIPMENT].slots[i]->enable_tablist_nav = true;
		}
		else {
			inventory[EQUIPMENT].slots[i]->visible = false;
			inventory[EQUIPMENT].slots[i]->enable_tablist_nav = false;
			if (current_tablist_widget && inventory[EQUIPMENT].slots[i] == current_tablist_widget) {
				reset_tablist_cursor = true;
			}
		}
	}

	if (!equipmentSetButton.empty()) {
		for (size_t i=0; i<equipmentSetButton.size(); i++) {
			if (player_inventory->active_equipment_set > 0) {
				if (i == player_inventory->active_equipment_set-1) {
					equipmentSetButton[i]->enabled = false;
					if (current_tablist_widget && equipmentSetButton[i] == current_tablist_widget) {
						reset_tablist_cursor = true;
					}

				}
				else {
					equipmentSetButton[i]->enabled = true;
				}
			}
		}
	}

	if (reset_tablist_cursor && first_active_slot) {
		tablist.setCurrent(first_active_slot);
	}

	if (equipmentSetLabel) {
		std::stringstream label;
		label << player_inventory->active_equipment_set << "/" << player_inventory->max_equipment_set;
		equipmentSetLabel->setText(label.str());
		equipmentSetLabel->setColor(font->getColor(FontEngine::COLOR_MENU_NORMAL));
	}

	changed_equipment = true;
}

bool MenuInventory::applyEquipmentSetDelta(int delta) {
	if (delta == 0 || player_inventory->max_equipment_set == 0)
		return false;

	if (delta > 0)
		applyNextEquipmentSet();
	else
		applyPreviousEquipmentSet();

	applyEquipment();
	clearHighlight();
	return true;
}

int MenuInventory::getEquippedCount() {
	return static_cast<int>(equipped_area.size());
}

int MenuInventory::getTotalSlotCount() {
	return player_inventory->MAX_CARRIED + player_inventory->MAX_EQUIPPED;
}

void MenuInventory::clearHighlight() {
	inventory[EQUIPMENT].highlightClear();
	inventory[CARRIED].highlightClear();
}

// fillEquipmentSlots() moved to PlayerInventory in P1.3d-4b-3, unchanged -- see PlayerInventory.h.

int MenuInventory::getMaxPurchasable(ItemStack item, int vendor_tab) {
	if (!items->isValid(item.item))
		return 0;

	if (vendor_tab == ItemManager::VENDOR_BUY)
		return player_inventory->currency / items->items[item.item]->getPrice(ItemManager::USE_VENDOR_RATIO);
	else if (vendor_tab == ItemManager::VENDOR_SELL)
		return player_inventory->currency / items->items[item.item]->getSellPrice(item.can_buyback);
	else if (vendor_tab == ItemManager::VENDOR_CRAFT)
		return items->items[item.item]->getCraftCount();
	else
		return 0;
}

// disableEquipmentSlot() moved to PlayerInventory in P1.3d-4b-3, as a private helper -- it was only
// ever called from applyEquipment(), which moved with it. See PlayerInventory.h.

bool MenuInventory::canEquipItem(const Point& position) {
	// clicked a carried item
	int slot = inventory[CARRIED].slotOver(position);
	if (slot == -1)
		return false;

	ItemID item_id = inventory[CARRIED][slot].item;

	// empty slot
	if (inventory[CARRIED][slot].empty() || !items->isValid(item_id))
		return false;

	return (player->stats.humanoid && !items->getItemType(items->items[item_id]->type).name.empty() && player_inventory->getEquipSlotFromItem(item_id, !PlayerInventory::ONLY_EMPTY_SLOTS) >= 0);
}

bool MenuInventory::canUseItem(const Point& position) {
	// clicked a carried item
	int slot = inventory[CARRIED].slotOver(position);
	if (slot == -1)
		return false;

	// empty slot
	if (inventory[CARRIED][slot].empty())
		return false;

	ItemID item_id = inventory[CARRIED][slot].item;

	return player_inventory->canActivateItem(item_id);
}

bool MenuInventory::canPlaceItemOnActionbar(const Point& position) {
	// clicked an item
	int area = areaOver(position);
	if (area == -1)
		return false;

	int slot = inventory[area].slotOver(position);
	if (slot == -1)
		return false;

	ItemID item_id = inventory[area][slot].item;

	// empty slot
	if (inventory[area][slot].empty() || !items->isValid(item_id))
		return false;

	return (player->stats.humanoid && items->items[item_id]->power > 0);
}

MenuInventory::~MenuInventory() {
	delete button_close;
	delete button_sort;
	for (size_t i=0; i<equipmentSetButton.size(); i++) {
		delete equipmentSetButton[i];
	}

	delete equipmentSetNext;
	delete equipmentSetPrevious;
	delete equipmentSetLabel;

	if (preview)
		delete preview;
}
