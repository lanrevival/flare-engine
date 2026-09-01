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
#include "Animation.h"
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
#include "net/NetProtocol.h"
#include "net/NetworkManager.h"
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

#include <algorithm>
#include <cassert>

GameStatePlay::GameStatePlay()
	: GameState()
	, enemy(NULL)
	, netmgr(NULL)
	, net_connect_attempted(false)
	, net_host_attempted(false)
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

// P3.4b. Connects lazily (called from logic(), not the constructor) because player->stats.name
// is not guaranteed loaded yet when this object is constructed -- playerm->create(0) alone only
// allocates a bare Avatar (PlayerManager::create(), PlayerManager.cpp) -- and by the time logic()
// first runs, whatever loaded the save has already run. No-op once attempted, successfully or not:
// this plan does not retry a failed/refused connection.
void GameStatePlay::netConnectIfNeeded() {
	if (net_connect_attempted || settings->net_connect_target.empty())
		return;
	net_connect_attempted = true;

	size_t colon = settings->net_connect_target.find(':');
	if (colon == std::string::npos) {
		Utils::logError("GameStatePlay: --connect must be <host>:<port>, got '%s'", settings->net_connect_target.c_str());
		return;
	}
	std::string host = settings->net_connect_target.substr(0, colon);
	unsigned short port = static_cast<unsigned short>(Parse::toInt(settings->net_connect_target.substr(colon + 1)));

	netmgr = new Net::NetworkManager();
	if (!netmgr->startClient(host, port, player->stats.name, Net::hashModList(mods->mod_list))) {
		Utils::logError("GameStatePlay: could not start network client to %s", settings->net_connect_target.c_str());
		delete netmgr;
		netmgr = NULL;
	}
}

// P3.4b. Pumps the connection (no-op if not connected), applies the latest MSG_PLAYER_SNAPSHOT to
// every OTHER player it names, and removes any previously-seen remote player the latest snapshot no
// longer names (P3.4's snapshot is a full dump every tick, not a delta -- absence IS the disconnect
// signal, the same contract the server side of this relies on). The local avatar (entry.id ==
// netmgr->localPlayerID()) is deliberately skipped -- see this plan's Why for the reasoning: the
// local player stays client-simulated, matching D2's "client-authoritative movement" on a LAN.
void GameStatePlay::netSyncPlayers() {
	if (!netmgr)
		return;
	netmgr->update();
	if (!netmgr->hasLocalPlayerID())
		return;

	PlayerID from;
	std::string payload;
	Net::MsgPlayerSnapshot snap;
	bool got_one = false;
	// P3.5a: MSG_MAP_SYNC is sent exactly once, right when this client's own peer is provisioned --
	// decoded here alongside MSG_PLAYER_SNAPSHOT since both arrive through the same popPacket() loop.
	Net::MsgMapSync map_sync;
	bool got_map_sync = false;
	while (netmgr->popPacket(&from, &payload)) {
		uint8_t type = Net::peekMessageType(payload);
		if (type == Net::MSG_PLAYER_SNAPSHOT && Net::decodePlayerSnapshot(payload, snap))
			got_one = true;
		else if (type == Net::MSG_MAP_SYNC && Net::decodeMapSync(payload, map_sync))
			got_map_sync = true;
		// Any other message type this tick is silently dropped -- nothing else is defined yet.
	}
	// Handled before the got_one early return below -- MSG_MAP_SYNC can arrive on a tick with no
	// accompanying MSG_PLAYER_SNAPSHOT if the two frames land in separate TCP reads.
	if (got_map_sync)
		netApplyMapSync(map_sync);
	if (!got_one)
		return;

	PlayerID local_id = netmgr->localPlayerID();
	std::vector<PlayerID> seen; // network ids, not playerm ids -- see remotePlayerId()
	for (size_t i = 0; i < snap.players.size(); ++i) {
		if (snap.players[i].id == local_id)
			continue;
		seen.push_back(snap.players[i].id);
		netApplySnapshotEntry(snap.players[i]);
	}

	// A playerm id below REMOTE_PLAYER_ID_BASE is never one this function provisioned (it's always
	// exactly {0}, this client's own local avatar) -- only ids at or above the base are candidates
	// for removal here.
	for (size_t i = 0; i < playerm->players.size(); ++i) {
		uint8_t id = playerm->players[i]->id;
		if (id < REMOTE_PLAYER_ID_BASE)
			continue;
		uint8_t network_id = static_cast<uint8_t>(id - REMOTE_PLAYER_ID_BASE);
		if (std::find(seen.begin(), seen.end(), network_id) == seen.end()) {
			playerm->remove(id);
			--i; // remove() erases in place; re-check this index
		}
	}
}

// P3.4b. Provisions a placeholder Avatar for a never-before-seen remote player id, mirroring
// serverProvisionPlayer()'s clone-from-local pattern (main_server.cpp) -- same reasons, same
// load-bearing calls (loadAnimations()/loadSounds(), not cosmetic -- see that function's own
// comment on AnimationManager's filename refcounting). Then always writes this tick's fields.
Avatar* GameStatePlay::netApplySnapshotEntry(const Net::PlayerSnapshotEntry& entry) {
	uint8_t local_id = remotePlayerId(entry.id);
	Avatar* av = playerm->get(local_id);
	if (!av) {
		PlayerInventory* source_inv = playerm->inventoryFor(playerm->local_id);
		ActionBarState* source_ab = playerm->actionbarFor(playerm->local_id);

		playerm->create(local_id);
		av = playerm->get(local_id);
		PlayerInventory* new_inv = playerm->inventoryFor(local_id);
		ActionBarState* new_ab = playerm->actionbarFor(local_id);
		if (!av || !new_inv || !new_ab || !source_inv || !source_ab)
			return av;

		av->stats = player->stats; // class/sprite/animation-set template -- see main_server.cpp:1545
		new_inv->loadEquipmentData();
		*new_ab = *source_ab;
		new_ab->owner = av;
		for (unsigned s = 0; s < new_ab->hotkeys.size(); ++s)
			new_ab->hotkeys[s] = 0;

		av->stats.pos.x = entry.pos_x;
		av->stats.pos.y = entry.pos_y;
		av->loadAnimations(); // load-bearing, not cosmetic -- see main_server.cpp:1573-1579
		av->loadSounds();

		// Deliberately NOT calling wmap->collider.blockPlayer() here (unlike
		// serverProvisionPlayer()'s D10 call, main_server.cpp:1582): Avatar::logic() is what
		// unblocks the old cell and blocks the new one on every real move (Avatar.cpp:1007,1137,
		// 1155), and a remote avatar here never runs its own logic() -- it is driven purely by
		// snapshot writes below. Blocking once at spawn with no matching per-tick unblock/reblock
		// as it moves would leave stale blocked cells behind on the map. The (acknowledged) cost:
		// enemies do not path around a visible remote player in this pass. Revisit once this plan's
		// snapshot-apply path also updates the collider correctly on every position change.
	}

	av->stats.pos.x = entry.pos_x;
	av->stats.pos.y = entry.pos_y;
	av->stats.direction = entry.direction;
	av->stats.hp = entry.hp;
	av->stats.current[Stats::HP_MAX] = entry.hp_max;
	av->stats.alive = entry.alive;
	if (!entry.animation.empty())
		av->setAnimation(entry.animation);

	return av;
}

// P3.5a. Routes through the exact same intermap-teleport machinery checkTeleport() already runs for
// a normal in-game teleport (portal, waypoint, ...), so a joining client's own map load reuses
// already-proven code rather than a second, parallel map-load path. checkTeleport() runs later in
// this same logic() call (see logic()'s own call order: netSyncPlayers() then checkTeleport()), so
// setting these fields here is enough for the load to happen before this tick ends. checkTeleport()'s
// own map-load branch calls entitym->handleNewMap(), which is what preloads this client's own
// summon-power prototypes into its own simulation -- no separate preload call is needed here, unlike
// the server/host side (see EntityManager::preloadSummonPrototypesForPlayer()'s own comment for why
// that side needs a narrower call instead of just reusing handleNewMap()).
void GameStatePlay::netApplyMapSync(const Net::MsgMapSync& sync) {
	mapr->teleportation = true;
	mapr->teleport_mapname = sync.map_filename;
	mapr->teleport_destination.x = sync.spawn_x;
	mapr->teleport_destination.y = sync.spawn_y;
	mapr->teleport_destination_id = 0;
	mapr->force_spawn_pos = false;
}

// P3.4c. Opens netmgr as a HOST -- mutually exclusive with netConnectIfNeeded() above; main.cpp
// already refused both --connect and --host being set together. Lazy for the same reason
// netConnectIfNeeded() is: mods->mod_list must be populated (hashModList()) and playerm->local()
// must already be a fully-formed Avatar (netHostProvisionPeer() clones from it), neither of which
// is guaranteed at construction time.
void GameStatePlay::netHostIfNeeded() {
	if (net_host_attempted || settings->net_host_port == 0)
		return;
	net_host_attempted = true;

	netmgr = new Net::NetworkManager();
	if (!netmgr->startHost(settings->net_host_port, static_cast<unsigned int>(settings->net_max_players), Net::hashModList(mods->mod_list))) {
		Utils::logError("GameStatePlay: --host could not open port %u.", static_cast<unsigned>(settings->net_host_port));
		delete netmgr;
		netmgr = NULL;
		return;
	}
	// Id 0 is this client's own local avatar (playerm->create(0) in the constructor) -- reserve it
	// so the first real connection can never collide with it. Mirrors main_server.cpp's own
	// seedNextPlayerID(1) call in main(), same reasoning: unlike P3.4b's remotePlayerId() offset
	// (needed because a --connect client's local id and a REMOTE host's own player-0 id are
	// different processes' id spaces colliding at 0), this client IS the id allocator here, so
	// reserving id 0 up front is sufficient -- no offset needed for anything this function creates.
	netmgr->seedNextPlayerID(1);
	Utils::logInfo("GameStatePlay: --host listening on port %u, max-players=%d.",
	               static_cast<unsigned>(settings->net_host_port), settings->net_max_players);
}

// P3.4c. Clones playerm->local()'s loaded class/gear/action-bar state into a freshly created id,
// exactly like main_server.cpp's serverProvisionPlayer() -- see that function's own comments for why
// every one of these calls is load-bearing rather than cosmetic (loadEquipmentData() instead of a
// struct-copy avoids double-freeing PlayerInventory's raw ItemStack* storage; loadAnimations()/
// loadSounds() are required for AnimationManager's filename refcounting; blockPlayer() seeds D10's
// player-blocks-enemies collision so the very first tick before this peer's own Avatar::logic() runs
// isn't unblocked). Unlike netApplySnapshotEntry() (P3.4b), this Avatar goes on to be driven by real,
// this-peer's-own decoded PLAYER_COMMANDs every tick via netHostDrivePeers() -- including its own
// real Avatar::logic() calls, which is what keeps the collider correctly blocked/unblocked on every
// subsequent move (Avatar.cpp:1007,1137,1155) -- so, unlike P3.4b's pure-snapshot remotes, seeding
// the initial block here is safe and complete, not half-correct.
Avatar* GameStatePlay::netHostProvisionPeer(uint8_t id, const FPoint& spawn_pos) {
	Avatar* source = player;
	PlayerInventory* source_inv = player_inventory;
	ActionBarState* source_ab = player_actionbar;
	if (!source || !source_inv || !source_ab) {
		Utils::logError("GameStatePlay: netHostProvisionPeer requires the local player to already be loaded.");
		return NULL;
	}

	playerm->create(id);
	Avatar* new_avatar = playerm->get(id);
	PlayerInventory* new_inv = playerm->inventoryFor(id);
	ActionBarState* new_ab = playerm->actionbarFor(id);
	if (!new_avatar || !new_inv || !new_ab)
		return NULL;

	new_avatar->stats = source->stats;

	new_inv->loadEquipmentData();
	if (new_inv->max_equipment_set > 0)
		new_inv->active_equipment_set = 1;

	*new_ab = *source_ab;
	new_ab->owner = new_avatar;
	for (unsigned s = 0; s < new_ab->hotkeys.size(); ++s)
		new_ab->hotkeys[s] = 0;

	new_avatar->stats.pos = spawn_pos;

	new_avatar->loadAnimations();
	new_avatar->loadSounds();
	mapr->collider.blockPlayer(new_avatar->stats.pos.x, new_avatar->stats.pos.y, id);

	// P3.4d: this peer's own step-FX are never loaded above -- only loadAnimations()/loadSounds()
	// are. Matches server_equipment_changed[8]'s own initial-true default: the first
	// netHostCheckEquipmentChange() pass after provisioning is what actually loads them.
	net_host_equip_changed.insert(id);

	Utils::logInfo("GameStatePlay: provisioned connected peer id=%u at (%.1f, %.1f)",
	               static_cast<unsigned>(id), static_cast<double>(new_avatar->stats.pos.x), static_cast<double>(new_avatar->stats.pos.y));

	return new_avatar;
}

// P3.4c. --host only (a no-op for a plain client or single-player -- netmgr is either NULL or,
// on the --connect path, not isHost()). Mirrors main_server.cpp's serverSyncNetworkPlayers(): pumps
// the transport, provisions a real Avatar for every newly-connected peer, frees a disconnected
// peer's Avatar (D26 -- PlayerManager::remove() already handles the in-flight-hazard/summon cleanup),
// and decodes this tick's PLAYER_COMMAND from every still-connected peer into net_host_cmd.
void GameStatePlay::netHostSyncPeers() {
	net_host_cmd.clear();
	if (!netmgr || !netmgr->isHost())
		return;

	netmgr->update();

	PlayerID id;
	while (netmgr->popDisconnected(&id)) {
		net_host_players.erase(id);
		if (playerm->get(id))
			playerm->remove(id);
	}

	while (netmgr->popConnected(&id)) {
		FPoint spawn_pos = player->stats.pos;
		if (mapr->teleportation)
			spawn_pos = mapr->teleport_destination;
		if (netHostProvisionPeer(id, spawn_pos)) {
			net_host_players.insert(id);
			// P3.5a: mirrors serverSyncNetworkPlayers()'s own two additions -- see
			// plans/phase3/P3.5a-join-map-sync.md.
			entitym->preloadSummonPrototypesForPlayer(id);
			netmgr->sendTo(id, Net::encodeMapSync(mapr->getFilename(), spawn_pos.x, spawn_pos.y));
		}
		// else: logged by netHostProvisionPeer() itself. The peer stays connected but bound to no
		// player -- its packets simply decode into net_host_cmd and are never read by anything,
		// since netHostDrivePeers() only ever consults net_host_players.
	}

	PlayerID from;
	std::string payload;
	while (netmgr->popPacket(&from, &payload)) {
		PlayerCommand cmd;
		if (Net::decodePlayerCommand(payload, cmd)) {
			net_host_cmd[from] = cmd;
		}
		else {
			// P3.2's own fuzz-safety guarantee is what makes this safe to just drop -- see
			// serverSyncNetworkPlayers()'s matching comment.
			Utils::logError("GameStatePlay: dropped a malformed PLAYER_COMMAND from player id=%u.", static_cast<unsigned>(from));
		}
	}
}

// P3.4c. Drives every connected peer's own real Avatar::logic() call, plus the same per-player
// "kind A" state main_server.cpp's serverLogic() already consumes for a driven player: equipment-set
// stepping, level-up, RESPEC, transform/revert, and respawn -- all sourced from that peer's own
// decoded PLAYER_COMMAND, never from this machine's menus (peers have no menu on this screen). The
// LOCAL player's own equivalent blocks, elsewhere in logic(), are untouched by this plan.
//
// Consolidated into one loop per peer rather than main_server.cpp's several separate per-player
// loops (level-up, then command+logic(), then RESPEC, then transform/respawn, each its own pass over
// every driven player) -- none of these five touch another player's state, so interleaving them
// per-peer instead of phase-separating across all peers cannot change the result, and there is no
// second real player on this client's own screen for whom the previous two-phase split was ever
// load-bearing.
//
// P3.4d added the kind-C generalisation this comment used to say was missing: loot auto-pickup and
// title-earning are called inline in the loop below (netHostCheckLoot()/netHostCheckTitle(), right
// before peer->logic(), matching checkLoot()/checkTitle()'s own tick position relative to
// player->logic()); death penalty, equipment-change reload, and used-item consumption are their own
// separate calls elsewhere in logic() (netHostCheckDeathPenalty()/netHostCheckEquipmentChange()/
// netHostCheckUsedItems()), matching those three's own tick positions too. See
// plans/phase3/P3.4d-host-peer-kind-c.md.
void GameStatePlay::netHostDrivePeers() {
	for (std::set<uint8_t>::const_iterator it = net_host_players.begin(); it != net_host_players.end(); ++it) {
		uint8_t id = *it;

		size_t idx = 0;
		for (; idx < playerm->players.size(); ++idx) {
			if (playerm->players[idx]->id == id)
				break;
		}
		if (idx == playerm->players.size())
			continue; // disconnected this same tick; already erased from net_host_players by netHostSyncPeers()

		Avatar* peer = playerm->players[idx];
		PlayerInventory* inventory = playerm->inventories[idx];
		ActionBarState* actionbar = playerm->actionbars[idx];
		PowerBonusState* powerbonus = playerm->powerbonuses[idx];

		PlayerCommand cmd;
		std::map<uint8_t, PlayerCommand>::const_iterator cmd_it = net_host_cmd.find(id);
		if (cmd_it != net_host_cmd.end())
			cmd = cmd_it->second;
		// else: neutral/idle default -- D2, the host never waits a tick on a slow peer.

		// P3.4d: mirrors main_server.cpp's own equip_set_delta trigger site -- see
		// net_host_equip_changed's own comment for why a set-insert is enough here (unlike
		// serverLogic()'s respawn block, this function runs entirely before the tail block that
		// consumes it).
		if (inventory->applyEquipmentSetDelta(cmd.equip_set_delta))
			net_host_equip_changed.insert(id);

		// Never populated from anything real -- remote click-arbitration has no meaning on this
		// client's screen, same reasoning as netApplySnapshotEntry() (P3.4b). Reused for both calls
		// below: neither is ever given real values to arbitrate either way.
		PlayerInputLocks peer_locks;

		// P3.4d: kind C, same tick position checkLoot()/checkTitle() occupy relative to
		// player->logic() in logic() itself -- see each function's own comment.
		netHostCheckLoot(peer, inventory);
		netHostCheckTitle(peer);

		peer->logic(cmd, peer_locks);

		if (peer->stats.level_up) {
			inventory->applyEquipment();
			peer->stats.hp = peer->stats.get(Stats::HP_MAX);
			peer->stats.mp = peer->stats.get(Stats::MP_MAX);
			peer->stats.level_up = false;
		}

		if (peer->close_menus)
			peer->close_menus = false;
		if (peer->show_game_over)
			peer->show_game_over = false;

		if (peer->respec_powers) {
			peer->respec_powers = false;
			EngineSettings::HeroClasses::HeroClass* peer_class = eset->hero_classes.getByName(peer->stats.character_class);

			for (size_t i = 0; i < powerbonus->current_cell.size(); ++i)
				powerbonus->current_cell[i] = 0;
			if (peer_class && !peer->respec_use_engine_defaults) {
				for (size_t j = 0; j < peer_class->powers.size(); j++)
					peer->stats.powers_list.push_back(peer_class->powers[j]);
			}
			powerbonus->clearActionBarBonusLevels();

			for (unsigned i = 0; i < actionbar->slots_count; ++i)
				actionbar->clearSlot(i);
			if (peer_class && !peer->respec_use_engine_defaults)
				actionbar->set(peer_class->hotkeys, ActionBarState::SET_SKIP_EMPTY);
		}

		peer->checkTransform(peer_locks);

		if (peer->setPowers) {
			peer->setPowers = false;
			for (int i = 0; i < MenuActionBar::SLOT_MAX; i++) {
				actionbar->hotkeys_temp[i] = actionbar->hotkeys[i];
				actionbar->hotkeys[i] = 0;
			}
			int count = MenuActionBar::SLOT_MAIN1;
			for (size_t i = 0; i < peer->charmed_stats->powers_ai.size(); i++) {
				if (powers->isValid(peer->charmed_stats->powers_ai[i].id) && powers->powers[peer->charmed_stats->powers_ai[i].id]->beacon != true) {
					actionbar->hotkeys[count] = peer->charmed_stats->powers_ai[i].id;
					actionbar->locked[count] = true;
					count++;
					if (count == MenuActionBar::SLOT_MAX)
						count = 0;
					else if (count == MenuActionBar::SLOT_MAIN1)
						break;
				}
			}
			if (peer->stats.manual_untransform && powers->isValid(peer->untransform_power)) {
				actionbar->hotkeys[count] = peer->untransform_power;
				actionbar->locked[count] = true;
			}
			else if (peer->stats.manual_untransform && peer->untransform_power == 0)
				Utils::logError("GameStatePlay: peer id=%u untransform power not found, cannot untransform manually.", static_cast<unsigned>(id));

			actionbar->updated = true;

			if (peer->stats.transform_with_equipment)
				inventory->applyEquipment();
		}
		if (peer->revertPowers) {
			peer->revertPowers = false;
			for (int i = 0; i < MenuActionBar::SLOT_MAX; i++) {
				actionbar->hotkeys[i] = actionbar->hotkeys_temp[i];
				actionbar->locked[i] = false;
			}
			actionbar->updated = true;
			inventory->applyEquipment();
		}

		if (peer->respawn) {
			peer->stats.alive = true;
			peer->stats.corpse = false;
			peer->stats.cur_state = StatBlock::ENTITY_STANCE;
			inventory->applyEquipment();
			// P3.4d: mirrors main_server.cpp's respawn block setting server_equipment_changed[id] =
			// true right before its own (inline) serverCheckEquipmentChange() call -- see
			// net_host_equip_changed's own comment for why a plain set-insert is enough here instead.
			net_host_equip_changed.insert(id);
			peer->stats.hp = peer->stats.get(Stats::HP_MAX);
			peer->stats.logic();
			peer->stats.recalc();

			for (size_t i = 0; i < powerbonus->current_cell.size(); ++i)
				powerbonus->current_cell[i] = 0;
			powerbonus->clearActionBarBonusLevels();

			powers->activatePassives(&peer->stats);
			peer->respawn = false;
		}
	}
}

// P3.4c. --host only. Called once per tick, unconditionally (even while isPaused() -- a connected
// peer should keep receiving periodic snapshots rather than appear to hang), right after this tick's
// simulation has settled. Identical in construction to main_server.cpp's serverBroadcastSnapshot():
// reads straight off playerm->players, so it reflects exactly what this tick's local and peer
// Avatar::logic() calls just produced, nothing recomputed or guessed.
void GameStatePlay::netHostBroadcastSnapshot() {
	if (!netmgr || !netmgr->isHost())
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

// P3.4d. Kind C: no input dependency, so every connected peer's own inventory applies its own
// death penalty too, not just local's -- mirrors main_server.cpp's own unconditional loop over
// every playerm->inventories entry. Called right after the local player's own
// player_inventory->applyDeathPenalty() call in logic(), same unconditional (not gated by
// isPaused()) position, for the same ordering reason that call's own comment gives: must run
// before checkLoot()/peer->logic().
void GameStatePlay::netHostCheckDeathPenalty() {
	for (std::set<uint8_t>::const_iterator it = net_host_players.begin(); it != net_host_players.end(); ++it) {
		PlayerInventory* peer_inv = playerm->inventoryFor(*it);
		if (peer_inv)
			peer_inv->applyDeathPenalty();
	}
}

// P3.4d. Kind C, auto-pickup only -- mirrors serverCheckLoot() exactly, not
// GameStatePlay::checkLoot(): that function's manual click-pickup branch is
// inpt->mouse/mapr->cam.pos-driven, meaningless for a peer with no camera on this screen, the same
// reasoning main_server.cpp's own comment gives for dropping it there. Called inline from
// netHostDrivePeers()'s own per-peer loop, immediately before peer->logic() -- the same tick
// position checkLoot() occupies relative to player->logic() in logic() itself.
void GameStatePlay::netHostCheckLoot(Avatar* peer, PlayerInventory* inv) {
	if (!peer->stats.alive)
		return;

	ItemStack pickup = loot->checkAutoPickup(peer->stats.pos);
	if (!pickup.empty()) {
		inv->add(pickup, PlayerInventory::CARRIED, ItemStorage::NO_SLOT, PlayerInventory::ADD_PLAY_SOUND, PlayerInventory::ADD_AUTO_EQUIP);
		if (items->isValid(pickup.item)) {
			StatusID pickup_status = camp->registerStatus(items->items[pickup.item]->pickup_status);
			camp->setStatus(pickup_status);
		}
		pickup.clear();
	}
}

// P3.4d. Ported unchanged from checkPrimaryStat(), reading the given peer's own stats (a
// parameter) instead of the player member -- mirrors serverCheckPrimaryStat() exactly. See
// netHostCheckTitle()'s own comment for why this is a duplicate rather than a shared, parameterised
// checkPrimaryStat().
bool GameStatePlay::netHostCheckPrimaryStat(const StatBlock& stats, const std::string& first, const std::string& second) {
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

// P3.4d. Kind C, ported from checkTitle() parameterised by peer -- mirrors serverCheckTitle(Avatar*)
// exactly. Reuses the existing titles vector directly: title definitions are shared game data, not
// per-player. Duplicated rather than parameterising checkTitle()/checkPrimaryStat() themselves --
// see plans/phase3/P3.4d-host-peer-kind-c.md's Notes for the executor. Called inline from
// netHostDrivePeers()'s own per-peer loop, immediately before peer->logic(), matching checkTitle()'s
// own tick position relative to player->logic() in logic() itself.
void GameStatePlay::netHostCheckTitle(Avatar* peer) {
	if (!peer->stats.check_title || titles.empty())
		return;

	int title_id = -1;

	for (unsigned i = 0; i < titles.size(); i++) {
		if (titles[i].title.empty())
			continue;

		if (titles[i].level > 0 && peer->stats.level < titles[i].level)
			continue;
		if (titles[i].power > 0 && std::find(peer->stats.powers_list.begin(), peer->stats.powers_list.end(), titles[i].power) == peer->stats.powers_list.end())
			continue;
		if (!titles[i].primary_stat_1.empty() && !netHostCheckPrimaryStat(peer->stats, titles[i].primary_stat_1, titles[i].primary_stat_2))
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

		title_id = static_cast<int>(i);
		break;
	}

	if (title_id != -1) peer->stats.character_subclass = titles[static_cast<size_t>(title_id)].title;
	peer->stats.check_title = false;
	peer->stats.refresh_stats = true;
}

// P3.4d. Peer-scoped substitute for checkEquipmentChange()'s menu->inv->changed_equipment --
// mirrors main_server.cpp's server_equipment_changed[8] array/serverCheckEquipmentChange() exactly,
// as a set instead (see net_host_equip_changed's own comment). Trigger sites: netHostProvisionPeer()
// (initial load -- matches the array's initial-true default, so this peer's own step-FX, which
// netHostProvisionPeer() never loads, gets loaded once), and netHostDrivePeers()'s own
// applyEquipmentSetDelta() and respawn blocks. Called once from logic()'s own "whether paused or
// not" tail block, right after the existing local checkEquipmentChange() call -- unlike
// main_server.cpp's respawn block (which must call serverCheckEquipmentChange() inline, because its
// own tail loop already ran earlier in serverLogic()'s tick), netHostDrivePeers() runs entirely
// before this tail block, so inserting into the set from its respawn block is enough; this call
// picks it up the same tick.
void GameStatePlay::netHostCheckEquipmentChange() {
	for (std::set<uint8_t>::iterator it = net_host_equip_changed.begin(); it != net_host_equip_changed.end(); ) {
		uint8_t id = *it;
		Avatar* peer = playerm->get(id);
		PlayerInventory* peer_inv = playerm->inventoryFor(id);
		ActionBarState* peer_ab = playerm->actionbarFor(id);

		if (peer && peer_inv && peer_ab) {
			peer_ab->updated = true;
			peer->loadAnimations();

			if (peer->feet_index != -1) {
				ItemID feet_id = peer_inv->inventory[PlayerInventory::EQUIPMENT][peer->feet_index].item;
				if (items->isValid(feet_id))
					peer->loadStepFX(items->items[feet_id]->stepfx);
			}
		}

		net_host_equip_changed.erase(it++);
	}
}

// P3.4d. Mirrors main_server.cpp's serverInventoryForCaster() -- resolved by StatBlock* pointer
// identity only, never dereferenced, since this function is what knows a StatBlock* maps to a
// PlayerID via playerm. Returns NULL for the local player's own caster (checkUsedItems() already
// handles that one) and for a caster that no longer matches any connected peer.
PlayerInventory* GameStatePlay::netHostInventoryForCaster(StatBlock* caster) {
	for (std::set<uint8_t>::const_iterator it = net_host_players.begin(); it != net_host_players.end(); ++it) {
		Avatar* peer = playerm->get(*it);
		if (peer && &peer->stats == caster)
			return playerm->inventoryFor(*it);
	}
	return NULL;
}

// P3.4d. Fixes the disclosed correctness bug named in P3.4c's own Status note: mirrors
// main_server.cpp's serverCheckUsedItems() almost unchanged. checkUsedItems() (local, above) now
// only consumes entries whose caster is player->stats -- see the caster guard added there -- so the
// two functions never double-consume the same entry. Both must run, in either order, before
// powers->clearUsedItems(), which is why that call moved to logic()'s own tail call site instead of
// staying inside checkUsedItems()'s own body.
void GameStatePlay::netHostCheckUsedItems() {
	for (unsigned i = 0; i < powers->used_items.size(); i++) {
		PlayerInventory* inventory = netHostInventoryForCaster(powers->used_items_caster[i]);
		if (inventory)
			inventory->remove(powers->used_items[i], 1);
	}
	for (unsigned i = 0; i < powers->used_equipped_items.size(); i++) {
		PlayerInventory* inventory = netHostInventoryForCaster(powers->used_equipped_items_caster[i]);
		if (inventory) {
			inventory->inventory[PlayerInventory::EQUIPMENT].remove(powers->used_equipped_items[i], 1);
			inventory->applyEquipment();
		}
	}
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
		// P3.4d: skip an entry caused by a connected peer's own item use -- see
		// netHostCheckUsedItems()'s own comment. PowerManager.cpp's payPowerCost() only ever tags a
		// caster when src_stats->hero is true, and before P3.4c the local player was the only
		// possible hero caster, so this guard is a provable no-op everywhere the replay corpus
		// reaches (see plans/phase3/P3.4d-host-peer-kind-c.md's Steps).
		if (powers->used_items_caster[i] != &player->stats)
			continue;
		// Deliberately still routed through MenuInventory's wrapper here, not PlayerInventory
		// directly -- it keeps the activated_item/activated_slot special case (P1.3d-4b-3), which
		// is what makes a right-click activation consume the exact carried slot the player clicked
		// rather than just any stack of the same item. Repointing this one needs that case
		// designed a sim-side equivalent first, not just deleted.
		menu->inv->remove(powers->used_items[i], 1);
	}
	for (unsigned i=0; i<powers->used_equipped_items.size(); i++) {
		if (powers->used_equipped_items_caster[i] != &player->stats)
			continue;
		player_inventory->inventory[PlayerInventory::EQUIPMENT].remove(powers->used_equipped_items[i], 1);
		player_inventory->applyEquipment();
	}
	// P3.4d: powers->clearUsedItems() moved to logic()'s own tail call site, after
	// netHostCheckUsedItems() also runs -- clearing here would wipe a connected peer's own entries
	// before that function ever saw them.
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

	netConnectIfNeeded();
	netSyncPlayers();
	netHostIfNeeded();
	netHostSyncPeers();

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

	// P3.4d: kind C, same unconditional position as the local call just above -- see
	// netHostCheckDeathPenalty()'s own comment.
	netHostCheckDeathPenalty();

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

		// P3.4b: the local avatar stays client-simulated (see this plan's Why) -- this just also
		// forwards the same finished command to the host, so OTHER connected clients can see this
		// player move. player_cmd is fully finished by this point in the tick (nothing above reads
		// or writes it again), so it is safe to serialize here unmodified.
		if (netmgr && netmgr->hasLocalPlayerID())
			netmgr->sendToHost(Net::encodePlayerCommand(player_cmd));

		// P3.4c: every connected peer's own Avatar::logic() call, plus the per-player state that
		// goes with it (level-up, RESPEC, transform/revert, respawn) -- see netHostDrivePeers()'s
		// own comment for exactly what is and isn't ported from main_server.cpp's serverLogic().
		// Gated the same way local's own block above is (inside !isPaused()) -- see isPaused()'s
		// own updated comment for why a connected peer is what makes that safe now.
		netHostDrivePeers();

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
	// P3.4d: kind C, same "whether paused or not" tail position as the two local calls just above.
	// netHostCheckUsedItems() must run before clearUsedItems() below, same as checkUsedItems() --
	// see clearUsedItems()'s own new call site comment.
	netHostCheckEquipmentChange();
	netHostCheckUsedItems();
	powers->clearUsedItems();
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

	// P3.4c: unconditional, like main_server.cpp's own call site -- a connected peer should keep
	// receiving periodic snapshots every tick, paused or not, rather than appear to hang.
	netHostBroadcastSnapshot();

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

	// P3.4b: loop over every currently-known player, not just the local one. In single-player
	// playerm->players has exactly one entry (nothing else calls playerm->create()), so this is
	// behaviour-identical there; it only renders more once a remote player has been provisioned by
	// netApplySnapshotEntry().
	for (size_t p = 0; p < playerm->players.size(); ++p)
		playerm->players[p]->addRenders(rens);

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
	// feel, which is why the client half was unchanged through P3.4b.
	//
	// P3.4c makes this concrete for the one multiplayer session this client can host: a
	// --host peer with at least one connected player refuses the pause outright, exactly as this
	// comment always said Phase 3 would. Deliberately narrower than "netmgr exists" -- a --connect
	// client pausing only stops sending its OWN commands (the other players' state comes from
	// snapshots, unaffected by this client's local menu), so that path is left alone; opening a menu
	// there is no different from just standing still. peerCount() alone isn't the right test either
	// -- a peer mid-handshake and not yet in net_host_players has no Avatar depending on this tick
	// advancing.
	return !settings->headless && menu->pause_requested && !(netmgr && netmgr->isHost() && !net_host_players.empty());
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
	// P3.4b: free any remote players' Avatar/PlayerInventory/ActionBarState/PowerBonusState before
	// playerm itself goes away -- PlayerManager::remove() is the only place those get delete'd
	// (PlayerManager.cpp), so leaving them in playerm->players here would leak them silently.
	while (playerm->players.size() > 1) {
		PlayerID id = (playerm->players[0]->id == playerm->local_id) ? playerm->players[1]->id : playerm->players[0]->id;
		playerm->remove(id);
	}
	if (netmgr) {
		netmgr->shutdown();
		delete netmgr;
		netmgr = NULL;
	}
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

