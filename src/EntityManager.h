/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Stefan Beller
Copyright © 2013 Henrik Andersson
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

/*
 * class EntityManager
 */


#ifndef ENTITY_MANAGER_H
#define ENTITY_MANAGER_H

#include "CommonIncludes.h"
#include "PlayerManager.h" // PlayerID
#include "Utils.h"

class Animation;
class Entity;

class EntityManager {
protected:
	/**
	 * callee is responsible for deleting returned entity object
	 */
	size_t loadEntityPrototype(const std::string& type_id);

	std::vector<Entity> prototypes;

public:
	EntityManager();
	~EntityManager();

	Entity *getEntityPrototype(const std::string& type_id);

	/** True if a prototype for this entity type is already loaded (prototypes is otherwise
	 * protected -- see the class comment on why the preload loop this backs matters for P2.2).
	 * Used by main_server.cpp's --dump-summon-prototypes diagnostic (AC7). */
	bool hasLoadedPrototype(const std::string& type_id) const;

	void handleNewMap();

	// P3.5a. Loads prototypes for one player's own summon powers (powers_list + action-bar hotkeys)
	// without handleNewMap()'s map-transition side effects (entity wipe/respawn, wmap->enemies/
	// ally-queue draining) -- those are not safe to re-run mid-session with live enemies on the map.
	// Called once, right after provisioning a newly-connected peer server/host-side: that peer's own
	// powers were never walked by any handleNewMap() call, since the server/host's own map was
	// already loaded before this peer connected. See plans/phase3/P3.5a-join-map-sync.md.
	void preloadSummonPrototypesForPlayer(PlayerID id);

	void handleSpawn();
	bool checkPartyMembers();
	void logic();
	void addRenders(std::vector<Renderable> &r, std::vector<Renderable> &r_dead);
	void checkEnemiesforXP();
	bool isCleared();
	void spawn(const std::string& entity_type, const Point& target, EventComponent* ec_spawn_level);
	Entity *entityFocus(const Point& mouse, const FPoint& cam, bool alive_only);
	Entity* getNearestEntity(const FPoint& pos, bool get_corpse, float *saved_distance, float max_range);

	// vars
	std::vector<Entity*> entities;

	bool player_blocked;
	Timer player_blocked_timer;

	static const bool GET_CORPSE = true;
	static const bool IS_ALIVE = true;
};


#endif
