/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
Copyright © 2012-2014 Henrik Andersson
Copyright © 2012 Stefan Beller
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
 * class GameStatePlay
 *
 * Handles logic and rendering of the main action game play
 * Also handles message passing between child objects, often to avoid circular dependencies.
 */

#include "ActionBarState.h"
#include "Avatar.h"
#include "CampaignManager.h"
#include "CombatText.h"
#include "CursorManager.h"
#include "EnemyGroupManager.h"
#include "Entity.h"
#include "EntityManager.h"
#include "EngineSettings.h"
#include "FileParser.h"
#include "FogOfWar.h"
#include "GameState.h"
#include "GameStateCutscene.h"
#include "GameStatePlay.h"
#include "GameStateTitle.h"
#include "Hazard.h"
#include "HazardManager.h"
#include "InputState.h"
#include "LootManager.h"
#include "MapRenderer.h"
#include "Menu.h"
#include "MenuActionBar.h"
#include "MenuBook.h"
#include "MenuCharacter.h"
#include "MenuDevConsole.h"
#include "MenuEnemy.h"
#include "MenuExit.h"
#include "MenuGameOver.h"
#include "MenuHUDLog.h"
#include "MenuInventory.h"
#include "MenuLog.h"
#include "MenuManager.h"
#include "MenuMiniMap.h"
#include "MenuPowers.h"
#include "MenuRegionTitle.h"
#include "MenuStash.h"
#include "MenuTalker.h"
#include "MenuVendor.h"
#include "ModManager.h"
#include "NPC.h"
#include "NPCManager.h"
#include "PlayerCommand.h"
#include "PlayerInventory.h"
#include "PlayerManager.h"
#include "PowerBonusState.h"
#include "PowerManager.h"
#include "QuestLog.h"
#include "RenderDevice.h"
#include "SaveLoad.h"
#include "Settings.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "SimEvents.h"
#include "SoundManager.h"
#include "UtilsParsing.h"
#include "WidgetLabel.h"
#include "XPScaling.h"

#include <cassert>

GameStatePlay::GameStatePlay()
	: GameState()
	, enemy(NULL)
	, npc_id(-1)
	, is_first_map_load(true)
	, player(NULL)
	, player_inventory(NULL)
	, player_actionbar(NULL)
	, player_powerbonus(NULL)
{
	second_timer.setDuration(Settings::SIM_TICK_HZ);

	hasMusic = true;
	has_background = false;
	// GameEngine scope variables

	if (items == NULL)
		items = new ItemManager();

	camp = new CampaignManager();
	eventm = new EventManager();

	loot = new LootManager();
	powers = new PowerManager();
	fow = new FogOfWar();
	mapr = new MapRenderer();
	wmap = mapr; // the simulation's view of the same object. See SharedGameResources.h, P1.4a.
	// playerm allocates pc/pinv/pab/pbs together -- see PlayerManager.cpp. pinv/pab must exist
	// before the menus below and survive after they're destroyed: MenuInventory's constructor
	// hands pinv the inventory shape it parses out of menus/inventory.txt and binds itself to
	// it, and MenuActionBar does the same to pab before menus/actionbar.txt has even been
	// parsed. See the destructor's matching playerm->remove(0) call, placed after delete menu
	// for exactly this reason.
	playerm = new PlayerManager();
	playerm->create(0);
	playerm->setLocal(0);
	player = playerm->local();
	player_inventory = playerm->inventoryFor(playerm->local_id);
	player_actionbar = playerm->actionbarFor(playerm->local_id);
	player_powerbonus = playerm->powerbonusFor(playerm->local_id);
	mapr->setPlayer(player);
	entitym = new EntityManager();
	enemyg = new EnemyGroupManager();
	hazards = new HazardManager();
	menu = new MenuManager();
	npcs = new NPCManager();
	quests = new QuestLog(menu->questlog);
	xp_scaling = new XPScaling();

	// load the config file for character titles
	loadTitles();

	refreshWidgets();
}

void GameStatePlay::refreshWidgets() {
	menu->alignAll();
}

/**
 * Reset all game states to a new game.
 */
void GameStatePlay::resetGame() {
	camp->resetAllStatuses();
	player->init();
	player->stats.currency = 0;
	menu->act->clear(!MenuActionBar::CLEAR_SKIP_ITEMS);
	player_inventory->inventory[0].clear();
	player_inventory->inventory[1].clear();
	menu->inv->changed_equipment = true;
	player_inventory->currency = 0;
	menu->questlog->clearAll();
	quests->createQuestList();
	menu->hudlog->clear();

	// Finalize new character settings
	menu->talker->setHero(player->stats);
	player->loadSounds();

	mapr->teleportation = true;
	mapr->teleport_mapname = "maps/spawn.txt";
}

/**
 * Check mouseover for enemies.
 * class variable "enemy" contains a live enemy on mouseover.
 * This function also sets enemy mouseover for Menu Enemy.
 */
void GameStatePlay::checkEnemyFocus() {
	player->stats.target_corpse = NULL;
	player->stats.target_nearest = NULL;
	player->stats.target_nearest_corpse = NULL;
	player->stats.target_nearest_dist = 0;
	player->stats.target_nearest_corpse_dist = 0;

	FPoint src_pos = player->stats.pos;

	// check the last hit enemy first
	// if there's none, then either get the nearest enemy or one under the mouse (depending on mouse mode)
	if (!inpt->usingMouse()) {
		if (hazards->last_enemy) {
			if (enemy == hazards->last_enemy) {
				if (!menu->enemy->timeout.isEnd() && hazards->last_enemy->stats.hp > 0)
					return;
				else
					hazards->last_enemy = NULL;
			}
			enemy = hazards->last_enemy;
		}
		else {
			enemy = entitym->getNearestEntity(player->stats.pos, !EntityManager::GET_CORPSE, NULL, eset->misc.interact_range);
		}
	}
	else {
		if (hazards->last_enemy) {
			enemy = hazards->last_enemy;
			hazards->last_enemy = NULL;
		}
		else {
			enemy = entitym->entityFocus(inpt->mouse, mapr->cam.pos, EntityManager::IS_ALIVE);
			if (enemy) {
				curs->setCursor(CursorManager::CURSOR_ATTACK);
			}
			src_pos = Utils::screenToMap(inpt->mouse.x, inpt->mouse.y, mapr->cam.pos.x, mapr->cam.pos.y);

		}
	}

	if (enemy) {
		// set the actual menu with the enemy selected above
		if (!enemy->stats.suppress_hp) {
			menu->enemy->enemy = enemy;
			menu->enemy->timeout.reset(Timer::BEGIN);
		}
	}
	else if (inpt->usingMouse()) {
		// if we're using a mouse and we didn't select an enemy, try selecting a dead one instead
		Entity *temp_enemy = entitym->entityFocus(inpt->mouse, mapr->cam.pos, !EntityManager::IS_ALIVE);
		if (temp_enemy && !temp_enemy->stats.suppress_hp) {
			player->stats.target_corpse = &(temp_enemy->stats);
			menu->enemy->enemy = temp_enemy;
			menu->enemy->timeout.reset(Timer::BEGIN);
		}
	}

	// save the highlighted enemy position for auto-targeting purposes
	if (enemy) {
		player->cursor_enemy = enemy;
	}
	else {
		player->cursor_enemy = NULL;
	}

	// save the positions of the nearest enemies for powers that use "target_nearest"
	Entity *nearest = entitym->getNearestEntity(src_pos, !EntityManager::GET_CORPSE, &(player->stats.target_nearest_dist), eset->misc.interact_range);
	if (nearest)
		player->stats.target_nearest = &(nearest->stats);
	Entity *nearest_corpse = entitym->getNearestEntity(src_pos, EntityManager::GET_CORPSE, &(player->stats.target_nearest_corpse_dist), eset->misc.interact_range);
	if (nearest_corpse)
		player->stats.target_nearest_corpse = &(nearest_corpse->stats);
}

/**
 * Similar to the above checkEnemyFocus(), but handles NPCManager instead
 */
void GameStatePlay::checkNPCFocus() {
	Entity *focus_npc;

	if (!inpt->usingMouse() && (!menu->enemy->enemy || menu->enemy->enemy->stats.hero_ally)) {
		// TODO bug? If mixed monster allies and npc allies, npc allies will always be highlighted, regardless of distance to player
		focus_npc = npcs->getNearestNPC(player->stats.pos);
	}
	else {
		focus_npc = npcs->npcFocus(inpt->mouse, mapr->cam.pos, true);
	}

	if (focus_npc) {
		// set the actual menu with the npc selected above
		if (!focus_npc->stats.suppress_hp) {
			menu->enemy->enemy = focus_npc;
			menu->enemy->timeout.reset(Timer::BEGIN);
		}
	}
	else if (inpt->usingMouse()) {
		// if we're using a mouse and we didn't select an npc, try selecting a dead one instead
		Entity *temp_npc = npcs->npcFocus(inpt->mouse, mapr->cam.pos, false);
		if (temp_npc) {
			menu->enemy->enemy = temp_npc;
			menu->enemy->timeout.reset(Timer::BEGIN);
		}
	}
}

/**
 * Check to see if the player is picking up loot on the ground
 */
void GameStatePlay::checkLoot() {

	if (!player->stats.alive)
		return;

	if (menu->isDragging())
		return;

	ItemStack pickup;

	// Autopickup
	pickup = loot->checkAutoPickup(player->stats.pos);

	// Normal pickups
	if (pickup.empty() && !player->using_main1) {
		pickup = loot->checkPickup(inpt->mouse, mapr->cam.pos, player->stats.pos);
	}

	if (!pickup.empty()) {
		player_inventory->add(pickup, PlayerInventory::CARRIED, ItemStorage::NO_SLOT, PlayerInventory::ADD_PLAY_SOUND, PlayerInventory::ADD_AUTO_EQUIP);
		if (items->isValid(pickup.item)) {
			StatusID pickup_status = camp->registerStatus(items->items[pickup.item]->pickup_status);
			camp->setStatus(pickup_status);
		}
		pickup.clear();
	}

}

void GameStatePlay::checkTeleport() {
	bool on_load_teleport = false;

	// both map events and player powers can cause teleportation
	if (mapr->teleportation || player->stats.teleportation) {

		if (mapr->fogofwar)
			if(fow->fog_layer_id != 0)
				fow->handleIntramapTeleport();

		mapr->collider.unblock(player->stats.pos.x, player->stats.pos.y);

		if (mapr->teleportation) {
			// camera gets interpolated movement during intramap teleport
			// during intermap teleport, we set the camera to the player position
			player->stats.pos.x = mapr->teleport_destination.x;
			player->stats.pos.y = mapr->teleport_destination.y;
			player->teleport_camera_lock = true;
		}
		else {
			player->stats.pos.x = player->stats.teleport_destination.x;
			player->stats.pos.y = player->stats.teleport_destination.y;
		}

		// if we're not changing map, move allies to a the player's new position
		// when changing maps, entitym->handleNewMap() does something similar to this
		if (mapr->teleport_mapname.empty()) {
			FPoint spawn_pos = mapr->collider.getRandomNeighbor(Point(player->stats.pos), 1, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_ALL_ENTITIES);
			for (unsigned int i=0; i < entitym->entities.size(); i++) {
				if(entitym->entities[i]->stats.hero_ally && entitym->entities[i]->stats.alive && entitym->entities[i]->stats.speed > 0) {
					mapr->collider.unblock(entitym->entities[i]->stats.pos.x, entitym->entities[i]->stats.pos.y);
					entitym->entities[i]->stats.pos = spawn_pos;
					mapr->collider.block(entitym->entities[i]->stats.pos.x, entitym->entities[i]->stats.pos.y, MapCollision::IS_ALLY);
				}
			}
		}

		// process intermap teleport
		if (mapr->teleportation && !mapr->teleport_mapname.empty()) {
			mapr->cam.warpTo(player->stats.pos);
			std::string teleport_mapname = mapr->teleport_mapname;
			mapr->teleport_mapname = "";
			inpt->lock_all = (teleport_mapname == "maps/spawn.txt");
			mapr->executeOnMapExitEvents();
			showLoading();
			render_device->cleanupQueuedImages();
			save_load->saveFOW(); // TODO handle save_onload/save_onexit?
			mapr->load(teleport_mapname);
			setLoadingFrame();

			// use the default hero spawn position for this map
			if (mapr->force_spawn_pos || (mapr->teleport_destination.x == -1 && mapr->teleport_destination.y == -1)) {
				player->stats.pos.x = mapr->hero_pos.x;
				player->stats.pos.y = mapr->hero_pos.y;

				if (mapr->teleport_destination_id > 0) {
					for (size_t i = 0; i < mapr->events.size(); ++i) {
						EventComponent* ec_hero_pos = mapr->events[i].getComponent(EventComponent::INTERMAP_ID);
						if (ec_hero_pos && ec_hero_pos->data[0].Int == mapr->teleport_destination_id) {
							player->stats.pos.x = static_cast<float>(mapr->events[i].location.x) + 0.5f;
							player->stats.pos.y = static_cast<float>(mapr->events[i].location.y) + 0.5f;
							break;
						}
					}
				}
				mapr->cam.warpTo(player->stats.pos);
			}

			// store this as the new respawn point (provided the tile is open)
			if (mapr->collider.isValidPosition(player->stats.pos.x, player->stats.pos.y, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_HERO)) {
				mapr->respawn_map = teleport_mapname;
				mapr->respawn_point = player->stats.pos;
			}
			else {
				Utils::logError("GameStatePlay: Spawn position (%d, %d) is blocked.", static_cast<int>(player->stats.pos.x), static_cast<int>(player->stats.pos.y));
			}

			player->handleNewMap();
			hazards->handleNewMap();
			loot->handleNewMap();
			powers->handleNewMap(&mapr->collider);
			menu->enemy->handleNewMap();
			menu->stash->visible = false;

			// switch off teleport flag so we can check if an on_load event has teleportation
			mapr->teleportation = false;

			mapr->executeOnLoadEvents();
			if (mapr->teleportation)
				on_load_teleport = true;

			// enemies and npcs should be initialized AFTER on_load events execute
			entitym->handleNewMap();
			npcs->handleNewMap();
			resetNPC();

			menu->mini->prerender(&mapr->collider, mapr->w, mapr->h);

			// return to title (permadeath) OR auto-save
			if (player->stats.permadeath && player->stats.cur_state == StatBlock::ENTITY_DEAD) {
				snd->stopMusic();
				showLoading();
				setRequestedGameState(new GameStateTitle());
			}
			else if (eset->misc.save_onload) {
				if (!is_first_map_load)
					save_load->saveGame(player, player_inventory, player_actionbar, player_powerbonus);
				else
					is_first_map_load = false;
			}
		}

		if (mapr->collider.isOutsideMap(player->stats.pos.x, player->stats.pos.y)) {
			Utils::logError("GameStatePlay: Teleport position is outside of map bounds.");
			player->stats.pos.x = 0.5f;
			player->stats.pos.y = 0.5f;
		}

		mapr->collider.block(player->stats.pos.x, player->stats.pos.y, !MapCollision::IS_ALLY);

		player->stats.teleportation = false;

		if (settings->mouse_move) {
			player->mm_target_object = Avatar::MM_TARGET_NONE;
			player->setDesiredMMTarget(player->stats.pos);
		}
	}

	if (!on_load_teleport && mapr->teleport_mapname.empty())
		mapr->teleportation = false;
}

/**
 * Check for cancel key to exit menus or exit the game.
 * Also check closing the game window entirely.
 */
void GameStatePlay::checkCancel() {
	bool save_on_exit = eset->misc.save_onexit && !(player->stats.permadeath && player->stats.cur_state == StatBlock::ENTITY_DEAD);

	if (save_on_exit && eset->misc.save_pos_onexit) {
		mapr->respawn_point = player->stats.pos;
	}

	// if user has clicked exit game from exit menu
	if (menu->requestingExit()) {
		menu->closeAll();

		if (save_on_exit)
			save_load->saveGame(player, player_inventory, player_actionbar, player_powerbonus);

		// audio levels can be changed in the pause menu, so update our settings file
		settings->saveSettings();
		inpt->saveKeyBindings();

		snd->stopMusic();
		showLoading();
		setRequestedGameState(new GameStateTitle());

		save_load->setGameSlot(0);
	}

	// if user closes the window
	if (inpt->done) {
		menu->closeAll();

		if (save_on_exit)
			save_load->saveGame(player, player_inventory, player_actionbar, player_powerbonus);

		settings->saveSettings();
		inpt->saveKeyBindings();

		snd->stopMusic();
		exitRequested = true;
	}
}

/**
 * Check for log messages from various child objects
 */
void GameStatePlay::checkLog() {

	// If the player has just respawned, we want to clear the HUD log
	if (player->respawn) {
		menu->hudlog->clear();
	}

	while (!player->log_msg.empty()) {
		const std::string& str = player->log_msg.front().first;
		const int msg_type = player->log_msg.front().second;

		menu->questlog->add(str, MenuLog::TYPE_MESSAGES, msg_type);
		menu->hudlog->add(str, msg_type);

		player->log_msg.pop();
	}
}

/**
 * Check if we need to open book
 */
void GameStatePlay::checkBook() {
	// Map events can open books
	if (!mapr->show_book.empty()) {
		menu->book->setBookFilename(mapr->show_book);
		mapr->show_book = "";
	}

	// items can be readable books
	if (!menu->inv->show_book.empty()) {
		menu->book->setBookFilename(menu->inv->show_book);
		menu->inv->show_book = "";
	}
}

void GameStatePlay::loadTitles() {
	FileParser infile;
	// @CLASS GameStatePlay: Titles|Description of engine/titles.txt
	if (infile.open("engine/titles.txt", FileParser::MOD_FILE, FileParser::ERROR_NORMAL)) {
		while (infile.next()) {
			if (infile.new_section && infile.section == "title") {
				Title t;
				titles.push_back(t);
			}

			if (titles.empty()) continue;

			Title& title = titles.back();

			if (infile.key == "title") {
				// @ATTR title.title|string|The displayed title.
				title.title = infile.val;
			}
			else if (infile.key == "level") {
				// @ATTR title.level|int|Requires level.
				title.level = Parse::toInt(infile.val);
			}
			else if (infile.key == "power") {
				// @ATTR title.power|power_id|Requires power.
				title.power = powers->verifyID(Parse::toPowerID(infile.val), &infile, !PowerManager::ALLOW_ZERO_ID);
			}
			else if (infile.key == "requires_status") {
				// @ATTR title.requires_status|list(string)|Requires status.
				std::string repeat_val = Parse::popFirstString(infile.val);
				while (!repeat_val.empty()) {
					title.requires_status.push_back(camp->registerStatus(repeat_val));
					repeat_val = Parse::popFirstString(infile.val);
				}
			}
			else if (infile.key == "requires_not_status") {
				// @ATTR title.requires_not_status|list(string)|Requires not status.
				std::string repeat_val = Parse::popFirstString(infile.val);
				while (!repeat_val.empty()) {
					title.requires_not_status.push_back(camp->registerStatus(repeat_val));
					repeat_val = Parse::popFirstString(infile.val);
				}
			}
			else if (infile.key == "primary_stat") {
				// @ATTR title.primary_stat|predefined_string, predefined_string : Primary stat, Lesser primary stat|Required primary stat(s). The lesser stat is optional.
				title.primary_stat_1 = Parse::popFirstString(infile.val);
				title.primary_stat_2 = Parse::popFirstString(infile.val);
			}
			else infile.error("GameStatePlay: '%s' is not a valid key.", infile.key.c_str());
		}
		infile.close();
	}
}

void GameStatePlay::checkTitle() {
	if (!player->stats.check_title || titles.empty())
		return;

	int title_id = -1;

	for (unsigned i=0; i<titles.size(); i++) {
		if (titles[i].title.empty())
			continue;

		if (titles[i].level > 0 && player->stats.level < titles[i].level)
			continue;
		if (titles[i].power > 0 && std::find(player->stats.powers_list.begin(), player->stats.powers_list.end(), titles[i].power) == player->stats.powers_list.end())
			continue;
		if (!titles[i].primary_stat_1.empty() && !checkPrimaryStat(titles[i].primary_stat_1, titles[i].primary_stat_2))
			continue;

		bool status_failed = false;
		for (size_t j = 0; j < titles[i].requires_status.size(); ++j) {
			if (!camp->checkStatus(titles[i].requires_status[j])) {
				status_failed = true;
				break;
			}
		}
		for (size_t j = 0; j < titles[i].requires_not_status.size(); ++j) {
			if (camp->checkStatus(titles[i].requires_not_status[j])) {
				status_failed = true;
				break;
			}
		}

		if (status_failed)
			continue;

		// Title meets the requirements
		title_id = i;
		break;
	}

	if (title_id != -1) player->stats.character_subclass = titles[title_id].title;
	player->stats.check_title = false;
	player->stats.refresh_stats = true;
}

void GameStatePlay::checkEquipmentChange() {
	if (menu->inv->changed_equipment) {
		// force the actionbar to update when we change gear
		player_actionbar->updated = true;

		player->loadAnimations();

		if (player->feet_index != -1) {
			ItemID feet_id = player_inventory->inventory[PlayerInventory::EQUIPMENT][player->feet_index].item;
			if (items->isValid(feet_id))
				player->loadStepFX(items->items[feet_id]->stepfx);
		}
	}

	menu->inv->changed_equipment = false;
}

void GameStatePlay::checkLootDrop() {

	// if the player has dropped an item from the inventory
	while (!menu->drop_stack.empty()) {
		if (!menu->drop_stack.front().empty()) {
			loot->addLoot(menu->drop_stack.front(), player->stats.pos, LootManager::DROPPED_BY_HERO);
		}
		menu->drop_stack.pop();
	}

	// if the player has dropped a quest reward because inventory full
	while (!camp->drop_stack.empty()) {
		if (!camp->drop_stack.front().empty()) {
			loot->addLoot(camp->drop_stack.front(), player->stats.pos, LootManager::DROPPED_BY_HERO);
		}
		camp->drop_stack.pop();
	}

	// if the player been directly given items, but their inventory is full
	// this happens when adding currency from older save files
	while (!menu->inv->drop_stack.empty()) {
		if (!menu->inv->drop_stack.front().empty()) {
			loot->addLoot(menu->inv->drop_stack.front(), player->stats.pos, LootManager::DROPPED_BY_HERO);
		}
		menu->inv->drop_stack.pop();
	}

	// Same as menu->inv->drop_stack above, but for overflow from player_inventory->add() -- P1.3d-4b-3 gave
	// PlayerInventory its own queue rather than reaching back into a menu that may not exist to
	// push into this one. UI-triggered overflow (drag-and-drop) still goes through
	// menu->inv->drop_stack above; sim-triggered overflow (loot pickup, quest rewards) goes here.
	while (!player_inventory->drop_stack.empty()) {
		if (!player_inventory->drop_stack.front().empty()) {
			loot->addLoot(player_inventory->drop_stack.front(), player->stats.pos, LootManager::DROPPED_BY_HERO);
		}
		player_inventory->drop_stack.pop();
	}
}

/**
 * Removes items as required by certain powers
 */
void GameStatePlay::checkUsedItems() {
	for (unsigned i=0; i<powers->used_items.size(); i++) {
		// Deliberately still routed through MenuInventory's wrapper here, not PlayerInventory
		// directly -- it keeps the activated_item/activated_slot special case (P1.3d-4b-3), which
		// is what makes a right-click activation consume the exact carried slot the player clicked
		// rather than just any stack of the same item. Repointing this one needs that case
		// designed a sim-side equivalent first, not just deleted.
		menu->inv->remove(powers->used_items[i], 1);
	}
	for (unsigned i=0; i<powers->used_equipped_items.size(); i++) {
		player_inventory->inventory[PlayerInventory::EQUIPMENT].remove(powers->used_equipped_items[i], 1);
		player_inventory->applyEquipment();
	}
	powers->clearUsedItems();
}

/**
 * Marks the menu if it needs attention.
 */
void GameStatePlay::checkNotifications() {
	if (player->newLevelNotification || menu->chr->getUnspent() > 0) {
		player->newLevelNotification = false;
		player_actionbar->requires_attention[MenuActionBar::MENU_CHARACTER] = !menu->chr->visible;
	}
	if (menu->pow->newPowerNotification) {
		menu->pow->newPowerNotification = false;
		player_actionbar->requires_attention[MenuActionBar::MENU_POWERS] = !menu->pow->visible;
	}
	if (quests->newQuestNotification) {
		quests->newQuestNotification = false;
		player_actionbar->requires_attention[MenuActionBar::MENU_LOG] = !menu->questlog->visible && !player->questlog_dismissed;
		player->questlog_dismissed = false;
	}

	// if the player is transformed into a creature, don't notifications for the powers menu
	if (player->stats.transformed) {
		player_actionbar->requires_attention[MenuActionBar::MENU_POWERS] = false;
	}
}

/**
 * If the player has clicked on an NPC, the game mode might be changed.
 * If a player walks away from an NPC, end the interaction with that NPC
 * If an NPC is giving a reward, process it
 */
void GameStatePlay::checkNPCInteraction() {
	if (player->using_main1 || !player->stats.humanoid)
		return;

	// reset movement restrictions when we're not in dialog
	if (!menu->talker->visible) {
		player->allow_movement = true;
	}

	if (npc_id != -1 && !menu->isNPCMenuVisible()) {
		// if we have an NPC, but no NPC windows are open, clear the NPC
		resetNPC();
	}

	// get NPC by ID
	// event NPCs take precedence over map NPCs
	if (mapr->event_npc != "") {
		// if the player is already interacting with an NPC when triggering an event NPC, clear the current NPC
		if (npc_id != -1) {
			resetNPC();
		}
		npc_id = mapr->npc_id = npcs->getID(mapr->event_npc);
		menu->talker->npc_from_map = false;
	}
	else if (mapr->npc_id != -1) {
		npc_id = mapr->npc_id;
		menu->talker->npc_from_map = true;
	}
	mapr->event_npc = "";
	mapr->npc_id = -1;

	if (npc_id != -1) {
		bool interact_with_npc = false;
		if (menu->talker->npc_from_map) {
			float interact_distance = Utils::calcDist(player->stats.pos, npcs->npcs[npc_id]->stats.pos);
			bool npc_is_alive = !npcs->npcs[npc_id]->stats.hero_ally || npcs->npcs[npc_id]->stats.hp > 0;

			if (interact_distance < eset->misc.interact_range && npc_is_alive) {
				interact_with_npc = true;
			}
			else {
				resetNPC();
			}
		}
		else {
			// npc is from event
			interact_with_npc = true;

			// since its impossible for the player to walk away from event NPCs, we disable their movement here
			player->allow_movement = false;
		}

		if (interact_with_npc) {
			if (!menu->isNPCMenuVisible()) {
				if (inpt->pressing[Input::MAIN1] && inpt->usingMouse()) inpt->lock[Input::MAIN1] = true;
				if (inpt->pressing[Input::ACCEPT]) inpt->lock[Input::ACCEPT] = true;

				menu->closeAll();
				menu->talker->setNPC(npcs->npcs[npc_id]);
				menu->talker->chooseDialogNode(-1);
			}
		}
	}
}

void GameStatePlay::checkStash() {
	if (mapr->stash) {
		// If triggered, open the stash and inventory menus
		menu->closeAll();
		menu->inv->visible = true;
		menu->stash->visible = true;
		mapr->stash = false;
		menu->stash->validate(menu->drop_stack);
	}
	else if (menu->stash->visible) {
		// Close stash if inventory is closed
		if (!menu->inv->visible) {
			menu->resetDrag();
			menu->stash->visible = false;
			if (menu->inv->sfx_close == 0) {
				snd->play(menu->stash->sfx_close, snd->DEFAULT_CHANNEL, snd->NO_POS, !snd->LOOP);
			}
		}

		// If the player walks away from the stash, close its menu
		float interact_distance = Utils::calcDist(player->stats.pos, mapr->stash_pos);
		if (interact_distance > eset->misc.interact_range || !player->stats.alive) {
			menu->resetDrag();
			menu->stash->visible = false;
			snd->play(menu->stash->sfx_close, snd->DEFAULT_CHANNEL, snd->NO_POS, !snd->LOOP);
		}

	}

	// If the stash has been updated, save the game
	if (menu->stash->checkUpdates()) {
		save_load->saveGame(player, player_inventory, player_actionbar, player_powerbonus);
	}
}

void GameStatePlay::checkCutscene() {
	if (!mapr->cutscene)
		return;

	showLoading();
	GameStateCutscene *cutscene = new GameStateCutscene(NULL);

	if (!cutscene->load(mapr->cutscene_file)) {
		delete cutscene;
		mapr->cutscene = false;
		return;
	}

	// handle respawn point and set game play game_slot
	cutscene->game_slot = save_load->getGameSlot();

	if (mapr->teleportation) {

		if (mapr->teleport_mapname != "")
			mapr->respawn_map = mapr->teleport_mapname;

		mapr->respawn_point = mapr->teleport_destination;

	}
	else {
		mapr->respawn_point = player->stats.pos;
	}

	if (eset->misc.save_oncutscene)
		save_load->saveGame(player, player_inventory, player_actionbar, player_powerbonus);

	menu->closeAll();

	setRequestedGameState(cutscene);
}

void GameStatePlay::checkSaveEvent() {
	if (mapr->save_game) {
		mapr->respawn_point = player->stats.pos;
		save_load->saveGame(player, player_inventory, player_actionbar, player_powerbonus);
		mapr->save_game = false;
	}
}

/**
 * Recursively update the action bar powers based on equipment
 */
void GameStatePlay::updateActionBar(unsigned index) {
	if (player_actionbar->slots_count == 0 || index > player_actionbar->slots_count - 1) return;

	if (items->items.empty()) return;

	for (unsigned i = index; i < player_actionbar->slots_count; i++) {
		if (player_actionbar->hotkeys[i] == 0) continue;

		PowerID id = player_inventory->getPowerMod(player_actionbar->hotkeys_mod[i]);
		if (id > 0) {
			player_actionbar->hotkeys_mod[i] = id;
			return updateActionBar(i);
		}
	}
}

/**
 * Process all actions for a single frame
 * This includes some message passing between child object
 */
void GameStatePlay::logic() {
	// Borrow the mouse-click arbitration state for this tick and hand it back at the end.
	// inpt->lock[] is shared with roughly twenty UI files, and three of Avatar's four writes to
	// it are conditional on simulation state, so they cannot be resolved at the boundary.
	// See PlayerCommand.h. logic() has a single exit, so this is safe as a local.
	PlayerCommand player_cmd;
	PlayerInputLocks player_locks;
	player_locks.copyFrom(*inpt);

	if (inpt->window_resized)
		refreshWidgets();

	curs->setLowHP(player->isLowHpCursorEnabled() && player->isLowHp());

	checkCutscene();

	// The death penalty is simulation and is driven from here, not from menu->logic(), which is
	// where it used to run. See PlayerInventory::applyDeathPenalty() (moved there outright in
	// P1.3d-4b-3, no menu involved any more). The call sits immediately before menu->logic() so
	// the tick it lands on is exactly the one it landed on before -- this was a pure relocation
	// and every golden had to stay put to prove it.
	player_inventory->applyDeathPenalty();

	// check menus first (top layer gets mouse click priority)
	menu->logic();

	if (!isPaused()) {
		if (!second_timer.isEnd())
			second_timer.tick();
		else {
			player->time_played++;
			second_timer.reset(Timer::BEGIN);
		}

		// these actions only occur when the game isn't paused
		if (player->stats.alive) checkLoot();
		checkEnemyFocus();
		checkNPCFocus();
		if (player->stats.alive) {
			mapr->checkHotspots();
			mapr->checkNearestEvent();
			checkNPCInteraction();
		}
		checkTitle();

		// The one place player intent is read out of global input. Screen-to-map conversion
		// happens here so the simulation never asks where the camera is pointing.
		PlayerCommandBuilder::build(player_cmd, *inpt, mapr->cam.pos);
		menu->act->checkAction(player->action_queue);
		player_cmd.actions = player->action_queue;
		player_cmd.click_consumed_by_ui = menu->act->isWithinSlots(inpt->mouse) || menu->act->isWithinMenus(inpt->mouse);

		// Respawn is a menu click, so it is resolved HERE rather than inside the simulation.
		// Avatar used to read menu->game_over->continue_clicked itself, which meant a dead player
		// could only come back if a UI button existed to press -- untrue on a headless server.
		// The click is consumed at the same boundary that reads it. See PlayerCommand.h.
		player_cmd.respawn = menu->game_over->visible && menu->game_over->continue_clicked;
		if (player_cmd.respawn)
			menu->game_over->close();

		// Stepping through equipment sets is simulation -- it decides which half of the slots the
		// character is wearing -- so it is driven from the tick now. MenuInventory::logic() used
		// to read the keyboard and do it itself.
		//
		// TWO THINGS MOVED, and they are worth separating:
		//
		//   the input read  is now in PlayerCommandBuilder::build(), the one place intent is
		//                   taken out of globals. Phase 3 fills equip_set_delta from a network
		//                   message and nothing below changes.
		//   the pause guard was '!menu->pause_requested' and is now this block's 'if (!isPaused())'.
		//                   On a headless server those differ, and that is P1.3c's point: a menu
		//                   asking for a pause is a request, and a server refuses it.
		//                   On a client they agree on the value but not on its age. MenuManager
		//                   computes pause_requested at the END of its logic() (MenuManager.cpp:916)
		//                   and calls inv->logic() before that (:642), so the old code was reading
		//                   the PREVIOUS tick's answer; this reads the current one. One tick, on
		//                   the tick a pause begins or ends, and only if the key is pressed on
		//                   exactly that tick. Stated because nothing in the corpus can see it:
		//                   P1.3c measured pause false on every tick of the six recordings that
		//                   existed then, and the two added since run on empty test maps where no
		//                   menu opens. Neither of those is a check -- if this one tick ever
		//                   matters, it will have to be found by hand.
		//
		// The lock is claimed here rather than in build(), which is const and claims nothing.
		if (menu->inv->applyEquipmentSetDelta(player_cmd.equip_set_delta))
			inpt->lock[player_cmd.equip_set_delta > 0 ? Input::EQUIPMENT_SWAP : Input::EQUIPMENT_SWAP_PREV] = true;

		player->logic(player_cmd, player_locks);

		// update camera -- moved out of Avatar::logic() (P1.4d); the camera has no sim
		// consequence, only mapr->logic()'s later cam.logic() smoothing step needs the target.
		mapr->cam.setTarget(player->stats.pos);

		// P2.2: stealth is per-player now -- EntityBehavior reads each evaluated player's own
		// Stats::STEALTH directly (via PlayerManager::nearestAliveTo()), so there's no longer a
		// single hero value to transfer onto EntityManager here.

		entitym->logic();
		hazards->logic();
		loot->logic();
		npcs->logic();

		comb->logic(mapr->cam.pos);
	}

	// close menus when the player dies, but still allow them to be reopened
	if (player->close_menus) {
		player->close_menus = false;
		menu->closeAll();
		if (player->stats.permadeath) {
			menu->exit->disableSave();
			menu->game_over->disableSave();
		}
	}

	// show the game-over menu once the death animation finishes -- Avatar.cpp sets this the same
	// tick stats.corpse flips true. GameStatePlay is presentation-only (P1.4 partitions it out of
	// flare_sim), so this push has to live here rather than in Avatar/GameStatePlay's sim callers.
	if (player->show_game_over) {
		player->show_game_over = false;
		menu->game_over->visible = true;
	}

	// Refresh the skill tree and action bar after a RESPEC event. EventManager.cpp sets this
	// instead of calling menu_powers/menu_act directly -- those types don't exist in flare_sim, and
	// this is the same close_menus-style boundary. Deferred verbatim, in the event's original
	// order, rather than split: menu_powers->resetToBasePowers()'s internal unlock pass and the
	// explicit setUnlockedPowers() call below depend on the order player->stats.powers_list is
	// populated in, and there is no replay coverage (RESPEC is unused by any corpus mod) to verify
	// a reordering is safe. See plans/00-ROADMAP.md's P1.3h note: a headless server defers this
	// forever, so RESPEC there resets a character's powers but never re-applies class defaults or
	// auto-unlocks free skill-tree nodes -- a real, documented gap, not a silent one.
	if (player->respec_powers) {
		player->respec_powers = false;
		EngineSettings::HeroClasses::HeroClass* pc_class = eset->hero_classes.getByName(player->stats.character_class);

		menu_powers->resetToBasePowers();
		if (pc_class && !player->respec_use_engine_defaults) {
			for (size_t j = 0; j < pc_class->powers.size(); j++) {
				player->stats.powers_list.push_back(pc_class->powers[j]);
			}
		}
		menu_powers->setUnlockedPowers();

		menu_act->clear(MenuActionBar::CLEAR_SKIP_ITEMS);
		if (pc_class && !player->respec_use_engine_defaults) {
			player_actionbar->set(pc_class->hotkeys, ActionBarState::SET_SKIP_EMPTY);
		}
		menu_powers->newPowerNotification = false;
	}

	// these actions occur whether the game is paused or not.
	// TODO Why? Some of these probably don't need to be executed when paused
	checkTeleport();
	checkLootDrop();
	checkLog();
	checkBook();
	checkEquipmentChange();
	checkUsedItems();
	checkStash();
	checkSaveEvent();
	checkNotifications();
	checkCancel();

	mapr->logic(isPaused());
	mapr->enemies_cleared = entitym->isCleared();
	quests->logic();

	player->checkTransform(player_locks);

	// change hero powers on transformation
	if (player->setPowers) {
		player->setPowers = false;
		if (!player->stats.humanoid && menu->pow->visible) menu->closeRight();
		// save ActionBar state and lock slots from removing/replacing power
		for (int i = 0; i < MenuActionBar::SLOT_MAX ; i++) {
			player_actionbar->hotkeys_temp[i] = player_actionbar->hotkeys[i];
			player_actionbar->hotkeys[i] = 0;
		}
		int count = MenuActionBar::SLOT_MAIN1;
		// put creature powers on action bar
		for (size_t i=0; i<player->charmed_stats->powers_ai.size(); i++) {
			if (powers->isValid(player->charmed_stats->powers_ai[i].id) && powers->powers[player->charmed_stats->powers_ai[i].id]->beacon != true) {
				player_actionbar->hotkeys[count] = player->charmed_stats->powers_ai[i].id;
				player_actionbar->locked[count] = true;
				count++;
				if (count == MenuActionBar::SLOT_MAX)
					count = 0;
				else if (count == MenuActionBar::SLOT_MAIN1)
					// we've filled the actionbar, stop adding powers to it
					break;
			}
		}
		if (player->stats.manual_untransform && powers->isValid(player->untransform_power)) {
			player_actionbar->hotkeys[count] = player->untransform_power;
			player_actionbar->locked[count] = true;
		}
		else if (player->stats.manual_untransform && player->untransform_power == 0)
			Utils::logError("GameStatePlay: Untransform power not found, you can't untransform manually");

		player_actionbar->updated = true;

		// reapply equipment if the transformation allows it
		if (player->stats.transform_with_equipment)
			player_inventory->applyEquipment();
	}
	// revert hero powers
	if (player->revertPowers) {
		player->revertPowers = false;

		// restore ActionBar state
		for (int i = 0; i < MenuActionBar::SLOT_MAX; i++) {
			player_actionbar->hotkeys[i] = player_actionbar->hotkeys_temp[i];
			player_actionbar->locked[i] = false;
		}

		player_actionbar->updated = true;

		// also reapply equipment here, to account items that give bonuses to base stats
		player_inventory->applyEquipment();
	}

	// when the hero (re)spawns, reapply equipment & passive effects
	if (player->respawn) {
		player->stats.alive = true;
		player->stats.corpse = false;
		player->stats.cur_state = StatBlock::ENTITY_STANCE;
		player_inventory->applyEquipment();
		menu->inv->changed_equipment = true;
		checkEquipmentChange();
		player->stats.hp = player->stats.get(Stats::HP_MAX);
		player->stats.logic();
		player->stats.recalc();
		menu->pow->resetToBasePowers();
		menu->pow->setUnlockedPowers();
		powers->activatePassives(&player->stats);
		player->respawn = false;
	}

	// use a normal mouse cursor is menus are open
	if (menu->menus_open) {
		curs->setCursor(CursorManager::CURSOR_NORMAL);
	}

	// update the action bar as it may have been changed by items
	if (player_actionbar->updated) {
		player_actionbar->updated = false;

		// set all hotkeys to their base powers
		for (unsigned i = 0; i < player_actionbar->slots_count; i++) {
			player_actionbar->hotkeys_mod[i] = player_actionbar->hotkeys[i];
		}

		updateActionBar(UPDATE_ACTIONBAR_ALL);
	}

	// reload music if changed in the pause menu
	if (menu->exit->reload_music) {
		mapr->loadMusic();
		menu->exit->reload_music = false;
	}

	player_locks.copyTo(*inpt);

	// Last thing in the tick, after both the simulation and the menus have had their say.
	drainSimEvents();
}

/**
 * Play everything the simulation reported this tick, then empty the queue.
 *
 * A headless server empties it without playing. That is the point of the queue: the simulation no
 * longer needs a sound manager to be correct, only a client does.
 *
 * The queue is cleared on EVERY path, including the headless one. An emit with no drain is a leak
 * on a server that runs for hours; see SimEventQueue::getHighWater().
 */
void GameStatePlay::drainSimEvents() {
	if (!settings->headless) {
		const std::vector<SimEvent>& q = sim_events->events();
		for (size_t i = 0; i < q.size(); ++i) {
			const SimEvent& e = q[i];

			if (e.stop) {
				snd->pauseChannel(e.channel);
				continue;
			}
			if (e.candidates.empty())
				continue;

			// The client chooses the variant, not the simulation. Two players may be running
			// different sound mods, and which of three grunts you hear is nobody else's business.
			const size_t pick = e.select ? fx_rng->index(e.candidates.size()) : 0;
			const SoundID sid = e.candidates[pick];

			std::string channel = e.channel;
			if (e.channel_by_sound) {
				std::stringstream ss;
				ss << e.channel << sid;
				channel = ss.str();
			}
			else if (channel.empty()) {
				channel = SoundManager::DEFAULT_CHANNEL;
			}

			snd->play(sid, channel, e.use_pos ? e.pos : SoundManager::NO_POS, e.loop, e.cleanup);
		}
	}

	sim_events->clear();
}


/**
 * Render all graphics for a single frame
 */
void GameStatePlay::render() {
	if (mapr->is_spawn_map)
		return;

	// Create a list of Renderables from all objects not already on the map.
	// split the list into the beings alive (may move) and dead beings (must not move)
	std::vector<Renderable> rens;
	std::vector<Renderable> rens_dead;

	player->addRenders(rens);

	entitym->addRenders(rens, rens_dead);

	npcs->addRenders(rens); // npcs cannot be dead

	loot->addRenders(rens, rens_dead);

	hazards->addRenders(rens, rens_dead);


	// render the static map layers plus the renderables
	mapr->render(rens, rens_dead);

	// mouseover tooltips
	loot->renderTooltips(mapr->cam.pos);

	if (mapr->map_change) {
		menu->mini->prerender(&mapr->collider, mapr->w, mapr->h);
		mapr->map_change = false;
	}
	menu->mini->setMapTitle(mapr->title);
	menu->mini->render(player->stats.pos);
	menu->region_title->setTitle(mapr->title);
	menu->render();

	// render combat text last - this should make it obvious you're being
	// attacked, even if you have menus open
	if (!isPaused())
		comb->render();
}

bool GameStatePlay::isPaused() {
	// A menu asking for a pause is a request, and this is where it is granted or refused.
	//
	// Headless refuses, always. A server must not stop simulating because a menu object it never
	// renders happens to be flagged visible -- and it can be: MenuManager sets the flag from the
	// exit menu, the dev console, an open book and both item pickers, none of which need a
	// display to exist. Before this, main_server's loop read isPaused() through GameSwitcher and
	// would have stopped advancing time.
	//
	// This is also the seam for Phase 3. A pause is a single-player affordance: with eight
	// players sharing a world, one of them opening a menu must not freeze the other seven. The
	// condition becomes "not headless AND not a multiplayer session"; single-player keeps today's
	// feel, which is why the client half is unchanged here.
	return !settings->headless && menu->pause_requested;
}

void GameStatePlay::resetNPC() {
	if (menu->vendor->visible) {
		snd->play(menu->vendor->sfx_close, snd->DEFAULT_CHANNEL, snd->NO_POS, !snd->LOOP);
	}

	npc_id = -1;
	menu->talker->npc_from_map = true;
	menu->resetDrag();
	menu->vendor->setNPC(NULL);
	menu->talker->setNPC(NULL);
}

bool GameStatePlay::checkPrimaryStat(const std::string& first, const std::string& second) {
	int high = 0;
	size_t high_index = eset->primary_stats.list.size();
	size_t low_index = eset->primary_stats.list.size();

	for (size_t i = 0; i < eset->primary_stats.list.size(); ++i) {
		int stat = player->stats.get_primary(i);
		if (stat > high) {
			if (high_index != eset->primary_stats.list.size()) {
				low_index = high_index;
			}
			high = stat;
			high_index = i;
		}
		else if (stat == high && low_index == eset->primary_stats.list.size()) {
			low_index = i;
		}
		else if (low_index == eset->primary_stats.list.size() || (low_index < eset->primary_stats.list.size() && stat > player->stats.get_primary(low_index))) {
			low_index = i;
		}
	}

	// if the first primary stat doesn't match, we don't care about the second one
	if (high_index != eset->primary_stats.list.size() && first != eset->primary_stats.list[high_index].id)
		return false;

	if (!second.empty()) {
		if (low_index != eset->primary_stats.list.size() && second != eset->primary_stats.list[low_index].id)
			return false;
	}
	else if (player->stats.get_primary(high_index) == player->stats.get_primary(low_index)) {
		// titles that require a single stat are ignored if two stats are equal
		return false;
	}

	return true;
}

GameStatePlay::~GameStatePlay() {
	curs->setLowHP(false);

	delete quests;
	delete npcs;
	delete hazards;
	// D26 (P2.5 step 5): playerm->remove(0) below expires any in-flight hazard the local player
	// owns, so hazards must already read as safely-NULL here rather than dangling from the delete
	// above -- see main_server.cpp::serverCleanup()'s matching comment.
	hazards = NULL;
	delete entitym;
	delete mapr;
	delete menu;
	// After delete menu, not before -- pinv/pab must outlive MenuInventory/MenuActionBar, which
	// bind into them (see the constructor's matching comment). playerm->remove(0) frees pc/pinv/
	// pab/pbs together and NULLs the four aliases itself (PlayerManager::remove()).
	playerm->remove(0);
	delete playerm;
	delete loot;
	delete camp;
	delete items;
	delete powers;
	delete fow;
	delete xp_scaling;

	delete enemyg;

	delete eventm;

	// NULL-ify shared game resources
	playerm = NULL;
	menu = NULL;
	camp = NULL;
	enemyg = NULL;
	entitym = NULL;
	eventm = NULL;
	items = NULL;
	loot = NULL;
	mapr = NULL;
	wmap = NULL; // does not own the object mapr's delete above already freed
	menu_act = NULL;
	menu_powers = NULL;
	powers = NULL;
	fow = NULL;
	xp_scaling = NULL;
}

