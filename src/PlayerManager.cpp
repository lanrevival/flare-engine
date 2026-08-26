/*
Copyright © 2026 Flare LAN Co-op contributors

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

#include "PlayerManager.h"

#include "ActionBarState.h"
#include "Avatar.h"
#include "PlayerInventory.h"
#include "PowerBonusState.h"
#include "SharedGameResources.h"

PlayerManager::PlayerManager()
	: players()
	, inventories()
	, actionbars()
	, powerbonuses()
	, local_id(0)
{
}

PlayerManager::~PlayerManager() {
	// Not relied on today -- both GameStatePlay and main_server.cpp call remove() explicitly for
	// every id they created, in their own destructor/serverCleanup(), before playerm itself is
	// ever destroyed. This is a safety net for a PlayerManager torn down with players still in
	// it, not the normal shutdown path.
	while (!players.empty())
		remove(players.back()->id);
}

size_t PlayerManager::indexOf(PlayerID id) const {
	for (size_t i = 0; i < players.size(); ++i) {
		if (players[i]->id == id)
			return i;
	}
	return players.size();
}

PlayerID PlayerManager::create(PlayerID id) {
	size_t existing = indexOf(id);
	if (existing != players.size())
		return id;

	// Keep every array sorted by id, in lockstep -- find the insertion point once and use it for
	// all four. players[i]->id is the only place an id is actually stored; the other three arrays
	// have no id field of their own and rely entirely on staying aligned with players by index.
	size_t insert_at = players.size();
	for (size_t i = 0; i < players.size(); ++i) {
		if (players[i]->id > id) {
			insert_at = i;
			break;
		}
	}

	Avatar* avatar = new Avatar();
	avatar->id = id;
	PlayerInventory* inventory = new PlayerInventory();
	ActionBarState* actionbar = new ActionBarState();
	PowerBonusState* powerbonus = new PowerBonusState();

	players.insert(players.begin() + static_cast<std::vector<Avatar*>::difference_type>(insert_at), avatar);
	inventories.insert(inventories.begin() + static_cast<std::vector<PlayerInventory*>::difference_type>(insert_at), inventory);
	actionbars.insert(actionbars.begin() + static_cast<std::vector<ActionBarState*>::difference_type>(insert_at), actionbar);
	powerbonuses.insert(powerbonuses.begin() + static_cast<std::vector<PowerBonusState*>::difference_type>(insert_at), powerbonus);

	return id;
}

void PlayerManager::remove(PlayerID id) {
	size_t i = indexOf(id);
	if (i == players.size())
		return;

	delete players[i];
	delete inventories[i];
	delete actionbars[i];
	delete powerbonuses[i];

	players.erase(players.begin() + static_cast<std::vector<Avatar*>::difference_type>(i));
	inventories.erase(inventories.begin() + static_cast<std::vector<PlayerInventory*>::difference_type>(i));
	actionbars.erase(actionbars.begin() + static_cast<std::vector<ActionBarState*>::difference_type>(i));
	powerbonuses.erase(powerbonuses.begin() + static_cast<std::vector<PowerBonusState*>::difference_type>(i));

	// Same NULL-ing GameStatePlay::~GameStatePlay() used to do inline, right after its own
	// delete pc/pinv/pab/pbs block.
	if (id == local_id) {
		pc = NULL;
		pinv = NULL;
		pab = NULL;
		pbs = NULL;
	}
}

Avatar* PlayerManager::get(PlayerID id) {
	size_t i = indexOf(id);
	return (i == players.size()) ? NULL : players[i];
}

PlayerInventory* PlayerManager::inventoryFor(PlayerID id) {
	size_t i = indexOf(id);
	return (i == players.size()) ? NULL : inventories[i];
}

ActionBarState* PlayerManager::actionbarFor(PlayerID id) {
	size_t i = indexOf(id);
	return (i == players.size()) ? NULL : actionbars[i];
}

PowerBonusState* PlayerManager::powerbonusFor(PlayerID id) {
	size_t i = indexOf(id);
	return (i == players.size()) ? NULL : powerbonuses[i];
}

Avatar* PlayerManager::local() {
	return get(local_id);
}

void PlayerManager::setLocal(PlayerID id) {
	local_id = id;
	pc = get(local_id);
	pinv = inventoryFor(local_id);
	pab = actionbarFor(local_id);
	pbs = powerbonusFor(local_id);
}

size_t PlayerManager::count() const {
	return players.size();
}
