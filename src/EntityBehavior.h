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

	// logic steps
	void doUpkeep();
	void findTarget();
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

public:
	explicit EntityBehavior(Entity *_e);
	~EntityBehavior();
	void logic();

	std::vector<FPoint>& getPath() { return path; }
	FPoint& getPursuePos() { return pursue_pos; };
};

#endif
