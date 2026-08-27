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
 * Through P2.3, SharedGameResources.h also declared four compatibility-alias globals (Avatar/
 * PlayerInventory/ActionBarState/PowerBonusState pointers, named pc/pinv/pab/pbs) that setLocal()
 * kept pointed at whichever player was local(), so every pre-P2.1 consumer of the single-player
 * globals kept compiling unchanged while P2.2 and P2.3 migrated them onto explicit bindings in
 * reviewable batches. P2.3b (plans/phase2/P2.3b-delete-player-globals.md) finished that migration
 * -- PlayerInventory/ActionBarState/PowerBonusState now carry their own owner/sibling back-pointers
 * (set right here in create(), below) instead of reaching for a global, and the four alias globals
 * are gone entirely: get()/inventoryFor()/actionbarFor()/powerbonusFor()/local() are the only way
 * to reach a player's state now, for any player including local().
 */

#ifndef PLAYER_MANAGER_H
#define PLAYER_MANAGER_H

#include "CommonIncludes.h"
#include <stdint.h>

class Avatar;
class ActionBarState;
class FPoint;
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

	/** Frees all four objects for this id and removes them from the parallel arrays. A remove()
	 * for an id not present is a no-op. */
	void remove(PlayerID id);

	Avatar*          get(PlayerID id);
	PlayerInventory* inventoryFor(PlayerID id);
	ActionBarState*  actionbarFor(PlayerID id);
	PowerBonusState* powerbonusFor(PlayerID id);

	/** The client's own player; NULL on a server with no local player, or before setLocal(). */
	Avatar* local();

	/** Records which player local() now names. Must be called again whenever local_id changes,
	 * not only at creation. */
	void setLocal(PlayerID id);

	size_t count() const;

	/** Nearest player (by any state) to pos, within max_range (0 = unlimited). NULL if no
	 * players exist or none are within range. Ties (exactly equal distance) break on the lower
	 * PlayerID, deterministically -- never on iteration/insertion order and never on float
	 * comparison alone, since two enemies picking different targets from identical state is a
	 * desync. players is kept sorted by id (see the field comment below), so a plain
	 * strictly-less-than compare while walking front-to-back already implements "lower id wins
	 * a tie" for free -- no separate tiebreak branch needed. */
	Avatar* nearestTo(const FPoint& pos, float max_range = 0.f);

	/** Same as nearestTo(), restricted to players with stats.alive true. NULL if no players
	 * exist, none are alive, or none alive are within range -- never dereferences a dead or
	 * absent player. */
	Avatar* nearestAliveTo(const FPoint& pos, float max_range = 0.f);

	/** True if at least one *alive* player is within range of pos. */
	bool anyAliveWithin(const FPoint& pos, float range);

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
