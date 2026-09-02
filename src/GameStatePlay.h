/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
Copyright © 2012-2015 Justin Jacobs

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

#ifndef GAMESTATEPLAY_H
#define GAMESTATEPLAY_H

#include "CommonIncludes.h"
#include "GameState.h"
#include "Utils.h"

#include <stdint.h>

#include "net/ChildProcess.h" // held by value in net_host_child below

class ActionBarState;
class Avatar;
class Entity;
class MenuManager;
class PlayerInventory;
class PowerBonusState;
class QuestLog;
class StatBlock;
class WidgetLabel;

namespace Net {
	class NetworkManager;
	struct PlayerSnapshotEntry;
	struct MsgMapSync;
	struct MsgEntitySpawn;
	struct MsgEntitySnapshot;
}

class ActionData;

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

class GameStatePlay : public GameState {
private:
	Entity *enemy;

	QuestLog *quests;

	void checkEnemyFocus();
	void checkNPCFocus();
	void checkLoot();
	void checkLootDrop();
	void checkTeleport();
	void checkCancel();
	void checkLog();
	void checkBook();
	void checkEquipmentChange();
	void drainSimEvents();
	void checkTitle();
	void checkUsedItems();
	void checkNotifications();
	void checkNPCInteraction();
	void checkStash();
	void checkCutscene();
	void checkSaveEvent();
	void updateActionBar(unsigned index);
	void loadTitles();
	void resetNPC();
	bool checkPrimaryStat(const std::string& first, const std::string& second);

	// P3.4b: joining a --dedicated server as a network client. See plans/phase3/
	// P3.4b-client-join-and-remote-rendering.md for the design this mirrors from main_server.cpp.
	void netConnectIfNeeded();
	void netSyncPlayers();
	Avatar* netApplySnapshotEntry(const Net::PlayerSnapshotEntry& entry);

	// P3.8. The six-field write netApplySnapshotEntry() already did for a freshly-created remote,
	// factored out so netSyncPlayers() can apply the exact same fields onto this client's own
	// avatar (playerm id 0, already exists -- no create-if-needed branch needed) once it too stops
	// self-simulating. See netSyncPlayers()'s own updated comment.
	void netApplySnapshotFields(Avatar* av, const Net::PlayerSnapshotEntry& entry);

	// P3.5a. Applies the one MSG_MAP_SYNC this client will ever receive -- see
	// plans/phase3/P3.5a-join-map-sync.md.
	void netApplyMapSync(const Net::MsgMapSync& sync);

	// P3.9. Creates a mirrored Entity for any not-yet-seen net_id (idempotent -- skips one already
	// present, defensive against a re-announce). Never assigns a net_id itself: entitym's own
	// next_net_id counter is server/single-player-only, an entity id here always comes off the
	// wire. See plans/phase3/P3.9-entity-replication.md.
	void netApplyEntitySpawn(const Net::MsgEntitySpawn& spawn);

	// P3.9. Full per-tick dump, same "absence is despawn" contract as netSyncPlayers()'s own player
	// handling -- writes every named entity's mutable fields, then deletes any non-NPC entity in
	// entitym->entities NOT named this tick.
	void netApplyEntitySnapshot(const Net::MsgEntitySnapshot& snapshot);

	// This client's own local avatar always occupies playerm id 0 (playerm->create(0) in the
	// constructor, unrelated to networking). A dedicated server ALWAYS has its own player 0 too
	// (whatever --load-slot loaded server-side, per main_server.cpp) and broadcasts it in every
	// snapshot like any other player -- so network PlayerID 0 is not this client's own id, and
	// reusing network ids directly as playerm ids would let the host's own id-0 entry silently
	// overwrite this client's own local avatar the moment a snapshot arrived. Remote players are
	// therefore given playerm ids offset by this base instead -- D3 caps real player counts at 8,
	// so network ids are always 0..7 and this leaves no possible overlap with playerm id 0.
	// uint8_t here, not PlayerID, so this header doesn't need to #include "PlayerManager.h" -- the
	// two are the same type (PlayerManager.h:53), matching this file's existing forward-declare
	// convention for everything else it only holds pointers/values of.
	static const uint8_t REMOTE_PLAYER_ID_BASE = 8;
	uint8_t remotePlayerId(uint8_t network_id) const { return static_cast<uint8_t>(REMOTE_PLAYER_ID_BASE + network_id); }

	Net::NetworkManager* netmgr;
	bool net_connect_attempted;

	// P3.8b (D28). --host no longer embeds a second simulation in this client -- it spawns
	// flare-server (--dedicated --no-local-player) as a child and points netConnectIfNeeded() at
	// it on loopback, so a host session and a guest session run the exact same client code path.
	// Replaces the ~630-line netHost* family P3.4c/P3.4d hand-ported from main_server.cpp (peer
	// provisioning, per-peer Avatar::logic() driving, loot/title/death-penalty/equipment/used-item
	// duplication, snapshot broadcast) -- see
	// plans/phase3/P3.8b-host-becomes-a-child-process.md.
	void netHostSpawnAndConnect();
	bool net_host_attempted;
	Net::ChildProcess net_host_child;

	int npc_id;

	std::vector<Title> titles;

	Timer second_timer;

	bool is_first_map_load;

	static const unsigned UPDATE_ACTIONBAR_ALL = 0;

public:
	GameStatePlay();
	~GameStatePlay();
	void refreshWidgets();

	bool isPaused();
	void logic();
	void render();
	void resetGame();

	/** Resolved once from playerm right after playerm->setLocal(0), in the constructor -- not a
	 * global lookup at every use site. Every reference in this file is kind A (the player whose
	 * screen this is): GameStatePlay is one client's own game loop, so the RESPEC/close_menus/
	 * show_game_over flags it consumes are always this same player's own, even though they were
	 * set by simulation code that only knows about "the triggering player". See P2.3. */
	Avatar* player;
	PlayerInventory* player_inventory;
	ActionBarState* player_actionbar;
	PowerBonusState* player_powerbonus;
};

#endif

