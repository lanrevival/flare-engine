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

/**
 * class PlayerManager
 *
 * Owns every player-scoped object -- Avatar, PlayerInventory, ActionBarState, PowerBonusState --
 * in four arrays kept in lockstep by PlayerID. create()/remove() allocate or free all four
 * together, mirroring the fact that both existing construction paths (GameStatePlay's
 * constructor, main_server.cpp's serverConstructSim()) already treated these four as one unit
 * before this class existed. See plans/phase2/P2.1-player-manager.md and
 * P2.1-DESIGN-playermanager-shape.md for why this is four parallel arrays here rather than four
 * fields folded onto Avatar itself.
 *
 * pc/pinv/pab/pbs (SharedGameResources.h) become compatibility aliases for whichever player is
 * local() -- setLocal() keeps them in sync. Every existing pc->/pinv->/pab->/pbs-> reference in
 * the tree keeps working unchanged; P2.2 and P2.3 migrate them off the aliases in reviewable
 * batches, and only then do the four global names get deleted.
 */

#ifndef PLAYER_MANAGER_H
#define PLAYER_MANAGER_H

#include "CommonIncludes.h"
#include <stdint.h>

class Avatar;
class ActionBarState;
class PlayerInventory;
class PowerBonusState;

// 8 players max (D3) -- keeps the eventual wire format small.
typedef uint8_t PlayerID;

class PlayerManager {
public:
	PlayerManager();
	~PlayerManager();

	/** Allocates an Avatar/PlayerInventory/ActionBarState/PowerBonusState for this id and
	 * inserts all four into the parallel arrays, keeping them sorted by id. A second create()
	 * for an id already present is a no-op (returns the existing id; nothing is reallocated). */
	PlayerID create(PlayerID id);

	/** Frees all four objects for this id and removes them from the parallel arrays. If id is
	 * the current local(), the pc/pinv/pab/pbs aliases are reset to NULL -- the same NULL-ing
	 * GameStatePlay's destructor used to do inline. A remove() for an id not present is a no-op. */
	void remove(PlayerID id);

	Avatar*          get(PlayerID id);
	PlayerInventory* inventoryFor(PlayerID id);
	ActionBarState*  actionbarFor(PlayerID id);
	PowerBonusState* powerbonusFor(PlayerID id);

	/** The client's own player; NULL on a server with no local player, or before setLocal(). */
	Avatar* local();

	/** Points pc/pinv/pab/pbs at whichever player local_id now names. Must be called again
	 * whenever local_id changes, not only at creation. */
	void setLocal(PlayerID id);

	size_t count() const;

	// Parallel, kept in lockstep by id: players[i]/inventories[i]/actionbars[i]/powerbonuses[i]
	// always describe the same player. Sorted by id -- iteration order must stay stable, since
	// the replay hash walks player state and any consumer that iterates these arrays directly
	// depends on it too.
	std::vector<Avatar*>          players;
	std::vector<PlayerInventory*> inventories;
	std::vector<ActionBarState*>  actionbars;
	std::vector<PowerBonusState*> powerbonuses;
	PlayerID local_id;

private:
	// Index into the four parallel arrays for id, or players.size() if id is not present.
	size_t indexOf(PlayerID id) const;
};

#endif // PLAYER_MANAGER_H
