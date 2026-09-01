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

#include "Animation.h"
#include "AnimationManager.h"
#include "AnimationSet.h"
#include "ActionBarState.h"
#include "Avatar.h"
#include "CampaignManager.h"
#include "EnemyGroupManager.h"
#include "Entity.h"
#include "EntityBehavior.h"
#include "EntityManager.h"
#include "EngineSettings.h"
#include "EntityBehavior.h"
#include "EventManager.h"
#include "FogOfWar.h"
#include "Hazard.h"
#include "InputState.h"
#include "MapRenderer.h"
#include "MenuActionBar.h"
#include "PlayerManager.h"
#include "PowerManager.h"
#include "RenderDevice.h"
#include "Rng.h"
#include "Settings.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "Utils.h"

#include <limits>

static Avatar* playerForSummoner(StatBlock* summoner) {
	if (!summoner || !playerm)
		return NULL;

	for (size_t i = 0; i < playerm->players.size(); ++i) {
		if (&playerm->players[i]->stats == summoner)
			return playerm->players[i];
	}

	return NULL;
}

EntityManager::EntityManager()
	: entities()
	, player_blocked(false)
	, player_blocked_timer(Settings::SIM_TICK_HZ / 6) {
	handleNewMap();
}

Entity *EntityManager::getEntityPrototype(const std::string& type_id) {
	Entity* e = new Entity(prototypes.at(loadEntityPrototype(type_id)));
	return e;
}

bool EntityManager::hasLoadedPrototype(const std::string& type_id) const {
	for (size_t i = 0; i < prototypes.size(); ++i) {
		if (prototypes[i].type_filename == type_id)
			return true;
	}
	return false;
}

size_t EntityManager::loadEntityPrototype(const std::string& type_id) {
	for (size_t i = 0; i < prototypes.size(); i++) {
		if (prototypes[i].type_filename == type_id) {
			return i;
		}
	}

	Entity e = Entity();

	e.stats.load(type_id);
	e.type_filename = type_id;

	if (e.stats.animations == "")
		Utils::logError("EntityManager: No animation file specified for entity: %s", type_id.c_str());

	e.loadAnimations();
	e.loadSounds();

	// set cooldown_hit to duration of hit animation if undefined
	if (!e.stats.cooldown_hit_enabled && e.animationSet) {
		Animation *hit_anim = e.animationSet->getAnimation("hit");
		if (hit_anim) {
			e.stats.cooldown_hit.setDuration(hit_anim->getDuration());
			delete hit_anim;
		}
		else {
			e.stats.cooldown_hit.setDuration(0);
		}
	}

	prototypes.push_back(e);
	size_t prototype = prototypes.size() - 1;

	for (size_t i = 0; i < e.stats.powers_ai.size(); i++) {
		PowerID power_index = e.stats.powers_ai[i].id;
		if (powers->isValid(power_index)) {
			const std::string& spawn_type = powers->powers[power_index]->spawn_type;
			if (!spawn_type.empty() && spawn_type != "untransform") {
				std::vector<Enemy_Level> spawn_enemies = enemyg->getEnemiesInCategory(spawn_type);
				for (size_t j = 0; j < spawn_enemies.size(); j++) {
					loadEntityPrototype(spawn_enemies[j].type);
				}
			}
		}
	}

	return prototype;
}

/**
 * When loading a new map, we eliminate existing entities and load the new ones.
 * The map will have loaded Entity blocks into an array; retrieve the entities and init them
 */
void EntityManager::handleNewMap () {

	Map_Enemy me;
	std::queue<Entity *> allies;

	// delete existing entities
	for (unsigned int i=0; i < entities.size(); i++) {
		if (entities[i]->stats.npc)
			continue;

		if (entities[i]->stats.hero_ally && entities[i]->stats.alive && entities[i]->stats.speed > 0)
			allies.push(entities[i]);
		else {
			entities[i]->unloadSounds();
			delete entities[i];
		}
	}
	entities.clear();


	for (unsigned int i=0; i < prototypes.size(); i++) {
		prototypes[i].unloadSounds();
	}
	prototypes.clear();

	// No specific triggering player exists for a map-defined enemy spawn, so its level remains
	// anchored to the local player. Persistent allies do carry their summoner pointer, and use it
	// below when their map-transition position is rebuilt.
	Avatar* local = playerm->local();

	// load new entities
	while (!wmap->enemies.empty()) {
		me = wmap->enemies.front();
		wmap->enemies.pop();

		if (me.type.empty()) {
			Utils::logError("EntityManager: Entity(%f, %f) doesn't have type attribute set, skipping", me.pos.x, me.pos.y);
			continue;
		}

		if (!camp->checkRequirementsInVector(me.requirements))
			continue;

		Entity *e = getEntityPrototype(me.type);

		e->stats.waypoints = me.waypoints;
		e->stats.pos.x = me.pos.x;
		e->stats.pos.y = me.pos.y;
		e->stats.direction = static_cast<unsigned char>(me.direction);
		e->stats.wander = me.wander_radius > 0;
		e->stats.setWanderArea(me.wander_radius);
		e->stats.invincible_requirements = me.invincible_requirements;

		// Set level. If spawn_level's ratio_source is RATIO_SOURCE_HERO, applyToStatBlock
		// (Map.cpp, P2.2 step 7b/D15) ignores this argument entirely and substitutes the party
		// average instead -- this argument only matters for other ratio_source/mode
		// combinations.
		if (local) {
			me.spawn_level.applyToStatBlock(&e->stats, &local->stats);
		}

		// apply Effects and set HP to max HP
		e->stats.recalc();

		entities.push_back(e);

		wmap->collider.block(me.pos.x, me.pos.y, !MapCollision::IS_ALLY);
	}

	// TODO support spawning flying enemies over pits?
	// Keep the legacy draw even when there are no persistent allies. The single-player replay path
	// depends on this exact RNG position; multi-player owners may request an additional neighbor
	// draw below when an ally belongs to a non-local player.
	FPoint legacy_spawn_pos = local ? wmap->collider.getRandomNeighbor(Point(local->stats.pos), 1, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_ALL_ENTITIES) : FPoint();
	while (!allies.empty()) {

		Entity *e = allies.front();
		allies.pop();

		//dont need the result of this. its only called to handle animation and sound
		Entity* temp = getEntityPrototype(e->type_filename);
		delete temp;

		Avatar* owner = playerForSummoner(e->stats.summoner);
		if (!owner)
			owner = local;
		FPoint spawn_pos = legacy_spawn_pos;
		if (playerm && playerm->players.size() > 1 && owner && owner != local)
			spawn_pos = wmap->collider.getRandomNeighbor(Point(owner->stats.pos), 1, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_ALL_ENTITIES);
		e->stats.pos = spawn_pos;
		if (owner) e->stats.direction = owner->stats.direction;

		entities.push_back(e);

		wmap->collider.block(e->stats.pos.x, e->stats.pos.y, MapCollision::IS_ALLY);
	}

	// Load entities that can be spawned by any connected player's powers or action bar.
	// P2.2 step 7b: the original draft only named the powers_list half of this loop; the
	// action-bar hotkeys half right below it has the exact same bug -- a player whose summon is bound
	// only to a hotkey (not also in their known-powers list) had no loaded prototype. Both now
	// iterate every player in playerm, not just local().
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		std::vector<PowerID>& powers_list = playerm->players[p]->stats.powers_list;
		for (size_t i = 0; i < powers_list.size(); i++) {
			PowerID power_index = powers_list[i];
			if (powers->isValid(power_index)) {
				const std::string& spawn_type = powers->powers[power_index]->spawn_type;
				if (!spawn_type.empty() && spawn_type != "untransform") {
					std::vector<Enemy_Level> spawn_enemies = enemyg->getEnemiesInCategory(spawn_type);
					for (size_t j = 0; j < spawn_enemies.size(); j++) {
						loadEntityPrototype(spawn_enemies[j].type);
					}
				}
			}
		}
	}

	for (size_t p = 0; p < playerm->actionbars.size(); ++p) {
		ActionBarState* actionbar = playerm->actionbars[p];
		if (!actionbar) continue;
		for (size_t i = 0; i < actionbar->hotkeys.size(); i++) {
			PowerID power_index = actionbar->hotkeys[i];
			if (power_index != 0) {
				const std::string& spawn_type = powers->powers[power_index]->spawn_type;
				if (!spawn_type.empty() && spawn_type != "untransform") {
					std::vector<Enemy_Level> spawn_enemies = enemyg->getEnemiesInCategory(spawn_type);
					for (size_t j = 0; j < spawn_enemies.size(); j++) {
						loadEntityPrototype(spawn_enemies[j].type);
					}
				}
			}
		}
	}

	// load entities that can be spawn by map events
	for (size_t i = 0; i < wmap->events.size(); i++) {
		for (size_t j = 0; j < wmap->events[i].components.size(); j++) {
			if (wmap->events[i].components[j].type == EventComponent::SPAWN) {
				std::vector<Enemy_Level> spawn_enemies = enemyg->getEnemiesInCategory(wmap->events[i].components[j].s);
				for (size_t k = 0; k < spawn_enemies.size(); k++) {
					loadEntityPrototype(spawn_enemies[k].type);
				}
			}
		}
	}

	anim->cleanUp();
}

// P3.5a. See this file's own header comment (EntityManager.h) for why this is not just another call
// to handleNewMap(): that function unconditionally deletes every non-NPC, non-living-ally entity and
// only repopulates from wmap->enemies -- a queue already drained by the map's own first load -- so a
// second call mid-session would delete every live enemy and spawn nothing back. This does only the
// prototype-loading loops handleNewMap() also has (EntityManager.cpp's own powers_list/action-bar
// loops above), scoped to one player instead of every player -- every other currently-provisioned
// player was already covered by the last real handleNewMap() call.
void EntityManager::preloadSummonPrototypesForPlayer(PlayerID id) {
	Avatar* av = playerm->get(id);
	if (av) {
		std::vector<PowerID>& powers_list = av->stats.powers_list;
		for (size_t i = 0; i < powers_list.size(); i++) {
			PowerID power_index = powers_list[i];
			if (powers->isValid(power_index)) {
				const std::string& spawn_type = powers->powers[power_index]->spawn_type;
				if (!spawn_type.empty() && spawn_type != "untransform") {
					std::vector<Enemy_Level> spawn_enemies = enemyg->getEnemiesInCategory(spawn_type);
					for (size_t j = 0; j < spawn_enemies.size(); j++)
						loadEntityPrototype(spawn_enemies[j].type);
				}
			}
		}
	}

	ActionBarState* actionbar = playerm->actionbarFor(id);
	if (actionbar) {
		for (size_t i = 0; i < actionbar->hotkeys.size(); i++) {
			PowerID power_index = actionbar->hotkeys[i];
			if (power_index != 0) {
				const std::string& spawn_type = powers->powers[power_index]->spawn_type;
				if (!spawn_type.empty() && spawn_type != "untransform") {
					std::vector<Enemy_Level> spawn_enemies = enemyg->getEnemiesInCategory(spawn_type);
					for (size_t j = 0; j < spawn_enemies.size(); j++)
						loadEntityPrototype(spawn_enemies[j].type);
				}
			}
		}
	}
}

/**
 * Powers can cause new entities to spawn
 * Check PowerManager for any new queued entities
 */
void EntityManager::handleSpawn() {

	Map_Enemy espawn;

	while (!powers->map_enemies.empty()) {
		espawn = powers->map_enemies.front();
		powers->map_enemies.pop();

		wmap->collider.unblock(espawn.pos.x, espawn.pos.y);

		Entity *e = new Entity();

		e->stats.hero_ally = espawn.hero_ally;
		e->stats.enemy_ally = espawn.enemy_ally;
		e->stats.summoned = true;
		e->stats.summoned_power_index = espawn.summon_power_index;

		if(espawn.summoner != NULL) {
			e->stats.summoner = espawn.summoner;
			espawn.summoner->summons.push_back(&(e->stats));
		}

		e->stats.direction = static_cast<unsigned char>(espawn.direction);

		Enemy_Level el = enemyg->getRandomEnemy(espawn.type, 0, 0);
		e->type_filename = el.type;

		if (el.type != "") {
			e->stats.load(el.type);
		}
		else {
			Utils::logError("EntityManager: Could not spawn creature type '%s'", espawn.type.c_str());
			delete e;
			return;
		}

		if (e->stats.animations == "") {
			Utils::logError("EntityManager: No animation file specified for entity: %s", espawn.type.c_str());
		}
		e->loadAnimations();
		e->loadSounds();

		//Set level
		if (powers->isValid(e->stats.summoned_power_index)) {
			SpawnLevel* spawn_level = &(powers->powers[e->stats.summoned_power_index]->spawn_level);
			spawn_level->applyToStatBlock(&e->stats, e->stats.summoner);

			// apply Effects and set HP to max HP
			e->stats.recalc();
		}
		else if (espawn.spawn_level.mode != SpawnLevel::MODE_DEFAULT) {
			espawn.spawn_level.applyToStatBlock(&e->stats, NULL);
			e->stats.recalc();
		}

		if (wmap->collider.isValidPosition(espawn.pos.x, espawn.pos.y, e->stats.movement_type, MapCollision::COLLIDE_TYPE_ALL_ENTITIES) || !e->stats.hero_ally) {
			e->stats.pos.x = espawn.pos.x;
			e->stats.pos.y = espawn.pos.y;
		}
		else {
			// Kind A: espawn.summoner (set just above, from the power activation that queued this
			// summon) is precisely "the player who triggered this" -- more precise than any
			// nearest/local fallback, and it's already right here. Only fall back to
			// playerm->local() for the (today never exercised) case of a summon with no summoner.
			StatBlock* anchor = espawn.summoner;
			if (!anchor) {
				Avatar* local = playerm->local();
				if (local) anchor = &local->stats;
			}
			if (anchor) e->stats.pos = wmap->collider.getRandomNeighbor(Point(anchor->pos), 1, e->stats.movement_type, MapCollision::COLLIDE_TYPE_ALL_ENTITIES);
		}

		// special animation state for spawning entities
		e->stats.cur_state = StatBlock::ENTITY_SPAWN;

		//now apply post effects to the spawned entity
		powers->effect(&e->stats, (espawn.summoner != NULL ? espawn.summoner : &e->stats), e->stats.summoned_power_index, e->stats.hero_ally ? Power::SOURCE_TYPE_HERO : Power::SOURCE_TYPE_ENEMY);

		//apply party passives
		//synchronise the party passives in every connected player's stat block with the passives
		//in the allies stat blocks (P2.2: kind C -- buff_party is explicitly party-wide, so this
		//now checks every player in playerm, not just local(); de-duplicated since two players
		//could carry the identical party-buff passive, which must apply once, not stack).
		//at the time the summon is spawned, it takes the passives available at that time. if the passives change later, the changes wont affect summons retrospectively. could be exploited with equipment switching
		for (size_t p = 0; p < playerm->players.size(); ++p) {
			StatBlock& pstats = playerm->players[p]->stats;

			for (unsigned i=0; i< pstats.powers_passive.size(); i++) {
				PowerID pwr = pstats.powers_passive[i];
				if (powers->isValid(pwr) && powers->powers[pwr]->passive && powers->powers[pwr]->buff_party && (e->stats.hero_ally || e->stats.enemy_ally)
						&& (powers->powers[pwr]->buff_party_power_id == 0 || powers->powers[pwr]->buff_party_power_id == e->stats.summoned_power_index)
						&& std::find(e->stats.powers_passive.begin(), e->stats.powers_passive.end(), pwr) == e->stats.powers_passive.end()) {

					e->stats.powers_passive.push_back(pwr);
				}
			}

			for (unsigned i=0; i<pstats.powers_list_items.size(); i++) {
				PowerID pwr = pstats.powers_list_items[i];
				if (powers->isValid(pwr) && powers->powers[pwr]->passive && powers->powers[pwr]->buff_party && (e->stats.hero_ally || e->stats.enemy_ally)
						&& (powers->powers[pwr]->buff_party_power_id == 0 || powers->powers[pwr]->buff_party_power_id == e->stats.summoned_power_index)
						&& std::find(e->stats.powers_passive.begin(), e->stats.powers_passive.end(), pwr) == e->stats.powers_passive.end()) {

					e->stats.powers_passive.push_back(pwr);
				}
			}
		}

		entities.push_back(e);

		wmap->collider.block(e->stats.pos.x, e->stats.pos.y, e->stats.hero_ally);
	}
}

bool EntityManager::checkPartyMembers() {
	// Every connected player is a party member, even when the party has not summoned an ally yet.
	// This is what lets a party-targeted power reach another player through StatBlock::party_buffs.
	if (playerm && playerm->players.size() > 1)
		return true;

	for (unsigned int i=0; i < entities.size(); i++) {
		if(entities[i]->stats.hero_ally && entities[i]->stats.hp > 0) {
			return true;
		}
	}
	return false;
}

/**
 * perform logic() for all entities
 */
void EntityManager::logic() {

	if (player_blocked) {
		player_blocked_timer.tick();
		if (player_blocked_timer.isEnd())
			player_blocked = false;
	}

	handleSpawn();

	bool any_hostile_in_combat = false;

	std::vector<Entity*>::iterator it;
	for (it = entities.begin(); it != entities.end(); ++it) {
		// new actions this round
		if (!(*it)->stats.npc) {
			(*it)->logic();

			if (!any_hostile_in_combat && (*it)->stats.alive && !(*it)->stats.hero_ally && (*it)->stats.in_combat)
				any_hostile_in_combat = true;
		}
	}

	// Kind C: whether a hostile is in combat is party-wide state -- every player's in_combat
	// flag now tracks it, not just local(). The controller LED is real local hardware, so that
	// part stays tied to playerm->local() specifically and only fires on the local player's own
	// transition, same edge-triggered shape as before.
	Avatar* local = playerm->local();
	if (local && any_hostile_in_combat && !local->stats.in_combat) {
		// if a supported controller is connected, change the LED to red when in combat
		Color led_color(255, 0, 0);
		inpt->setJoystickLED(led_color);
	}
	else if (local && !any_hostile_in_combat && local->stats.in_combat) {
		inpt->setJoystickLED(InputState::DEFAULT_CONTROLLER_LED_COLOR);
	}

	for (size_t p = 0; p < playerm->players.size(); ++p) {
		playerm->players[p]->stats.in_combat = any_hostile_in_combat;
	}
}

Entity* EntityManager::entityFocus(const Point& mouse, const FPoint& cam, bool alive_only) {
	Point p;
	Rect r;

	Entity* nearest = NULL;
	float best_distance = std::numeric_limits<float>::max();
	FPoint mousef = FPoint(mouse);
	FPoint render_bounds_center;

	for(unsigned int i = 0; i < entities.size(); i++) {
		if (entities[i]->stats.cur_state == StatBlock::ENTITY_DEAD || entities[i]->stats.cur_state == StatBlock::ENTITY_CRITDEAD) {
			if (alive_only)
				continue;
			else if (entities[i]->stats.corpse && entities[i]->stats.corpse_timer.isEnd() && entities[i]->stats.corpse_has_timeout)
				continue;
		}

		Rect render_bounds = entities[i]->getRenderBounds(cam);
		if (Utils::isWithinRect(render_bounds, mouse)) {
			render_bounds_center.x = static_cast<float>(render_bounds.x) + (static_cast<float>(render_bounds.w)/2);
			render_bounds_center.y = static_cast<float>(render_bounds.y) + (static_cast<float>(render_bounds.h)/2);
			float distance = Utils::calcDist(mousef, render_bounds_center);
			if (distance < best_distance) {
				best_distance = distance;
				nearest = entities[i];
			}
		}
	}
	return nearest;
}

Entity* EntityManager::getNearestEntity(const FPoint& pos, bool get_corpse, float *saved_distance, float max_range) {
	Entity* nearest = NULL;
	float best_distance = std::numeric_limits<float>::max();

	for (unsigned i=0; i<entities.size(); i++) {
		if(!get_corpse && (entities[i]->stats.cur_state == StatBlock::ENTITY_DEAD || entities[i]->stats.cur_state == StatBlock::ENTITY_CRITDEAD)) {
			continue;
		}
		if (get_corpse && !entities[i]->stats.corpse) {
			continue;
		}

		float distance = Utils::calcDist(pos, entities[i]->stats.pos);
		if (distance < best_distance) {
			best_distance = distance;
			nearest = entities[i];
		}
	}

	if (nearest && saved_distance)
		*saved_distance = best_distance;

	if (!saved_distance && best_distance > max_range)
		nearest = NULL;

	return nearest;
}

bool EntityManager::isCleared() {
	if (entities.empty()) return true;

	for (unsigned int i=0; i < entities.size(); i++) {
		if (entities[i]->stats.alive && !entities[i]->stats.hero_ally)
			return false;
	}

	return true;
}

void EntityManager::spawn(const std::string& entity_type, const Point& target, EventComponent* ec_spawn_level) {
	Map_Enemy espawn;

	espawn.type = entity_type;
	espawn.pos = FPoint(target);
	espawn.pos.x += 0.5f;
	espawn.pos.y += 0.5f;

	// quick spawns start facing a random direction
	espawn.direction = sim_rng->range(0, 7);

	if (!wmap->collider.isValidPosition(espawn.pos.x, espawn.pos.y, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_NONE)) {
		return;
	}
	else {
		wmap->collider.block(espawn.pos.x, espawn.pos.y, !MapCollision::IS_ALLY);
	}

	if (ec_spawn_level) {
		espawn.spawn_level.parseString(ec_spawn_level->s);
	}

	powers->map_enemies.push(espawn);
}

/**
 * addRenders()
 * Map objects need to be drawn in Z order, so we allow a parent object (GameEngine)
 * to collect all mobile sprites each frame.
 */
void EntityManager::addRenders(std::vector<Renderable> &r, std::vector<Renderable> &r_dead) {
	// This client's own render list -- always playerm->local(), same reasoning as
	// LootManager::addRenders()/NPCManager::addRenders().
	Avatar* local = playerm->local();

	std::vector<Entity*>::iterator it;
	for (it = entities.begin(); it != entities.end(); ++it) {
		if (wmap->fogofwar > FogOfWar::TYPE_MINIMAP && local) {
			float delta = Utils::calcDist(local->stats.pos, (*it)->stats.pos);
			if (delta > fow->mask_radius-1.0) {
				continue;
			}
		}

		bool dead = (*it)->stats.corpse;
		if (!dead || !(*it)->stats.corpse_timer.isEnd() || !(*it)->stats.corpse_has_timeout) {
			if (dead && (*it)->stats.corpse_render_below)
				(*it)->addRenders(r_dead);
			else
				(*it)->addRenders(r);
		}
	}
}

EntityManager::~EntityManager() {
	Utils::logInfo("Cleaning up: EntityManager");

	for (unsigned int i=0; i < entities.size(); i++) {
		if (entities[i]->stats.npc)
			continue;

		entities[i]->unloadSounds();
		delete entities[i];
	}
	for (unsigned int i=0; i < prototypes.size(); i++) {
		prototypes[i].unloadSounds();
	}
}
