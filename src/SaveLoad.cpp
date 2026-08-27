/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
Copyright © 2012 Stefan Beller
Copyright © 2013 Henrik Andersson
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
 * class SaveLoad
 *
 * Save and Load functions for the GameStatePlay.
 *
 * I put these in a separate cpp file just to keep GameStatePlay.cpp devoted to its core.
 *
 */

#include "ActionBarState.h"
#include "Avatar.h"
#include "CampaignManager.h"
#include "CommonIncludes.h"
#include "EngineSettings.h"
#include "FileParser.h"
#include "FogOfWar.h"
#include "GameStatePlay.h"
#include "MapRenderer.h"
#include "Menu.h"
#include "MenuActionBar.h"
#include "MenuCharacter.h"
#include "MenuHUDLog.h"
#include "MenuInventory.h"
#include "MenuLog.h"
#include "MenuManager.h"
#include "MenuPowers.h"
#include "MenuStash.h"
#include "MenuVendor.h"
#include "MessageEngine.h"
#include "ModManager.h"
#include "NPC.h"
#include "Platform.h"
#include "PlayerInventory.h"
#include "PowerBonusState.h"
#include "PowerManager.h"
#include "SaveLoad.h"
#include "Settings.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "Utils.h"
#include "UtilsFileSystem.h"
#include "UtilsParsing.h"
#include "Version.h"

SaveLoad::SaveLoad()
	: game_slot(0) {
}

SaveLoad::~SaveLoad() {
	Utils::logInfo("Cleaning up: SaveLoad");
}

/**
 * Before exiting the game, save to file
 */
void SaveLoad::saveGame() {
	// P2.3b: the pc/pinv/pab/pbs compatibility-alias globals this used to forward through are
	// gone; playerm->local() and its three siblings are the direct equivalent (kind A -- SaveLoad
	// is still single-character-per-slot, P4.1, so there is nothing to iterate here yet). This
	// wrapper's eight out-of-P2.3-scope callers (main_server.cpp, GameStateLoad.cpp,
	// GameStateCutscene.cpp, GameStateNew.cpp, MenuExit.cpp, SDLInputState.cpp, and the two
	// Platform*.cpp files) keep calling this no-arg form unchanged.
	saveGame(playerm->local(), playerm->inventoryFor(playerm->local_id), playerm->actionbarFor(playerm->local_id), playerm->powerbonusFor(playerm->local_id));
}

void SaveLoad::saveGame(Avatar* avatar, PlayerInventory* inventory, ActionBarState* actionbar, PowerBonusState* powerbonus) {

	if (game_slot <= 0) return;

	// if needed, create the save file structure
	Utils::createSaveDir(game_slot);

	// remove items with zero quantity from inventory
	inventory->inventory[PlayerInventory::EQUIPMENT].clean();
	inventory->inventory[PlayerInventory::CARRIED].clean();

	std::ofstream outfile;

	std::stringstream ss;
	ss << settings->path_user << "saves/" << eset->misc.save_prefix << "/" << game_slot << "/avatar.txt";

	outfile.open(Filesystem::convertSlashes(ss.str()).c_str(), std::ios::out);

	if (outfile.is_open()) {

		// comment
		outfile << "## flare-engine save file ##" << "\n";

		// hero name
		outfile << "name=" << avatar->stats.name << "\n";

		// permadeath
		outfile << "permadeath=" << avatar->stats.permadeath << "\n";

		// hero visual option
		outfile << "option=";

		if (!avatar->stats.gfx_base_original.empty())
			outfile << avatar->stats.gfx_base_original;
		else
			outfile << avatar->stats.gfx_base;

		outfile << ",";

		if (!avatar->stats.gfx_head_original.empty())
			outfile << avatar->stats.gfx_head_original;
		else
			outfile << avatar->stats.gfx_head;

		outfile << "," << avatar->stats.gfx_portrait << "\n";

		// hero class
		outfile << "class=" << avatar->stats.character_class << "," << avatar->stats.character_subclass << "\n";

		// current experience
		outfile << "xp=" << avatar->stats.xp << "\n";

		// hp and mp
		if (eset->misc.save_hpmp) outfile << "hpmp=" << avatar->stats.hp << "," << avatar->stats.mp << "\n";

		// stat spec
		outfile << "build=";
		for (size_t i = 0; i < eset->primary_stats.list.size(); ++i) {
			outfile << avatar->stats.primary[i];
			if (i < eset->primary_stats.list.size() - 1)
				outfile << ",";
		}
		outfile << "\n";

		// equipped gear
		outfile << "equipped_quantity=" << inventory->inventory[PlayerInventory::EQUIPMENT].getQuantities() << "\n";
		outfile << "equipped=" << inventory->inventory[PlayerInventory::EQUIPMENT].getItems() << "\n";

		// active equipped set
		outfile << "active_equipment_set=" << inventory->active_equipment_set << "\n";

		// carried items
		outfile << "carried_quantity=" << inventory->inventory[PlayerInventory::CARRIED].getQuantities() << "\n";
		outfile << "carried=" << inventory->inventory[PlayerInventory::CARRIED].getItems() << "\n";

		// spawn point
		outfile << "spawn=" << wmap->respawn_map << "," << static_cast<int>(wmap->respawn_point.x) << "," << static_cast<int>(wmap->respawn_point.y) << "\n";

		// action bar
		// NOTE we need to reset any bonus-modified powers in the action bar before writing
		// we use menu->pow->setUnlockedPowers() after to restore the action bar state
		powerbonus->clearActionBarBonusLevels();
		outfile << "actionbar=";
		for (unsigned i = 0; i < static_cast<unsigned>(MenuActionBar::SLOT_MAX); i++) {
			if (i < actionbar->slots_count)
			{
				if (avatar->stats.transformed) outfile << actionbar->hotkeys_temp[i];
				else outfile << actionbar->hotkeys[i];
			}
			else
			{
				outfile << 0;
			}
			if (i < MenuActionBar::SLOT_MAX - 1) outfile << ",";
		}
		outfile << "\n";
		// Widget-only skill-tree refresh, restoring what clearActionBarBonusLevels() above
		// temporarily cleared for the write -- see loadPowerTree()'s comment for why a headless
		// server doesn't need this to have run. Skipped rather than dereferencing a menu that
		// doesn't exist (P1.4c).
		if (menu)
			menu->pow->setUnlockedPowers();

		//shapeshifter value
		if (avatar->stats.transform_type == "untransform" || avatar->stats.transform_duration != -1) outfile << "transformed=" << "\n";
		else outfile << "transformed=" << avatar->stats.transform_type << "," << avatar->stats.manual_untransform << "\n";

		// restore hero powers
		if (avatar->stats.transformed && avatar->hero_stats) {
			avatar->stats.powers_list = avatar->hero_stats->powers_list;
		}

		// enabled powers
		outfile << "powers=";
		for (unsigned int i=0; i<avatar->stats.powers_list.size(); i++) {
			if (i < avatar->stats.powers_list.size()-1) {
				if (avatar->stats.powers_list[i] > 0)
					outfile << avatar->stats.powers_list[i] << ",";
			}
			else {
				if (avatar->stats.powers_list[i] > 0)
					outfile << avatar->stats.powers_list[i];
			}
		}
		outfile << "\n";

		// restore transformed powers
		if (avatar->stats.transformed && avatar->charmed_stats) {
			avatar->stats.powers_list = avatar->charmed_stats->powers_list;
		}

		// campaign data
		outfile << "campaign=" << camp->getAll() << "\n";

		outfile << "time_played=" << avatar->time_played << "\n";

		// save the engine version for troubleshooting purposes
		outfile << "engine_version=" << VersionInfo::ENGINE.getString() << "\n";

		// save the vendor buyback -- MenuVendor-owned UI cache, no sim-side equivalent (P1.4c,
		// matches checkNPCInteraction()'s "drop entirely").
		if (eset->misc.save_buyback && menu) {
			std::map<std::string, ItemStorage>::iterator it;

			for (it = menu->vendor->buyback_stock.begin(); it != menu->vendor->buyback_stock.end(); ++it) {
				if (it->second.empty())
					continue;

				outfile << "buyback_item=" << it->first << ";" << it->second.getItems() << "\n";
				outfile << "buyback_quantity=" << it->first << ";" << it->second.getQuantities() << "\n";
			}
		}

		outfile << "questlog_dismissed=" << !actionbar->requires_attention[MenuActionBar::MENU_LOG] << "\n";

		// menu->stash's tab selection is a widget concern with no sim state behind it; a headless
		// server writes the field's default (0) rather than reading through a menu that doesn't
		// exist (P1.4c).
		outfile << "stash_tab=" << (menu ? menu->stash->getTab() : 0);

		outfile << std::endl;

		if (outfile.bad()) Utils::logError("SaveLoad: Unable to save the game. No write access or disk is full!");
		outfile.close();
		outfile.clear();

		platform.FSCommit();
	}

	// Save stashes -- MenuStash-owned, no sim-side equivalent (P1.4c, matches checkStash()).
	if (menu) {
		for (size_t i = 0; i < menu->stash->tabs.size(); ++i) {
			// shared stashes are not saved for permadeath characters
			if (avatar->stats.permadeath && !menu->stash->tabs[i].is_private)
				continue;

			ss.str("");
			ss << settings->path_user << "saves/" << eset->misc.save_prefix;
			if (menu->stash->tabs[i].is_private)
				ss << "/" << game_slot;
			ss << "/" << menu->stash->tabs[i].filename;
			outfile.open(Filesystem::convertSlashes(ss.str()).c_str(), std::ios::out);

			if (outfile.is_open()) {

				// comment
				outfile << "# flare-engine stash file: \"" << menu->stash->tabs[i].id << "\"\n";

				outfile << "quantity=" << menu->stash->tabs[i].stock.getQuantities() << "\n";
				outfile << "item=" << menu->stash->tabs[i].stock.getItems() << "\n";

				outfile << std::endl;

				if (outfile.bad()) Utils::logError("SaveLoad: Unable to save stash. No write access or disk is full!");
				outfile.close();
				outfile.clear();

				platform.FSCommit();
			}
		}
	}

	// save fog-of-war layers
	saveFOW();

	saveExtendedItems(SAVE_STORAGE_ITEMS, inventory);
	settings->prev_save_slot = game_slot-1;

	// display a log message saying that we saved the game -- widget-only, no sim-side equivalent
	// (P1.4c).
	if (menu) {
		menu->questlog->add(msg->get("Game saved."), MenuLog::TYPE_MESSAGES, WidgetLog::MSG_NORMAL);
		menu->hudlog->add(msg->get("Game saved."), MenuHUDLog::MSG_NORMAL);
	}
}

void SaveLoad::saveExtendedItems(bool save_storage_items) {
	// P2.3b: same as saveGame() above -- kind A, playerm->local()'s own inventory.
	saveExtendedItems(save_storage_items, playerm->inventoryFor(playerm->local_id));
}

void SaveLoad::saveExtendedItems(bool save_storage_items, PlayerInventory* inventory) {
	// Save extended Items
	std::stringstream ss;
	ss << settings->path_user << "saves/" << eset->misc.save_prefix << "/extended_items.txt";

	std::ofstream outfile;
	outfile.open(Filesystem::convertSlashes(ss.str()).c_str(), std::ios::out);

	if (outfile.is_open()) {
		for (size_t i = eset->loot.extended_items_offset; i < items->items.size(); ++i) {
			Item* item = items->items[i];

			if (!item || item->parent == 0)
				continue;

			bool item_in_storage = false;
			if (save_storage_items && menu) {
				if (menu->inv && inventory->inventory[PlayerInventory::EQUIPMENT].contain(i, 1)) {
					item_in_storage = true;
				}
				else if (menu->inv && inventory->inventory[PlayerInventory::CARRIED].contain(i, 1)) {
					item_in_storage = true;
				}
				else if (menu->stash) {
					for (size_t j = 0; j < menu->stash->tabs.size(); ++j) {
						if (menu->stash->tabs[j].stock.contain(i, 1)) {
							item_in_storage = true;
							break;
						}
					}
				}
			}

			if (!item_in_storage && !item->is_foreign)
				continue;

			outfile << "[item]" << std::endl;
			outfile << "id=" << i << "," << item->parent << std::endl;
			outfile << "level=" << item->level << std::endl;

			if (item->quality < items->item_qualities.size() && !items->item_qualities[item->quality].name.empty()) {
				outfile << "quality=" << items->item_qualities[item->quality].id << std::endl;
			}

			if (item->requires_level.randomized) {
				outfile << "requires_level=" << item->requires_level.serialize(false) << std::endl;
			}

			for (size_t j = 0; j < eset->primary_stats.list.size(); ++j) {
				if (item->requires_stat[j].randomized) {
					outfile << "requires_stat=" << eset->primary_stats.list[j].id << "," << item->requires_stat[j].serialize(false) << std::endl;
				}
			}

			if (item->price.randomized) {
				outfile << "price=" << item->price.serialize(false) << std::endl;
			}

			if (item->price_sell.randomized) {
				outfile << "price=" << item->price_sell.serialize(false) << std::endl;
			}

			if (item->base_abs.min.randomized) {
				outfile << "abs_min=" << item->base_abs.min.serialize(false) << std::endl;
			}

			if (item->base_abs.max.randomized) {
				outfile << "abs_max=" << item->base_abs.max.serialize(false) << std::endl;
			}

			for (size_t j = 0; j < eset->damage_types.list.size(); ++j) {
				if (item->base_dmg[j].min.randomized) {
					outfile << "dmg_min=" << eset->damage_types.list[j].id << "," << item->base_dmg[j].min.serialize(false) << std::endl;
				}
				if (item->base_dmg[j].max.randomized) {
					outfile << "dmg_max=" << eset->damage_types.list[j].id << "," << item->base_dmg[j].max.serialize(false) << std::endl;
				}
			}

			for (size_t j = 0; j < item->bonus.size(); ++j) {
				BonusData* bonus = &(item->bonus[j]);

				if (!bonus->is_extended)
					continue;

				if (bonus->power_id > 0)
					outfile << "bonus_power_level=";
				else
					outfile << "bonus=";

				if (bonus->type == BonusData::SPEED)
					outfile << "speed";
				else if (bonus->type == BonusData::ATTACK_SPEED)
					outfile << "attack_speed";
				else if (bonus->type == BonusData::STAT)
					outfile << Stats::KEY[bonus->index];
				else if (bonus->type == BonusData::DAMAGE_MIN)
					outfile << eset->damage_types.list[bonus->index].min;
				else if (bonus->type == BonusData::DAMAGE_MAX)
					outfile << eset->damage_types.list[bonus->index].max;
				else if (bonus->type == BonusData::RESIST_ELEMENT)
					outfile << eset->damage_types.list[bonus->index].resist;
				else if (bonus->type == BonusData::PRIMARY_STAT)
					outfile << eset->primary_stats.list[bonus->index].id;
				else if (bonus->type == BonusData::RESOURCE_STAT)
					outfile << eset->resource_stats.list[bonus->index].ids[bonus->sub_index];
				else if (bonus->type == BonusData::POWER_LEVEL)
					outfile << bonus->power_id;
				else
					continue;

				outfile << "," << bonus->value.serialize(bonus->is_multiplier);

				outfile << std::endl;
			}
			outfile << std::endl;
		}
	}

}

/**
 * When loading the game, load from file if possible
 */
void SaveLoad::loadGame() {
	// P2.3b: same as saveGame() above -- kind A, playerm->local() and its three siblings.
	loadGame(playerm->local(), playerm->inventoryFor(playerm->local_id), playerm->actionbarFor(playerm->local_id), playerm->powerbonusFor(playerm->local_id));
}

void SaveLoad::loadGame(Avatar* avatar, PlayerInventory* inventory, ActionBarState* actionbar, PowerBonusState* powerbonus) {
	if (game_slot <= 0) return;

	// ensure that the save folder has all its sub-folders
	Utils::createSaveDir(game_slot);

	float saved_hp = 0;
	float saved_mp = 0;
	int currency = 0;
	size_t stash_tab = 0;
	Version save_version(VersionInfo::MIN);

	FileParser infile;
	std::vector<PowerID> hotkeys(MenuActionBar::SLOT_MAX, -1);

	std::stringstream ss;
	ss << settings->path_user << "saves/" << eset->misc.save_prefix << "/" << game_slot << "/avatar.txt";

	if (infile.open(ss.str(), !FileParser::MOD_FILE, FileParser::ERROR_NORMAL)) {
		while (infile.next()) {
			if (infile.key == "name") avatar->stats.name = infile.val;
			else if (infile.key == "permadeath") {
				avatar->stats.permadeath = Parse::toBool(infile.val);
			}
			else if (infile.key == "option") {
				avatar->stats.gfx_base = Parse::popFirstString(infile.val);
				avatar->stats.gfx_head = Parse::popFirstString(infile.val);
				avatar->stats.gfx_portrait = Parse::popFirstString(infile.val);

				avatar->stats.checkGFXPaths();
			}
			else if (infile.key == "class") {
				avatar->stats.character_class = Parse::popFirstString(infile.val);
				avatar->stats.character_subclass = Parse::popFirstString(infile.val);
			}
			else if (infile.key == "xp") {
				avatar->stats.xp = Parse::toUnsignedLong(infile.val);
			}
			else if (infile.key == "hpmp") {
				saved_hp = Parse::popFirstFloat(infile.val);
				saved_mp = Parse::popFirstFloat(infile.val);
			}
			else if (infile.key == "build") {
				for (size_t i = 0; i < eset->primary_stats.list.size(); ++i) {
					avatar->stats.primary[i] = Parse::popFirstInt(infile.val);
					if (avatar->stats.primary[i] < 0 || avatar->stats.primary[i] > avatar->stats.max_points_per_stat) {
						Utils::logInfo("SaveLoad: Primary stat value for '%s' is out of bounds, setting to zero.", eset->primary_stats.list[i].id.c_str());
						avatar->stats.primary[i] = 0;
					}
				}
			}
			else if (infile.key == "currency") {
				currency = Parse::toInt(infile.val);
			}
			else if (infile.key == "equipped") {
				inventory->inventory[PlayerInventory::EQUIPMENT].setItems(infile.val);
				inventory->inventory[PlayerInventory::EQUIPMENT].setForeign(false);
			}
			else if (infile.key == "equipped_quantity") {
				inventory->inventory[PlayerInventory::EQUIPMENT].setQuantities(infile.val);
			}
			else if (infile.key == "active_equipment_set") {
				// menu->inv->applyEquipmentSet() does two things: the bounds-checked assignment
				// below, which is the only sim-relevant part (P1.4c: a headless server has no
				// menu to route it through), and updateEquipmentSetWidgets(), a widget refresh
				// that's moot here since applyPlayerData() unconditionally calls
				// inventory->applyEquipment() right after this parse loop either way.
				unsigned set = static_cast<unsigned>(Parse::toInt(infile.val));
				if (menu)
					menu->inv->applyEquipmentSet(set);
				else if (set > 0 && set <= inventory->max_equipment_set)
					inventory->active_equipment_set = set;
			}
			else if (infile.key == "carried") {
				inventory->inventory[PlayerInventory::CARRIED].setItems(infile.val);
				inventory->inventory[PlayerInventory::CARRIED].setForeign(false);
			}
			else if (infile.key == "carried_quantity") {
				inventory->inventory[PlayerInventory::CARRIED].setQuantities(infile.val);
			}
			else if (infile.key == "spawn") {
				wmap->teleport_mapname = Parse::popFirstString(infile.val);
				if (wmap->teleport_mapname != "" && Filesystem::fileExists(mods->locate(wmap->teleport_mapname))) {
					wmap->teleport_destination.x = static_cast<float>(Parse::popFirstInt(infile.val)) + 0.5f;
					wmap->teleport_destination.y = static_cast<float>(Parse::popFirstInt(infile.val)) + 0.5f;
					wmap->teleportation = true;
					// prevent spawn.txt from putting us on the starting map
					wmap->clearEvents();
				}
				else {
					Utils::logError("SaveLoad: Unable to find %s, loading maps/spawn.txt", wmap->teleport_mapname.c_str());
					wmap->teleport_mapname = "maps/spawn.txt";
					wmap->teleport_destination.x = 0.5f;
					wmap->teleport_destination.y = 0.5f;
					wmap->teleportation = true;
				}
			}
			else if (infile.key == "actionbar") {
				for (int i = 0; i < MenuActionBar::SLOT_MAX; i++) {
					hotkeys[i] = powers->verifyID(Parse::popFirstInt(infile.val), &infile, PowerManager::ALLOW_ZERO_ID);
				}
				actionbar->set(hotkeys, !ActionBarState::SET_SKIP_EMPTY);
			}
			else if (infile.key == "transformed") {
				avatar->stats.transform_type = Parse::popFirstString(infile.val);
				if (avatar->stats.transform_type != "") {
					avatar->stats.transform_duration = -1;
					avatar->stats.manual_untransform = Parse::toBool(Parse::popFirstString(infile.val));
				}
			}
			else if (infile.key == "powers") {
				std::string power;
				while ( (power = Parse::popFirstString(infile.val)) != "") {
					PowerID power_id = powers->verifyID(Parse::toInt(power), &infile, !PowerManager::ALLOW_ZERO_ID);
					if (power_id > 0)
						avatar->stats.powers_list.push_back(power_id);
				}
			}
			else if (infile.key == "campaign") camp->setAll(infile.val);
			else if (infile.key == "time_played") avatar->time_played = Parse::toUnsignedLong(infile.val);
			else if (infile.key == "engine_version") save_version.setFromString(infile.val);
			// Vendor buyback stock is a MenuVendor-owned UI cache with no sim-side equivalent
			// (matches checkNPCInteraction()'s "drop entirely" -- P1.4c) -- skipped headless
			// rather than dereferencing a menu that doesn't exist.
			else if (eset->misc.save_buyback && menu && infile.key == "buyback_item") {
				std::string npc_filename = Parse::popFirstString(infile.val, ';');
				if (!npc_filename.empty()) {
					menu->vendor->buyback_stock[npc_filename].init(NPC::VENDOR_MAX_STOCK);
					menu->vendor->buyback_stock[npc_filename].setItems(infile.val);
				}
			}
			else if (eset->misc.save_buyback && menu && infile.key == "buyback_quantity") {
				std::string npc_filename = Parse::popFirstString(infile.val, ';');
				if (!npc_filename.empty()) {
					menu->vendor->buyback_stock[npc_filename].init(NPC::VENDOR_MAX_STOCK);
					menu->vendor->buyback_stock[npc_filename].setQuantities(infile.val);
				}
			}
			else if (infile.key == "questlog_dismissed") avatar->questlog_dismissed = Parse::toBool(infile.val);
			else if (infile.key == "stash_tab") stash_tab = Parse::toInt(infile.val);
		}

		infile.close();
	}
	else Utils::logError("SaveLoad: Unable to open %s!", ss.str().c_str());

	// set starting values for primary stats based on class
	EngineSettings::HeroClasses::HeroClass* pc_class;
	pc_class = eset->hero_classes.getByName(avatar->stats.character_class);
	if (pc_class) {
		for (size_t i = 0; i < eset->primary_stats.list.size(); ++i) {
			avatar->stats.primary_starting[i] = pc_class->primary[i] + 1;
		}
	}

	// add legacy currency to inventory
	inventory->addCurrency(currency);

	// apply stats, inventory, and powers
	applyPlayerData(avatar, inventory, actionbar, powerbonus);

	// trigger passive effects here? Saved HP/MP values might depend on passively boosted HP/MP
	// powers->activatePassives(avatar->stats);
	if (eset->misc.save_hpmp && saved_hp != 0) {
		if (saved_hp < 0 || saved_hp > avatar->stats.get(Stats::HP_MAX)) {
			Utils::logError("SaveLoad: HP value is out of bounds, setting to maximum");
			avatar->stats.hp = avatar->stats.get(Stats::HP_MAX);
		}
		else avatar->stats.hp = saved_hp;

		if (saved_mp < 0 || saved_mp > avatar->stats.get(Stats::MP_MAX)) {
			Utils::logError("SaveLoad: MP value is out of bounds, setting to maximum");
			avatar->stats.mp = avatar->stats.get(Stats::MP_MAX);
		}
		else avatar->stats.mp = saved_mp;
	}
	else {
		avatar->stats.hp = avatar->stats.get(Stats::HP_MAX);
		avatar->stats.mp = avatar->stats.get(Stats::MP_MAX);
	}

	if (save_version != VersionInfo::ENGINE)
		Utils::logInfo("SaveLoad: Warning! Engine version of save file (%s) does not match current engine version (%s). Be on the lookout for bugs.", save_version.getString().c_str(), VersionInfo::ENGINE.getString().c_str());

	// reset character menu
	if (menu)
		menu->chr->refreshStats();

	// Widget-only skill-tree unlock display -- see loadPowerTree()'s own comment. Called
	// unconditionally on purpose; it guards itself.
	loadPowerTree(avatar);

	// Stash tabs are MenuStash-owned UI state with no sim-side equivalent (matches
	// checkStash()'s "drop entirely" -- P1.4c) -- skipped headless.
	if (menu) {
		// disable the shared stash for permadeath characters
		menu->stash->enableSharedTab(avatar->stats.permadeath);

		menu->stash->setTab(stash_tab);
	}

	avatar->loadAnimations();
}

/**
 * Load a class definition, index
 */
void SaveLoad::loadClass(int index) {
	// P2.3b: same as saveGame() above -- kind A, playerm->local() and its three siblings.
	loadClass(index, playerm->local(), playerm->inventoryFor(playerm->local_id), playerm->actionbarFor(playerm->local_id), playerm->powerbonusFor(playerm->local_id));
}

void SaveLoad::loadClass(int index, Avatar* avatar, PlayerInventory* inventory, ActionBarState* actionbar, PowerBonusState* powerbonus) {
	if (game_slot <= 0) return;

	if (index < 0 || static_cast<unsigned>(index) >= eset->hero_classes.list.size()) {
		Utils::logError("SaveLoad: Class index out of bounds.");
		return;
	}

	EngineSettings::HeroClasses::HeroClass& hero_class = eset->hero_classes.list[index];

	// name
	avatar->stats.character_class = hero_class.name;

	// stat points
	for (size_t i = 0; i < eset->primary_stats.list.size(); ++i) {
		// Avatar::init() sets primary stats to 1, so we add to that here
		avatar->stats.primary[i] += hero_class.primary[i];
		avatar->stats.primary_starting[i] = avatar->stats.primary[i];
	}

	// inventory
	inventory->addCurrency(hero_class.currency);

	ItemStack stack;

	std::string equipment = hero_class.equipment;
	while (!equipment.empty()) {
		stack = Parse::toItemQuantityPair(Parse::popFirstString(equipment));
		int equip_slot = inventory->getEquipSlotFromItem(stack.item, PlayerInventory::ONLY_EMPTY_SLOTS);
		inventory->add(stack, PlayerInventory::EQUIPMENT, equip_slot, !PlayerInventory::ADD_PLAY_SOUND, !PlayerInventory::ADD_AUTO_EQUIP);
	}

	for (size_t i = 0; i < hero_class.equipment_sets.size(); ++i) {
		menu->inv->applyEquipmentSet(hero_class.equipment_sets[i].first);
		std::string equipment_set = hero_class.equipment_sets[i].second;
		while (!equipment_set.empty()) {
			stack = Parse::toItemQuantityPair(Parse::popFirstString(equipment_set));
			int equip_slot = inventory->getEquipSlotFromItem(stack.item, PlayerInventory::ONLY_EMPTY_SLOTS);
			inventory->add(stack, PlayerInventory::EQUIPMENT, equip_slot, !PlayerInventory::ADD_PLAY_SOUND, !PlayerInventory::ADD_AUTO_EQUIP);
		}

	}
	menu->inv->applyEquipmentSet(1);

	std::string carried = hero_class.carried;
	while (!carried.empty()) {
		stack = Parse::toItemQuantityPair(Parse::popFirstString(carried));
		inventory->add(stack, PlayerInventory::CARRIED, ItemStorage::NO_SLOT, !PlayerInventory::ADD_PLAY_SOUND, !PlayerInventory::ADD_AUTO_EQUIP);
	}

	// powers & action bar
	for (size_t i = 0; i < hero_class.powers.size(); ++i) {
		PowerID power_id = powers->verifyID(hero_class.powers[i], NULL, !PowerManager::ALLOW_ZERO_ID);
		hero_class.powers[i] = power_id;
		if (power_id > 0)
			avatar->stats.powers_list.push_back(power_id);
	}
	for (size_t i = 0; i < hero_class.hotkeys.size(); ++i) {
		hero_class.hotkeys[i] = powers->verifyID(hero_class.hotkeys[i], NULL, PowerManager::ALLOW_ZERO_ID);
	}

	actionbar->set(hero_class.hotkeys, !ActionBarState::SET_SKIP_EMPTY);

	// campaign statuses
	for (size_t i = 0; i < hero_class.statuses.size(); ++i) {
		StatusID class_status = camp->registerStatus(hero_class.statuses[i]);
		camp->setStatus(class_status);
	}

	// apply stats, inventory, and powers
	applyPlayerData(avatar, inventory, actionbar, powerbonus);

	// reset character menu
	menu->chr->refreshStats();

	loadPowerTree(avatar);
}

/**
 * This is used to load the stash when starting a new game
 */
void SaveLoad::loadStash(Avatar* avatar) {
	// Load stash
	FileParser infile;
	std::stringstream ss;

	for (size_t i = 0; i < menu->stash->tabs.size(); ++i) {
		// shared stashes are not loaded for permadeath characters
		if (avatar->stats.permadeath && !menu->stash->tabs[i].is_private)
			continue;

		ss.str("");
		ss << settings->path_user << "saves/" << eset->misc.save_prefix;
		if (menu->stash->tabs[i].is_private)
			ss << "/" << game_slot;
		ss << "/" << menu->stash->tabs[i].filename;

		if (infile.open(ss.str(), !FileParser::MOD_FILE, FileParser::ERROR_NONE)) {
			while (infile.next()) {
				if (infile.key == "item") {
					menu->stash->tabs[i].stock.setItems(infile.val);
					if (menu->stash->tabs[i].is_private) {
						menu->stash->tabs[i].stock.setForeign(false);
					}
				}
				else if (infile.key == "quantity") {
					menu->stash->tabs[i].stock.setQuantities(infile.val);
				}
			}
			infile.close();
		}
		else Utils::logInfo("SaveLoad: Could not open stash file '%s'. This may be because it hasn't been created yet.", ss.str().c_str());

		menu->stash->tabs[i].stock.clean();
	}
}

/**
 * Performs final calculations after loading a save or a new class
 */
void SaveLoad::applyPlayerData(Avatar* avatar, PlayerInventory* inventory, ActionBarState* actionbar, PowerBonusState* powerbonus) {
	// actionbar/powerbonus aren't read directly in this function -- they're accepted anyway so
	// this stays consistent with saveGame()/loadGame()'s full four-object signature, since both of
	// this function's callers already have all four in hand from their own parameters.
	(void)actionbar;
	(void)powerbonus;

	inventory->fillEquipmentSlots();

	// remove items with zero quantity from inventory
	inventory->inventory[PlayerInventory::EQUIPMENT].clean();
	inventory->inventory[PlayerInventory::CARRIED].clean();

	// Load stash -- MenuStash-owned, no sim-side equivalent (P1.4c, matches checkStash()).
	if (menu)
		loadStash(avatar);

	// initialize vars
	avatar->stats.recalc();
	avatar->stats.loadHeroSFX();
	inventory->applyEquipment();
	avatar->stats.logic(); // run stat logic once to apply items bonuses

	// just for aesthetics, turn the hero to face the camera
	avatar->stats.direction = 6;

	// set up MenuTalker for this hero -- widget-only dialogue-portrait cache, no sim-side
	// equivalent (P1.4c, matches checkNPCInteraction()'s "drop entirely" -- NPC dialogue is
	// menu->talker-driven and doesn't exist headless at all). Dropped rather than guarded: an
	// if(menu) guard stops this from running server-side but still needs MenuTalker::setHero()
	// to link, and SaveLoad.cpp has no other reason to reach into MenuTalker.cpp at all -- so
	// dropping the reference is strictly simpler than keeping a guarded call to a class this
	// file otherwise has nothing to do with. (MenuTalker.cpp ends up linked into flare-server
	// regardless, via MenuManager.cpp -- see CMakeLists.txt's
	// FLARE_SERVER_EXCLUDED_PRESENTATION_SOURCES comment -- this drop just means SaveLoad.cpp
	// itself isn't why.)

	// load sounds (gender specific)
	avatar->loadSounds();

	// apply power upgrades -- see loadPowerTree()'s own comment.
	if (menu)
		menu->pow->setUnlockedPowers();
}

// menu->pow->loadPowerTree()/setUnlockedPowers() populate power_cell[i].cells[j].is_unlocked --
// widget-level skill-tree grid state, read by nothing outside MenuPowers.cpp (P1.4c's own
// "Powers reset" investigation). The sim-relevant gate this used to feed,
// StatBlock::canUsePower()'s menu_powers->meetsUsageStats() term, is itself conditioned on
// !menu_powers now (P1.4c), so a headless server -- which never constructs a MenuPowers at all --
// treats every power as already unlocked rather than needing this to have run. Guards itself
// rather than every call site, since both callers (here and applyPlayerData()) call it
// unconditionally today.
void SaveLoad::loadPowerTree(Avatar* avatar) {
	if (!menu)
		return;

	EngineSettings::HeroClasses::HeroClass* pc_class;
	pc_class = eset->hero_classes.getByName(avatar->stats.character_class);
	if (pc_class && !pc_class->power_tree.empty()) {
		menu->pow->loadPowerTree(pc_class->power_tree);
		return;
	}

	// fall back to the default power tree
	menu->pow->loadPowerTree("powers/trees/default.txt");
}

void SaveLoad::saveFOW() {
	std::ofstream outfile;

	// Save fow dark layer
	if (wmap->fogofwar && wmap->save_fogofwar && !wmap->getFilename().empty() && fow->dark_layer_id < wmap->layernames.size()) {
		std::string fow_filename = wmap->getFOWFilename();

		outfile.open(Filesystem::convertSlashes(fow_filename).c_str(), std::ios::out);

		if (outfile.is_open()) {
			outfile << "# " << wmap->getFilename() << std::endl;
			outfile << "[layer]" << std::endl;
			outfile << "type=" << wmap->layernames[fow->dark_layer_id] << std::endl;
			outfile << "data=" << std::endl;

			std::string layer = "";
			for (int line = 0; line < wmap->h; line++) {
				std::stringstream map_row;
				for (int tile = 0; tile < wmap->w; tile++) {
					unsigned short val = wmap->layers[fow->dark_layer_id][tile][line];
					map_row << val << ",";
				}
				layer += map_row.str();
				layer += '\n';
			}
			layer.erase(layer.end()-2, layer.end());
			layer += '\n';
			outfile << layer << std::endl;

			if (outfile.bad()) Utils::logError("SaveLoad: Unable to save map data. No write access or disk is full!");
			outfile.close();
			outfile.clear();

			platform.FSCommit();
		}
	}

}
