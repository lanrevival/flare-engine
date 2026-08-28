/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Stefan Beller
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
 * class HazardManager
 *
 * Holds the collection of hazards (active attacks, spells, etc) and handles group operations
 */

#include "Avatar.h"
#include "Animation.h"
#include "Entity.h"
#include "EntityManager.h"
#include "EventManager.h"
#include "Hazard.h"
#include "HazardManager.h"
#include "MapRenderer.h"
#include "PlayerManager.h"
#include "PowerManager.h"
#include "RenderDevice.h"
#include "Rng.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "SimEvents.h"
#include "SoundManager.h"
#include "Utils.h"
#include "UtilsMath.h"

HazardManager::HazardManager()
	: last_enemy(NULL)
	, dump_damage_events(false)
	, dump_tick(0)
{
}

/**
 * P2.4 step 0/6 (--dump-damage-events, AC6). source/dest are each 'hero', 'ally' or 'enemy' --
 * printed as "src->dst" specifically so a friendly-fire hit between two players greps as the
 * literal substring 'hero->hero'.
 */
static const char* damageEventLabel(const StatBlock& stats) {
	if (stats.hero) return "hero";
	if (stats.hero_ally) return "ally";
	return "enemy";
}

void HazardManager::logic() {
	dump_tick++;

	// remove all hazards with lifespan 0.  Most hazards still display their last frame.
	for (size_t i=h.size(); i>0; i--) {
		if (h[i-1]->lifespan == 0) {
			for (size_t j = 0; j < h[i-1]->power->chain_powers.size(); ++j) {
				ChainPower& chain_power = h[i-1]->power->chain_powers[j];
				if (chain_power.type == ChainPower::TYPE_EXPIRE && sim_rng->percentChanceF(chain_power.chance)) {
					powers->activate(chain_power.id, h[i-1]->src_stats, h[i-1]->pos, h[i-1]->pos);

					if (powers->powers[chain_power.id]->directional) {
						powers->hazards.back()->direction = h[i-1]->direction;
					}
				}
			}

			delete h[i-1];
			h.erase(h.begin()+(i-1));
		}
	}

	checkNewHazards();

	// handle single-frame transforms
	for (size_t i=h.size(); i>0; i--) {
		size_t hindex = i-1;
		Hazard* hazard = h[hindex];

		hazard->logic();

		// remove all hazards that need to die immediately (e.g. exit the map)
		if (hazard->remove_now) {
			delete hazard;
			h.erase(h.begin()+(hindex));
			continue;
		}


		// if a moving hazard hits a wall, check for an after-effect
		if (hazard->hit_wall) {
			if (hazard->power->script_trigger == Power::SCRIPT_TRIGGER_WALL) {
				eventm->executeScript(hazard->power->script, hazard->pos.x, hazard->pos.y);
			}

			for (size_t j = 0; j < hazard->power->chain_powers.size(); ++j) {
				ChainPower& chain_power = hazard->power->chain_powers[j];
				if (chain_power.type == ChainPower::TYPE_WALL && sim_rng->percentChanceF(chain_power.chance)) {
					powers->activate(chain_power.id, hazard->src_stats, hazard->pos, hazard->pos);

					if (powers->powers[chain_power.id]->directional) {
						powers->hazards.back()->direction = hazard->direction;
					}
				}
			}

			// clear wall hit
			hazard->hit_wall = false;
		}

		// handle collisions
		if (hazard->isDangerousNow()) {

			// process hazards that can hurt enemies & allies
			for (size_t eindex = 0; eindex < entitym->entities.size(); eindex++) {
				Entity *e = entitym->entities[eindex];

				// hero/ally powers can only hit allies if target_party is true
				if ((hazard->source_type == Power::SOURCE_TYPE_HERO || hazard->source_type == Power::SOURCE_TYPE_ALLY) && e->stats.hero_ally && !hazard->power->target_party) {
					continue;
				}

				// enemy hazard can't hurt other enemies
				if (hazard->source_type == Power::SOURCE_TYPE_ENEMY && !e->stats.hero_ally) {
					continue;
				}

				// only check living enemies
				if (e->stats.hp > 0 && hazard->active) {
					if (Utils::isWithinRadius(hazard->pos, hazard->power->radius, e->stats.pos)) {
						if (!hazard->hasEntity(e)) {
							// hit!
							hazard->addEntity(e);
							bool hit = e->takeHit(*hazard);
							hitEntity(hindex, hit);
							if (!hazard->power->beacon) {
								last_enemy = e;
							}

							if (hit) {
								if (dump_damage_events) {
									printf("tick=%lu %s->%s dmg=hit\n", dump_tick,
									       damageEventLabel(*hazard->src_stats), damageEventLabel(e->stats));
								}

								// P2.4 step 1: record threat for chooseAggroTarget() (EntityBehavior.cpp)
								// whenever a specific player's own hazard lands -- ally/summon-dealt
								// damage isn't attributed to a player id here, kept simple per the
								// plan's own "keep it simple" guidance rather than walking summoner
								// chains to find an owning player.
								if (hazard->source_type == Power::SOURCE_TYPE_HERO) {
									for (size_t pi = 0; pi < playerm->players.size(); ++pi) {
										if (&playerm->players[pi]->stats == hazard->src_stats) {
											e->stats.registerThreat(playerm->players[pi]->id);
											break;
										}
									}
								}
							}
						}
					}
				}

			}

			// process hazards that can hurt the hero
			// P2.2 step (kind C): with several players, a hazard can hit any/all of them
			// independently -- checked and applied per player, not just the single old pc.
			if (hazard->source_type != Power::SOURCE_TYPE_HERO && hazard->source_type != Power::SOURCE_TYPE_ALLY) { //enemy or neutral sources
				for (size_t pindex = 0; pindex < playerm->players.size(); ++pindex) {
					Avatar* player = playerm->players[pindex];
					if (player->stats.hp > 0 && hazard->active) {
						if (Utils::isWithinRadius(hazard->pos, hazard->power->radius, player->stats.pos)) {
							if (!hazard->hasEntity(player)) {
								// hit!
								hazard->addEntity(player);
								bool hit = player->takeHit(*hazard);
								hitEntity(hindex, hit);

								// This loop only ever runs for enemy/neutral-sourced hazards (the
								// guard above), so 'src' here is never 'hero' or 'ally' -- a
								// friendly-fire hit between two players is architecturally
								// impossible today (see AC6/step 6): a hero/ally-sourced hazard
								// only ever checks entitym->entities, never playerm->players, and
								// this is the only loop that checks playerm->players at all.
								if (hit && dump_damage_events) {
									printf("tick=%lu %s->%s dmg=hit\n", dump_tick,
									       damageEventLabel(*hazard->src_stats), damageEventLabel(player->stats));
								}
							}
						}
					}
				}
			}

			// dispel hazards can remove other hazards by ID
			for (size_t j = 0; j < hazard->power->dispel_power_ids.size(); ++j) {
				PowerID dispel_id = hazard->power->dispel_power_ids[j];

				for (size_t k = 0; k < h.size(); ++k) {
					if (dispel_id != h[k]->power_index)
						continue;

					if (hazard->source_type == Power::SOURCE_TYPE_NEUTRAL ||
						(hazard->source_type == Power::SOURCE_TYPE_ENEMY && h[k]->source_type != Power::SOURCE_TYPE_ENEMY) ||
						(hazard->source_type != Power::SOURCE_TYPE_ENEMY && h[k]->source_type == Power::SOURCE_TYPE_ENEMY))
					{
						if (Utils::isWithinRadius(hazard->pos, hazard->power->radius, h[k]->pos)) {
							h[k]->lifespan = 0;
							// h[k]->remove_now = true;
						}
					}
				}
			}
		}
	}
}

void HazardManager::hitEntity(size_t index, const bool hit) {
	if (!hit) return;

	if (!h[index]->power->multitarget) {
		h[index]->active = false;
		if (!h[index]->power->complete_animation) h[index]->lifespan = 0;
	}
	if (h[index]->power->sfx_hit_enable && !h[index]->sfx_hit_played) {
		sim_events->pushSound(SimEvent::SFX_HAZARD_HIT, h[index]->power->sfx_hit, "", h[index]->pos);
		h[index]->sfx_hit_played = true;
	}

	if (h[index]->power->script_trigger == Power::SCRIPT_TRIGGER_HIT) {
		eventm->executeScript(h[index]->power->script, h[index]->pos.x, h[index]->pos.y);
	}
}

/**
 * Look for hazards generated this frame
 */
void HazardManager::checkNewHazards() {

	// check PowerManager for hazards
	while (!powers->hazards.empty()) {
		Hazard *new_haz = powers->hazards.front();
		powers->hazards.pop();

		h.push_back(new_haz);
	}
}

/**
 * Reset all hazards and get new collision object
 */
void HazardManager::handleNewMap() {
	for (unsigned int i = 0; i < h.size(); i++) {
		delete h[i];
	}
	h.clear();
	last_enemy = NULL;
}

/**
 * addRenders()
 * Map objects need to be drawn in Z order, so we allow a parent object (GameEngine)
 * to collect all mobile sprites each frame.
 */
void HazardManager::addRenders(std::vector<Renderable> &r, std::vector<Renderable> &r_dead) {
	for (unsigned int i=0; i<h.size(); i++) {
		if (mapr && wmap->collider.isOutsideMap(h[i]->pos.x, h[i]->pos.y))
			continue;

		h[i]->addRenderable(r, r_dead);
	}
}

HazardManager::~HazardManager() {
	Utils::logInfo("Cleaning up: HazardManager");

	for (unsigned int i = 0; i < h.size(); i++)
		delete h[i];
	// h.clear(); not needed in destructor
	last_enemy = NULL;
}


