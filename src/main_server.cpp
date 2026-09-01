/*
Copyright © 2026 Flare LAN Revival contributors

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
 * flare-server entry point.
 *
 * Runs the simulation with no window, no GPU and no audio device, using the null devices
 * from DeviceList. Through P1.4b this ran main.cpp's client game-state machine
 * (GameSwitcher -> GameStateTitle/GameStateLoad/GameStatePlay -> MenuManager) headlessly, backed
 * by null devices -- the "headless" property came from the devices, not from actually not
 * running the client stack. P1.4c is what stops that: this file now constructs the simulation's
 * own objects directly (the same ones GameStatePlay's constructor builds, minus menu/mapr/quests
 * -- see SharedGameResources.h's wmap/mapr note) and drives them with a tick function ported from
 * GameStatePlay::logic(), not through any GameState at all. See plans/phase1/P1.4c's plan doc for
 * the full classification this rewrite is built from.
 *
 * P3.3 adds real networking: --dedicated opens a NetworkManager host socket and every tick's
 * per-player command loop (see serverSyncNetworkPlayers()/serverLogic()) drives connected peers'
 * Avatars for real, not just player 0's. See plans/phase3/P3.3-authoritative-server-tick.md.
 */

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <cstdlib>

#include "ActionBarState.h"
#include "AnimationManager.h"
#include "Animation.h"
#include "Avatar.h"
#include "CampaignManager.h"
#include "PlayerCommand.h"
#include "PlayerInventory.h"
#include "PlayerManager.h"
#include "PowerBonusState.h"
#include "SharedGameResources.h"
#include "CombatText.h"
#include "CommonIncludes.h"
#include "EngineSettings.h"
#include "EnemyGroupManager.h"
#include "EntityBehavior.h"
#include "EntityManager.h"
#include "EventManager.h"
#include "FileParser.h"
#include "FogOfWar.h"
#include "FontEngine.h"
#include "HazardManager.h"
#include "InputState.h"
#include "LootManager.h"
#include "Map.h"
#include "MenuActionBar.h"
#include "MenuManager.h"
#include "MessageEngine.h"
#include "ModManager.h"
#include "NPCManager.h"
#include "net/NetProtocol.h"
#include "net/NetworkManager.h"
#include "PowerManager.h"
#include "QuestLog.h"
#include "RenderDevice.h"
#include "Replay.h"
#include "SimEvents.h"
#include "StatBlock.h"
#include "WorldHash.h"
#include "Rng.h"
#include "SaveLoad.h"
#include "Settings.h"
#include "SharedResources.h"
#include "SoundManager.h"
#include "Stats.h"
#include "TooltipManager.h"
#include "Utils.h"
#include "UtilsFileSystem.h"
#include "UtilsParsing.h"
#include "Version.h"
#include "XPScaling.h"

#include "NullFontEngine.h"
#include "NullInputState.h"
#include "NullRenderDevice.h"
#include "NullSoundManager.h"

#include <SDL.h>

// Character titles (engine/titles.txt). GameStatePlay::loadTitles()/checkTitle()'s server-side
// equivalents. Title itself is a plain data class declared in GameStatePlay.h -- duplicated here
// rather than pulled in via that header, the same call ActionBarState/PlayerInventory already
// make for their own small presentation-adjacent constants (see e.g. ActionBarState::MENU_COUNT).
class Title {
public:
	std::string title;
	int level;
	PowerID power;
	std::vector<StatusID> requires_status;
	std::vector<StatusID> requires_not_status;
	std::string primary_stat_1;
	std::string primary_stat_2;

	Title()
		: title("")
		, level(0)
		, power(0)
		, requires_status()
		, requires_not_status()
		, primary_stat_1("")
		, primary_stat_2("") {
	}
};

static std::vector<Title> server_titles;

// Set true the moment a permadeath character's death finishes teleport-processing (see
// serverCheckTeleport()) -- the server-appropriate equivalent of the client switching to
// GameStateTitle, which does not exist here.
static bool server_exit_requested = false;

// GameStatePlay::checkTeleport()'s is_first_map_load, moved here for the same reason: without
// it, the server's very first tick would call save_load->saveGame() (eset->misc.save_onload
// defaults true) on a character that was just loaded from that exact save -- harmless to the
// corpus's digest, but a real, avoidable divergence from what the client does on the same input.
static bool server_is_first_map_load = true;

// GameStatePlay's MenuInventory::changed_equipment, tracked directly since no MenuInventory
// exists headless. Genuinely load-bearing, not cosmetic: an avatar's own loadAnimations() (called
// from serverCheckEquipmentChange() below when that avatar's flag is true) unconditionally
// rebuilds Entity::anims and calls setAnimation("stance") on the way (Entity::loadAnimations(),
// Entity.cpp), which resets activeAnimation to frame 0 -- including the ATTACK animation
// mid-cast. Calling loadAnimations() every tick regardless of this flag -- P1.4c's first attempt,
// on the theory that it was cheap and safe to over-call -- silently froze every power cast:
// Avatar::logic()'s ENTITY_POWER case only calls powers->activate() on
// activeAnimation->isActiveFrame(), which a frame-0 reset every tick can never reach. Found by
// bisecting the replay corpus's melee digest, which only reads as "wrong" hundreds of ticks
// downstream (enemies never take damage, never die, no loot, no currency) -- the actual break is
// a stall on the very first attack. Starts true to match MenuInventory's own constructor default
// (MenuInventory.cpp), which is what makes the *very first* logic tick redundantly reload
// animations already loaded once by SaveLoad::loadGame() -- harmless duplication, replicated for
// fidelity rather than special-cased away.
//
// P2.3b: one flag per PlayerID (D3's 8-player cap -- PlayerManager.h), not one global bool. Every
// trigger site for this flag (below, in serverLogic()) is currently input-driven and only
// playerm->local() ever receives real per-tick input, so in every scenario this plan can test only
// index local_id is ever set true -- but the CHECK-and-clear itself has no input dependency (see
// serverCheckEquipmentChange() below), so it is kind C: checked for every player, not defaulted to
// a single global.
static bool server_equipment_changed[8] = { true, true, true, true, true, true, true, true };

// quests was a private GameStatePlay member, not a SharedGameResources.h global -- nothing else
// in the sim reads a global named "quests", so this server holds its own pointer, purely to
// construct QuestLog once (for camp->registerStatus()'s side effect -- see serverConstructSim())
// and delete it once. Never dereferenced beyond that; see the construction site's own comment.
static QuestLog* server_quests = NULL;

// P3.3. NULL unless --dedicated was passed -- every use below is gated on that, so a non-dedicated
// run (today's only mode before this plan) never touches it.
static Net::NetworkManager* netmgr = NULL;
static const unsigned short DEFAULT_SERVER_PORT = 44680; // arbitrary, dynamic/private range

class ServerCmdLineArgs {
public:
	ServerCmdLineArgs()
		: mod_list(), load_slot(), data_path(), max_ticks(0), hash_every(0), hash_at_exit(false)
		, hash_replicated(false)
		, sim_seed(RNG_DEFAULT_SIM_SEED), record_path(), replay_path(), dump_players(false)
		, assert_player_wiring(false), spawn_test_players(0), test_player_summon(0)
		, dump_ai_targets(false), dump_summon_prototypes(false), dump_damage_events(false), kill_player(-1)
		, dedicated(false), port(0), max_players(8) {}
	std::vector<std::string> mod_list;
	std::string load_slot;
	std::string data_path;
	unsigned long max_ticks;
	unsigned long hash_every;
	bool hash_at_exit;
	// P3.7: WorldHash::computeReplicated() instead of compute() for --hash-every's periodic print
	// -- so a headless flare client's own --hash-every output is directly comparable, line for
	// line, to this process's. Does not affect --hash (hash_at_exit), which stays the full digest.
	bool hash_replicated;
	uint64_t sim_seed;
	std::string record_path;
	std::string replay_path;
	bool dump_players;
	bool assert_player_wiring;

	// P2.2 AC5-AC7 test infrastructure -- see the P2.2 report for what this can and cannot
	// prove. None of this touches SaveLoad.cpp (out of scope): test players are constructed
	// directly here by cloning player 0's already-loaded state, not through a save file.
	int spawn_test_players;      // total player count, including the one --load-slot already loads
	PowerID test_player_summon;  // if nonzero, bound to the last spawned test player's action bar
	bool dump_ai_targets;
	bool dump_summon_prototypes;
	bool dump_damage_events;
	int kill_player;

	// P3.3: real network multiplayer. All three are no-ops unless dedicated is set -- see
	// plans/phase3/P3.3-authoritative-server-tick.md's Scope note.
	bool dedicated;
	unsigned short port;    // 0 means "use DEFAULT_SERVER_PORT", set below main()
	int max_players;        // includes the reserved local id 0 -- see serverInitNetwork()
};

// The Platform implementations are compiled by #include-ing them into the entry point rather
// than as translation units of their own, so the server has to repeat what main.cpp does or
// the global 'platform' instance is never defined. Only the desktop platforms are listed: a
// dedicated server on Android, iOS, GCW0 or Emscripten is not a thing we intend to support.
#define PLATFORM_CPP_INCLUDE

#ifdef _WIN32
#include "PlatformWin32.cpp"
#else
// Linux stuff should work on Mac OSX/BSD/etc, too
#include "PlatformLinux.cpp"
#endif

// Set from a signal handler, so it must be sig_atomic_t and volatile. Nothing else may be
// touched from the handler -- logging or freeing from here is not async-signal-safe.
static volatile sig_atomic_t shutdown_requested = 0;

extern "C" void serverSignalHandler(int sig) {
	(void)sig;
	shutdown_requested = 1;
}

// Ported unchanged from GameStatePlay::loadTitles() -- pure FileParser parsing, no presentation
// reference at all.
static void serverLoadTitles() {
	FileParser infile;
	if (infile.open("engine/titles.txt", FileParser::MOD_FILE, FileParser::ERROR_NORMAL)) {
		while (infile.next()) {
			if (infile.new_section && infile.section == "title") {
				Title t;
				server_titles.push_back(t);
			}

			if (server_titles.empty()) continue;

			Title& title = server_titles.back();

			if (infile.key == "title") {
				title.title = infile.val;
			}
			else if (infile.key == "level") {
				title.level = Parse::toInt(infile.val);
			}
			else if (infile.key == "power") {
				title.power = powers->verifyID(Parse::toPowerID(infile.val), &infile, !PowerManager::ALLOW_ZERO_ID);
			}
			else if (infile.key == "requires_status") {
				std::string repeat_val = Parse::popFirstString(infile.val);
				while (!repeat_val.empty()) {
					title.requires_status.push_back(camp->registerStatus(repeat_val));
					repeat_val = Parse::popFirstString(infile.val);
				}
			}
			else if (infile.key == "requires_not_status") {
				std::string repeat_val = Parse::popFirstString(infile.val);
				while (!repeat_val.empty()) {
					title.requires_not_status.push_back(camp->registerStatus(repeat_val));
					repeat_val = Parse::popFirstString(infile.val);
				}
			}
			else if (infile.key == "primary_stat") {
				title.primary_stat_1 = Parse::popFirstString(infile.val);
				title.primary_stat_2 = Parse::popFirstString(infile.val);
			}
			else infile.error("main_server: '%s' is not a valid key.", infile.key.c_str());
		}
		infile.close();
	}
}

// Ported unchanged from GameStatePlay::checkPrimaryStat(), except reading the given player's own
// stats (a parameter) instead of the global pc -- P2.3b, kind C: title/subclass earning has no
// input dependency at all (unlike most of serverLogic()'s single-input-driven block below), so it
// applies to every player, not just whichever one real input happens to be driving this tick. See
// serverCheckTitle()'s own call site in serverLogic() for the per-player loop.
static bool serverCheckPrimaryStat(const StatBlock& stats, const std::string& first, const std::string& second) {
	int high = 0;
	size_t high_index = eset->primary_stats.list.size();
	size_t low_index = eset->primary_stats.list.size();

	for (size_t i = 0; i < eset->primary_stats.list.size(); ++i) {
		int stat = stats.get_primary(i);
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
		else if (low_index == eset->primary_stats.list.size() || (low_index < eset->primary_stats.list.size() && stat > stats.get_primary(low_index))) {
			low_index = i;
		}
	}

	if (high_index != eset->primary_stats.list.size() && first != eset->primary_stats.list[high_index].id)
		return false;

	if (!second.empty()) {
		if (low_index != eset->primary_stats.list.size() && second != eset->primary_stats.list[low_index].id)
			return false;
	}
	else if (stats.get_primary(high_index) == stats.get_primary(low_index)) {
		// titles that require a single stat are ignored if two stats are equal
		return false;
	}

	return true;
}

// Ported unchanged from GameStatePlay::checkTitle() -- 100% sim, no presentation reference at all.
// P2.3b, kind C (see serverCheckPrimaryStat() above): player is passed in and this is called once
// per player from serverLogic(), not just for whichever player has real input this tick.
static void serverCheckTitle(Avatar* player) {
	if (!player->stats.check_title || server_titles.empty())
		return;

	int title_id = -1;

	for (unsigned i = 0; i < server_titles.size(); i++) {
		if (server_titles[i].title.empty())
			continue;

		if (server_titles[i].level > 0 && player->stats.level < server_titles[i].level)
			continue;
		if (server_titles[i].power > 0 && std::find(player->stats.powers_list.begin(), player->stats.powers_list.end(), server_titles[i].power) == player->stats.powers_list.end())
			continue;
		if (!server_titles[i].primary_stat_1.empty() && !serverCheckPrimaryStat(player->stats, server_titles[i].primary_stat_1, server_titles[i].primary_stat_2))
			continue;

		bool status_failed = false;
		for (size_t j = 0; j < server_titles[i].requires_status.size(); ++j) {
			if (!camp->checkStatus(server_titles[i].requires_status[j])) {
				status_failed = true;
				break;
			}
		}
		for (size_t j = 0; j < server_titles[i].requires_not_status.size(); ++j) {
			if (camp->checkStatus(server_titles[i].requires_not_status[j])) {
				status_failed = true;
				break;
			}
		}

		if (status_failed)
			continue;

		title_id = static_cast<int>(i);
		break;
	}

	if (title_id != -1) player->stats.character_subclass = server_titles[static_cast<size_t>(title_id)].title;
	player->stats.check_title = false;
	player->stats.refresh_stats = true;
}

// The sim-relevant subset of GameStatePlay::resetGame(). Dropped: menu->questlog/menu->hudlog
// clearing (no widgets), quests->createQuestList() (QuestLog isn't constructed -- see P1.4c's
// plan doc, "Dropping QuestLog"), menu->talker->setHero() (widget), player->loadSounds() (audio
// asset queueing, presentation-lifetime per P1.2). menu->act->clear()'s STATE half -- nothing
// else clears the action bar's slots at startup, since no MenuActionBar ever parses
// menus/actionbar.txt here -- is replaced by looping actionbar->clearSlot() directly.
//
// P2.3b, kind A: only ever called from serverLoadGame(), itself a --load-slot helper -- SaveLoad's
// slot model is still single-character (P4.1 unimplemented), so there is exactly one player whose
// game this resets. Bound explicitly to playerm->local() rather than left an implicit global read.
static void serverResetGame() {
	Avatar* local = playerm->local();
	PlayerInventory* local_inv = playerm->inventoryFor(playerm->local_id);
	ActionBarState* local_ab = playerm->actionbarFor(playerm->local_id);

	camp->resetAllStatuses();
	local->init();
	local->stats.currency = 0;
	for (unsigned i = 0; i < local_ab->slots_count; ++i)
		local_ab->clearSlot(i);
	local_inv->inventory[0].clear();
	local_inv->inventory[1].clear();
	local_inv->currency = 0;

	wmap->teleportation = true;
	wmap->teleport_mapname = "maps/spawn.txt";
}

// The non-widget data flow of GameStateLoad::logicLoading() -- construct-then-load, with no slot
// browser: --load-slot names the slot directly (the same number SaveLoad::setGameSlot() takes),
// not an index into a directory scan of existing saves the way GameStateLoad's own
// settings->load_slot handling works. A headless server has no character-creation UI
// (GameStateNew, and the SaveLoad::loadClass() path it drives -- both left with their existing
// unconditional menu-> dereferences, deliberately not fixed: see the SaveLoad.cpp fix commit),
// so an empty --load-slot is a hard error rather than a silent "new game": every corpus fixture,
// and every real host, loads an existing save.
static bool serverLoadGame(const std::string& load_slot_arg) {
	if (load_slot_arg.empty()) {
		Utils::logError("main_server: --load-slot is required (no character-creation UI exists headless).");
		return false;
	}

	serverResetGame();

	save_load->setGameSlot(Parse::toInt(load_slot_arg));
	save_load->loadGame();
	return true;
}

// The sim-relevant subset of GameStatePlay::checkCutscene() -- GameStateCutscene itself is 100%
// presentation and cannot run headless. wmap->cutscene is reset here even though the client
// never does: the client relies on the whole GameStatePlay/Map being torn down and rebuilt
// across a cutscene transition to clear it, which a headless server -- with no cutscene state to
// transition through -- never does. Left set, this would re-fire
// eset->misc.save_oncutscene's saveGame() every tick forever. Not exercised by the corpus (no
// fixture uses a cutscene event) -- documented, not measured.
//
// P2.3b, kind A: wmap->respawn_point is a single field on the one shared Map (there is no
// per-player map-instancing concept yet -- Phase 3+), so "whose position becomes the respawn
// point" can only mean one player until that exists. Bound explicitly to playerm->local().
static void serverCheckCutscene() {
	if (!wmap->cutscene)
		return;

	if (wmap->teleportation) {
		if (wmap->teleport_mapname != "")
			wmap->respawn_map = wmap->teleport_mapname;
		wmap->respawn_point = wmap->teleport_destination;
	}
	else {
		wmap->respawn_point = playerm->local()->stats.pos;
	}

	if (eset->misc.save_oncutscene)
		save_load->saveGame();

	wmap->cutscene = false;
}

// Ported unchanged from GameStatePlay::checkSaveEvent() (mapr-> renamed wmap->, per P1.4a).
//
// P2.3b, kind A -- not a default, a real constraint: SaveLoad's slot model is still
// single-character (P4.1 unimplemented, save_load->saveGame() writes exactly one player's
// avatar.txt into game_slot). Iterating every player here would serialise N different characters
// into the same save slot, silently keeping only the last -- worse than today's single-player
// behaviour, not more correct. Bound explicitly to playerm->local() until per-player save slots
// exist. wmap->save_game/wmap->respawn_point are themselves single fields on the one shared Map,
// same reasoning as serverCheckCutscene() above.
static void serverCheckSaveEvent() {
	if (wmap->save_game) {
		wmap->respawn_point = playerm->local()->stats.pos;
		save_load->saveGame();
		wmap->save_game = false;
	}
}

// The sim-relevant subset of GameStatePlay::checkTeleport() -- collision block/unblock, entity
// repositioning, on-load/on-exit event execution and respawn-point tracking are all ported
// unchanged (mapr-> renamed wmap->, per P1.4a). Dropped: menu->enemy->handleNewMap() (mouseover
// widget), menu->stash->visible (UI), menu->mini->prerender() (minimap widget),
// showLoading()/setLoadingFrame() (loading-screen state), render_device->cleanupQueuedImages()
// (presentation asset cache), resetNPC() (100% presentation -- see P1.4c's classification
// table). save_load->saveFOW() is kept: the SaveLoad.cpp fix made it safe headless, and
// persisting fog-of-war is genuinely part of "host owns the world".
//
// permadeath-to-title: the client switches to GameStateTitle, which does not exist here. The
// server-appropriate equivalent is ending the simulation -- a permadead character has nothing
// left to simulate, and Phase 3 (the only reason a second player could still be present) is not
// built yet. Sets server_exit_requested rather than looping forever pointed at a map that no
// longer has a living player on it. Not exercised by the corpus (no fixture both enables
// permadeath and teleports after death) -- documented, not measured.
//
// P2.3b, kind A: wmap->teleportation/teleport_mapname/respawn_point/respawn_map are single fields
// on the one shared Map -- true per-player teleport (two players stepping on two different
// teleporters independently) needs per-player map-instancing, which does not exist yet
// (Phase 3+). Bound explicitly to playerm->local() until it does.
//
// P3.6a: forward-declared here (real definition is further down, alongside serverLogic()'s own
// per-player loops that already use it) so this function's own new party-wide loop can reuse the
// established "which players does multi-player code touch" membership test instead of inventing a
// second one.
static bool serverPlayerIsDriven(PlayerID id);

// P3.6b: D23's 3-second countdown-with-cancel for a party-wide (event-triggered) intermap travel.
// See serverCheckTeleport() for the state machine and Map::teleport_from_event's own comment for
// why only EventManager-triggered travel ever engages this.
static bool server_travel_countdown_active = false;
static Timer server_travel_timer;
static bool server_travel_cancel_requested = false;

static void serverCheckTeleport() {
	Avatar* local = playerm->local();
	bool on_load_teleport = false;

	// P3.6b: consume-and-clear teleport_from_event unconditionally, every tick wmap->teleportation
	// is set at all, so it can never survive stale into a later, unrelated, administrative
	// teleport (see Map::teleport_from_event's own comment).
	bool travel_countdown_started_this_tick = false;
	if (wmap->teleportation) {
		bool from_event = wmap->teleport_from_event;
		wmap->teleport_from_event = false;

		// D23's countdown only ever applies when there's an actual party to coordinate with --
		// single-player (and every existing replay-corpus fixture, which is always exactly one
		// player) takes neither branch below, and an intermap walk stays exactly as instant as it
		// always has been. See AC-REPLAY.
		if (from_event && playerm->players.size() > 1) {
			if (!server_travel_countdown_active) {
				server_travel_countdown_active = true;
				travel_countdown_started_this_tick = true;
				server_travel_timer.setDuration(3 * Settings::SIM_TICK_HZ);

				std::vector<MessageArg> args;
				args.push_back(MessageArg(wmap->teleport_mapname));
				args.push_back(MessageArg(3));
				if (netmgr)
					netmgr->broadcast(Net::encodeSystemMessage("Traveling to %s in %d seconds...", args));
			}
			// else: a second event trigger while a countdown is already running -- the party is
			// already committed to leaving via the first one. Dropped, not restarted, not stacked.

			wmap->teleportation = false; // held -- teleport_mapname/teleport_destination/etc are
			                              // left exactly as EventManager.cpp just set them,
			                              // untouched, until the countdown ends (or is cancelled,
			                              // below)
		}
	}

	if (server_travel_countdown_active && !travel_countdown_started_this_tick) {
		if (server_travel_cancel_requested) {
			server_travel_countdown_active = false;
			server_travel_cancel_requested = false;
			wmap->teleport_mapname = "";
			if (netmgr)
				netmgr->broadcast(Net::encodeSystemMessage("Party travel cancelled.", std::vector<MessageArg>()));
		}
		else if (server_travel_timer.tick()) {
			server_travel_countdown_active = false;
			wmap->teleportation = true; // let the unmodified block below run, this same tick
		}
	}

	if (wmap->teleportation || local->stats.teleportation) {

		if (wmap->fogofwar)
			if (fow->fog_layer_id != 0)
				fow->handleIntramapTeleport();

		if (playerm->players.size() > 1)
			wmap->collider.unblockPlayer(local->stats.pos.x, local->stats.pos.y, local->id);
		else
			wmap->collider.unblock(local->stats.pos.x, local->stats.pos.y);

		if (wmap->teleportation) {
			local->stats.pos.x = wmap->teleport_destination.x;
			local->stats.pos.y = wmap->teleport_destination.y;
			local->teleport_camera_lock = true;
		}
		else {
			local->stats.pos.x = local->stats.teleport_destination.x;
			local->stats.pos.y = local->stats.teleport_destination.y;
		}

		// if we're not changing map, move allies to the player's new position
		if (wmap->teleport_mapname.empty()) {
			FPoint spawn_pos = wmap->collider.getRandomNeighbor(Point(local->stats.pos), 1, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_ALL_ENTITIES);
			for (unsigned int i = 0; i < entitym->entities.size(); i++) {
				if (entitym->entities[i]->stats.hero_ally && entitym->entities[i]->stats.alive && entitym->entities[i]->stats.speed > 0) {
					wmap->collider.unblock(entitym->entities[i]->stats.pos.x, entitym->entities[i]->stats.pos.y);
					entitym->entities[i]->stats.pos = spawn_pos;
					wmap->collider.block(entitym->entities[i]->stats.pos.x, entitym->entities[i]->stats.pos.y, MapCollision::IS_ALLY);
				}
			}
		}

		// process intermap teleport
		if (wmap->teleportation && !wmap->teleport_mapname.empty()) {
			wmap->executeOnMapExitEvents();
			std::string teleport_mapname = wmap->teleport_mapname;
			wmap->teleport_mapname = "";
			inpt->lock_all = (teleport_mapname == "maps/spawn.txt");
			save_load->saveFOW();
			wmap->load(teleport_mapname);

			// use the default hero spawn position for this map
			if (wmap->force_spawn_pos || (wmap->teleport_destination.x == -1 && wmap->teleport_destination.y == -1)) {
				local->stats.pos.x = wmap->hero_pos.x;
				local->stats.pos.y = wmap->hero_pos.y;

				if (wmap->teleport_destination_id > 0) {
					for (size_t i = 0; i < wmap->events.size(); ++i) {
						EventComponent* ec_hero_pos = wmap->events[i].getComponent(EventComponent::INTERMAP_ID);
						if (ec_hero_pos && ec_hero_pos->data[0].Int == wmap->teleport_destination_id) {
							local->stats.pos.x = static_cast<float>(wmap->events[i].location.x) + 0.5f;
							local->stats.pos.y = static_cast<float>(wmap->events[i].location.y) + 0.5f;
							break;
						}
					}
				}
			}

			// store this as the new respawn point (provided the tile is open)
			if (wmap->collider.isValidPosition(local->stats.pos.x, local->stats.pos.y, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_HERO)) {
				wmap->respawn_map = teleport_mapname;
				wmap->respawn_point = local->stats.pos;
			}
			else {
				Utils::logError("main_server: Spawn position (%d, %d) is blocked.", static_cast<int>(local->stats.pos.x), static_cast<int>(local->stats.pos.y));
			}

			local->handleNewMap();
			hazards->handleNewMap();
			loot->handleNewMap();
			powers->handleNewMap(&wmap->collider);

			// switch off teleport flag so we can check if an on_load event has teleportation
			wmap->teleportation = false;

			wmap->executeOnLoadEvents();
			if (wmap->teleportation)
				on_load_teleport = true;

			// enemies and npcs should be initialized AFTER on_load events execute
			entitym->handleNewMap();
			npcs->handleNewMap();

			// P3.6a: every OTHER currently driven player travels along too (D12 -- one map is loaded
			// at a time, the party travels together). Purely additive: local's own handling above and
			// below this loop is untouched line-for-line, so a single-player run
			// (playerm->players.size() == 1, the only case the replay corpus can exercise) takes this
			// loop zero times and is byte-for-byte unaffected -- see AC-REPLAY.
			//
			// Deliberately NOT handled here: a non-local player's own PERSONAL teleport
			// (player->stats.teleportation, e.g. a blink power) -- serverCheckTeleport() has only ever
			// read local->stats.teleportation for that, unchanged by this plan; only the SHARED
			// wmap->teleportation intermap case is generalised. See P3.6a's own Why/Out of scope.
			for (size_t p = 0; p < playerm->players.size(); ++p) {
				Avatar* player = playerm->players[p];
				if (player->id == local->id || !serverPlayerIsDriven(player->id))
					continue;

				// Scatter around local's own (already-computed, a few lines above) landing spot
				// rather than stacking everyone on the exact same tile -- same idiom already used a
				// few lines above this function for ally-repositioning on an intramap move.
				FPoint dest = wmap->collider.getRandomNeighbor(Point(local->stats.pos), 1, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_ALL_ENTITIES);
				if (wmap->collider.isOutsideMap(dest.x, dest.y)) {
					Utils::logError("main_server: Party-travel position for player %u is outside of map bounds.", static_cast<unsigned>(player->id));
					dest.x = 0.5f;
					dest.y = 0.5f;
				}
				player->stats.pos = dest;
				player->teleport_camera_lock = true; // self-clears next tick server-side (mapr==NULL, Avatar.cpp:639) -- same one-tick movement settle local already gets
				player->handleNewMap();

				// P3.6b: tell this peer's own client which map it just landed on and where --
				// MSG_MAP_SYNC was previously sent only once, at join (P3.5a); this is its second
				// and only other call site, on completion of a party-wide travel. Closes the gap
				// MsgMapSync's own comment named: "not a response to later party travel -- P3.6's
				// job."
				if (netmgr)
					netmgr->sendTo(player->id, Net::encodeMapSync(wmap->getFilename(), dest.x, dest.y));

				if (playerm->players.size() > 1)
					wmap->collider.blockPlayer(dest.x, dest.y, player->id);
				else
					wmap->collider.block(dest.x, dest.y, !MapCollision::IS_ALLY);
			}

			// return to title (permadeath) OR auto-save
			if (local->stats.permadeath && local->stats.cur_state == StatBlock::ENTITY_DEAD) {
				Utils::logInfo("main_server: permadeath character died -- ending simulation.");
				server_exit_requested = true;
			}
			else if (eset->misc.save_onload) {
				if (!server_is_first_map_load)
					save_load->saveGame();
				else
					server_is_first_map_load = false;
			}
		}

		if (wmap->collider.isOutsideMap(local->stats.pos.x, local->stats.pos.y)) {
			Utils::logError("main_server: Teleport position is outside of map bounds.");
			local->stats.pos.x = 0.5f;
			local->stats.pos.y = 0.5f;
		}

		if (playerm->players.size() > 1)
			wmap->collider.blockPlayer(local->stats.pos.x, local->stats.pos.y, local->id);
		else
			wmap->collider.block(local->stats.pos.x, local->stats.pos.y, !MapCollision::IS_ALLY);

		local->stats.teleportation = false;

		if (settings->mouse_move) {
			local->mm_target_object = Avatar::MM_TARGET_NONE;
			local->setDesiredMMTarget(local->stats.pos);
		}
	}

	// P3.6d: a non-local player's own PERSONAL teleport (stats.teleportation, e.g. a blink power).
	// Always intramap -- StatBlock has no teleport_mapname field, matching the outline's own
	// "Intramap: blink power... None [coordination needed]" row, so no countdown, no map load, no
	// wmap involvement at all, just a per-player position correction. PowerManager::buff() already
	// sets this correctly for whichever player's own StatBlock cast the power (the
	// player->action_queue = cmd.actions line above is what makes that activation reachable at all
	// for a connected peer); this loop is purely the consumption side, mirroring P3.6a's own
	// party-repositioning loop's shape.
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		Avatar* player = playerm->players[p];
		if (player->id == local->id || !serverPlayerIsDriven(player->id) || !player->stats.teleportation)
			continue;

		if (playerm->players.size() > 1)
			wmap->collider.unblockPlayer(player->stats.pos.x, player->stats.pos.y, player->id);
		else
			wmap->collider.unblock(player->stats.pos.x, player->stats.pos.y);

		player->stats.pos.x = player->stats.teleport_destination.x;
		player->stats.pos.y = player->stats.teleport_destination.y;
		player->teleport_camera_lock = true; // self-clears next tick server-side (mapr==NULL,
		                                      // Avatar.cpp:639) -- same settle P3.6a's own loop uses

		if (wmap->collider.isOutsideMap(player->stats.pos.x, player->stats.pos.y)) {
			Utils::logError("main_server: Personal teleport position for player %u is outside of map bounds.", static_cast<unsigned>(player->id));
			player->stats.pos.x = 0.5f;
			player->stats.pos.y = 0.5f;
		}

		if (playerm->players.size() > 1)
			wmap->collider.blockPlayer(player->stats.pos.x, player->stats.pos.y, player->id);
		else
			wmap->collider.block(player->stats.pos.x, player->stats.pos.y, !MapCollision::IS_ALLY);

		player->stats.teleportation = false;
	}

	if (!on_load_teleport && wmap->teleport_mapname.empty())
		wmap->teleportation = false;
}

// The sim-relevant subset of GameStatePlay::checkLoot() -- auto-pickup only. Dropped: the
// menu->isDragging() guard (no UI, never dragging) and the manual click-pickup branch
// (mapr->cam.pos-based, mouse-only). The caller gates this on the player's own stats.alive,
// matching logic()'s own call site.
//
// P2.3b, kind C: auto-pickup has no input dependency at all -- it only reads a position and
// alive-status -- so every alive player gets their own check against loot near THEM, not just
// whichever player has real input this tick. See the per-player loop in serverLogic().
static void serverCheckLoot(Avatar* player, PlayerInventory* inventory) {
	ItemStack pickup = loot->checkAutoPickup(player->stats.pos);

	if (!pickup.empty()) {
		inventory->add(pickup, PlayerInventory::CARRIED, ItemStorage::NO_SLOT, PlayerInventory::ADD_PLAY_SOUND, PlayerInventory::ADD_AUTO_EQUIP);
		if (items->isValid(pickup.item)) {
			StatusID pickup_status = camp->registerStatus(items->items[pickup.item]->pickup_status);
			camp->setStatus(pickup_status);
		}
		pickup.clear();
	}
}

// The sim-relevant subset of GameStatePlay::checkLootDrop() -- menu->drop_stack and
// menu->inv->drop_stack are UI-triggered overflow queues (drag-and-drop) with no headless
// producer; camp->drop_stack and each player's own PlayerInventory::drop_stack are sim-triggered
// and drain unchanged.
//
// P2.3b: camp->drop_stack is campaign-triggered, session-global loot with no per-player position
// of its own -- kind A, anchored to playerm->local() until campaign events gain their own
// drop-position concept (out of this plan's scope). Each PlayerInventory::drop_stack, by
// contrast, is that player's own equip-overflow queue -- kind C, drained at that player's own
// position for every player, not just local().
static void serverCheckLootDrop() {
	Avatar* local = playerm->local();
	while (!camp->drop_stack.empty()) {
		if (!camp->drop_stack.front().empty()) {
			loot->addLoot(camp->drop_stack.front(), local->stats.pos, LootManager::DROPPED_BY_HERO);
		}
		camp->drop_stack.pop();
	}

	for (size_t p = 0; p < playerm->players.size(); ++p) {
		Avatar* player = playerm->players[p];
		PlayerInventory* inventory = playerm->inventories[p];
		while (!inventory->drop_stack.empty()) {
			if (!inventory->drop_stack.front().empty()) {
				loot->addLoot(inventory->drop_stack.front(), player->stats.pos, LootManager::DROPPED_BY_HERO);
			}
			inventory->drop_stack.pop();
		}
	}
}

// The sim-relevant subset of GameStatePlay::checkLog() -- drains each player's own log_msg so the
// queue doesn't grow unbounded over a long-running server. The pushes into menu->hudlog/
// menu->questlog are dropped along with the widgets they'd update.
//
// P2.3b, kind C: log_msg is per-avatar with no input dependency -- every player's own queue is
// drained, not just local()'s.
static void serverCheckLog() {
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		Avatar* player = playerm->players[p];
		while (!player->log_msg.empty()) {
			player->log_msg.pop();
		}
	}
}

// The sim-relevant subset of GameStatePlay::checkEquipmentChange() -- ported to read
// server_equipment_changed (see its own comment) instead of menu->inv->changed_equipment, which
// doesn't exist without a MenuInventory. Gated exactly like the original: actionbar->updated only
// flips true when equipment genuinely changed, matching GameStatePlay.cpp's own
// checkEquipmentChange() body, not the unconditional version P1.4c shipped first.
//
// player->loadAnimations()/loadStepFX() were ORIGINALLY dropped here as "presentation, sprite
// state" -- wrong, twice over. First: they matter at all, because the power's own cast duration
// is not power-data-only -- Avatar::logic()'s ENTITY_POWER case sets
// power_cast_timers[...]->setDuration(activeAnimation->getDuration()), reading the actual loaded
// sprite ANIMATION's frame timing. Second, and more load-bearing: calling loadAnimations()
// unconditionally every tick (this function's first fix) is actively wrong, not just wasteful --
// Entity::loadAnimations() (Entity.cpp) unconditionally calls setAnimation("stance") on every
// invocation, which resets activeAnimation to frame 0. Called every tick regardless of whether
// gear changed, that stalls Avatar::logic()'s ENTITY_POWER case forever: it only calls
// powers->activate() on activeAnimation->isActiveFrame(), a frame a stance-reset animation can
// never reach starting from frame 0 every tick. Found by bisecting the replay corpus's melee
// digest down to tick 32 -- the tick the player's first attack should have connected but didn't,
// visible as mp never being deducted -- against a freshly regenerated, uncontaminated fixture (a
// prior bisection against a fixture whose on-disk save had drifted from repeated reuse pointed at
// a much later, unrelated tick). Both calls are safe to call headless when server_equipment_changed
// is genuinely true: anim (AnimationManager) and render_device (NullRenderDevice) are already
// constructed in serverInit(), and this exact code path already ran headlessly every tick before
// P1.4c, via the full client stack -- P1.4c changed WHERE it's called from and added the flag
// gate, not whether it's safe to call. loadStepFX() is restored alongside it for the same
// reason: it isn't just presentation either -- it populates sound_steps, whose emptiness gates
// whether Avatar::logic() ever pushes an SFX_STEP SimEvent at all (Avatar.cpp's ENTITY_MOVE case
// checks !sound_steps.empty() before pushing), so a stale/empty sound_steps after a footwear
// change would silently stop "step" coverage exactly like the corpus's own step-event check
// exists to catch.
//
// P2.3b, kind C: the check-and-clear itself has no input dependency (only the flag's trigger
// sites, in serverLogic(), currently do -- see server_equipment_changed's own comment), so this
// runs for every player, keyed by that player's own id.
static void serverCheckEquipmentChange(Avatar* player, PlayerInventory* inventory, ActionBarState* actionbar) {
	if (server_equipment_changed[player->id]) {
		// force the actionbar to update when we change gear
		actionbar->updated = true;

		player->loadAnimations();

		if (player->feet_index != -1) {
			ItemID feet_id = inventory->inventory[PlayerInventory::EQUIPMENT][player->feet_index].item;
			if (items->isValid(feet_id))
				player->loadStepFX(items->items[feet_id]->stepfx);
		}
	}

	server_equipment_changed[player->id] = false;
}

// Ported from GameStatePlay::checkUsedItems() -- the equipped-item loop was already sim-only.
// The carried-item loop calls PlayerInventory::remove() directly instead of menu->inv->remove():
// the special case that wrapper adds (consuming the exact clicked slot) is UI interaction state
// with no simulation meaning outside a live click -- see PlayerInventory.h's own comment.
// PlayerInventory::remove()'s general-case fallback is already the correct sim-side equivalent.
//
// P2.3b, kind A -- not a default: PowerManager::used_items/used_equipped_items (PowerManager.h)
// USED TO be single queues with no per-caster tag at all, so there was no way to tell which
// player's power consumption produced a given entry without PowerManager itself recording the
// caster. That gap was real but unreachable before P3.3 -- only one player ever had real input,
// so "apply every consumption to playerm->local()" was correct by coincidence. P3.3 is the first
// plan where two real players can use an item in the same tick, so it closes the gap for real
// (see PowerManager.h's used_items_caster/used_equipped_items_caster) instead of shipping it
// broken. See this plan's Why section for the full finding.
//
// P3.3. PowerManager::used_items/used_equipped_items are attributed by caster StatBlock* (see
// PowerManager.h's own comment) -- resolved here, by pointer identity only, never dereferenced,
// since main_server.cpp is what knows a StatBlock* maps to a PlayerID via playerm. A caster that
// no longer matches any current player (should be structurally impossible within one tick given
// serverSyncNetworkPlayers()'s drain order -- see this plan's Notes) is skipped rather than acted
// on incorrectly.
static PlayerInventory* serverInventoryForCaster(StatBlock* caster) {
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		if (&playerm->players[p]->stats == caster)
			return playerm->inventories[p];
	}
	return NULL;
}

static void serverCheckUsedItems() {
	for (unsigned i = 0; i < powers->used_items.size(); i++) {
		PlayerInventory* inventory = serverInventoryForCaster(powers->used_items_caster[i]);
		if (inventory)
			inventory->remove(powers->used_items[i], 1);
	}
	for (unsigned i = 0; i < powers->used_equipped_items.size(); i++) {
		PlayerInventory* inventory = serverInventoryForCaster(powers->used_equipped_items_caster[i]);
		if (inventory) {
			inventory->inventory[PlayerInventory::EQUIPMENT].remove(powers->used_equipped_items[i], 1);
			inventory->applyEquipment();
		}
	}
	powers->clearUsedItems();
}

// Ported unchanged from GameStatePlay::updateActionBar() / its UPDATE_ACTIONBAR_ALL constant.
//
// P2.3b, kind C: recomputing hotkeys_mod from hotkeys has no input dependency and no shared state
// with any other player's action bar -- actionbar/inventory are passed in explicitly and this is
// called once per player (see the loop in serverLogic()), not defaulted to the single local pair.
static const unsigned SERVER_ACTIONBAR_ALL = 0;
static void serverUpdateActionBar(ActionBarState* actionbar, PlayerInventory* inventory, unsigned index) {
	if (actionbar->slots_count == 0 || index > actionbar->slots_count - 1) return;
	if (items->items.empty()) return;

	for (unsigned i = index; i < actionbar->slots_count; i++) {
		if (actionbar->hotkeys[i] == 0) continue;

		PowerID id = inventory->getPowerMod(actionbar->hotkeys_mod[i]);
		if (id > 0) {
			actionbar->hotkeys_mod[i] = id;
			return serverUpdateActionBar(actionbar, inventory, i);
		}
	}
}

// Forward declaration -- defined near serverSpawnTestPlayers() below, which shares this same
// per-id provisioning mechanism (see that function's own comment). serverSyncNetworkPlayers()
// needs it earlier in the file, at the real connect-time call site.
static Avatar* serverProvisionPlayer(PlayerID id, FPoint spawn_pos);

// P3.3. Ids currently bound to a connected, handshake-completed peer -- distinct from
// server_net_cmd below, which only holds THIS tick's decoded packets. A connected player who sent
// no packet this tick is still driven (they're just idle, not disconnected); server_net_cmd being
// empty for their id is what makes serverNetCommandFor() fall back to a neutral PlayerCommand().
static std::set<PlayerID> server_net_players;
static std::map<PlayerID, PlayerCommand> server_net_cmd;

// True for playerm->local_id (this machine's own keyboard) and for any id currently bound to a
// connected peer. Every other player -- e.g. a --spawn-test-players clone nothing has bound to a
// real connection -- stays exactly as inert as it always has been (see serverSpawnTestPlayers()'s
// own comment): never passed a command, never ticked by the per-player loops this plan adds.
static bool serverPlayerIsDriven(PlayerID id) {
	return id == playerm->local_id || server_net_players.find(id) != server_net_players.end();
}

// This tick's decoded network command for 'id', or a neutral/idle default if none arrived --
// D2: the server never waits a tick on a slow peer. Never called for playerm->local_id (that
// player's command is built from *inpt* instead -- see serverLogic()'s own per-player loop).
static PlayerCommand serverNetCommandFor(PlayerID id) {
	std::map<PlayerID, PlayerCommand>::const_iterator it = server_net_cmd.find(id);
	if (it != server_net_cmd.end())
		return it->second;
	return PlayerCommand();
}

// P3.3. --dedicated only (netmgr is NULL otherwise, and this is a no-op). Called first thing in
// serverLogic(), before anything reads playerm->players -- a newly-connected peer must already be
// a real player by the time this tick's kind-C loops (loot, title, death penalty, ...) run, and a
// just-removed one must already be gone.
static void serverSyncNetworkPlayers() {
	server_net_cmd.clear();
	if (!netmgr)
		return;

	netmgr->update();

	PlayerID id;
	while (netmgr->popDisconnected(&id)) {
		server_net_players.erase(id);
		// D26 (P2.5 step 5): frees any in-flight Hazard/summons this player owned. get() guards
		// against a disconnect racing a provisioning failure (serverProvisionPlayer() returned
		// NULL for this id -- see popConnected() below), which would otherwise call remove() on an
		// id playerm never actually created.
		if (playerm->get(id))
			playerm->remove(id);
	}

	while (netmgr->popConnected(&id)) {
		FPoint spawn_pos = playerm->local()->stats.pos;
		if (wmap->teleportation)
			spawn_pos = wmap->teleport_destination;
		if (serverProvisionPlayer(id, spawn_pos)) {
			server_net_players.insert(id);
			// P3.5a: this peer's own summon powers were never walked by any handleNewMap() call --
			// the server's own map was already loaded before this peer connected. See
			// plans/phase3/P3.5a-join-map-sync.md.
			entitym->preloadSummonPrototypesForPlayer(id);
			// Tells this peer's own client which map to load and where its own local avatar
			// belongs on it -- sent exactly once, right here, never again this session.
			netmgr->sendTo(id, Net::encodeMapSync(wmap->getFilename(), spawn_pos.x, spawn_pos.y));
		}
		// else: logged by serverProvisionPlayer() itself. The peer stays connected but bound to no
		// player -- its packets simply decode into server_net_cmd and are never read by anything,
		// since serverPlayerIsDriven() only ever consults server_net_players.
	}

	PlayerID from;
	std::string payload;
	while (netmgr->popPacket(&from, &payload)) {
		PlayerCommand cmd;
		if (Net::decodePlayerCommand(payload, cmd)) {
			server_net_cmd[from] = cmd;
		}
		else {
			// P3.2's own fuzz-safety guarantee (AC4) is what makes this safe to just drop: a
			// malformed payload cannot corrupt state, only fail to decode. One bad frame is not
			// grounds to disconnect a peer over -- matches every other decode-failure path in this
			// codebase.
			Utils::logError("main_server: dropped a malformed PLAYER_COMMAND from player id=%u.", static_cast<unsigned>(from));
		}
	}
}

// P3.4. --dedicated only (netmgr is NULL otherwise, and this is a no-op). Called once per tick,
// right after serverLogic() has advanced the simulation, so every field read here is this tick's
// settled server-computed state -- not last tick's. playerm->players already holds exactly the
// players worth broadcasting (local id 0 plus every id serverSyncNetworkPlayers() currently keeps
// provisioned), sorted by id.
static void serverBroadcastSnapshot() {
	if (!netmgr)
		return;

	std::vector<Net::PlayerSnapshotEntry> entries;
	for (size_t i = 0; i < playerm->players.size(); ++i) {
		Avatar* av = playerm->players[i];
		Net::PlayerSnapshotEntry entry;
		entry.id = av->id;
		entry.pos_x = av->stats.pos.x;
		entry.pos_y = av->stats.pos.y;
		entry.direction = av->stats.direction;
		entry.animation = av->activeAnimation ? av->activeAnimation->getName() : std::string();
		entry.hp = av->stats.hp;
		entry.hp_max = av->stats.get(Stats::HP_MAX);
		entry.alive = av->stats.alive;
		entries.push_back(entry);
	}
	netmgr->broadcast(Net::encodePlayerSnapshot(entries));
}

// The tick-order-preserving port of GameStatePlay::logic(), replacing gswitch->logic(). See
// P1.4c's plan doc for the classification every drop/port/substitution below is based on.
static Timer server_second_timer;

static void serverLogic() {
	serverSyncNetworkPlayers();

	// P2.3b named this whole block kind A -- bound to one explicit player, not looped -- because
	// every piece of state driving it came from *this machine's own* InputState (inpt, a single
	// global) and there was no second source of PlayerCommand input to iterate over "until Phase 3
	// actually connects a second real client." P3.3 is that connection: the blocks below that are
	// genuinely per-player state (level-up consumption, the command+logic() call itself, RESPEC,
	// transform/revert, respawn) now loop over every player serverPlayerIsDriven() returns true
	// for -- playerm->local_id (still built from *inpt*, exactly as before) plus any id currently
	// bound to a connected peer (built from that tick's decoded network command instead). A
	// --spawn-test-players clone nothing has connected to stays exactly as inert as it always was.
	// The two blocks that stay genuinely local-only -- wmap->checkNearestEventInteraction()
	// (mouse-driven, no headless equivalent for anyone) and serverCheckTeleport()/
	// serverCheckCutscene() (party-wide map-change coordination, P3.6's job, not this plan's) -- are
	// called out individually below, not generalised.
	// local_inv/local_ab/local_pbs no longer live here -- every block below that needs them now
	// derives its own inventory/actionbar/powerbonus per player inside its own loop (playerm->
	// inventories[p]/actionbars[p]/powerbonuses[p]), local included.
	Avatar* local = playerm->local();

	PlayerCommand player_cmd;
	PlayerInputLocks player_locks;
	player_locks.copyFrom(*inpt);

	serverCheckCutscene();

	// See GameStatePlay.cpp's own comment: sits immediately before menu->logic() there so the
	// tick it lands on doesn't move -- there is no menu->logic() here to be "immediately before".
	// P2.3b, kind C: death-penalty application has no input dependency (it only reads that
	// player's own stats.death_penalty flag, set by StatBlock on death), so every player's own
	// inventory applies its own penalty, not just local_inv's.
	for (size_t p = 0; p < playerm->inventories.size(); ++p) {
		playerm->inventories[p]->applyDeathPenalty();
	}

	// menu->logic() itself is dropped, but one piece of sim-relevant state inside it was missed by
	// that "everything sim-relevant already moved out" assumption: MenuManager.cpp's own
	// `if (chr->checkUpgrade() || player->stats.level_up) { inv->applyEquipment();
	// player->stats.hp = HP_MAX; player->stats.mp = MP_MAX; player->stats.level_up = false; }`
	// (P2.3b reworded this quote to match MenuManager.cpp's own P2.3 migration off the pc global
	// -- unchanged in substance). chr->checkUpgrade() is correctly dropped -- it only fires from
	// a character-sheet "+" click on a stat point, the
	// same class of menu-only gap as RESPEC's stat-point spending. But stats.level_up has no
	// UI dependency at all: Avatar::logic() sets it (Avatar.cpp, the level-up check) the tick XP
	// crosses a level threshold, and nothing except this block ever reads or clears it -- without
	// a consumer, it would stay true forever after the first level gained. Its two other effects
	// are just as real: a full HP/MP heal on level-up, and an equipment-slot re-evaluation
	// (passive powers or level requirements can newly enable a slot).
	//
	// The applyEquipment() call carries a side effect that is NOT cosmetic: PlayerInventory.cpp's
	// applyEquipment() ends with `if (cur_state == ENTITY_POWER) cur_state = ENTITY_STANCE;`,
	// interrupting an in-progress attack. Found by bisecting the replay corpus's melee digest
	// past every earlier fix, down to a tick where a mid-swing player, mid-level-up, exits to
	// STANCE one tick later in a headless run than in the reference client -- because this whole
	// block was simply missing, so nothing here ever interrupted the swing. Placement matters as
	// much as content: this must run BEFORE serverCheckLoot()/local->logic() below, matching
	// MenuManager::logic()'s own position (menu->logic() runs before checkLoot() and
	// player->logic() in GameStatePlay::logic()) -- level_up is set inside THIS tick's own
	// upcoming local->logic()
	// call, so the interrupt+heal a tick sees is for the PREVIOUS tick's level-up, one full tick
	// after Avatar::logic() sets the flag. Moving this after local->logic() would apply it a tick
	// early instead and desync from every golden that already banked the one-tick delay. Kind A:
	// see this function's own header comment -- level_up only ever becomes true for a player whose
	// logic() ran with a real command, i.e. a driven player (P3.3: local or a connected peer).
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		Avatar* player = playerm->players[p];
		if (!serverPlayerIsDriven(player->id))
			continue;
		if (player->stats.level_up) {
			playerm->inventories[p]->applyEquipment();
			player->stats.hp = player->stats.get(Stats::HP_MAX);
			player->stats.mp = player->stats.get(Stats::MP_MAX);
			player->stats.level_up = false;
		}
	}

	// isPaused() is the constant false server-side (settings->headless is always true -- see
	// GameStatePlay::isPaused()'s own body), so this whole block always executes.
	{
		if (!server_second_timer.isEnd())
			server_second_timer.tick();
		else {
			local->time_played++;
			server_second_timer.reset(Timer::BEGIN);
		}

		// P2.3b, kind C: auto-pickup has no input dependency -- see serverCheckLoot()'s own
		// comment. Every alive player gets their own check, not just local.
		for (size_t p = 0; p < playerm->players.size(); ++p) {
			if (playerm->players[p]->stats.alive)
				serverCheckLoot(playerm->players[p], playerm->inventories[p]);
		}
		// checkEnemyFocus()/checkNPCFocus() dropped -- mouseover highlighting only.
		if (local->stats.alive) {
			// checkHotspots() dropped -- mouse-only (gated on !inpt->usingMouse() at its own
			// top). Point-and-click map-event interaction has no headless equivalent.
			// Kind A: this reads inpt->pressing[Input::ACCEPT] internally (Map.cpp) -- the same
			// single-input-source reasoning as this whole block.
			wmap->checkNearestEventInteraction(local->stats.pos);
			// checkNPCInteraction() dropped -- NPC dialog is menu->talker-driven, no headless
			// equivalent.
		}
		// P2.3b, kind C: title/subclass earning has no input dependency -- see
		// serverCheckTitle()'s own comment. Every player is checked, not just local.
		for (size_t p = 0; p < playerm->players.size(); ++p) {
			serverCheckTitle(playerm->players[p]);
		}

		// P3.3: one PlayerCommand+logic() call per DRIVEN player, not just local. local's command
		// is still the one place player intent is read out of global input (mapr->cam.pos becomes
		// player->stats.pos -- see P1.4c's "Camera-position substitution" note), using the SAME
		// player_cmd/player_locks declared at this function's top -- checkTransform() and the
		// respawn/RESPEC/transform blocks further down still read player_locks by that name, and
		// it must be the exact object local->logic() just wrote into, not a copy. Every other
		// driven player uses a fresh, per-iteration command/locks pair: their command comes from
		// this tick's decoded network packet (or a neutral default -- see serverNetCommandFor()),
		// and their locks are never copied to/from anything -- remote click-arbitration has no
		// meaning yet (no presentation layer has reached a remote player).
		// P3.6b: recomputed fresh every tick, never accumulated -- true only if some driven
		// player's command THIS tick asks to cancel a pending party travel countdown. See
		// serverCheckTeleport().
		server_travel_cancel_requested = false;

		for (size_t p = 0; p < playerm->players.size(); ++p) {
			Avatar* player = playerm->players[p];
			if (!serverPlayerIsDriven(player->id))
				continue;

			PlayerInventory* inventory = playerm->inventories[p];
			ActionBarState* actionbar = playerm->actionbars[p];
			bool is_local = (player->id == playerm->local_id);

			PlayerCommand net_cmd;
			PlayerInputLocks net_locks;
			PlayerCommand& cmd = is_local ? player_cmd : net_cmd;
			PlayerInputLocks& locks = is_local ? player_locks : net_locks;

			if (is_local) {
				PlayerCommandBuilder::build(cmd, *inpt, player->stats.pos);
				actionbar->checkHotkeyActions(player->action_queue);
				cmd.actions = player->action_queue;
				cmd.click_consumed_by_ui = false;

				// Respawn: no "Continue" button exists headless, so holding/pressing ACCEPT while
				// dead is the server's respawn gesture. See P1.4c's "Respawn trigger" note -- the
				// one piece of this plan with no client-side equivalent to diff against.
				// death/deathfull are the corpus rows that exercise it.
				cmd.respawn = inpt->pressing[Input::ACCEPT] && !player->stats.alive;
			}
			else {
				// PlayerCommand.h's own comment on the respawn field: "Phase 3 sets this from a
				// network message instead; nothing in Avatar has to change for that." This is that
				// wiring -- cmd.respawn arrives already decoded from the wire (P3.2's
				// encode/decodePlayerCommand), same field, same downstream Avatar::logic() path.
				cmd = serverNetCommandFor(player->id);
			}

			// P3.6b: same "arrives already decoded off the wire" story as respawn above --
			// local has no keybinding for this (see PlayerCommand.h's own comment on the field),
			// so only a connected peer's command can ever set it.
			if (cmd.cancel_travel)
				server_travel_cancel_requested = true;

			if (inventory->applyEquipmentSetDelta(cmd.equip_set_delta)) {
				if (is_local) {
					inpt->lock[cmd.equip_set_delta > 0 ? Input::EQUIPMENT_SWAP : Input::EQUIPMENT_SWAP_PREV] = true;
				}
				// Mirrors MenuInventory's changeEquipmentSet() setting changed_equipment = true
				// (MenuInventory.cpp) -- a switch changes which items are active, same as an equip/
				// unequip drag would.
				server_equipment_changed[player->id] = true;
			}

			// P3.6d: mirrors the local-only write two branches above (cmd.actions =
			// player->action_queue, for the wire) in the opposite direction -- Avatar::logic()'s own
			// power-processing loop (Avatar.cpp:654) reads action_queue, never cmd.actions directly,
			// so without this a connected peer's own queued power activations (decoded correctly off
			// the wire) were silently discarded. For local this is a true no-op: action_queue already
			// equals cmd.actions from the assignment two branches above, unchanged in between -- see
			// AC-REPLAY.
			player->action_queue = cmd.actions;

			player->logic(cmd, locks);
		}

		// P2.2: stealth is per-player now -- EntityBehavior reads each evaluated player's own
		// Stats::STEALTH directly (via PlayerManager::nearestAliveTo()), so there's no longer a
		// single hero value to transfer onto EntityManager here.

		entitym->logic();
		hazards->logic();
		loot->logic();
		npcs->logic();

		// comb->logic() dropped -- CombatText, floating damage numbers.
	}

	// close menus when the player dies, but still allow them to be reopened: no menus exist
	// headless, so only the flags themselves need resetting -- matches the already-documented
	// P1.3f-permadeath-gap. These are one-shot flags consumed by a menu system that (headless)
	// only ever exists conceptually for a driven player's own screen -- P3.3: every driven player,
	// not just local, since close_menus/show_game_over can now be set for any of them.
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		Avatar* player = playerm->players[p];
		if (!serverPlayerIsDriven(player->id))
			continue;
		if (player->close_menus) {
			player->close_menus = false;
		}
		if (player->show_game_over) {
			player->show_game_over = false;
		}
	}

	// RESPEC. See P1.4c's "Powers reset" note: menu_powers->resetToBasePowers() (which itself
	// calls setUnlockedPowers()) and the client's own separate, redundant setUnlockedPowers()
	// call both boil down, once menu_powers doesn't exist, to current_cell reset +
	// clearActionBarBonusLevels() -- current_cell is already a public field and
	// clearActionBarBonusLevels() is already a public, presentation-free PowerBonusState method
	// (P1.3g). A headless server defers class defaults and skill-tree auto-unlocks forever as a
	// result -- a real, documented gap, not a silent one (RESPEC is unused by any corpus mod).
	// See plans/00-ROADMAP.md's P1.3h note. P3.3: respec_powers is a one-shot flag consumed for
	// every driven player, same reasoning as level_up above, not just local.
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		Avatar* player = playerm->players[p];
		if (!serverPlayerIsDriven(player->id))
			continue;
		if (!player->respec_powers)
			continue;

		player->respec_powers = false;
		PowerBonusState* powerbonus = playerm->powerbonuses[p];
		ActionBarState* actionbar = playerm->actionbars[p];
		EngineSettings::HeroClasses::HeroClass* player_class = eset->hero_classes.getByName(player->stats.character_class);

		for (size_t i = 0; i < powerbonus->current_cell.size(); ++i)
			powerbonus->current_cell[i] = 0;

		if (player_class && !player->respec_use_engine_defaults) {
			for (size_t j = 0; j < player_class->powers.size(); j++) {
				player->stats.powers_list.push_back(player_class->powers[j]);
			}
		}
		powerbonus->clearActionBarBonusLevels();

		for (unsigned i = 0; i < actionbar->slots_count; ++i)
			actionbar->clearSlot(i);
		if (player_class && !player->respec_use_engine_defaults) {
			actionbar->set(player_class->hotkeys, ActionBarState::SET_SKIP_EMPTY);
		}
	}

	// these actions occur whether the game is paused or not.
	serverCheckTeleport();
	serverCheckLootDrop();
	serverCheckLog();
	// checkBook() dropped -- wmap->show_book is unconditionally overwritten (not gated) by the
	// next show_book event, confirmed in EventManager.cpp, so leaving it unconsumed is harmless.
	// P2.3b, kind C: see serverCheckEquipmentChange()'s own comment -- checked for every player.
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		serverCheckEquipmentChange(playerm->players[p], playerm->inventories[p], playerm->actionbars[p]);
	}
	serverCheckUsedItems();
	// checkStash() dropped -- MenuStash-driven, no corpus coverage; wmap->stash/stash_pos are
	// overwritten by the next event, same reasoning as show_book.
	serverCheckSaveEvent();
	// checkNotifications() dropped -- action-bar notification badges only.
	// checkCancel() dropped -- the server's shutdown path is SIGINT/SIGTERM (see
	// serverSignalHandler), not a window-close/menu-exit click.

	wmap->logic(false);
	wmap->enemies_cleared = entitym->isCleared();
	// quests->logic()/createQuestList() dropped -- QuestLog IS constructed (see
	// serverConstructSim()'s own note on why), but these two write through its MenuLog* pointer
	// unconditionally, which is NULL here; its only output otherwise feeds a widget nothing else
	// reads (QuestLog.cpp/MenuLog.cpp/GameStatePlay.cpp only).

	// checkTransform()/setPowers/revertPowers/respawn: one-shot, per-player, simulation-state-driven
	// (not literally input-driven -- checkTransform() only takes 'locks' for the click-arbitration
	// transform()/untransform() may need). P3.3: every driven player, not just local. local reuses
	// player_locks, the exact object its own logic() call wrote into earlier this tick (checkTransform
	// consuming a DIFFERENT PlayerInputLocks would silently drop whatever Avatar::logic() itself set
	// there) -- every other driven player gets a fresh, discarded-after-use default, same reasoning
	// as the command+logic() loop above.
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		Avatar* player = playerm->players[p];
		if (!serverPlayerIsDriven(player->id))
			continue;

		PlayerInventory* inventory = playerm->inventories[p];
		ActionBarState* actionbar = playerm->actionbars[p];
		PowerBonusState* powerbonus = playerm->powerbonuses[p];
		bool is_local = (player->id == playerm->local_id);
		PlayerInputLocks net_locks;
		PlayerInputLocks& locks = is_local ? player_locks : net_locks;

		player->checkTransform(locks);

		// change hero powers on transformation.
		if (player->setPowers) {
			player->setPowers = false;
			// save ActionBar state and lock slots from removing/replacing power
			for (int i = 0; i < MenuActionBar::SLOT_MAX; i++) {
				actionbar->hotkeys_temp[i] = actionbar->hotkeys[i];
				actionbar->hotkeys[i] = 0;
			}
			int count = MenuActionBar::SLOT_MAIN1;
			// put creature powers on action bar
			for (size_t i = 0; i < player->charmed_stats->powers_ai.size(); i++) {
				if (powers->isValid(player->charmed_stats->powers_ai[i].id) && powers->powers[player->charmed_stats->powers_ai[i].id]->beacon != true) {
					actionbar->hotkeys[count] = player->charmed_stats->powers_ai[i].id;
					actionbar->locked[count] = true;
					count++;
					if (count == MenuActionBar::SLOT_MAX)
						count = 0;
					else if (count == MenuActionBar::SLOT_MAIN1)
						// we've filled the actionbar, stop adding powers to it
						break;
				}
			}
			if (player->stats.manual_untransform && powers->isValid(player->untransform_power)) {
				actionbar->hotkeys[count] = player->untransform_power;
				actionbar->locked[count] = true;
			}
			else if (player->stats.manual_untransform && player->untransform_power == 0)
				Utils::logError("main_server: Untransform power not found, you can't untransform manually");

			actionbar->updated = true;

			// reapply equipment if the transformation allows it
			if (player->stats.transform_with_equipment)
				inventory->applyEquipment();
		}
		// revert hero powers
		if (player->revertPowers) {
			player->revertPowers = false;

			// restore ActionBar state
			for (int i = 0; i < MenuActionBar::SLOT_MAX; i++) {
				actionbar->hotkeys[i] = actionbar->hotkeys_temp[i];
				actionbar->locked[i] = false;
			}

			actionbar->updated = true;

			// also reapply equipment here, to account for items that give bonuses to base stats
			inventory->applyEquipment();
		}

		// when the hero (re)spawns, reapply equipment & passive effects
		if (player->respawn) {
			player->stats.alive = true;
			player->stats.corpse = false;
			player->stats.cur_state = StatBlock::ENTITY_STANCE;
			inventory->applyEquipment();
			// Mirrors GameStatePlay.cpp's respawn block setting menu->inv->changed_equipment = true
			// right before its own checkEquipmentChange() call.
			server_equipment_changed[player->id] = true;
			serverCheckEquipmentChange(player, inventory, actionbar);
			player->stats.hp = player->stats.get(Stats::HP_MAX);
			player->stats.logic();
			player->stats.recalc();

			// menu_powers->resetToBasePowers()/setUnlockedPowers() substitute -- see this
			// function's own RESPEC comment above; the same pair applies here.
			for (size_t i = 0; i < powerbonus->current_cell.size(); ++i)
				powerbonus->current_cell[i] = 0;
			powerbonus->clearActionBarBonusLevels();

			powers->activatePassives(&player->stats);
			player->respawn = false;
		}
	}

	// menu->menus_open cursor block dropped -- no cursor state to set.

	// update the action bar as it may have been changed by items. P2.3b, kind C: see
	// serverUpdateActionBar()'s own comment -- every player's own action bar is checked, not just
	// local_ab's.
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		ActionBarState* actionbar = playerm->actionbars[p];
		if (actionbar->updated) {
			actionbar->updated = false;

			// set all hotkeys to their base powers
			for (unsigned i = 0; i < actionbar->slots_count; i++) {
				actionbar->hotkeys_mod[i] = actionbar->hotkeys[i];
			}

			serverUpdateActionBar(actionbar, playerm->inventories[p], SERVER_ACTIONBAR_ALL);
		}
	}

	// menu->exit->reload_music dropped -- no menu, no music.

	player_locks.copyTo(*inpt);

	// Last thing in the tick, after both the simulation and the (nonexistent) menus have had
	// their say. GameStatePlay::drainSimEvents()'s own doc comment already says a headless
	// server "empties it without playing" -- that function lives in GameStatePlay.cpp
	// (presentation), so this is its trivial standalone equivalent.
	sim_events->clear();
}

static void serverInit(const ServerCmdLineArgs& args) {
	platform.setPaths();

	settings->setCustomPathData();
	settings->setGame();

	// The server never participates in the single-instance lock: several servers, or a
	// server alongside a client, is the normal case on one machine.
	settings->no_lock_file = true;
	settings->headless = true;

	Utils::createLogFile();
	Utils::logInfo("%s", VersionInfo::createVersionStringFull().c_str());
	Utils::logInfo("flare-server: headless, no window and no audio device.");

	Utils::logInfo("main_server: PATH_CONF = '%s'", settings->path_conf.c_str());
	Utils::logInfo("main_server: PATH_USER = '%s'", settings->path_user.c_str());
	Utils::logInfo("main_server: PATH_DATA = '%s'", settings->path_data.c_str());

	// SDL_INIT_TIMER only. No VIDEO, no AUDIO, no GAMECONTROLLER -- requesting any of those
	// would defeat the entire point of this binary and fail on a machine with no display.
	if (SDL_Init(SDL_INIT_TIMER) < 0) {
		Utils::logError("main_server: Could not initialize SDL: %s", SDL_GetError());
		Utils::Exit(1);
	}

	mods = new ModManager(&(args.mod_list));

	if (!mods->haveFallbackMod()) {
		Utils::logError("main_server: Could not find the default mod in the following locations:");
		Utils::logError("%smods/", settings->path_user.c_str());
		Utils::logError("%smods/", settings->path_data.c_str());
		Utils::logError("A copy of the default mod is in the \"mods\" directory of the flare-engine repo.");
		Utils::Exit(1);
	}

	settings->loadSettings();

	// Audio is off at the source as well as at the device, so nothing even tries to decode.
	settings->audio = false;

	// The server is the simulation authority, so sim_rng's seed is the one that will eventually
	// be handed to joining clients (Phase 3). Fixed for now. See Rng.h.
	sim_rng = new Rng();
	sim_rng->seed(args.sim_seed);
	fx_rng = new Rng();
	fx_rng->seed(static_cast<uint64_t>(time(NULL)));

	// What the simulation did, for the presentation layer to react to. Emptied every tick.
	sim_events = new SimEventQueue();
	Utils::logInfo("main_server: sim_rng seeded with 0x%llx", static_cast<unsigned long long>(sim_rng->getSeed()));

	replay = new Replay();

	save_load = new SaveLoad();
	msg = new MessageEngine();
	// Constructed directly rather than through DeviceList.cpp's getInputManager()/getRenderDevice()/
	// getSoundManager() (P1.4d): those are runtime string/bool factories, so even their
	// "null"/headless branch still references SDLInputState/SDLSoftwareRenderDevice/
	// SDLHardwareRenderDevice/SDLSoundManager in the same function body, which drags
	// SDL2_image/SDL2_mixer into any binary that links DeviceList.cpp at all -- including one that
	// only ever takes the null branch at runtime. Constructing the Null* classes here instead means
	// main_server.cpp, and everything it links, never references the SDL-backed render/input/sound
	// classes even at the symbol level. FontEngine is the one exception: MenuConfig.cpp (linked
	// in for reasons that have nothing to do with fonts -- see CMakeLists.txt's
	// FLARE_SERVER_EXCLUDED_PRESENTATION_SOURCES comment) unconditionally needs SDLFontEngine to
	// exist for its own linking, so flare-server accepts SDL2_ttf regardless of what font is
	// constructed here; a NullFontEngine is still used here rather than SDLFontEngine so the
	// server's *own* font object never does real font work, even though the library is present.
	font = new NullFontEngine();
	anim = new AnimationManager();
	comb = new CombatText();

	eset = new EngineSettings();
	eset->load();

	inpt = new NullInputState();
	icons = NULL;

	Stats::init();

	platform.setScreenSize();

	render_device = new NullRenderDevice();

	if (render_device->createContext() == -1) {
		Utils::logError("main_server: Could not create rendering context.");
		Utils::Exit(1);
	}
	render_device->reloadGraphics();

	snd = new NullSoundManager();

	tooltipm = new TooltipManager();
}

// Everything that touches sim_rng -- directly, or indirectly through a constructor like
// EntityManager's (handleNewMap() -> collider.getRandomNeighbor(), unconditional even with no
// entities yet) -- has to run AFTER main() reseeds sim_rng from the replay/save, not before.
// GameStatePlay itself is only ever constructed that late on the client: GameSwitcher's initial
// state is GameStateTitle, and GameStatePlay doesn't exist until GameStateLoad::logicLoading()
// runs, several ticks into gswitch->logic() -- well after main.cpp's own reseed-from-replay line.
// This function is that reseed's replacement in the timeline: serverInit() above only sets up
// device/mod/settings state that reseeding doesn't depend on and that the sim doesn't draw
// against, and main() calls this function after the reseed, not from inside serverInit().
// Getting this wrong doesn't crash anything -- it just draws the sim's very first random numbers
// from the wrong seed, so every enemy spawn position onward silently diverges from a client
// given the same save and the same recorded input. Found by bisecting the replay corpus down to
// a single extra draw that could not be explained by any difference in what was drawn, only by
// when: SharedGameResources.cpp's own globals confirm nothing here is order-sensitive for any
// reason other than the RNG.
static void serverConstructSim(const ServerCmdLineArgs& args) {
	// Direct construction of the simulation's own objects -- the same set GameStatePlay's
	// constructor builds, in the same order, except menu, mapr and quests: see
	// SharedGameResources.h's wmap/mapr note and P1.4c's plan doc for why each is skipped.
	// menu/mapr stay NULL (static storage duration zero-initializes SharedGameResources.cpp's
	// globals), and every sim-side reader of menu/menu_powers/menu_act/mapr this plan found
	// unconditionally dereferencing one of them -- StatBlock::canUsePower(), FogOfWar::logic(),
	// WorldHash::compute(), and a dozen spots in SaveLoad.cpp -- was fixed to guard on it first.
	//
	// quests IS constructed, unlike menu/mapr -- see the note at its own construction line below
	// for why "drop QuestLog entirely" (this plan's original design) turned out to be wrong.
	if (items == NULL)
		items = new ItemManager();

	camp = new CampaignManager();
	eventm = new EventManager();

	loot = new LootManager();
	powers = new PowerManager();
	fow = new FogOfWar();
	wmap = new Map();
	// playerm allocates pc/pinv/pab/pbs together -- see PlayerManager.cpp. The server has no
	// MenuManager, so unlike GameStatePlay's constructor there is no menu-binding ordering
	// constraint on pinv/pab here; this is a TODO: P3.5, same as the comment on the id below.
	playerm = new PlayerManager();
	// TODO: P3.5 -- players arrive when clients connect. Until then, create one so the server
	// keeps working.
	playerm->create(0);
	playerm->setLocal(0);
	entitym = new EntityManager();
	enemyg = new EnemyGroupManager();
	hazards = new HazardManager();
	npcs = new NPCManager();

	// QuestLog's own display state (the quests[] vector, log formatting) has no sim consumer --
	// confirmed, see this plan's own "Dropping QuestLog" note -- but its CONSTRUCTOR has a real
	// side effect nothing else replicates: QuestLog::load() calls camp->registerStatus() for
	// every quest's complete_status/requires_status/requires_not_status key, registering those
	// StatusIDs into camp's shared, WorldHash-covered status map. Skipping construction entirely
	// (this plan's first attempt) silently dropped those registrations -- found by bisecting a
	// full-corpus digest mismatch down to camp->status being empty on a headless server and
	// non-empty on the client, with every other hashed field already matching bit-for-bit.
	// QuestLog(NULL) is safe: its constructor and load() never dereference the MenuLog* it's
	// given (confirmed by reading both). What stays unsafe, and stays dropped, is logic()/
	// createQuestList() -- both write through log-> unconditionally the moment any quest is
	// active or complete, so this object is constructed and then never called again.
	server_quests = new QuestLog(NULL);

	xp_scaling = new XPScaling();

	// P2.3b, kind A: this is specifically player 0's setup, bootstrapped from --load-slot --
	// serverSpawnTestPlayers() already correctly wires ids 1..N-1 through their own explicit
	// playerm-> calls (P2.2), so this function's job is exactly one player regardless of how many
	// exist by the time the server finishes starting up.
	PlayerInventory* inv0 = playerm->inventoryFor(0);
	ActionBarState* ab0 = playerm->actionbarFor(0);

	// inv0/ab0's D1-style menu-free sizing -- see PlayerInventory.h's own header comment and
	// ActionBarState.h's new P1.4c note.
	//
	// inv0->loadEquipmentData() reads engine/equipment.txt directly, exactly the "no menu layout
	// file at all" path P1.3d-4d built for this. A mod chain without one has no menus/
	// inventory.txt-derived fallback available here (that path needs a live MenuInventory to
	// parse screen rectangles this server has no reason to draw), so it's a hard, loud error
	// rather than a silent zero-slot character.
	if (!inv0->loadEquipmentData()) {
		Utils::logError("main_server: mod chain has no engine/equipment.txt -- headless play requires it (see P1.3d-4d).");
		Utils::Exit(1);
	}

	// MenuInventory's own constructor defaults the active set to 1 whenever the mod defines any
	// (menus/inventory.txt/menus/actionbar.txt's usual "wraparound arithmetic on
	// active_equipment_set" narrative -- PlayerInventory.h -- covers changing it, never covers
	// this one-time default). Nothing else in the sim ever sets it, so a headless server, never
	// constructing a MenuInventory, left it at active_equipment_set's constructor default of 0
	// forever: PlayerInventory::isEquipSlotActive() treats set 0 as "no non-shared slot is ever
	// active." Found by bisecting a one-field divergence in the replay corpus (equipset stayed 0
	// instead of reaching 1) down to construction time, not gameplay -- active_equipment_set
	// was already 1 on the very first tick of the working build, before any input had been read.
	if (inv0->max_equipment_set > 0) {
		inv0->active_equipment_set = 1;
	}

	// The action bar has no equivalent engine-data file yet (D1 is unsolved for it, unlike
	// equipment): menus/actionbar.txt's slot= lines are screen positions AND the only place
	// slot count/lock flags come from. Substituted with the compile-time assumption
	// SaveLoad.cpp's own actionbar save/load loops already make -- MenuActionBar::SLOT_MAX
	// (12) slots, none locked. Measured against the corpus's actual mod
	// (tests/flaredata/mods/fantasycore/menus/actionbar.txt): exactly 12 slots, no slot defines
	// a lock flag, so this is exact for every corpus fixture, not just a plausible default.
	// A mod that locks a slot, or defines fewer than 12, would diverge here -- documented, not
	// measured for that case.
	ab0->initSlots(MenuActionBar::SLOT_MAX);
	ab0->prevent_changing.resize(MenuActionBar::SLOT_MAX, false);
	for (unsigned i = 0; i < ab0->slots_count; ++i)
		ab0->clearSlot(i);

	serverLoadTitles();

	if (!serverLoadGame(args.load_slot)) {
		Utils::Exit(1);
	}
}

// P2.2 AC5-AC7 test infrastructure. Not a general multi-player join path (that's P3.5) and not
// routed through SaveLoad.cpp (out of scope for P2.2): each additional player is built by
// cloning player 0's already-loaded state (Avatar::stats, PlayerInventory, ActionBarState) into
// a freshly playerm->create()-d id, then offsetting position so proximity-based AI targeting has
// something to distinguish. playerm->create()'s Avatar constructor already sizes/allocates
// power_cooldown_timers, power_cast_timers and calls Avatar::init() -- see Avatar.cpp -- so this
// only needs to overlay the class/gear/action-bar state init() doesn't set.
//
// Known gap, stated rather than papered over: these test players have no input channel. Replay
// (Replay.h) drives a single global InputState, and Avatar::logic() is called once per player,
// each from its own PlayerCommand -- there is no per-player recorded-input format to extend this
// into "player 1 replays its own actions." A spawned test player is inert (never calls
// Avatar::logic() with real commands) unless something else moves it. That is enough for AC5/AC6
// (proximity-based targeting of a stationary second player) and for AC7 (the summon-prototype
// preload is a map-load-time effect of the action-bar binding below, not something that needs
// the player to actually cast anything at run time) -- but it is not a general second playable
// character, and building one is a materially larger feature (a real per-player replay format)
// that this plan's file list does not include.
//
// P3.3 factors the per-id body out into serverProvisionPlayer() below, reused by the real
// connect-time path in serverSyncNetworkPlayers() -- the mechanism (clone player 0's template)
// and its documented limitation (not a real second character, no independent items -- that is
// P3.5) are unchanged; only WHEN it runs and WHETHER the result is driven by real input changes.
static Avatar* serverProvisionPlayer(PlayerID id, FPoint spawn_pos) {
	Avatar* source = playerm->local();
	PlayerInventory* source_inv = playerm->inventoryFor(playerm->local_id);
	ActionBarState* source_ab = playerm->actionbarFor(playerm->local_id);
	if (!source || !source_inv || !source_ab) {
		Utils::logError("main_server: serverProvisionPlayer requires player 0 (--load-slot) to already be loaded.");
		return NULL;
	}

	PlayerID new_id = playerm->create(id);
	Avatar* new_avatar = playerm->get(new_id);
	PlayerInventory* new_inv = playerm->inventoryFor(new_id);
	ActionBarState* new_ab = playerm->actionbarFor(new_id);
	if (!new_avatar || !new_inv || !new_ab)
		return NULL;

	// Copy the loaded class/stats state. Does NOT touch power_cooldown_timers/power_cast_timers
	// (Avatar fields, not StatBlock) -- new_avatar's own constructor already allocated those
	// correctly-sized for THIS avatar; overwriting stats wholesale leaves them alone since
	// they live outside the StatBlock being assigned here.
	new_avatar->stats = source->stats;

	// NOT *new_inv = *source_inv -- ItemStorage (PlayerInventory::inventory[]) owns a raw
	// `ItemStack* storage` array with no user-defined copy assignment, so a struct-copy here
	// would shallow-copy that pointer and double-free it the moment either player's inventory
	// was destroyed. Found by running this directly (SIGABRT, no assertion message, during
	// PlayerManager::remove() -- see the P2.2 report) rather than through the replay corpus,
	// which never destructs a second player. Sized/initialized the same way player 0's was in
	// serverConstructSim() above instead: a properly-owned, empty inventory. A provisioned
	// player doesn't get player 0's actual items, only a valid, empty one of their own.
	new_inv->loadEquipmentData();
	if (new_inv->max_equipment_set > 0)
		new_inv->active_equipment_set = 1;

	// ActionBarState's only non-vector member is P2.3b's owner back-pointer -- everything
	// else is a std::vector, so a struct-copy is otherwise safe and gives the provisioned player
	// the same slot count/lock flags as player 0's (initSlots() alone, without menus/actionbar.txt
	// unavailable headless, would leave it zero-sized). The struct-copy DOES overwrite owner
	// with source_ab's own (player 0's) -- restored to new_avatar right after, the same way
	// PlayerManager::create() wired it originally. Found via --assert-player-wiring reporting
	// ok=0 for every spawned test player, not by inspection -- see this plan's report.
	*new_ab = *source_ab;
	new_ab->owner = new_avatar;
	for (unsigned s = 0; s < new_ab->hotkeys.size(); ++s)
		new_ab->hotkeys[s] = 0;

	new_avatar->stats.pos = spawn_pos;

	// SaveLoad::loadGame() (out of scope) is what calls this for player 0 -- see
	// serverCheckEquipmentChange()'s own comment on why it is load-bearing, not cosmetic:
	// AnimationManager reference-counts by filename, and skipping this left the clone's
	// stats.animations ("animations/hero.txt") never increfed, so its destructor's
	// decreaseCount() had nothing to release -- logErrorDialog() then blocked waiting on a
	// display that does not exist headless. Found by running this directly rather than
	// through the replay corpus (see the P2.2 report).
	new_avatar->loadAnimations();
	new_avatar->loadSounds();
	wmap->collider.blockPlayer(new_avatar->stats.pos.x, new_avatar->stats.pos.y, new_id);

	Utils::logInfo("main_server: provisioned player id=%u at (%.1f, %.1f)",
	               static_cast<unsigned>(new_id), static_cast<double>(new_avatar->stats.pos.x), static_cast<double>(new_avatar->stats.pos.y));

	return new_avatar;
}

static void serverSpawnTestPlayers(const ServerCmdLineArgs& args) {
	if (args.spawn_test_players <= 1)
		return;

	if (args.spawn_test_players > 8) {
		Utils::logError("main_server: --spawn-test-players=%d exceeds the 8-player max (D3).", args.spawn_test_players);
		Utils::Exit(1);
	}

	Avatar* source = playerm->local();
	if (!source) {
		Utils::logError("main_server: --spawn-test-players requires player 0 (--load-slot) to already be loaded.");
		Utils::Exit(1);
	}

	PlayerID last_id = 0;
	FPoint spawn_anchor = source->stats.pos;
	if (wmap->teleportation)
		spawn_anchor = wmap->teleport_destination;

	// These are deliberately explicit, stable map offsets rather than a random spread. They keep
	// the acceptance fixtures deterministic and put several clones near different enemy groups.
	static const int spawn_offset_x[] = {-11, 2, 9, -18, 4, -9, 14};
	static const int spawn_offset_y[] = {2, 15, -2, 4, 24, 16, -4};
	for (int i = 1; i < args.spawn_test_players; ++i) {
		FPoint spawn_pos;
		spawn_pos.x = spawn_anchor.x + static_cast<float>(spawn_offset_x[i - 1]);
		spawn_pos.y = spawn_anchor.y + static_cast<float>(spawn_offset_y[i - 1]);

		Avatar* new_avatar = serverProvisionPlayer(static_cast<PlayerID>(i), spawn_pos);
		if (!new_avatar)
			continue;

		last_id = new_avatar->id;
	}

	if (args.test_player_summon != 0 && last_id != 0) {
		ActionBarState* last_ab = playerm->actionbarFor(last_id);
		if (last_ab && !last_ab->hotkeys.empty()) {
			last_ab->hotkeys[0] = args.test_player_summon;
			Utils::logInfo("main_server: bound power id=%zu to player id=%u's action bar slot 0",
			               args.test_player_summon, static_cast<unsigned>(last_id));

			// The summon-prototype preload (EntityManager::handleNewMap(), P2.2 step 7b) already
			// ran once, inside serverLoadGame() above, before this player -- and this binding --
			// existed. Re-running it is safe: the enemy-spawn and ally-repositioning queues it
			// also processes are already drained (harmless no-ops), and loadEntityPrototype()
			// itself skips anything already loaded, so this only adds what the new binding needs.
			entitym->handleNewMap();
		}
	}
}

static void serverKillPlayer(const ServerCmdLineArgs& args) {
	if (args.kill_player < 0)
		return;

	Avatar* victim = playerm->get(static_cast<PlayerID>(args.kill_player));
	if (!victim) {
		Utils::logError("main_server: --kill-player=%d does not name a connected player.", args.kill_player);
		Utils::Exit(1);
	}

	victim->stats.hp = 0;
	victim->stats.takeDamage(0, false, Power::SOURCE_TYPE_ENEMY);
	victim->stats.alive = false;
	wmap->collider.unblockPlayer(victim->stats.pos.x, victim->stats.pos.y, victim->id);
}

// P2.2 AC7 diagnostic. Prints one line per player per spawn-type power bound to their
// known-powers list or action bar, and whether EntityManager has a loaded prototype for it.
// `player=N power=P spawn_type=S loaded=0|1`. Exits without running the simulation, same shape
// as --dump-players below.
static void serverDumpSummonPrototypes() {
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		Avatar* player = playerm->players[p];
		std::vector<PowerID> to_check = player->stats.powers_list;

		ActionBarState* ab = playerm->actionbarFor(player->id);
		if (ab) {
			for (size_t i = 0; i < ab->hotkeys.size(); ++i) {
				if (ab->hotkeys[i] != 0)
					to_check.push_back(ab->hotkeys[i]);
			}
		}

		for (size_t i = 0; i < to_check.size(); ++i) {
			PowerID power_index = to_check[i];
			if (!powers->isValid(power_index))
				continue;

			const std::string& spawn_type = powers->powers[power_index]->spawn_type;
			if (spawn_type.empty() || spawn_type == "untransform")
				continue;

			std::vector<Enemy_Level> spawn_enemies = enemyg->getEnemiesInCategory(spawn_type);
			for (size_t j = 0; j < spawn_enemies.size(); ++j) {
				bool loaded = entitym->hasLoadedPrototype(spawn_enemies[j].type);
				printf("player=%u power=%zu spawn_type=%s loaded=%d\n",
				       static_cast<unsigned>(player->id), power_index, spawn_type.c_str(), loaded ? 1 : 0);
			}
		}
	}
}

// P2.4 AC3/AC4/AC5b diagnostic. One line per hostile entity per tick, reading the target and LOS
// selected by EntityBehavior itself rather than independently re-deriving a nearest-player guess.
// `tick=T entity=I target_los=0|1 target=<player id>|none`.
static void serverDumpAiTargets(unsigned long tick) {
	for (size_t i = 0; i < entitym->entities.size(); ++i) {
		Entity* e = entitym->entities[i];
		if (e->stats.hero_ally || e->stats.npc || !e->stats.alive)
			continue;

		int target_id = e->behavior->getAggroTargetId();
		if (target_id >= 0)
			printf("tick=%lu entity=%zu target_los=%d target=%d\n", tick, i, e->behavior->getAggroTargetLos() ? 1 : 0, target_id);
		else
			printf("tick=%lu entity=%zu target_los=- target=none\n", tick, i);
	}
}

static void serverCleanup() {
	// P3.3. Before the playerm->remove() loop below -- shutdown() just closes sockets, it doesn't
	// touch playerm, so ordering here isn't load-bearing the way hazards=NULL is, but there is no
	// reason to keep sockets open one tick longer than necessary.
	if (netmgr) {
		netmgr->shutdown();
		delete netmgr;
		netmgr = NULL;
	}

	// Same deletion order GameStatePlay::~GameStatePlay() uses for the objects both sides
	// construct; menu/mapr are simply absent from both the construction and this list.
	delete server_quests;
	delete npcs;
	delete hazards;
	// D26 (P2.5 step 5): PlayerManager::remove(), called by the loop right below, expires any
	// in-flight hazard owned by the departing player -- so hazards must already be a safe-to-check
	// NULL here, not a pointer dangling from the delete above. Every other global this function
	// deletes gets the same treatment further down; this one was missing it, harmlessly until
	// remove() started reading it.
	hazards = NULL;
	delete entitym;
	while (!playerm->players.empty())
		playerm->remove(playerm->players.back()->id);
	delete playerm;
	delete loot;
	delete camp;
	delete items;
	delete powers;
	delete fow;
	delete xp_scaling;
	delete enemyg;
	delete eventm;
	delete wmap;

	server_quests = NULL;
	playerm = NULL;
	camp = NULL;
	enemyg = NULL;
	entitym = NULL;
	eventm = NULL;
	items = NULL;
	loot = NULL;
	wmap = NULL;
	powers = NULL;
	fow = NULL;
	xp_scaling = NULL;

	delete anim;
	delete comb;
	delete font;
	delete inpt;
	delete mods;
	delete msg;
	delete snd;
	delete save_load;
	delete eset;
	delete replay;
	replay = NULL;
	delete sim_rng;
	delete fx_rng;
	delete sim_events;
	sim_events = NULL;
	delete tooltipm;

	if (render_device)
		render_device->destroyContext();
	delete render_device;

	SDL_Quit();
}

static float getSecondsElapsed(uint64_t prev_ticks, uint64_t now_ticks) {
	return (static_cast<float>(now_ticks - prev_ticks) / static_cast<float>(SDL_GetPerformanceFrequency()));
}

/**
 * main.cpp's mainLoop() with every presentation step removed: no blankScreen(), no render(), no
 * commitFrame(), no FPS counter. The fixed-step logic ticker is kept exactly as-is; pinning the
 * tick rate is P0.4 and must not be done here.
 */
// Mirrors main.cpp. A stall must not become a fast-forward.
static const int MAX_CATCHUP_TICKS = 5;

// How often the trajectory digest samples the world. 30 ticks is twice a second at
// Settings::SIM_TICK_HZ.
//
// A digest of the FINAL state only is not enough, and that is measured rather than assumed:
// raising the hero's melee damage by one point in engine/stats.txt did not move smoke, patrol or
// melee, even though melee.rec kills two goblins. The extra damage changed when they died, not
// whether -- a dead enemy is at hp 0 either way, so the end state converges and the difference
// disappears. A golden that cannot see a change to weapon damage is not much of a regression
// test for a combat refactor. Sampling the whole run fixes that; --hash-every is still the tool
// for finding WHICH tick diverged.
static const unsigned long TRAJECTORY_SAMPLE_TICKS = 30;

static unsigned long serverMainLoop(unsigned long max_ticks, unsigned long hash_every,
                                    bool hash_replicated,
                                    uint64_t* trajectory, unsigned long* last_event_tick,
                                    unsigned long* died_tick, bool dump_ai_targets) {
	bool done = false;
	unsigned long total_ticks = 0;
	uint64_t traj = WorldHash::init();

	unsigned long prev_ev_total = sim_events->getTotal();
	unsigned long last_ev_tick = 0;
	unsigned long died_at = 0;
	// P2.3b, kind A: this liveness check is specifically about the corpus's own real test player
	// (the "central problem" P0.5e's died_tick gate is written against) -- see the died_at check
	// below.
	Avatar* local = playerm->local();

	// The server renders nothing, so there is only one rate here: the shared simulation step.
	// --max-fps is accepted and ignored on purpose; see parseServerArgs().
	const float seconds_per_sim_tick = 1.f/static_cast<float>(Settings::SIM_TICK_HZ);

	uint64_t prev_ticks = SDL_GetPerformanceCounter();
	uint64_t logic_ticks = SDL_GetPerformanceCounter();

	while (!done) {
		int loops = 0;
		uint64_t now_ticks = SDL_GetPerformanceCounter();


		// Bound the accumulated debt. Capping work per iteration is not enough on its own --
		// the outer loop simply spins until the debt is repaid, so a five second stall still
		// replays 300 ticks in milliseconds. Clients cannot follow that, so the missed time is
		// dropped rather than simulated. Measured: without this, a 5s SIGSTOP cost 0s of wall
		// clock; with it, it costs 5s.
		const uint64_t max_debt = static_cast<uint64_t>(
			seconds_per_sim_tick * static_cast<float>(MAX_CATCHUP_TICKS)
			* static_cast<float>(SDL_GetPerformanceFrequency()));
		if (now_ticks > logic_ticks + max_debt)
			logic_ticks = now_ticks - max_debt;
		while (now_ticks >= logic_ticks && loops < MAX_CATCHUP_TICKS) {
			// No SDL_PumpEvents(): there is no event queue worth pumping without a window.
			// inpt->handle() is still called because NullInputState inherits InputState's
			// bookkeeping, which the game logic expects to have run.
			inpt->handle();

			// Drive input from the recording before the logic that reads it. Recording
			// happens at the same point so that a record/replay round trip sees the same
			// state at the same moment.
			if (replay && replay->isPlaying())
				replay->applyTick(total_ticks + 1);
			else if (replay && replay->isRecording())
				replay->recordTick(total_ticks + 1);

			// The client's tick 1 is spent entirely in GameStateTitle, which requests a switch
			// to GameStateLoad but does not run GameStatePlay::logic() -- this function's
			// ancestor -- itself until the tick after. replay->applyTick()/recordTick() above
			// still consume/produce that tick's input regardless of which GameState is active,
			// exactly as they do here, so the corpus's golden hashes and every recording's
			// input-to-tick mapping were captured against a server whose first REAL simulated
			// tick is tick 2, not tick 1. Replicated verbatim rather than eliminated: skipping
			// this makes every subsequent tick of serverLogic() run one tick "early" against
			// input recorded for the tick after it -- found by bisecting a divergence that
			// looked like a single mismatched field (enemy positions) down to every entity
			// having been placed one full tick's worth of RNG draws off from the golden run.
			static bool first_tick = true;
			if (first_tick)
				first_tick = false;
			else {
				serverLogic();
				// P3.4: broadcast this tick's settled state. Guarded inside the function itself
				// (netmgr is NULL on every non---dedicated run), so this call is free elsewhere.
				serverBroadcastSnapshot();
			}
			inpt->resetScroll();

			total_ticks++;

			// P2.2 AC5/AC6 diagnostic -- after this tick's logic (and its total_ticks++), same
			// tick number the digest/liveness bookkeeping just below uses.
			if (dump_ai_targets)
				serverDumpAiTargets(total_ticks);

			// Liveness. A recording that has stopped simulating still produces a digest and
			// still satisfies every 'requires' entry it satisfied earlier, because those ask
			// whether an event EVER fired. This is the tick it last did. See plans/phase0/P0.5e.
			unsigned long ev_total = sim_events->getTotal();
			if (ev_total != prev_ev_total) {
				prev_ev_total = ev_total;
				last_ev_tick = total_ticks;
			}

			// The tick the player died on, if they did. First transition only -- a corpse does
			// not come back, and the interesting number is when the recording stopped being
			// about a player. See plans/phase0/P0.5e.
			if (died_at == 0 && local && !local->stats.alive)
				died_at = total_ticks;

			// Per-tick digests make a divergence bisectable: diff two runs and the first
			// differing line is the exact tick they parted.
			if (hash_every > 0 && total_ticks % hash_every == 0) {
				uint64_t h = hash_replicated ? WorldHash::computeReplicated(total_ticks) : WorldHash::compute(total_ticks);
				printf("tick %lu %s\n", total_ticks, WorldHash::toString(h).c_str());
			}

			if (total_ticks % TRAJECTORY_SAMPLE_TICKS == 0)
				traj = WorldHash::mixU64(traj, WorldHash::compute(total_ticks));

			done = server_exit_requested || shutdown_requested;
			if (max_ticks > 0 && total_ticks >= max_ticks)
				done = true;

			// Leave the catch-up loop immediately. Setting 'done' does not end it -- its
			// condition is the accumulator, not this flag -- so without this break the server
			// simulates one to four extra ticks past the stopping point, and exactly how many
			// depends on wall-clock timing. That made --max-ticks nondeterministic and showed
			// up as a bimodal world digest before the tick-by-tick digest localised it.
			if (done)
				break;

			logic_ticks += static_cast<uint64_t>(seconds_per_sim_tick * static_cast<float>(SDL_GetPerformanceFrequency()));
			loops++;
		}

		if (shutdown_requested)
			done = true;

		// Same frame pacing as the client, minus the busy-wait: a dedicated server has no
		// reason to burn a core spinning for sub-millisecond accuracy.
		if (getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter()) < seconds_per_sim_tick) {
			int32_t delay_ms = static_cast<int32_t>((seconds_per_sim_tick - getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter())) * 1000.f);
			if (delay_ms > 0)
				SDL_Delay(delay_ms);
		}
		prev_ticks = SDL_GetPerformanceCounter();
	}

	// Always fold in the final state, whether or not it landed on a sample boundary.
	traj = WorldHash::mixU64(traj, WorldHash::compute(total_ticks));
	if (trajectory)
		*trajectory = traj;
	if (last_event_tick)
		*last_event_tick = last_ev_tick;
	if (died_tick)
		*died_tick = died_at;

	return total_ticks;
}

static void printHelp() {
	printf("Command line options:\n"
	       "--help                   Prints this message.\n"
	       "--version                Prints the release version.\n"
	       "--data-path=<PATH>       Specifies an exact path to look for mod data.\n"
	       "--mods=<MOD>,...         Starts the server with only these mods enabled.\n"
	       "--load-slot=<SLOT>       Loads a save slot by numerical index.\n"
	       "--max-ticks=<N>          Stops after N logic ticks. For testing.\n"
	       "--sim-seed=<N>           Seeds the simulation RNG. Default is fixed.\n"
	       "--record=<FILE>          Records per-tick input to FILE.\n"
	       "--replay=<FILE>          Replays input from FILE. Refuses a version or mod mismatch.\n"
	       "--hash                   Prints a digest of world state at exit.\n"
	       "--hash-every=<N>         Prints a digest every N ticks, for bisecting a divergence.\n"
	       "--hash-replicated        With --hash-every, digest only the fields the network\n"
	       "                         already replicates -- comparable to a headless flare\n"
	       "                         client's own --hash-replicated output. P3.7.\n"
	       "--dump-players           Prints one line per player right after construction, then\n"
	       "                         runs normally. P2.1 diagnostic -- see PlayerManager.h.\n"
	       "--assert-player-wiring   Checks every player's PlayerInventory/ActionBarState/\n"
	       "                         PowerBonusState owner/sibling back-pointers (P2.3b) against\n"
	       "                         playerm's own parallel arrays right after construction,\n"
	       "                         prints one 'player=N ok=0|1' line per player, and exits\n"
	       "                         without running the simulation: 0 if every player's wiring\n"
	       "                         matches, 1 otherwise.\n"
	       "--max-fps=<N>            Render frame limit. Accepted and ignored: the server\n"
	       "                         renders nothing and always simulates at 60 Hz.\n"
	       "--headless               Accepted and implied; the server is always headless.\n"
	       "--spawn-test-players=<N> P2.2 test infrastructure. Total player count (including the\n"
	       "                         one --load-slot loads as id 0): clones player 0's loaded\n"
	       "                         stats/inventory/action bar into ids 1..N-1, spaced apart so\n"
	       "                         proximity-based AI targeting has something to distinguish.\n"
	       "                         Does not go through SaveLoad.cpp.\n"
	       "--test-player-summon=<ID>  With --spawn-test-players, binds power ID to the LAST\n"
	       "                         spawned test player's action bar (not player 0's), then\n"
	       "                         re-runs the summon-prototype preload so it sees the new\n"
	       "                         binding.\n"
		   "--dump-ai-targets        Prints one line per hostile entity per tick: which player\n"
		   "                         id the scored AI target selector chose, with target_los=0|1|-,\n"
		   "                         or target=none.\n"
		   "--dump-damage-events    Prints one line per resolved damage event as source->target,\n"
		   "                         including hero->hero when friendly fire occurs.\n"
		   "--kill-player=<N>       Sets player N dead immediately after startup, for continuity tests.\n"
	       "--dump-summon-prototypes Prints one line per player per spawn-type power bound to\n"
	       "                         their known-powers list or action bar, and whether\n"
	       "                         EntityManager has a loaded prototype for it, then exits\n"
	       "                         without running the simulation.\n"
	       "--dedicated              Opens a real network host. Player id 0 stays the\n"
	       "                         --load-slot-loaded local/operator player; connecting clients\n"
	       "                         are provisioned into ids 1..--max-players-1 and their\n"
	       "                         PLAYER_COMMAND packets drive their own Avatar for real. See\n"
	       "                         plans/phase3/P3.3-authoritative-server-tick.md.\n"
	       "--port=<N>               With --dedicated, the TCP port to listen on. Default 44680.\n"
	       "--max-players=<N>        With --dedicated, the connection cap, 2-8 (D3). Default 8.\n");
}

static std::string parseServerArg(const std::string& arg) {
	if (arg.length() > 2 && arg.substr(0, 2) == "--") {
		size_t eq = arg.find('=');
		if (eq == std::string::npos)
			return arg.substr(2);
		return arg.substr(2, eq - 2);
	}
	return "";
}

static std::string parseServerArgValue(const std::string& arg) {
	size_t eq = arg.find('=');
	if (eq == std::string::npos)
		return "";
	return arg.substr(eq + 1);
}

int main(int argc, char *argv[]) {
	settings = new Settings();

	ServerCmdLineArgs args;
	bool done = false;

	for (int i = 1; i < argc; i++) {
		std::string arg_full = std::string(argv[i]);
		std::string arg = parseServerArg(arg_full);

		if (arg == "version") {
			printf("%s\n", VersionInfo::createVersionStringFull().c_str());
			done = true;
		}
		else if (arg == "help") {
			printHelp();
			done = true;
		}
		else if (arg == "headless") {
			// Accepted for symmetry with the client and for explicit scripts. The server
			// is always headless; there is no way to turn this off.
		}
		else if (arg == "data-path") {
			settings->custom_path_data = parseServerArgValue(arg_full);
		}
		else if (arg == "ignore-data-path") {
			settings->custom_path_data_ignore = true;
		}
		else if (arg == "mods") {
			std::string mod_list_str = parseServerArgValue(arg_full);
			while (!mod_list_str.empty())
				args.mod_list.push_back(Parse::popFirstString(mod_list_str));
		}
		else if (arg == "load-slot") {
			args.load_slot = parseServerArgValue(arg_full);
		}
		else if (arg == "max-ticks") {
			args.max_ticks = strtoul(parseServerArgValue(arg_full).c_str(), NULL, 10);
		}
		else if (arg == "record") {
			args.record_path = parseServerArgValue(arg_full);
		}
		else if (arg == "replay") {
			args.replay_path = parseServerArgValue(arg_full);
		}
		else if (arg == "sim-seed") {
			// Phase 3 will take this from the host instead. Until then it exists so that the
			// RNG-to-world-state link is testable: two seeds must produce two digests.
			args.sim_seed = strtoull(parseServerArgValue(arg_full).c_str(), NULL, 0);
		}
		else if (arg == "hash") {
			args.hash_at_exit = true;
		}
		else if (arg == "hash-every") {
			args.hash_every = strtoul(parseServerArgValue(arg_full).c_str(), NULL, 10);
		}
		else if (arg == "hash-replicated") {
			args.hash_replicated = true;
		}
		else if (arg == "dump-players") {
			args.dump_players = true;
		}
		else if (arg == "assert-player-wiring") {
			args.assert_player_wiring = true;
		}
		else if (arg == "spawn-test-players") {
			args.spawn_test_players = strtol(parseServerArgValue(arg_full).c_str(), NULL, 10);
		}
		else if (arg == "test-player-summon") {
			args.test_player_summon = static_cast<PowerID>(strtoul(parseServerArgValue(arg_full).c_str(), NULL, 10));
		}
		else if (arg == "dump-ai-targets") {
			args.dump_ai_targets = true;
		}
		else if (arg == "dump-damage-events") {
			args.dump_damage_events = true;
		}
		else if (arg == "kill-player") {
			args.kill_player = strtol(parseServerArgValue(arg_full).c_str(), NULL, 10);
		}
		else if (arg == "dump-summon-prototypes") {
			args.dump_summon_prototypes = true;
		}
		else if (arg == "dedicated") {
			args.dedicated = true;
		}
		else if (arg == "port") {
			args.port = static_cast<unsigned short>(strtoul(parseServerArgValue(arg_full).c_str(), NULL, 10));
		}
		else if (arg == "max-players") {
			args.max_players = static_cast<int>(strtol(parseServerArgValue(arg_full).c_str(), NULL, 10));
		}
		else if (arg == "max-fps") {
			// Accepted so that the sim-rate-vs-render-rate claim can actually be tested: a
			// server run at 30 and at 144 must produce the same tick count in the same wall
			// time. The value is stored and then never read, because the server renders
			// nothing -- which is exactly the property under test.
			settings->max_frames_per_sec = static_cast<unsigned short>(
				strtoul(parseServerArgValue(arg_full).c_str(), NULL, 10));
		}
		else {
			printf("'%s' is not a valid command line option. Try '--help' for a list of valid options.\n", argv[i]);
			delete settings;
			return 1;
		}
	}

	if (done) {
		delete settings;
		return 0;
	}

	signal(SIGINT, serverSignalHandler);
	signal(SIGTERM, serverSignalHandler);

	if (args.load_slot.empty()) {
		Utils::logError("main_server: --load-slot is required.");
		delete settings;
		return 1;
	}

	serverInit(args);

	// After serverInit so the mod list is loaded and can be validated against the recording.
	if (!args.replay_path.empty()) {
		if (!replay->startPlayback(args.replay_path)) {
			// Utils::Exit, not serverCleanup + return: the engine is half-initialised here and
			// ~AnimationManager asserts that every animation has been released, which has not
			// happened yet. This is the engine's own idiom for a fatal startup error, and it is
			// what the --headless assertion above uses.
			Utils::logError("main_server: refusing to replay. Nothing was simulated.");
			Utils::Exit(1);
		}
		// The recording's seed wins. Replaying under a different seed is not a replay.
		sim_rng->seed(replay->getSeed());
		Utils::logInfo("main_server: sim_rng reseeded from replay with 0x%llx",
		               static_cast<unsigned long long>(sim_rng->getSeed()));
	}
	else if (!args.record_path.empty()) {
		if (!replay->startRecording(args.record_path)) {
			Utils::Exit(1);
		}
	}

	// After the reseed above, not before -- see serverConstructSim()'s own comment for why this
	// ordering is load-bearing rather than cosmetic.
	serverConstructSim(args);

	// P3.3. After serverConstructSim() -- player 0 (the reserved local/operator id) must already
	// exist before any real connection can be accepted, and mods (hashModList's input) are already
	// loaded by serverInit() above. Before serverSpawnTestPlayers()/serverMainLoop() so nothing can
	// race a connection in.
	if (args.dedicated) {
		if (args.max_players < 2 || args.max_players > 8) {
			Utils::logError("main_server: --max-players=%d out of range (D3: 2-8).", args.max_players);
			Utils::Exit(1);
		}
		unsigned short port = (args.port != 0) ? args.port : DEFAULT_SERVER_PORT;
		uint32_t mod_hash = Net::hashModList(mods->mod_list);
		netmgr = new Net::NetworkManager();
		if (!netmgr->startHost(port, static_cast<unsigned int>(args.max_players), mod_hash)) {
			Utils::logError("main_server: --dedicated could not open port %u.", static_cast<unsigned>(port));
			Utils::Exit(1);
		}
		// Id 0 is player 0, the local/operator slot serverConstructSim() just created -- reserve
		// it so the first real connection can never collide with it. See NetworkManager.cpp's
		// acceptLoop() comment for why this is a floor, not merely a starting value.
		netmgr->seedNextPlayerID(1);
		Utils::logInfo("main_server: --dedicated listening on port %u, max-players=%d.",
		               static_cast<unsigned>(port), args.max_players);
		// stdout, not just the log: a test harness driving this server as a subprocess (no
		// access to ModManager's internal resolution) needs this exact value to send a HELLO the
		// handshake will accept -- see plans/artifacts/P3.3-net-server-smoke.sh.
		printf("dedicated mod_hash=0x%08x port=%u\n", static_cast<unsigned>(mod_hash), static_cast<unsigned>(port));
		fflush(stdout); // stdout is fully buffered under a pipe -- a harness reading this line
		                 // before the process exits needs it flushed now, not at the next buffer
		                 // fill or at exit.
	}

	// P2.2 AC5-AC7 test infrastructure -- after player 0 is fully loaded (serverSpawnTestPlayers()
	// clones its state), before any tick has run.
	serverSpawnTestPlayers(args);
	hazards->dump_damage_events = args.dump_damage_events;
	serverKillPlayer(args);

	if (args.dump_summon_prototypes) {
		serverDumpSummonPrototypes();
		// Diagnostic mode, same idiom as --assert-player-wiring below: exits without running the
		// simulation.
		Utils::Exit(0);
	}

	// P2.1 diagnostics -- both read playerm right after construction, before any tick has run.
	// stdout, not the log: golden/CI parsing shouldn't have to deal with timestamps.
	if (args.dump_players) {
		printf("players=%zu\n", playerm->count());
		for (size_t i = 0; i < playerm->players.size(); ++i) {
			printf("player id=%u\n", static_cast<unsigned>(playerm->players[i]->id));
		}
	}

	// P2.3b: --assert-pc-alias checked pc/pinv/pab/pbs against playerm->local()'s entry -- an
	// invariant that stopped meaning anything the moment those four globals were deleted (there
	// is no alias left to drift out of sync). Repurposed rather than dropped outright: the same
	// class of bug this plan fixes -- a PlayerInventory/ActionBarState/PowerBonusState wired to
	// the wrong player -- can still happen one level deeper now, in the owner/sibling
	// back-pointers PlayerManager::create() sets (P2.3b step 1). This checks THAT invariant, for
	// every player, not just local(): each object's owner/actionbar/powerbonus pointer must
	// resolve back to the SAME player's own entry in playerm's parallel arrays.
	if (args.assert_player_wiring) {
		bool ok = true;
		for (size_t i = 0; i < playerm->players.size(); ++i) {
			Avatar* player = playerm->players[i];
			PlayerInventory* inventory = playerm->inventories[i];
			ActionBarState* actionbar = playerm->actionbars[i];
			PowerBonusState* powerbonus = playerm->powerbonuses[i];

			bool player_ok = (inventory->owner == player) &&
			                 (inventory->actionbar == actionbar) &&
			                 (inventory->powerbonus == powerbonus) &&
			                 (actionbar->owner == player) &&
			                 (powerbonus->actionbar == actionbar);

			printf("assert-player-wiring: player=%u ok=%d\n", static_cast<unsigned>(player->id), player_ok ? 1 : 0);
			ok = ok && player_ok;
		}
		// Diagnostic mode: checks a construction-time invariant and exits, same idiom as the
		// startPlayback failure above -- the sim is fully constructed but never ticked, and
		// nothing downstream expects that state, so skip serverCleanup() and the main loop.
		Utils::Exit(ok ? 0 : 1);
	}

	uint64_t trajectory = WorldHash::init();
	unsigned long last_event_tick = 0;
	unsigned long died_tick = 0;
	unsigned long ticks = serverMainLoop(args.max_ticks, args.hash_every, args.hash_replicated, &trajectory,
	                                     &last_event_tick, &died_tick, args.dump_ai_targets);

	replay->finish();

	if (shutdown_requested)
		Utils::logInfo("main_server: shutdown requested, stopping.");
	Utils::logInfo("main_server: simulated %lu logic ticks.", ticks);

	// Reported unconditionally rather than behind a flag: an emit path with no drain is a slow
	// leak, and a leak you have to remember to ask about is one you find in production. This is
	// the deepest the queue ever got, not a total -- it should stay small no matter how long the
	// server ran. See SimEventQueue::getHighWater().
	Utils::logInfo("main_server: sim event queue high water = %lu",
	               static_cast<unsigned long>(sim_events->getHighWater()));

	// stdout and unconditional, for the same reason the high water is: a coverage claim you have
	// to remember to ask for is a coverage claim nobody checks. tests/run-replays.sh parses this
	// line to assert that a recording named 'attack' actually attacks -- P0.5b's did not, and the
	// digest could not tell anyone. Single line, "name=count" pairs, stable names.
	{
		printf("simevents");
		for (int i = 0; i < SimEvent::TYPE_COUNT; ++i)
			printf(" %s=%lu", SimEvent::typeName(i), sim_events->getCount(i));
		// Two liveness fields, on the same line for the same reason the counts are here at all:
		// a claim you have to remember to ask for is a claim nobody checks.
		//
		// died_tick is the gate run-replays.sh enforces (0 = the player survived). A fixture
		// that dies mid-recording leaves a corpse for the rest of the run: P0.5d's beatdown
		// died at 1186 of 2956 and every 'requires' entry still passed, because those ask only
		// whether an event EVER fired.
		//
		// last_tick is diagnostic, not a gate. It is the tick of the last simulation event, and
		// it is a poor liveness measure on its own -- smoke's last event is at 378 of 600 while
		// its world keeps changing to the final tick. It is printed because it is what made the
		// beatdown problem visible. See plans/phase0/P0.5e.
		printf(" last_tick=%lu died_tick=%lu", last_event_tick, died_tick);

		// How many equipment slots still hold something. Third liveness field, and the one that
		// speaks to P1.3's own stated failure mode: "a subtle error means players lose gear".
		//
		// Read through playerm->local()'s own inventory (P2.3b -- was the global pinv), deliberately,
		// while WorldHash.cpp still reads the same storages through menu->inv. P1.3d-4a made
		// MenuInventory::inventory a POINTER INTO this array rather than a second one, and a
		// commit that got that wrong produces byte-identical goldens -- measured, all nine, so
		// "nothing moved" is that bug's symptom too. One reader on each side of the alias is what
		// makes the pair falsifiable: give the menu its own copy and these counters go to 0 while
		// the digest does not notice.
		// The digest covers equipment contents, but a golden can only say "different", and the
		// obvious way for a refactor of the inventory to go wrong is for gear to quietly stop
		// arriving or quietly fall out. tests/replays/MANIFEST pins this per row, so that is a
		// named number a reviewer can read rather than a hex digest nobody can interpret.
		PlayerInventory* local_inv = playerm ? playerm->inventoryFor(playerm->local_id) : NULL;
		int equipped = 0;
		if (local_inv) {
			int slots = local_inv->inventory[PlayerInventory::EQUIPMENT].getSlotNumber();
			for (int i = 0; i < slots; ++i) {
				if (!local_inv->inventory[PlayerInventory::EQUIPMENT][i].empty())
					equipped++;
			}
		}
		printf(" equipped=%d", equipped);

		// Which equipment set is live. Separate from the count above because they answer
		// different questions: 'equipped' is how much gear exists, 'equipset' is how much of it
		// the character is actually wearing. MenuInventory::isEquipSlotActive() returns false for
		// every slot when this is 0, so the two numbers can disagree completely.
		printf(" equipset=%d", local_inv ? static_cast<int>(local_inv->active_equipment_set) : -1);

		// How many carried slots hold something. The COUNTERPART to 'equipped', and the pair is
		// the point: P1.3d-4 moves the item storage out of the menus, and the way that goes wrong
		// is one copy of the data becoming two. A single number cannot see that. Two can --
		// an item that moves from the carried area to an equipment slot must make one go down as
		// the other goes up, and a duplicate shows as both going up.
		int carried = 0;
		if (local_inv) {
			int cslots = local_inv->inventory[PlayerInventory::CARRIED].getSlotNumber();
			for (int i = 0; i < cslots; ++i) {
				if (!local_inv->inventory[PlayerInventory::CARRIED][i].empty())
					carried++;
			}
		}
		printf(" carried=%d", carried);

		// Diagnostic, like last_tick -- NOT pinned in the MANIFEST. It is here because without it
		// a death-penalty failure is unreadable: the penalty's random draw removes ONE UNIT of a
		// randomly chosen stack, so drawing a 1-quantity item empties its slot and drawing the
		// 750-strong currency stack does not. 'carried' alone shows the difference and cannot
		// explain it. Currency is already hashed, so this is not new coverage, only new legibility.
		printf(" currency=%d", local_inv ? local_inv->currency : -1);

		// P1.3e-a's own version of the equipped/carried pair above, and the reason it needs one:
		// WorldHash never hashes action bar bindings at all, on either side of this change, so a
		// broken alias here would be invisible to every golden AND to 'equipped'/'carried' -- this
		// is the only thing that can see it. Two readers of the same claimed-single hotkeys array:
		// one through playerm->local()'s own action bar (P2.3b -- was the global pab) and one
		// through menu->act (MenuActionBar's reference member, bound to that same ActionBarState's
		// hotkeys in its constructor). Since P1.4c, menu->act does not exist on the server at all,
		// so hotkeys_menu is always 0 here -- not a broken alias, just a diagnostic with nothing on
		// the other side to compare against any more. Not pinned in the MANIFEST, so this is
		// cosmetic, not a regression.
		ActionBarState* local_ab = playerm ? playerm->actionbarFor(playerm->local_id) : NULL;
		int hotkeys_pab = 0;
		if (local_ab) {
			for (unsigned i = 0; i < local_ab->slots_count; ++i)
				if (local_ab->hotkeys[i] != 0) hotkeys_pab++;
		}
		int hotkeys_menu = 0;
		if (menu && menu->act) {
			for (unsigned i = 0; i < menu->act->slots_count; ++i)
				if (menu->act->hotkeys[i] != 0) hotkeys_menu++;
		}
		printf(" hotkeys_pab=%d hotkeys_menu=%d", hotkeys_pab, hotkeys_menu);

		printf("\n");
	}

	if (args.dump_players) {
		for (size_t p = 0; p < playerm->players.size(); ++p) {
			Avatar* player = playerm->players[p];
			printf("player id=%u xp=%lu alive=%d\n", static_cast<unsigned>(player->id), player->stats.xp, player->stats.alive ? 1 : 0);
		}
	}

	// stdout, not the log: golden-file comparison should not have to parse timestamps.
	//
	// This is the TRAJECTORY digest -- every sample taken during the run folded together, not
	// just the world as it stands now. See TRAJECTORY_SAMPLE_TICKS for why the end state alone
	// was not enough. The final-state digest is still printed underneath it for debugging; the
	// golden files compare the first line, because tests/run-replays.sh greps '^0x'.
	if (args.hash_at_exit) {
		printf("%s\n", WorldHash::toString(trajectory).c_str());
		Utils::logInfo("main_server: final-state digest = %s",
		               WorldHash::toString(WorldHash::compute(ticks)).c_str());
	}

	serverCleanup();

	delete settings;
	return 0;
}
