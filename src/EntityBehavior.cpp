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

#include "Animation.h"
#include "Avatar.h"
#include "CommonIncludes.h"
#include "Entity.h"
#include "EntityManager.h"
#include "EngineSettings.h"
#include "Entity.h"
#include "EntityBehavior.h"
#include "MapRenderer.h"
#include "PowerManager.h"
#include "Rng.h"
#include "Settings.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "StatBlock.h"
#include "UtilsMath.h"

const float EntityBehavior::ALLY_FLEE_DISTANCE = 2;
const float EntityBehavior::ALLY_FOLLOW_DISTANCE_WALK = 5.5;
const float EntityBehavior::ALLY_FOLLOW_DISTANCE_STOP = 5;
const float EntityBehavior::ALLY_TELEPORT_DISTANCE = 40;

// P2.4 step 1 -- see the member comments in EntityBehavior.h.
//
// AGGRO_LOS_BONUS is deliberately huge relative to the other terms, not a few tiles' worth: LOS
// is meant to dominate the choice whenever there IS a visible candidate (AC5b -- an enemy must
// not fixate on a player it cannot see while another one is in plain sight), and the other terms
// only really matter for breaking ties WITHIN a visibility tier (visible-vs-visible or
// invisible-vs-invisible). It is still a soft preference, not a hard filter, on purpose: with
// only one alive player (single-player, AC1) they are the sole candidate regardless of score, and
// with several players all currently out of sight, an entity still needs to pick its best-guess
// target rather than going target-less, exactly like the old nearest-player code always did.
const float EntityBehavior::AGGRO_LOS_BONUS = 1000.0f;
// A hit at the very top of its recency window (threat_timer[p].getCurrent() == THREAT_WINDOW_TICKS)
// is worth ~6 distance-equivalent tiles; it decays linearly to 0 as the window runs out.
const float EntityBehavior::AGGRO_RECENCY_WEIGHT = 6.0f / static_cast<float>(StatBlock::THREAT_WINDOW_TICKS);
const float EntityBehavior::AGGRO_THREAT_POINTS_WEIGHT = 0.5f;
const float EntityBehavior::AGGRO_HYSTERESIS_BONUS = 4.0f;

static Avatar* playerForSummoner(StatBlock* summoner) {
	if (!summoner || !playerm)
		return NULL;

	for (size_t i = 0; i < playerm->players.size(); ++i) {
		if (&playerm->players[i]->stats == summoner)
			return playerm->players[i];
	}

	return NULL;
}

EntityBehavior::EntityBehavior(Entity *_e)
	: e(_e)
	, path()
	, prev_target()
	, collided(false)
	, path_found(false)
	, chance_calc_path(0)
	, path_found_fails(0)
	, path_found_fail_timer()
	, warp_to_hero(false)
	, target_dist(0)
	, hero_dist(0)
	, pursue_pos(-1, -1)
	, los(false)
	, fleeing(false)
	, move_to_safe_dist(false)
	, turn_timer()
	, instant_power(false)
	, replaced_power_id(0)
	, nearest_player(NULL)
	, nearest_alive_player(NULL)
	, follow_player(NULL)
	, aggro_target_id(-1)
	, aggro_target_los(false)
{
	// wait when PATH_FOUND_FAIL_THRESHOLD is exceeded
	path_found_fail_timer.setDuration(Settings::SIM_TICK_HZ * PATH_FOUND_FAIL_WAIT_SECONDS);
	path_found_fail_timer.reset(Timer::END);
}

/**
 * One frame of logic for this behavior
 */
void EntityBehavior::logic() {
	// skip all logic if the enemy is dead and no longer animating
	if (e->stats.corpse) {
		if (eset->misc.corpse_timeout_enabled)
			e->stats.corpse_timer.tick();

		return;
	}

	// Resolved once per tick -- see the member comment in EntityBehavior.h. Everything below
	// this point that used to read the single-player `pc` global reads one of these two instead.
	nearest_player = playerm->nearestTo(e->stats.pos);
	nearest_alive_player = playerm->nearestAliveTo(e->stats.pos);
	follow_player = e->stats.hero_ally ? playerForSummoner(e->stats.summoner) : NULL;
	if (!follow_player)
		follow_player = nearest_player;

	if (!e->stats.hero_ally) {
		// "within encounter_dist of any player" (not gated on alive -- the original single-player
		// check wasn't either). This is equivalent to checking the *nearest* player's distance:
		// if any player is within range, the nearest one is necessarily within range too, since
		// nearest-overall distance is a lower bound on every other player's distance.
		// `encountered` latches true and is never cleared, so activation is monotone -- a
		// joining player can only wake more enemies, never put any back to sleep.
		if (nearest_player && Utils::calcDist(e->stats.pos, nearest_player->stats.pos) <= eset->combat.encounter_dist)
			e->stats.encountered = true;

		if (!e->stats.encountered)
			return;
	}

	doUpkeep();
	findTarget();
	checkPower();
	checkMove();
	updateState();

	fleeing = false;

}

/**
 * Various upkeep on stats
 */
void EntityBehavior::doUpkeep() {
	// activate all passive powers
	if (e->stats.hp > 0 || e->stats.effects.triggered_death)
		powers->activatePassives(&e->stats);

	e->stats.logic();

	// check for teleport powers
	if (e->stats.teleportation) {

		wmap->collider.unblock(e->stats.pos.x,e->stats.pos.y);

		e->stats.pos.x = e->stats.teleport_destination.x;
		e->stats.pos.y = e->stats.teleport_destination.y;

		wmap->collider.block(e->stats.pos.x,e->stats.pos.y, e->stats.hero_ally);

		e->stats.teleportation = false;
	}
}

/**
 * P2.4 step 1. See the declaration in EntityBehavior.h for the summary; this is only ever called
 * for hostile (!hero_ally) entities, from findTarget() below.
 */
Avatar* EntityBehavior::chooseAggroTarget() {
	Avatar* best = NULL;
	float best_score = 0.f;
	bool best_los = false;

	// Deterministic by construction: playerm->players is kept sorted by id (PlayerManager.h), we
	// walk it front-to-back, and only replace `best` on a STRICTLY greater score -- so an exact
	// tie keeps whichever candidate has the lower id, the same idiom PlayerManager::nearestTo()
	// itself uses. No float-equality comparison ever decides anything here.
	for (size_t i = 0; i < playerm->players.size(); ++i) {
		Avatar* candidate = playerm->players[i];
		if (!candidate->stats.alive)
			continue;

		float dist = Utils::calcDist(e->stats.pos, candidate->stats.pos);
		float score = -dist;

		// LOS is evaluated per candidate, BEFORE scoring -- with several players the nearest one
		// may be behind a wall, and a distance-only score would still pick them. This is a soft
		// preference, not a hard filter: with only one alive player (single-player, AC1) they are
		// the sole candidate regardless of score, so this can never change WHO gets picked there,
		// only the unused score value.
		bool candidate_los = wmap->collider.lineOfSight(e->stats.pos.x, e->stats.pos.y, candidate->stats.pos.x, candidate->stats.pos.y);
		if (candidate_los)
			score += AGGRO_LOS_BONUS;

		// Recency + threat/taunt term -- see StatBlock.h's threat_timer/threat_points comment.
		// PlayerID is small (D3: max 8 players), used directly as the table index.
		if (candidate->id < StatBlock::THREAT_TABLE_SIZE) {
			score += static_cast<float>(e->stats.threat_timer[candidate->id].getCurrent()) * AGGRO_RECENCY_WEIGHT;
			score += static_cast<float>(e->stats.threat_points[candidate->id]) * AGGRO_THREAT_POINTS_WEIGHT;
		}

		// Hysteresis: the entity's CURRENT target needs to be beaten by more than this bonus
		// before another candidate takes over, or two near-equidistant players cause the target
		// to flip every tick -- the most likely first bug here (see the plan's executor notes).
		// Applied as a bonus to the current target's own score, so the comparison below stays a
		// plain deterministic '>' throughout.
		if (aggro_target_id >= 0 && candidate->id == static_cast<PlayerID>(aggro_target_id))
			score += AGGRO_HYSTERESIS_BONUS;

		if (best == NULL || score > best_score) {
			best = candidate;
			best_score = score;
			best_los = candidate_los;
		}
	}

	aggro_target_id = best ? static_cast<int>(best->id) : -1;
	aggro_target_los = best_los;
	return best;
}

/**
 * Locate the player and set various targeting info
 */
void EntityBehavior::findTarget() {
	// dying enemies can't target anything
	if (e->stats.cur_state == StatBlock::ENTITY_DEAD || e->stats.cur_state == StatBlock::ENTITY_CRITDEAD)
		return;

	// standard NPCs don't target anything
	if (e->stats.npc && !e->stats.hero_ally && !e->stats.wander && e->stats.waypoints.empty())
		return;

	// stunned enemies can't act
	if (e->stats.effects.stun)
		return;

	// NPCs engaged in dialog can't act
	if (e->stats.npc && e->stats.in_dialog)
		return;

	StatBlock *target_stats = NULL;
	// A hero ally follows the player whose stats are its actual summoner. If the summoner is
	// unavailable (for example, a legacy converted ally with no owner), retain the old nearest
	// player fallback. This is deliberately resolved by pointer identity through PlayerManager;
	// dereferencing an owner pointer that no longer belongs to a connected player would be unsafe.
	Avatar* summoner_player = e->stats.hero_ally ? playerForSummoner(e->stats.summoner) : NULL;
	Avatar* follow_alive_player = (follow_player && follow_player->stats.alive) ? follow_player : nearest_alive_player;

	// check distance and line of sight between enemy and hero
	// P2.4 step 1: a HOSTILE entity scores every alive player (distance + threat/recency +
	// hysteresis, LOS evaluated per candidate -- see chooseAggroTarget()) instead of always
	// pursuing whichever player is geometrically nearest. hero_ally entities (summons/allies)
	// follow their own summoner, with the old nearest-player fallback only for legacy allies that
	// have no connected owner; scored aggro is specifically about which PLAYER a hostile creature
	// fixates on. With exactly one player chooseAggroTarget() always resolves to that player (or
	// NULL if none is alive), identical to the old nearest_alive_player-only read (AC1).
	Avatar* player_target = e->stats.hero_ally ? (summoner_player ? summoner_player : nearest_alive_player) :
		(playerm->players.size() > 1 ? chooseAggroTarget() : nearest_alive_player);
	if (player_target) {
		target_dist = Utils::calcDist(e->stats.pos, player_target->stats.pos);
		target_stats = &player_target->stats;
	}
	else {
		target_dist = 0;
	}
	hero_dist = target_dist;

	// Per-player stealth (P2.2) must be read from the selected player, not from whichever player
	// happens to be geometrically nearest. Once scored aggro can select a different player, using
	// nearest_alive_player here would make the selected target's stealth ineffective (or apply the
	// wrong player's stealth to it).
	float evaluated_player_stealth = player_target ? std::min(player_target->stats.get(Stats::STEALTH), 100.f) : 0.f;
	float stealth_threat_range = (e->stats.threat_range * (100 - evaluated_player_stealth)) / 100;

	// if the minion gets too far, transport it to its summoner's position
	if (e->stats.hero_ally && e->stats.speed > 0 && (warp_to_hero || hero_dist > ALLY_TELEPORT_DISTANCE) && !e->stats.in_combat && follow_player) {
		wmap->collider.unblock(e->stats.pos.x, e->stats.pos.y);
		e->stats.pos.x = follow_player->stats.pos.x;
		e->stats.pos.y = follow_player->stats.pos.y;
		wmap->collider.block(e->stats.pos.x, e->stats.pos.y, MapCollision::IS_ALLY);
		hero_dist = 0;
		warp_to_hero = false;
	}

	// AI can target other AI
	for (size_t i = 0; i < entitym->entities.size(); ++i) {
		Entity* entity = entitym->entities[i];
		if (!entity->stats.alive)
			continue;

		if ((!e->stats.hero_ally && entity->stats.hero_ally) || (e->stats.hero_ally && !entity->stats.hero_ally && entity->stats.in_combat)) {
			float entity_dist = Utils::calcDist(e->stats.pos, entity->stats.pos);
			if (!target_stats || (e->stats.hero_ally && target_stats->hero)) {
				// pick the first available target if none is already selected
				target_stats = &entitym->entities[i]->stats;
				target_dist = entity_dist;
				e->stats.in_combat = true;
			}
			else if (entity_dist < target_dist) {
				// pick a new target if it's closer
				target_stats = &entitym->entities[i]->stats;
				target_dist = entity_dist;
			}
		}
	}

	// check line-of-sight
	if (target_stats && target_dist < e->stats.threat_range && nearest_alive_player)
		los = wmap->collider.lineOfSight(e->stats.pos.x, e->stats.pos.y, target_stats->pos.x, target_stats->pos.y);
	else
		los = false;

	if (los)
		e->stats.cooldown_los.reset(Timer::BEGIN);

	// aggressive enemies are always in combat
	if (!e->stats.in_combat && e->stats.combat_style == StatBlock::COMBAT_AGGRESSIVE) {
		e->stats.join_combat = true;
	}

	// check entering combat (because the player got too close)
	bool close_to_target = false;
	if (!e->stats.hero_ally && player_target && target_stats == &player_target->stats)
		close_to_target = target_dist < stealth_threat_range;
	else if (target_stats)
		close_to_target = target_dist < e->stats.threat_range;

	if (e->stats.alive && !e->stats.in_combat && los && close_to_target && e->stats.combat_style != StatBlock::COMBAT_PASSIVE) {
		e->stats.join_combat = true;
	}

	// if the join_combat flag wasn't set above, it could have been set if the enemy was hit by a hazard
	// we put the entity in a combat state and activate powers that trigger when entering combat
	if (e->stats.join_combat) {
		e->stats.in_combat = true;

		// we need to reset the los cooldown here to prevent getting stuck in the join_combat state
		// this happens when the entity doesn't have los, but enters combat due to being hit (by a beacon or otherwise)
		e->stats.cooldown_los.reset(Timer::BEGIN);

		StatBlock::AIPower* ai_power;
		if (!e->stats.hero_ally) {
			ai_power = e->stats.getAIPower(StatBlock::AI_POWER_BEACON);
			if (ai_power != NULL) {
				powers->activate(ai_power->id, &e->stats, e->stats.pos, e->stats.pos); //emit beacon
			}
		}

		ai_power = e->stats.getAIPower(StatBlock::AI_POWER_JOIN_COMBAT);
		if (ai_power != NULL) {
			e->stats.cur_state = StatBlock::ENTITY_POWER;
			e->stats.activated_power = ai_power;
			replaced_power_id = powers->checkReplaceByEffect(ai_power->id, &e->stats);
		}

		e->stats.join_combat = false;
	}

	// exit combat if target got too far away
	if (e->stats.combat_style != StatBlock::COMBAT_AGGRESSIVE) {
		if (target_dist > e->stats.threat_range_far)
			e->stats.in_combat = false;

		if (e->stats.cooldown_los.getDuration() > 0 && e->stats.cooldown_los.isEnd())
			e->stats.in_combat = false;
	}

	// exit combat if either party is dead
	if (!e->stats.alive || !nearest_alive_player || (target_stats && !target_stats->alive))
		e->stats.in_combat = false;

	// exit combat if ally is targeting its follow target rather than an enemy
	if (e->stats.hero_ally && follow_alive_player && target_stats == &follow_alive_player->stats)
		e->stats.in_combat = false;

	if (target_stats)
		pursue_pos = target_stats->pos;

	// if we just started wandering, set the first waypoint
	if (e->stats.wander && e->stats.waypoints.empty()) {
		FPoint waypoint = getWanderPoint();
		e->stats.waypoints.push(waypoint);
		e->stats.waypoint_timer.reset(Timer::BEGIN);
	}

	// if we're not in combat, pursue the next waypoint
	if (!(e->stats.in_combat || e->stats.waypoints.empty())) {
		FPoint waypoint = e->stats.waypoints.front();
		pursue_pos.x = waypoint.x;
		pursue_pos.y = waypoint.y;
	}

	// if the player is blocked, all summons which the player is facing to move away for the specified frames
	// need to set the flag player_blocked so that other allies know to get out of the way as well
	// if hero is facing the summon
	if (e->stats.hero_ally && eset->misc.enable_ally_collision_ai && follow_player) {
		if (!entitym->player_blocked && hero_dist < ALLY_FLEE_DISTANCE
				&& wmap->collider.isFacing(follow_player->stats.pos.x,follow_player->stats.pos.y,follow_player->stats.direction,e->stats.pos.x,e->stats.pos.y)) {
			entitym->player_blocked = true;
			entitym->player_blocked_timer.reset(Timer::BEGIN);
		}

		bool player_closer_than_target = Utils::calcDist(e->stats.pos, pursue_pos) > Utils::calcDist(e->stats.pos, follow_player->stats.pos);

		if (entitym->player_blocked && (!e->stats.in_combat || player_closer_than_target)
				&& wmap->collider.isFacing(follow_player->stats.pos.x,follow_player->stats.pos.y,follow_player->stats.direction,e->stats.pos.x,e->stats.pos.y)) {
			fleeing = true;
			pursue_pos = follow_player->stats.pos;
		}
	}

	if (e->stats.effects.fear) fleeing = true;

	// If we have a successful chance_flee roll, try to move to a safe distance
	if (
			e->stats.in_combat &&
			e->stats.cur_state == StatBlock::ENTITY_STANCE &&
			!move_to_safe_dist && target_dist < e->stats.flee_range &&
			target_dist >= e->stats.melee_range &&
			sim_rng->percentChanceF(e->stats.chance_flee) &&
			e->stats.flee_cooldown_timer.isEnd()
		)
	{
		move_to_safe_dist = true;
	}

	if (move_to_safe_dist) fleeing = true;

	if (fleeing) {
		FPoint target_pos = pursue_pos;

		std::vector<int> flee_dirs;

		int middle_dir = Utils::calcDirection(target_pos.x, target_pos.y, e->stats.pos.x, e->stats.pos.y);
		for (int i = -2; i <= 2; ++i) {
			int test_dir = Utils::rotateDirection(middle_dir, i);

			FPoint test_pos = Utils::calcVector(e->stats.pos, test_dir, 1);
			if (wmap->collider.isValidPosition(test_pos.x, test_pos.y, e->stats.movement_type, MapCollision::COLLIDE_TYPE_ALL_ENTITIES)) {
				if (test_dir == e->stats.direction) {
					// if we're already moving in a good direction, favor it over other directions
					flee_dirs.clear();
					flee_dirs.push_back(test_dir);
					break;
				}
				else {
					flee_dirs.push_back(test_dir);
				}
			}
		}

		if (flee_dirs.empty()) {
			// trapped and can't move
			move_to_safe_dist = false;
			fleeing = false;
		}
		else {
			int index = sim_rng->range(0, static_cast<int>(flee_dirs.size())-1);
			pursue_pos = Utils::calcVector(e->stats.pos, flee_dirs[index], 1);

			if (e->stats.flee_timer.isEnd()) {
				e->stats.flee_timer.reset(Timer::BEGIN);
			}
		}
	}
}

/**
 * Begin using a power if idle, based on behavior % chances.
 * Activate a ready power, if the attack animation has followed through
 */
void EntityBehavior::checkPower() {

	// stunned enemies can't act
	if (e->stats.effects.stun || e->stats.effects.fear || fleeing) return;

	// currently all enemy power use happens during combat
	if (!e->stats.in_combat) return;

	// if the enemy is on global cooldown it cannot act
	if (!e->stats.cooldown.isEnd()) return;

	// NPCs engaged in dialog can't act
	if (e->stats.npc && e->stats.in_dialog)
		return;

	// Note there are two stages to activating a power.
	// First is the enemy choosing to use a power based on behavioral chance
	// Second is the power actually firing off once the related animation reaches the active frame.
	// The second stage occurs in updateState()

	// pick a power from the available powers for this creature
	if (e->stats.cur_state == StatBlock::ENTITY_STANCE || e->stats.cur_state == StatBlock::ENTITY_MOVE) {
		StatBlock::AIPower* ai_power = NULL;

		// check half dead power use
		if (e->stats.half_dead_power && e->stats.hp <= e->stats.get(Stats::HP_MAX)/2) {
			ai_power = e->stats.getAIPower(StatBlock::AI_POWER_HALF_DEAD);
		}
		// check ranged power use
		else if (target_dist > e->stats.melee_range) {
			ai_power = e->stats.getAIPower(StatBlock::AI_POWER_RANGED);
		}
		// check melee power use
		else {
			ai_power = e->stats.getAIPower(StatBlock::AI_POWER_MELEE);
		}

		if (ai_power != NULL && powers->isValid(ai_power->id)) {
			PowerID replaced_id = powers->checkReplaceByEffect(ai_power->id, &e->stats);
			if (replaced_id == 0) {
				ai_power = NULL;
			}
			else {
				Power* pwr = powers->powers[replaced_id];
				if (!los && (pwr->requires_los || pwr->requires_los_default)) {
					ai_power = NULL;
				}
				if (ai_power != NULL) {
					e->stats.cur_state = StatBlock::ENTITY_POWER;
					e->stats.activated_power = ai_power;
					replaced_power_id = replaced_id;

					// we might already be in the animation used by this power,
					// so we reset the animation here to keep us from getting stuck on the last frame
					if (pwr->new_state != Power::STATE_INSTANT) {
						e->resetActiveAnimation();
					}
				}
			}
		}
	}

	if (e->stats.cur_state != StatBlock::ENTITY_POWER && e->stats.activated_power) {
		e->stats.activated_power = NULL;
	}
}

/**
 * Check state changes related to movement
 */
void EntityBehavior::checkMove() {

	// dying enemies can't move
	if (e->stats.cur_state == StatBlock::ENTITY_DEAD || e->stats.cur_state == StatBlock::ENTITY_CRITDEAD) return;

	// stunned enemies can't act
	if (e->stats.effects.stun) return;

	// NPCs engaged in dialog can't act
	if (e->stats.npc && e->stats.in_dialog) {
		if (e->stats.cur_state == StatBlock::ENTITY_MOVE) {
			e->stats.cur_state = StatBlock::ENTITY_STANCE;
		}
		return;
	}

	// handle not being in combat and (not patrolling waypoints or waiting at waypoint)
	if (!e->stats.hero_ally && !e->stats.in_combat && (e->stats.waypoints.empty() || !e->stats.waypoint_timer.isEnd())) {

		if (e->stats.cur_state == StatBlock::ENTITY_MOVE) {
			e->stats.cur_state = StatBlock::ENTITY_STANCE;
		}

		// currently enemies only move while in combat or patrolling
		return;
	}

	float real_speed = e->stats.speed * StatBlock::SPEED_MULTIPLIER[e->stats.direction] * e->stats.effects.speed / 100;

	unsigned turn_ticks = turn_timer.getCurrent();
	turn_timer.setDuration(e->stats.turn_delay);

	// If an enemy's turn_delay is too long compared to their speed, they will be unable to follow a path properly.
	// So here, we get how many frames it takes to traverse a single tile and then compare it to the turn delay time.
	// We then cap the turn delay the time at the number of frames we calculated for tile traversal.
	// There may be other solutions to this problem, such as having the enemy pause when they reach a path point,
	// but I was unable to get anything else working as cleanly/bug-free as this.
	int max_turn_ticks = (real_speed == 0) ? e->stats.turn_delay : static_cast<int>(1.f / real_speed);
	if (e->stats.turn_delay > max_turn_ticks) {
		turn_timer.setDuration(max_turn_ticks);
	}
	turn_timer.setCurrent(turn_ticks);

	// clear current space to allow correct movement
	wmap->collider.unblock(e->stats.pos.x, e->stats.pos.y);

	path_found_fail_timer.tick();

	// update direction
	if (e->stats.facing) {
		turn_timer.tick();
		if (turn_timer.isEnd()) {

			// if blocked, face in pathfinder direction instead
			if (!wmap->collider.lineOfMovement(e->stats.pos.x, e->stats.pos.y, pursue_pos.x, pursue_pos.y, e->stats.movement_type)) {

				// if a path is returned, target first waypoint

				bool recalculate_path = false;

				// add a 5% chance to recalculate on every frame. This prevents reclaulating lots of entities in the same frame
				chance_calc_path += 5;

				bool calc_path_success = sim_rng->percentChance(chance_calc_path);
				if (calc_path_success)
					recalculate_path = true;

				// if a collision ocurred then recalculate
				if (collided)
					recalculate_path = true;

				// if theres no path, it needs to be calculated
				if (!recalculate_path && path.empty())
					recalculate_path = true;

				// if the target moved more than 1 tile away, recalculate
				if (!recalculate_path && Utils::calcDist(FPoint(Point(prev_target)), FPoint(Point(pursue_pos))) > 1.f)
					recalculate_path = true;

				// dont recalculate if we were blocked and no path was found last time
				// this makes sure that pathfinding calculation is not spammed when the target is unreachable and the entity is as close as its going to get
				if (!path_found && collided && !calc_path_success) {
					recalculate_path = false;
				}
				else {
					// reset the collision flag only if we dont want the cooldown in place
					collided = false;
				}

				if (!path_found_fail_timer.isEnd()) {
					recalculate_path = false;
					chance_calc_path = -100;
				}

				prev_target = pursue_pos;

				// target first waypoint
				if (recalculate_path) {
					chance_calc_path = -100;
					path.clear();
					path_found = wmap->collider.computePath(e->stats.pos, pursue_pos, path, e->stats.movement_type, MapCollision::DEFAULT_PATH_LIMIT);

					if (!path_found) {
						path_found_fails++;
						if (path_found_fails >= PATH_FOUND_FAIL_THRESHOLD) {
							// could not find a path after several tries, so wait a little before the next attempt
							path_found_fail_timer.reset(Timer::BEGIN);
						}
					}
					else {
						path_found_fails = 0;
						path_found_fail_timer.reset(Timer::END);
					}
				}

				if (!path.empty()) {
					pursue_pos = path.back();

					// if distance to node is lower than a tile size, the node is going to be passed and can be removed
					if (Utils::calcDist(e->stats.pos, pursue_pos) <= 1.f)
						path.pop_back();
				}
	else if (e->stats.hero_ally && follow_player && pursue_pos == follow_player->stats.pos) {
					warp_to_hero = true;
				}
			}
			else {
				path.clear();
			}

			if (e->stats.charge_speed == 0.0f) {
				e->stats.direction = Utils::calcDirection(e->stats.pos.x, e->stats.pos.y, pursue_pos.x, pursue_pos.y);
			}
			turn_timer.reset(Timer::BEGIN);
		}
	}

	e->stats.flee_timer.tick();
	e->stats.flee_cooldown_timer.tick();

	// try to start moving
	if (e->stats.cur_state == StatBlock::ENTITY_STANCE) {
		checkMoveStateStance();
	}

	// already moving
	else if (e->stats.cur_state == StatBlock::ENTITY_MOVE) {
		checkMoveStateMove();
	}

	// if patrolling waypoints and has reached a waypoint, cycle to the next one
	if (!e->stats.waypoints.empty()) {
		// if the patroller is close to the waypoint
		FPoint waypoint = e->stats.waypoints.front();
		float waypoint_dist = Utils::calcDist(waypoint, e->stats.pos);

		FPoint saved_pos = e->stats.pos;
		e->move();
		float new_dist = Utils::calcDist(waypoint, e->stats.pos);
		e->stats.pos = saved_pos;

		if (waypoint_dist <= real_speed || (waypoint_dist <= 0.5f && new_dist > waypoint_dist)) {
			e->stats.pos = waypoint;
			turn_timer.reset(Timer::END);
			e->stats.waypoints.pop();
			// pick a new random point if we're wandering
			if (e->stats.wander) {
				waypoint = getWanderPoint();
			}
			e->stats.waypoints.push(waypoint);
			e->stats.waypoint_timer.reset(Timer::BEGIN);
		}
	}

	// re-block current space to allow correct movement
	wmap->collider.block(e->stats.pos.x, e->stats.pos.y, e->stats.hero_ally);

}

void EntityBehavior::checkMoveStateStance() {

	// If the enemy is capable of fleeing and is at a safe distance, have it hold its position instead of moving
	if (target_dist >= e->stats.flee_range && e->stats.chance_flee > 0 && e->stats.waypoints.empty()) return;

	// try to move to the target if we're either:
	// 1. too far away and chance_pursue roll succeeds
	// 2. within range, but lack line-of-sight (required to attack)
	bool ally_targeting_hero = e->stats.hero_ally && !e->stats.in_combat && hero_dist > ALLY_FOLLOW_DISTANCE_WALK;
	bool should_move_to_target = (e->stats.in_combat || !e->stats.waypoints.empty()) && ((target_dist > e->stats.melee_range && sim_rng->percentChanceF(e->stats.chance_pursue)) || (target_dist <= e->stats.melee_range && !los));

	if (should_move_to_target || fleeing || ally_targeting_hero) {

		if (e->move()) {
			e->stats.cur_state = StatBlock::ENTITY_MOVE;
		}
		else {
			collided = true;
			unsigned char prev_direction = e->stats.direction;

			// hit an obstacle, try the next best angle
			e->stats.direction = e->faceNextBest(pursue_pos.x, pursue_pos.y);
			if (e->move()) {
				e->stats.cur_state = StatBlock::ENTITY_MOVE;
			}
			else
				e->stats.direction = prev_direction;
		}
	}
}

void EntityBehavior::checkMoveStateMove() {
	bool can_attack = true;

	if (!e->stats.cooldown.isEnd()) {
		can_attack = false;
	}
	else {
		can_attack = false;
		for (size_t i = 0; i < e->stats.powers_ai.size(); ++i) {
			if (e->stats.powers_ai[i].cooldown.isEnd()) {
				can_attack = true;
				break;
			}
		}
	}
	// in order to prevent infinite fleeing, we re-roll our chance to flee after a certain duration
	bool stop_fleeing = can_attack && fleeing && e->stats.flee_timer.isEnd() && !sim_rng->percentChanceF(e->stats.chance_flee);

	if (!stop_fleeing && e->stats.flee_timer.isEnd()) {
		// if the roll to continue fleeing succeeds, but the flee duration has expired, we don't want to reset the duration to the full amount
		// instead, we scehdule the next re-roll to happen on the next frame
		// this will continue until a roll fails, returning to the stance state
		e->stats.flee_timer.setCurrent(1);
	}

	// close enough to the hero or is at a safe distance
	bool ally_targeting_hero = e->stats.hero_ally && !e->stats.in_combat && !fleeing && hero_dist < ALLY_FOLLOW_DISTANCE_STOP;
	bool has_alive_follow_target = e->stats.hero_ally ? (follow_player && follow_player->stats.alive) : (nearest_alive_player != NULL);
	if (has_alive_follow_target && ((target_dist < e->stats.melee_range && !fleeing) || (move_to_safe_dist && target_dist >= e->stats.flee_range) || stop_fleeing || ally_targeting_hero)) {
		if (stop_fleeing) {
			e->stats.flee_cooldown_timer.reset(Timer::BEGIN);
		}
		e->stats.cur_state = StatBlock::ENTITY_STANCE;
		move_to_safe_dist = false;
		fleeing = false;
	}

	// try to continue moving
	else if (!e->move()) {
		collided = true;
		unsigned char prev_direction = e->stats.direction;
		// hit an obstacle.  Try the next best angle
		e->stats.direction = e->faceNextBest(pursue_pos.x, pursue_pos.y);
		if (!e->move()) {
			// this prevents an ally trying to move perpendicular to a 1-tile-wide path if the player gets close to it in a certain position and gets blocked
			if (e->stats.hero_ally && entitym->player_blocked && !e->stats.in_combat && follow_player) {
				e->stats.direction = follow_player->stats.direction;
				if (!e->move()) {
					e->stats.cur_state = StatBlock::ENTITY_STANCE;
					e->stats.direction = prev_direction;
				}
			}
			else {
				e->stats.cur_state = StatBlock::ENTITY_STANCE;
				e->stats.direction = prev_direction;
			}
		}
	}
}

void EntityBehavior::checkOnStatePower(StatBlock::AIPower** on_state_power) {
	if (!on_state_power || !(*on_state_power))
		return;

	StatBlock::AIPower* ai_power = *on_state_power;
	PowerID replaced_id = powers->checkReplaceByEffect(ai_power->id, &e->stats);

	if (replaced_id != 0) {
		Power* pwr = powers->powers[replaced_id];
		if (pwr->new_state == Power::STATE_INSTANT) {
			powers->activate(replaced_id, &e->stats, e->stats.pos, pursue_pos);
		}
		else if (e->stats.cur_state == StatBlock::ENTITY_POWER) {
			// non-instant hit powers can only activate if the entity is not already using a power (and only one at a time)
			*on_state_power = NULL;
			return;
		}

		if (pwr->new_state != Power::STATE_INSTANT) {
			e->stats.cur_state = Power::STATE_ATTACK;
			e->stats.activated_power = ai_power;
			replaced_power_id = replaced_id;
		}

		// set cooldown for all ai powers with the same power id
		for (size_t i = 0; i < e->stats.powers_ai.size(); ++i) {
			if (ai_power->id == e->stats.powers_ai[i].id) {
				e->stats.powers_ai[i].cooldown.setDuration(pwr->cooldown);
			}
		}
	}

	*on_state_power = NULL;
}

/**
 * Perform miscellaneous state-based actions.
 * 1) Set animations and sound effects
 * 2) Return to the default state (Stance) when actions are complete
 */
void EntityBehavior::updateState() {

	// stunned enemies can't act
	if (e->stats.effects.stun) return;

	checkOnStatePower(&e->stats.ai_debuff_power);
	checkOnStatePower(&e->stats.ai_hit_power);

	PowerID power_id, power_id_base;
	Power* epower;
	int power_state;

	// continue current animations
	if (e->activeAnimation && !e->stats.hold_state)
		e->activeAnimation->advanceFrame();

	for (size_t i = 0; i < e->anims.size(); ++i) {
		if (e->anims[i])
			e->anims[i]->advanceFrame();
	}

	switch (e->stats.cur_state) {

		case StatBlock::ENTITY_STANCE:

			e->setAnimation("stance");
			break;

		case StatBlock::ENTITY_MOVE:

			e->setAnimation("run");
			break;

		case StatBlock::ENTITY_POWER:

			if (e->stats.activated_power == NULL) {
				e->stats.cur_state = StatBlock::ENTITY_STANCE;
				break;
			}

			power_id = replaced_power_id;
			power_id_base = e->stats.activated_power->id;

			if (!powers->isValid(power_id) || !powers->isValid(power_id_base)) {
				e->stats.cur_state = StatBlock::ENTITY_STANCE;
				break;
			}

			epower = powers->powers[power_id];
			power_state = epower->new_state;
			e->stats.prevent_interrupt = epower->prevent_interrupt;

			// animation based on power type
			if (power_state == Power::STATE_INSTANT)
				instant_power = true;
			else if (power_state == Power::STATE_ATTACK)
				e->setAnimation(epower->attack_anim);

			// sound effect based on power type
			if (e->activeAnimation->isFirstFrame()) {
				// pre power
				for (size_t i = 0; i < epower->chain_powers.size(); ++i) {
					ChainPower& chain_power = epower->chain_powers[i];
					if (chain_power.type == ChainPower::TYPE_PRE && sim_rng->percentChanceF(chain_power.chance)) {
						powers->activate(chain_power.id, &e->stats, e->stats.pos, pursue_pos);
					}
				}

				float attack_speed = (e->stats.effects.getAttackSpeed(epower->attack_anim) * epower->attack_speed) / 100.0f;
				e->activeAnimation->setSpeed(attack_speed);
				e->playAttackSound(epower->attack_anim);

				if (epower->state_duration > 0)
					e->stats.state_timer.setDuration(epower->state_duration);

				if (epower->charge_speed != 0.0f) {
					e->stats.charge_speed = epower->charge_speed;

					if (epower->charge_duration > 0) {
						e->stats.charge_timer.setDuration(epower->charge_duration);
					}
					else {
						e->stats.charge_timer.setDuration(0);
					}
				}
			}

			if (epower->state_hold_mode == Power::HOLD_ON_FRAME) {
				if (e->activeAnimation->isFrame(epower->state_hold_frame) && !e->stats.hold_state) {
					if (!e->stats.state_timer.isEnd())
						e->stats.hold_state = true;
				}
				if (e->stats.state_timer.isEnd())
					e->stats.hold_state = false;
			}

			// Activate Power:
			// if we're at the active frame of a power animation,
			// activate the power and set the local and global cooldowns
			if ((e->activeAnimation->isActiveFrame() || instant_power) && !e->stats.hold_state) {
				powers->activate(power_id, &e->stats, e->stats.pos, pursue_pos);

				// set cooldown for all ai powers with the same power id
				for (size_t i = 0; i < e->stats.powers_ai.size(); ++i) {
					if (e->stats.activated_power->id == e->stats.powers_ai[i].id) {
						e->stats.powers_ai[i].cooldown.setDuration(epower->cooldown);
					}
				}

				if (e->stats.activated_power->type == StatBlock::AI_POWER_HALF_DEAD) {
					e->stats.half_dead_power = false;
				}

				if (epower->state_hold_mode == Power:: HOLD_ON_ACTIVE_FRAME && !e->stats.state_timer.isEnd())
					e->stats.hold_state = true;
			}

			// animation is finished
			if ((e->activeAnimation->isLastFrame() && e->stats.state_timer.isEnd()) || (power_state == Power::STATE_ATTACK && e->activeAnimation->getName() != epower->attack_anim) || instant_power) {
				if (!instant_power)
					e->stats.cooldown.reset(Timer::BEGIN);
				else
					instant_power = false;

				e->stats.activated_power = NULL;
				e->stats.prevent_interrupt = false;
				if (e->stats.hp > 0) {
					e->stats.cur_state = StatBlock::ENTITY_STANCE;
				}
			}
			break;

		case StatBlock::ENTITY_SPAWN:

			e->setAnimation("spawn");
			//the second check is needed in case the entity does not have a spawn animation
			if (e->activeAnimation->isLastFrame() || e->activeAnimation->getName() != "spawn") {
				e->stats.cur_state = StatBlock::ENTITY_STANCE;
			}
			break;

		case StatBlock::ENTITY_BLOCK:

			e->setAnimation("block");
			break;

		case StatBlock::ENTITY_HIT:

			e->setAnimation("hit");
			if (e->activeAnimation->isFirstFrame()) {
				e->stats.effects.triggered_hit = true;
			}
			if (e->activeAnimation->isLastFrame() || e->activeAnimation->getName() != "hit")
				e->stats.cur_state = StatBlock::ENTITY_STANCE;
			break;

		case StatBlock::ENTITY_DEAD:
			if (e->stats.effects.triggered_death) break;

			e->setAnimation("die");
			if (e->activeAnimation->isFirstFrame()) {
				e->playSound(Entity::SOUND_DIE);
				e->stats.corpse_timer.setDuration(eset->misc.corpse_timeout);
			}
			if ((e->activeAnimation->default_active_frames && e->activeAnimation->isSecondLastFrame()) || (!e->activeAnimation->default_active_frames && e->activeAnimation->isActiveFrame())) {
				StatBlock::AIPower* ai_power = e->stats.getAIPower(StatBlock::AI_POWER_DEATH);
				if (ai_power != NULL)
					powers->activate(ai_power->id, &e->stats, e->stats.pos, e->stats.pos);

				e->stats.effects.clearEffects();
			}
			if (e->activeAnimation->isLastFrame() || e->activeAnimation->getName() != "die") {
				// puts renderable under object layer
				e->stats.corpse = true;

				//allow free movement over the corpse
				if (!e->stats.corpse_has_collision) {
					wmap->collider.unblock(e->stats.pos.x, e->stats.pos.y);
				}

				// remove corpses that land on blocked tiles, such as water or pits
				if (!wmap->collider.isValidPosition(e->stats.pos.x, e->stats.pos.y, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_ALL_ENTITIES)) {
					e->stats.corpse_timer.reset(Timer::END);
				}

				// prevent "jumping" when rendering
				e->stats.pos.align();
			}

			break;

		case StatBlock::ENTITY_CRITDEAD:
			if (e->stats.effects.triggered_death) break;

			e->setAnimation("critdie");
			if (e->activeAnimation->isFirstFrame()) {
				e->playSound(Entity::SOUND_CRITDIE);
				e->stats.corpse_timer.setDuration(eset->misc.corpse_timeout);
			}
			if ((e->activeAnimation->default_active_frames && e->activeAnimation->isSecondLastFrame()) || (!e->activeAnimation->default_active_frames && e->activeAnimation->isActiveFrame())) {
				StatBlock::AIPower* ai_power = e->stats.getAIPower(StatBlock::AI_POWER_DEATH);
				if (ai_power != NULL)
					powers->activate(ai_power->id, &e->stats, e->stats.pos, e->stats.pos);

				e->stats.effects.clearEffects();
			}
			if (e->activeAnimation->isLastFrame() || e->activeAnimation->getName() != "critdie") {
				// puts renderable under object layer
				e->stats.corpse = true;

				//allow free movement over the corpse
				if (!e->stats.corpse_has_collision) {
					wmap->collider.unblock(e->stats.pos.x, e->stats.pos.y);
				}

				// remove corpses that land on blocked tiles, such as water or pits
				if (!wmap->collider.isValidPosition(e->stats.pos.x, e->stats.pos.y, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_ALL_ENTITIES)) {
					e->stats.corpse_timer.reset(Timer::END);
				}

				// prevent "jumping" when rendering
				e->stats.pos.align();
			}

			break;

		default:
			break;
	}

	if (e->stats.state_timer.isEnd() && e->stats.hold_state)
		e->stats.hold_state = false;

	if ((e->stats.cur_state != StatBlock::ENTITY_POWER || (e->stats.charge_timer.getDuration() > 0 && e->stats.charge_timer.isEnd())) && e->stats.charge_speed != 0.0f) {
		e->stats.charge_speed = 0.0f;
		e->stats.charge_timer.reset(Timer::END);
	}
}

FPoint EntityBehavior::getWanderPoint() {
	FPoint waypoint;
	waypoint.x = static_cast<float>(e->stats.wander_area.x) + static_cast<float>(sim_rng->range(0, e->stats.wander_area.w - 1)) + 0.5f;
	waypoint.y = static_cast<float>(e->stats.wander_area.y) + static_cast<float>(sim_rng->range(0, e->stats.wander_area.h - 1)) + 0.5f;

	if (wmap->collider.isValidPosition(waypoint.x, waypoint.y, e->stats.movement_type, wmap->collider.getCollideType(e->stats.hero)) &&
	    wmap->collider.lineOfMovement(e->stats.pos.x, e->stats.pos.y, waypoint.x, waypoint.y, e->stats.movement_type))
	{
		return waypoint;
	}
	else {
		// didn't get a valid waypoint, so keep our current position
		return e->stats.pos;
	}
}
EntityBehavior::~EntityBehavior() {
}
