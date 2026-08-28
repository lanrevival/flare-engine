/*
Copyright © 2012 Clint Bellanger
Copyright © 2012 Stefan Beller
Copyright © 2013 Ryan Dansie
Copyright © 2012-2021 Justin Jacobs

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
 * class EntityBehavior
 *
 * Interface for entity behaviors.
 * The behavior object is a component of Entity.
 * Make AI decisions (movement, actions) for entities.
 */


#ifndef ENTITY_BEHAVIOR_H
#define ENTITY_BEHAVIOR_H

#include "StatBlock.h"
#include <queue>

class Avatar;
class Entity;

class EntityBehavior {
private:
	static const float ALLY_FLEE_DISTANCE;
	static const float ALLY_FOLLOW_DISTANCE_WALK;
	static const float ALLY_FOLLOW_DISTANCE_STOP;
	static const float ALLY_TELEPORT_DISTANCE;

	// P2.4 step 1 -- chooseAggroTarget()'s scoring weights. All in "distance-equivalent tiles" so
	// they can be added straight onto -distance. See EntityBehavior.cpp for the reasoning behind
	// each: LOS is a near-dominant preference (AC5b), recency/threat matter within a visibility
	// tier, and hysteresis (deliberately smaller than the LOS gap, so a target going out of sight
	// doesn't get "stuck") keeps an entity from flipping between two near-equidistant players.
	static const float AGGRO_LOS_BONUS;
	static const float AGGRO_RECENCY_WEIGHT;
	static const float AGGRO_THREAT_POINTS_WEIGHT;
	static const float AGGRO_HYSTERESIS_BONUS;

	// logic steps
	void doUpkeep();
	void findTarget();
	// P2.4 step 1. Scores every alive player as a candidate aggro target for a HOSTILE entity
	// (distance + per-player threat/recency + hysteresis toward whoever is already the target),
	// evaluating LOS per candidate before scoring rather than after picking one. Returns NULL if
	// no player is alive. With exactly one player this always resolves to that player (or NULL),
	// identical to the old nearest_alive_player-only read (AC1). See EntityBehavior.cpp for the
	// scoring weights and the executor notes in plans/phase2/P2.4-n-hero-ai.md on why the
	// hysteresis term exists at all (oscillating aggro between two near-equidistant players).
	Avatar* chooseAggroTarget();
	void checkPower();
	void checkMove();
	void checkMoveStateStance();
	void checkMoveStateMove();
	void checkOnStatePower(StatBlock::AIPower** on_state_power);
	void updateState();
	FPoint getWanderPoint();

protected:
	Entity *e;

	static const int PATH_FOUND_FAIL_THRESHOLD = 1;
	static const int PATH_FOUND_FAIL_WAIT_SECONDS = 2;

	//variables for patfinding
	std::vector<FPoint> path;
	FPoint prev_target;
	bool collided;
	bool path_found;
	int chance_calc_path;
	int path_found_fails;
	Timer path_found_fail_timer;
	bool warp_to_hero;

	float target_dist;
	float hero_dist;
	FPoint pursue_pos;
	// targeting vars
	bool los;
	//when fleeing, the entity moves away from the pursue_pos
	bool fleeing;
	bool move_to_safe_dist;
	Timer turn_timer;

	bool instant_power;
	PowerID replaced_power_id;

	// Resolved once per logic() tick, before doUpkeep()/findTarget() run, and read by every
	// subsequent step this tick (findTarget/checkPower/checkMove*) -- this is what the old
	// single-player `pc` global (the Avatar pointer in SharedGameResources.h) fanned out to
	// everywhere in this file. Two variants because the pre-P2.2 code used that pointer two
	// different ways that must stay distinguishable:
	//   nearest_player       -- closest player regardless of alive status. NULL only if
	//                            PlayerManager has zero players, which does not happen while the
	//                            sim is running. Stands in for the old unconditional position
	//                            reads used for ally warp/follow/facing bookkeeping, which never
	//                            checked aliveness either.
	//   nearest_alive_player  -- closest ALIVE player. NULL whenever no player is alive,
	//                            replacing the old alive-gated reads used for target selection
	//                            and combat-exit conditions.
	// With exactly one player both resolve to that player (or both NULL if it's dead), so
	// AC-REPLAY holds; with several, targeting/ally logic becomes per-entity-correct.
	Avatar* nearest_player;
	Avatar* nearest_alive_player;
	// For hero allies, this is the connected player whose stats are their summoner. It falls back
	// to nearest_player for legacy allies without an owner, preserving the single-player behavior.
	Avatar* follow_player;

	// P2.4 step 1: sticky aggro target across ticks, read/written only by chooseAggroTarget() --
	// the hysteresis bonus goes to whichever candidate matches this id, so a target needs to
	// score meaningfully higher elsewhere before this entity switches. -1 means "no current
	// target" (never chosen one yet, or the previous one is no longer a valid candidate -- e.g.
	// dead -- and simply stopped being iterated, no explicit clearing needed).
	int aggro_target_id;
	// Whether THAT target had line of sight at the moment it was chosen -- exposed for
	// main_server.cpp's --dump-ai-targets (AC3/AC4/AC5b), which needs to report the AI's actual
	// scored choice, not independently recompute nearestAliveTo() the way it did before scored
	// aggro existed (that would silently stop matching what the entity really targets).
	bool aggro_target_los;

public:
	explicit EntityBehavior(Entity *_e);
	~EntityBehavior();
	void logic();

	std::vector<FPoint>& getPath() { return path; }
	FPoint& getPursuePos() { return pursue_pos; };

	// P2.4 step 1 -- see the member comments above. -1 = this entity currently has no player
	// target (hero_ally, or no player is alive).
	int getAggroTargetId() const { return aggro_target_id; }
	bool getAggroTargetLos() const { return aggro_target_los; }
};

#endif

