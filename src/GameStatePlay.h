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
#include <map>
#include <set>

#include "PlayerCommand.h" // held by value in net_host_cmd below

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

	// P3.4c: embedding a NetworkManager HOST directly in this client, so a second player can
	// connect straight to this machine without a separate --dedicated process. Mutually exclusive
	// with netConnectIfNeeded() above -- main.cpp refuses --connect and --host together, and
	// netmgr is shared by both (a GameStatePlay is a client or a host, never both). Unlike P3.4b's
	// remotePlayerId() offset, connected peers here keep their raw network PlayerID as their
	// playerm id directly -- this client's own local avatar is always id 0 (playerm->create(0) in
	// the constructor) and netmgr->seedNextPlayerID(1) in netHostIfNeeded() guarantees no accepted
	// peer can ever be assigned id 0, so there is no collision to offset around. Mirrors
	// main_server.cpp's serverSyncNetworkPlayers()/serverLogic()/serverBroadcastSnapshot() --
	// see plans/phase3/P3.4c-host-embedded-mode.md for exactly what is and isn't ported.
	void netHostIfNeeded();
	void netHostSyncPeers();
	void netHostDrivePeers();
	void netHostBroadcastSnapshot();
	Avatar* netHostProvisionPeer(uint8_t id, const FPoint& spawn_pos);

	bool net_host_attempted;
	std::set<uint8_t> net_host_players;           // ids currently bound to a connected, handshake-complete peer
	std::map<uint8_t, PlayerCommand> net_host_cmd; // this tick's decoded command per connected peer

	// P3.4d: kind-C generalisation for connected peers -- loot auto-pickup, title-earning, death
	// penalty, equipment-change notification, used-item consumption. Deliberately separate
	// functions from checkLoot()/checkTitle()/checkPrimaryStat()/checkEquipmentChange()/
	// checkUsedItems() above, parameterised by peer instead of sharing those -- mirrors
	// main_server.cpp's own precedent of never sharing these with GameStatePlay.cpp either
	// (serverCheckLoot()/serverCheckTitle()/serverCheckPrimaryStat()/serverCheckEquipmentChange()/
	// serverCheckUsedItems() are separate functions there too), so this plan's diff never touches a
	// line any prior plan's replay-corpus verification already depends on. See
	// plans/phase3/P3.4d-host-peer-kind-c.md.
	void netHostCheckDeathPenalty();
	void netHostCheckLoot(Avatar* peer, PlayerInventory* inv);
	bool netHostCheckPrimaryStat(const StatBlock& stats, const std::string& first, const std::string& second);
	void netHostCheckTitle(Avatar* peer);
	void netHostCheckEquipmentChange();
	void netHostCheckUsedItems();
	PlayerInventory* netHostInventoryForCaster(StatBlock* caster);

	// Peer-scoped substitute for menu->inv->changed_equipment (peers have no MenuInventory on this
	// screen) -- presence of an id means "netHostCheckEquipmentChange() has a reload pending for
	// it". Mirrors main_server.cpp's server_equipment_changed[8] array, as a set instead, matching
	// this file's existing net_host_players/net_host_cmd style. See netHostCheckEquipmentChange()'s
	// own comment for its two trigger sites.
	std::set<uint8_t> net_host_equip_changed;

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

