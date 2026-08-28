/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
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

/**
 * class Avatar
 *
 * Contains logic and rendering routines for the player avatar.
 */

#include "Animation.h"
#include "AnimationManager.h"
#include "AnimationSet.h"
#include "Avatar.h"
#include "CommonIncludes.h"
#include "CursorManager.h"
#include "EnemyGroupManager.h"
#include "Entity.h"
#include "EntityManager.h"
#include "EngineSettings.h"
#include "FileParser.h"
#include "InputState.h"
#include "MapRenderer.h"
#include "MessageEngine.h"
#include "ModManager.h"
#include "PlayerInventory.h"
#include "PowerManager.h"
#include "RenderDevice.h"
#include "Rng.h"
#include "SaveLoad.h"
#include "Settings.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "SimEvents.h"
#include "SoundManager.h"
#include "Utils.h"
#include "UtilsFileSystem.h"
#include "UtilsMath.h"
#include "UtilsParsing.h"

static bool usePerPlayerCollision() {
	return playerm && playerm->players.size() > 1;
}

Avatar::Avatar()
	: Entity()
	, mm_key(settings->mouse_move_swap ? Input::MAIN2 : Input::MAIN1)
	, mm_is_distant(false)
	, path()
	, prev_target()
	, collided(false)
	, path_found(false)
	, chance_calc_path(0)
	, path_found_fails(0)
	, path_found_fail_timer()
	, mm_target(-1, -1)
	, mm_target_desired(-1, -1)
	, hero_stats(NULL)
	, charmed_stats(NULL)
	, act_target()
	, drag_walking(false)
	, respawn(false)
	, close_menus(false)
	, show_game_over(false)
	, respec_powers(false)
	, respec_use_engine_defaults(false)
	, allow_movement(true)
	, cursor_enemy(NULL)
	, lock_enemy(NULL)
	, time_played(0)
	, questlog_dismissed(false)
	, using_main1(false)
	, using_main2(false)
	, prev_hp(0)
	, playing_lowhp(false)
	, teleport_camera_lock(false)
	, feet_index(-1)
	, mm_target_object(MM_TARGET_NONE)
	, mm_target_object_pos()
	, block_xp_gain(false)
	, id(0)
{
	power_cooldown_timers.resize(powers->powers.size(), NULL);
	power_cast_timers.resize(powers->powers.size(), NULL);

	for (size_t i = 0; i < powers->powers.size(); ++i) {
		if (powers->isValid(i)) {
			power_cooldown_ids.push_back(i);
			power_cooldown_timers[i] = new Timer();
			power_cast_timers[i] = new Timer();
		}
	}

	init();

	loadLayerDefinitions();

	// load foot-step definitions
	// @CLASS Avatar: Step sounds|Description of items/step_sounds.txt
	Step_sfx temp;
	Step_sfx* current = &temp;

	FileParser infile;
	if (infile.open("items/step_sounds.txt", FileParser::MOD_FILE, FileParser::ERROR_NONE)) {
		while (infile.next()) {
			// if we want to replace a list item by ID, the ID needs to be parsed first
			// but it is not essential if we're just adding to the list, so this is simply a warning
			if (infile.key != "id" && current->id.empty()) {
				infile.error("Avatar: Expected 'id', but found '%s'.", infile.key.c_str());
			}

			if (infile.key == "id") {
				// @ATTR id|string|An identifier name for a set of step sounds.
				bool found_id = false;
				for (size_t i = 0; i < step_def.size(); ++i) {
					if (step_def[i].id == infile.val) {
						step_def[i] = Step_sfx();
						current = &step_def[i];
						current->id = infile.val;
						found_id = true;
					}
				}
				if (!found_id) {
					step_def.push_back(temp);
					current = &(step_def.back());
					current->id = infile.val;
				}
			}

			if (infile.key == "step") {
				// @ATTR step|filename|Filename of a step sound effect.
				current->steps.push_back(infile.val);
			}
		}
		infile.close();
	}

	loadStepFX(stats.sfx_step);
}

void Avatar::init() {

	// name, base, look are set by GameStateNew so don't reset it here

	// other init
	sprites = 0;
	stats.cur_state = StatBlock::ENTITY_STANCE;
	if (wmap->hero_pos_enabled) {
		stats.pos.x = wmap->hero_pos.x;
		stats.pos.y = wmap->hero_pos.y;
	}
	current_power = 0;
	current_power_original = 0;
	newLevelNotification = false;

	stats.hero = true;
	stats.humanoid = true;
	stats.level = 1;
	stats.xp = 0;
	for (size_t i = 0; i < eset->primary_stats.list.size(); ++i) {
		stats.primary[i] = stats.primary_starting[i] = 1;
		stats.primary_additional[i] = 0;
	}
	stats.speed = 0.2f;
	stats.recalc();

	while (!log_msg.empty()) {
		log_msg.pop();
	}
	respawn = false;

	stats.cooldown.reset(Timer::END);

	body = -1;

	transform_triggered = false;
	setPowers = false;
	revertPowers = false;
	last_transform = "";

	// Find untransform power index to use for manual untransfrom ability
	untransform_power = 0;
	for (size_t i = 0; i < powers->powers.size(); ++i) {
		if (!powers->isValid(i))
			continue;

		if (untransform_power == 0 && powers->powers[i]->required_items.empty() && powers->powers[i]->spawn_type == "untransform") {
			untransform_power = i;
		}

		if (power_cooldown_timers[i])
			*(power_cooldown_timers[i]) = Timer();
		if (power_cast_timers[i])
			*(power_cast_timers[i]) = Timer();
	}

	stats.animations = "animations/hero.txt";
}

void Avatar::handleNewMap() {
	cursor_enemy = NULL;
	lock_enemy = NULL;
	playing_lowhp = false;

	stats.target_corpse = NULL;
	stats.target_nearest = NULL;
	stats.target_nearest_corpse = NULL;

	path.clear();
	mm_target_desired = stats.pos;
	mm_target_object_pos = stats.pos;

	mm_target_object = MM_TARGET_NONE;
}

/**
 * Load avatar sprite layer definitions into vector.
 */
void Avatar::loadLayerDefinitions() {
	if (!stats.layer_reference_order.empty())
		return;

	Utils::logError("Avatar: Loading render layers from engine/hero_layers.txt is deprecated! Render layers should be loaded in the 'render_layers' section of engine/stats.txt.");

	FileParser infile;
	if (infile.open("engine/hero_layers.txt", FileParser::MOD_FILE, FileParser::ERROR_NORMAL)) {
		while(infile.next()) {
			// hero_layers.txt doesn't have sections, but we now expect the layer key to be under one called "render_layers"
			if (infile.section.empty())
				infile.section = "render_layers";

			if (!stats.loadRenderLayerStat(&infile)) {
				infile.error("Avatar: '%s' is not a valid key.", infile.key.c_str());
			}
		}
		infile.close();
	}
}

/**
 * Walking/running steps sound depends on worn armor
 */
void Avatar::loadStepFX(const std::string& stepname) {
	std::string filename = stats.sfx_step;
	if (stepname != "") {
		filename = stepname;
	}

	// clear previous sounds
	for (unsigned i=0; i<sound_steps.size(); i++) {
		snd->unload(sound_steps[i]);
	}
	sound_steps.clear();

	if (filename == "") return;

	// A literal "NULL" means we don't want to load any new sounds
	// This is used when transforming, since creatures don't have step sound effects
	if (stepname == "NULL") return;

	// load new sounds
	for (unsigned i=0; i<step_def.size(); i++) {
		if (step_def[i].id == filename) {
			sound_steps.resize(step_def[i].steps.size());
			for (unsigned j=0; j<sound_steps.size(); j++) {
				sound_steps[j] = snd->load(step_def[i].steps[j], "Avatar loading foot steps");
			}
			return;
		}
	}

	// Could not find step sound fx
	Utils::logError("Avatar: Could not find footstep sounds for '%s'.", filename.c_str());
}


bool Avatar::pressing_move(const PlayerCommand& cmd) {
	if (!allow_movement || teleport_camera_lock) {
		return false;
	}
	else if (stats.effects.knockback_speed != 0) {
		return false;
	}
	else if (settings->mouse_move) {
		return mm_is_distant && !isNearMMtarget();
	}
	else {
		return cmd.move_up || cmd.move_down || cmd.move_left || cmd.move_right;
	}
}

void Avatar::set_direction(const PlayerCommand& cmd, PlayerInputLocks& locks) {
	if (teleport_camera_lock || !set_dir_timer.isEnd())
		return;

	int old_dir = stats.direction;

	// handle direction changes
	if (settings->mouse_move) {
		if (mm_is_distant) {
			if (cmd.mm_pressed && (!locks.lock[mm_key] || drag_walking)) {
				FPoint mm_target_test = cmd.mm_map_target;
				if (wmap->collider.isValidPosition(mm_target_test.x, mm_target_test.y, stats.movement_type, MapCollision::COLLIDE_TYPE_HERO)) {
					locks.lock[mm_key] = true;
					mm_target_desired = mm_target_test;
				}
			}

			mm_target = mm_target_desired;

			// if blocked, face in pathfinder direction instead
			if (collided || !wmap->collider.lineOfMovement(stats.pos.x, stats.pos.y, mm_target.x, mm_target.y, stats.movement_type)) {

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
				if (!recalculate_path && Utils::calcDist(FPoint(Point(prev_target)), FPoint(Point(mm_target))) > 1.f)
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

				prev_target = mm_target;

				// target first waypoint
				if (recalculate_path) {
					chance_calc_path = -100;
					path.clear();
					path_found = wmap->collider.computePath(stats.pos, mm_target, path, stats.movement_type, MapCollision::DEFAULT_PATH_LIMIT);

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
					mm_target = path.back();

					// if distance to node is lower than a tile size, the node is going to be passed and can be removed
					if (Utils::calcDist(stats.pos, mm_target) <= 1.f)
						path.pop_back();
				}
			}
			else {
				path.clear();
			}

			stats.direction = Utils::calcDirection(stats.pos.x, stats.pos.y, mm_target.x, mm_target.y);
		}
	}
	else {
		// movement keys take top priority for setting direction
		bool press_up = cmd.move_up;
		bool press_down = cmd.move_down;
		bool press_left = cmd.move_left;
		bool press_right = cmd.move_right;

		// aiming keys can set direction as well
		if (!press_up && !press_down && !press_left && !press_right) {
			press_up = cmd.aim_up;
			press_down = cmd.aim_down;
			press_left = cmd.aim_left;
			press_right = cmd.aim_right;
		}

		if (press_up && press_left) stats.direction = 1;
		else if (press_up && press_right) stats.direction = 3;
		else if (press_down && press_right) stats.direction = 5;
		else if (press_down && press_left) stats.direction = 7;
		else if (press_left) stats.direction = 0;
		else if (press_up) stats.direction = 2;
		else if (press_right) stats.direction = 4;
		else if (press_down) stats.direction = 6;
		// Adjust for ORTHO tilesets
		if (eset->tileset.orientation == eset->tileset.TILESET_ORTHOGONAL && (press_up || press_down || press_left || press_right))
			stats.direction = static_cast<unsigned char>((stats.direction == 7) ? 0 : stats.direction + 1);
	}
	if (settings->mouse_move) {
		// when using mouse move, we use a longer turn delay to prevent the movement from being "jittery" with too many direction changes
		// however, we lower this delay if the player can't reach their destination before the next direction check
		// this keeps the player's movement smooth for most of the journey towards a given point, although there is still some jitter as they near their destination.
		int delay_ticks = Settings::SIM_TICK_HZ / 2;

		float real_speed = stats.speed * StatBlock::SPEED_MULTIPLIER[stats.direction] * stats.effects.speed / 100;
		// we multiply by 0.5 here because when we don't, the player tends to turn 180 degrees for brief moments
		int max_turn_ticks = static_cast<int>(Utils::calcDist(stats.pos, mm_target) * 0.5f / real_speed);
		if (delay_ticks > max_turn_ticks) {
			set_dir_timer.setDuration(max_turn_ticks);
		}
		else {
			set_dir_timer.setDuration(delay_ticks);
		}
	}
	else {
		// give direction changing a 100ms cooldown
		// this allows the player to quickly change direction on their own without becoming overly "jittery"
		// the cooldown can be ended by releasing the move button, but the cooldown is so fast that it doesn't matter much (maybe a speed run tactic?)
		if (stats.direction != old_dir) {
			set_dir_timer.setDuration(Settings::SIM_TICK_HZ / 10);
		}
	}
}

/**
 * logic()
 * Handle a single frame.  This includes:
 * - move the avatar based on buttons pressed
 * - calculate the next frame of animation
 * - calculate camera position based on avatar position
 */
void Avatar::logic(const PlayerCommand& cmd, PlayerInputLocks& locks) {
	bool restrict_power_use = false;
	if (settings->mouse_move) {
		if(cmd.mm_pressed && !cmd.shift && !cmd.click_consumed_by_ui) {
			restrict_power_use = true;
		}
	}

	// clear current space to allow correct movement
	if (usePerPlayerCollision())
		wmap->collider.unblockPlayer(stats.pos.x, stats.pos.y, id);
	else
		wmap->collider.unblock(stats.pos.x, stats.pos.y);

	// turn on all passive powers
	if ((stats.hp > 0 || stats.effects.triggered_death) && !respawn && !transform_triggered)
		powers->activatePassives(&stats);

	if (transform_triggered)
		transform_triggered = false;

	// handle when the player stops blocking
	if (stats.effects.triggered_block && !stats.blocking) {
		stats.cur_state = StatBlock::ENTITY_STANCE;
		stats.effects.triggered_block = false;
		stats.effects.clearTriggerEffects(Power::TRIGGER_BLOCK);
		stats.refresh_stats = true;
		stats.block_power = 0;
	}

	stats.logic();

	// alert on low health
	if (isDroppedToLowHp()) {
		// show message if set
		if (isLowHpMessageEnabled()) {
			logMsg(msg->get("Your health is low!"), MSG_NORMAL);
		}
		// play a sound if set in settings
		if (isLowHpSoundEnabled() && !playing_lowhp) {
			// if looping, then do not cleanup
			SimEvent e(SimEvent::SFX_LOWHP_START);
			e.candidates.push_back(sound_lowhp);
			e.channel = "lowhp";
			e.loop = stats.sfx_lowhp_loop;
			e.cleanup = !stats.sfx_lowhp_loop;
			sim_events->push(e);
			playing_lowhp = true;
		}
	}
	// if looping, stop sounds when HP recovered above threshold
	if (isLowHpSoundEnabled() && !isLowHp() && playing_lowhp && stats.sfx_lowhp_loop) {
		sim_events->pushStop(SimEvent::SFX_LOWHP_STOP, "lowhp");
		playing_lowhp = false;
	}
	else if (isLowHpSoundEnabled() && isLowHp() && !playing_lowhp && stats.sfx_lowhp_loop) {
		SimEvent e(SimEvent::SFX_LOWHP_START);
		e.candidates.push_back(sound_lowhp);
		e.channel = "lowhp";
		e.loop = stats.sfx_lowhp_loop;
		e.cleanup = !stats.sfx_lowhp_loop;
		sim_events->push(e);
		playing_lowhp = true;
	}
	else if (!isLowHpSoundEnabled() && playing_lowhp) {
		sim_events->pushStop(SimEvent::SFX_LOWHP_STOP, "lowhp");
		playing_lowhp = false;
	}

	// we can not use stats.prev_hp here
	prev_hp = stats.hp;

	// check level up
	if (stats.level < eset->xp.getMaxLevel() && stats.xp >= eset->xp.getLevelXP(stats.level + 1)) {
		stats.level_up = true;
		stats.level = eset->xp.getLevelFromXP(stats.xp);
		logMsg(msg->getv("Congratulations, you have reached level %d!", stats.level), MSG_NORMAL);
		// P2.3b: this used to read the stat_points_per_level/power_points_per_level fields off
		// the global player's stats -- a copy of a per-avatar field being checked on THIS avatar's
		// own level-up, one line above. This avatar's own stats (already a member, no lookup
		// needed) is the right read regardless of which avatar this is (kind A: the specific
		// player already in scope, via "this").
		if (stats.stat_points_per_level > 0) {
			logMsg(msg->get("You may increase one or more attributes through the Character Menu."), MSG_NORMAL);
			newLevelNotification = true;
		}
		if (stats.power_points_per_level > 0) {
			logMsg(msg->get("You may unlock one or more abilities through the Powers Menu."), MSG_NORMAL);
		}
		stats.recalc();
		SimEvent e(SimEvent::SFX_LEVELUP);
		e.candidates.push_back(sound_levelup);
		sim_events->push(e);

		// if the player managed to level up while dead (e.g. via a bleeding creature), restore to life
		if (stats.cur_state == StatBlock::ENTITY_DEAD) {
			stats.cur_state = StatBlock::ENTITY_STANCE;
		}
	}

	// we have a window of 1 frame to check if the player leveled up when dying. Once it passes, we block gaining any additional XP.
	block_xp_gain = (stats.hp == 0);

	// assist mouse movement
	mm_key = settings->mouse_move_swap ? Input::MAIN2 : Input::MAIN1;
	if (!cmd.mm_pressed) {
		drag_walking = false;
	}

	// block some interactions when attacking/moving
	using_main1 = cmd.main1_active;
	using_main2 = cmd.main2_active;

	// handle animation
	if (!stats.effects.stun && !stats.hold_state) {
		if (activeAnimation)
			activeAnimation->advanceFrame();

		for (size_t i = 0; i < anims.size(); ++i) {
			if (anims[i])
				anims[i]->advanceFrame();
		}
	}

	// save a valid tile position in the event that we untransform on an invalid tile
	if (stats.transformed && wmap->collider.isValidPosition(stats.pos.x, stats.pos.y, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_HERO)) {
		transform_pos = stats.pos;
		transform_map = wmap->getFilename();
	}

	if (settings->mouse_move) {
		if (cmd.mm_pressed) {
			// prevents erratic behavior when mouse move is too close to player
			FPoint target = cmd.mm_map_target;
			if (stats.cur_state == StatBlock::ENTITY_MOVE) {
				mm_is_distant = Utils::calcDist(stats.pos, target) >= eset->misc.mouse_move_deadzone_moving;
			}
			else {
				mm_is_distant = Utils::calcDist(stats.pos, target) >= eset->misc.mouse_move_deadzone_not_moving;
			}

			if (!locks.lock[mm_key]) {
				if (settings->mouse_move_attack && cursor_enemy && !cursor_enemy->stats.hero_ally) {
					locks.lock[mm_key] = true;
					lock_enemy = cursor_enemy;
					mm_target_object = MM_TARGET_ENTITY;
				}

				if (!cursor_enemy) {
					lock_enemy = NULL;
					if (mm_target_object == MM_TARGET_ENTITY)
						mm_target_object = MM_TARGET_NONE;
				}
			}
		}

		if (lock_enemy) {
			if (lock_enemy->stats.hp <= 0) {
				lock_enemy = NULL;
				mm_target_object = MM_TARGET_NONE;
			}
			else {
				mm_target_object_pos = lock_enemy->stats.pos;
				setDesiredMMTarget(mm_target_object_pos);
			}
		}
	}

	// mapr is NULL on a headless server (P1.4c) -- there is no camera to wait for, so the lock
	// releases immediately rather than never, which would permanently block movement/direction
	// changes gated on this same flag a few lines above and below (allow_movement,
	// set_dir_timer). Found the way every other menu/mapr crash in this plan was: this flag gets
	// set true by checkTeleport()'s port on tick 1, so tick 2's Avatar::logic() is the first
	// unconditional dereference to actually run, not tick 1's.
	if (teleport_camera_lock && (!mapr || Utils::calcDist(stats.pos, mapr->cam.pos) < 0.5f)) {
		teleport_camera_lock = false;
	}

	set_dir_timer.tick();
	if (!pressing_move(cmd)) {
		set_dir_timer.reset(Timer::END);
	}

	if (!stats.effects.stun) {
		bool allowed_to_move;
		bool allowed_to_turn;

		stats.blocking = false;

		for (unsigned i=0; i<action_queue.size(); i++) {
			ActionData &action = action_queue[i];
			PowerID replaced_id = powers->checkReplaceByEffect(action.power, &stats);
			if (replaced_id == 0)
				continue;

			Power* power = powers->powers[replaced_id];

			if (power->new_state == Power::STATE_INSTANT) {
				// instant power, so no need to switch animation state
				FPoint target = action.target;
				beginPower(cmd, replaced_id, &target);
				powers->activate(replaced_id, &stats, stats.pos, target);
				power_cooldown_timers[action.power]->setDuration(power->cooldown);
				power_cooldown_timers[replaced_id]->setDuration(power->cooldown);
			}
			else if (stats.cur_state == StatBlock::ENTITY_BLOCK) {
				// special case in order to continue blocking
				if (power->type == Power::TYPE_BLOCK) {
					current_power = replaced_id;
					current_power_original = action.power;
					act_target = action.target;
					attack_anim = power->attack_anim;

					stats.cur_state = StatBlock::ENTITY_BLOCK;
					beginPower(cmd, replaced_id, &act_target);
					powers->activate(replaced_id, &stats, stats.pos, act_target);
					stats.refresh_stats = true;
				}
			}
			else if (stats.cur_state == StatBlock::ENTITY_STANCE || stats.cur_state == StatBlock::ENTITY_MOVE) {
				// this power has an animation, so prepare to switch to it
				current_power = replaced_id;
				current_power_original = action.power;
				act_target = action.target;
				attack_anim = power->attack_anim;
				resetActiveAnimation();

				if (power->new_state == Power::STATE_ATTACK) {
					stats.cur_state = StatBlock::ENTITY_POWER;
				}
				else if (power->type == Power::TYPE_BLOCK) {
					stats.cur_state = StatBlock::ENTITY_BLOCK;
					beginPower(cmd, replaced_id, &act_target);
					powers->activate(replaced_id, &stats, stats.pos, act_target);
					stats.refresh_stats = true;
				}
			}
		}

		action_queue.clear();

		switch(stats.cur_state) {
			case StatBlock::ENTITY_STANCE:

				setAnimation("stance");

				// allowed to move or use powers?
				if (settings->mouse_move) {
					allowed_to_move = restrict_power_use && (!locks.lock[mm_key] || drag_walking);
					allowed_to_turn = allowed_to_move;

					if ((cmd.mm_pressed && cmd.shift)) {
						locks.lock[mm_key] = false;
					}
				}
				else if (!settings->mouse_aim) {
					allowed_to_move = !cmd.shift;
					allowed_to_turn = true;
				}
				else {
					allowed_to_move = true;
					allowed_to_turn = true;
				}

				// handle transitions to RUN
				if (allowed_to_turn)
					set_direction(cmd, locks);

				if (pressing_move(cmd) && allowed_to_move) {
					if (move()) { // no collision
						if (settings->mouse_move && cmd.mm_pressed) {
							drag_walking = true;
						}

						stats.cur_state = StatBlock::ENTITY_MOVE;

						mm_target_object = MM_TARGET_NONE;
					}
				}

				break;

			case StatBlock::ENTITY_MOVE:

				setAnimation("run");

				if (!sound_steps.empty() && (activeAnimation->isFirstFrame() || activeAnimation->isActiveFrame())) {
					SimEvent e(SimEvent::SFX_STEP);
					e.candidates = sound_steps;
					e.select = true;
					sim_events->push(e);
				}

				// handle direction changes
				set_direction(cmd, locks);

				// handle transition to STANCE
				if (!pressing_move(cmd)) {
					stats.cur_state = StatBlock::ENTITY_STANCE;
					break;
				}
				else if (!move()) { // collide with wall
					if (settings->mouse_move && !isNearMMtarget()) {
						collided = true;
					}
					stats.cur_state = StatBlock::ENTITY_STANCE;
					break;
				}
				else if ((settings->mouse_move || !settings->mouse_aim) && cmd.shift) {
					// Shift should stop movement in some cases.
					// With mouse_move, it allows the player to stop moving and begin attacking.
					// With mouse_aim disabled, it allows the player to aim their attacks without having to move.
					stats.cur_state = StatBlock::ENTITY_STANCE;
					break;
				}

				if (settings->mouse_move && cmd.mm_pressed && (drag_walking || !locks.lock[mm_key])) {
					drag_walking = true;
				}

				if (activeAnimation->getName() != "run")
					stats.cur_state = StatBlock::ENTITY_STANCE;

				break;

			case StatBlock::ENTITY_POWER:

				setAnimation(attack_anim);

				if (powers->isValid(current_power)) {
					Power* power = powers->powers[current_power];

					// if the player is attacking, show the attack cursor
					if (!power->buff && !power->buff_teleport &&
						power->type != Power::TYPE_TRANSFORM &&
						power->type != Power::TYPE_BLOCK &&
						!(power->starting_pos == Power::STARTING_POS_SOURCE && power->speed == 0)
					) {
						curs->setCursor(CursorManager::CURSOR_ATTACK);
					}

					if (activeAnimation->isFirstFrame()) {
						beginPower(cmd, current_power, &act_target);
						float attack_speed = (stats.effects.getAttackSpeed(attack_anim) * power->attack_speed) / 100.0f;
						activeAnimation->setSpeed(attack_speed);
						for (size_t i=0; i<anims.size(); ++i) {
							if (anims[i])
								anims[i]->setSpeed(attack_speed);
						}
						playAttackSound(attack_anim);
						power_cast_timers[current_power]->setDuration(activeAnimation->getDuration());
						power_cast_timers[current_power_original]->setDuration(activeAnimation->getDuration()); // for replace_by_effect
					}

					if (power->state_hold_mode == Power::HOLD_ON_FRAME) {
						if (activeAnimation->isFrame(power->state_hold_frame) && !stats.hold_state) {
							if (!stats.state_timer.isEnd())
								stats.hold_state = true;
						}
						if (stats.state_timer.isEnd())
							stats.hold_state = false;
					}

					// do power
					if (activeAnimation->isActiveFrame() && !stats.hold_state) {
						// some powers check if the caster is blocking a tile
						// so we block the player tile prematurely here
						if (usePerPlayerCollision())
							wmap->collider.blockPlayer(stats.pos.x, stats.pos.y, id);
						else
							wmap->collider.block(stats.pos.x, stats.pos.y, !MapCollision::IS_ALLY);

						powers->activate(current_power, &stats, stats.pos, act_target);
						power_cooldown_timers[current_power]->setDuration(power->cooldown);
						power_cooldown_timers[current_power_original]->setDuration(power->cooldown); // for replace_by_effect

						if (power->state_hold_mode == Power::HOLD_ON_ACTIVE_FRAME && !stats.state_timer.isEnd())
							stats.hold_state = true;
					}
				}

				// animation is done, switch back to normal stance
				if ((activeAnimation->isLastFrame() && stats.state_timer.isEnd()) || activeAnimation->getName() != attack_anim) {
					stats.cur_state = StatBlock::ENTITY_STANCE;
					stats.cooldown.reset(Timer::BEGIN);
					stats.prevent_interrupt = false;
				}

				break;

			case StatBlock::ENTITY_BLOCK:

				setAnimation("block");

				break;

			case StatBlock::ENTITY_HIT:

				setAnimation("hit");

				if (activeAnimation->isFirstFrame()) {
					stats.effects.triggered_hit = true;

					if (powers->isValid(stats.block_power)) {
						power_cooldown_timers[stats.block_power]->setDuration(powers->powers[stats.block_power]->cooldown);
						stats.block_power = 0;
					}
				}

				if (activeAnimation->getTimesPlayed() >= 1 || activeAnimation->getName() != "hit") {
					stats.cur_state = StatBlock::ENTITY_STANCE;
				}

				break;

			case StatBlock::ENTITY_DEAD:
				if (stats.effects.triggered_death) break;

				if (stats.transformed) {
					stats.transform_duration = 0;
					untransform(locks);
				}

				setAnimation("die");

				if (!stats.corpse && activeAnimation->isFirstFrame() && activeAnimation->getTimesPlayed() < 1) {
					stats.effects.clearEffects();
					stats.powers_passive.clear();

					// reset power cooldowns
					std::map<size_t, Timer>::iterator pct_it;
					for (size_t i = 0; i < power_cooldown_timers.size(); ++i) {
						if (power_cooldown_timers[i])
							power_cooldown_timers[i]->reset(Timer::END);
						if (power_cast_timers[i])
							power_cast_timers[i]->reset(Timer::END);
					}

					// close menus in GameStatePlay
					close_menus = true;

					playSound(Entity::SOUND_DIE);

					logMsg(msg->get("You are defeated."), MSG_NORMAL);

					if (stats.permadeath) {
						// ignore death penalty on permadeath and instead delete the player's saved game.
						// GameStatePlay disables the save UI for permadeath at the same close_menus
						// boundary that already closes menus on death (GameStatePlay.cpp).
						stats.death_penalty = false;
						Utils::removeSaveDir(save_load->getGameSlot());
					}
					else {
						// raise the death penalty flag.  This is handled in MenuInventory
						stats.death_penalty = true;
					}

					// if the player is attacking, we need to block further input
					if (cmd.main1_pressed)
						locks.lock[Input::MAIN1] = true;
				}

				if (!stats.corpse && (activeAnimation->getTimesPlayed() >= 1 || activeAnimation->getName() != "die")) {
					stats.corpse = true;
					// GameStatePlay shows the game-over menu (GameStatePlay.cpp) when it sees this.
					show_game_over = true;
				}

				// allow respawn with Accept if not permadeath
				//
				// The command carries this now. It used to be a direct read of the game-over
				// menu's continue flag, which made a simulation state transition wait on a UI
				// button; GameStatePlay resolves and consumes the click at the boundary. Phase 3
				// sets cmd.respawn from the network and nothing here changes.
				//
				// Named without the arrow on purpose: the acceptance greps for menu access count
				// comments as well as code, which is how a guard avoids rotting into decoration.
				if (cmd.respawn) {
					if (stats.permadeath) {
						wmap->teleportation = true;
						wmap->teleport_mapname = wmap->respawn_map;

						// set these positions so it doesn't flash before jumping to Title
						wmap->teleport_destination.x = stats.pos.x;
						wmap->teleport_destination.y = stats.pos.y;
					}
					else if (usePerPlayerCollision()) {
						// D21: respawn near a living party member, after the delay the death
						// animation already imposes (see the corpse/game-over gate above this
						// state), and local to the current map -- NOT the single-player intermap
						// teleport below, which under D12 (one shared map) would drag the whole
						// party back to an old checkpoint just because one member died. Falls
						// back to the map's own entry point (hero_pos) if no other party member
						// is alive.
						respawn = true;

						FPoint respawn_pos = wmap->hero_pos;
						for (size_t i = 0; i < playerm->players.size(); ++i) {
							Avatar* member = playerm->players[i];
							if (member != this && member->stats.alive && !member->stats.corpse) {
								respawn_pos = wmap->collider.getRandomNeighbor(Point(member->stats.pos), 1, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_HERO);
								break;
							}
						}

						stats.pos = respawn_pos;
					}
					else {
						wmap->teleportation = true;
						wmap->teleport_mapname = wmap->respawn_map;

						respawn = true;

						// set teleportation variables.  GameEngine acts on these.
						wmap->teleport_destination.x = wmap->respawn_point.x;
						wmap->teleport_destination.y = wmap->respawn_point.y;
					}
				}

				break;

			default:
				break;
		}
	}

	// Camera::setTarget() has no sim consequence -- it only records where the viewport should
	// smoothly scroll to. Moved to GameStatePlay::logic() (P1.4d), which is the only caller
	// that ever had a real mapr to update; that keeps Avatar.cpp free of MapRenderer entirely.

	// check for map events
	wmap->checkEvents(stats.pos);

	// decrement all cooldowns
	for (size_t i = 0; i < power_cooldown_ids.size(); ++i) {
		PowerID power_id = power_cooldown_ids[i];
		power_cooldown_timers[power_id]->tick();
		power_cast_timers[power_id]->tick();
	}

	// make the current square solid
	if (usePerPlayerCollision())
		wmap->collider.blockPlayer(stats.pos.x, stats.pos.y, id);
	else
		wmap->collider.block(stats.pos.x, stats.pos.y, !MapCollision::IS_ALLY);

	if (stats.state_timer.isEnd() && stats.hold_state)
		stats.hold_state = false;

	if ((stats.cur_state != StatBlock::ENTITY_POWER || (stats.charge_timer.getDuration() > 0 && stats.charge_timer.isEnd())) && stats.charge_speed != 0.0f) {
		stats.charge_speed = 0.0f;
		stats.charge_timer.reset(Timer::END);
	}
}

void Avatar::beginPower(const PlayerCommand& cmd, PowerID replaced_id, FPoint* target) {
	if (!target)
		return;

	const Power* power = powers->powers[replaced_id];

	if (power->type == Power::TYPE_BLOCK)
		stats.blocking = true;

	// automatically target the selected enemy with melee attacks
	if (cmd.using_mouse && cursor_enemy && !cmd.shift) {
		*target = cursor_enemy->stats.pos;
	}

	// is this a power that requires changing direction?
	if (power->face) {
		stats.direction = Utils::calcDirection(stats.pos.x, stats.pos.y, target->x, target->y);
	}

	if (power->state_duration > 0)
		stats.state_timer.setDuration(power->state_duration);

	if (power->charge_speed != 0.0f) {
		stats.charge_speed = power->charge_speed;

		if (power->charge_duration > 0) {
			stats.charge_timer.setDuration(power->charge_duration);
		}
		else {
			stats.charge_timer.setDuration(0);
		}
	}

	stats.prevent_interrupt = power->prevent_interrupt;

	for (size_t j = 0; j < power->chain_powers.size(); ++j) {
		const ChainPower& chain_power = power->chain_powers[j];
		if (chain_power.type == ChainPower::TYPE_PRE && sim_rng->percentChanceF(chain_power.chance)) {
			powers->activate(chain_power.id, &stats, stats.pos, *target);
		}
	}
}

void Avatar::transform(PlayerInputLocks& locks) {
	// dead players can't transform
	if (stats.hp <= 0)
		return;

	// calling a transform power locks the actionbar, so we unlock it here
	locks.unlockActionBar();

	delete charmed_stats;
	charmed_stats = NULL;

	Enemy_Level el = enemyg->getRandomEnemy(stats.transform_type, 0, 0);

	if (el.type != "") {
		charmed_stats = new StatBlock();
		charmed_stats->load(el.type);
	}
	else {
		Utils::logError("Avatar: Could not transform into creature type '%s'", stats.transform_type.c_str());
		stats.transform_type = "";
		return;
	}

	transform_triggered = true;
	stats.transformed = true;
	setPowers = true;

	// temporary save hero stats
	delete hero_stats;

	hero_stats = new StatBlock();
	*hero_stats = stats;

	// do not allow two copies of the summons list
	hero_stats->summons.clear();

	// replace some hero stats
	stats.speed = charmed_stats->speed;
	stats.movement_type = charmed_stats->movement_type;
	stats.humanoid = charmed_stats->humanoid;
	stats.animations = charmed_stats->animations;
	stats.powers_list = charmed_stats->powers_list;
	stats.powers_passive = charmed_stats->powers_passive;
	stats.effects.clearEffects();
	stats.animations = charmed_stats->animations;
	stats.layer_reference_order = charmed_stats->layer_reference_order;
	stats.layer_def = charmed_stats->layer_def;
	stats.animation_slots = charmed_stats->animation_slots;

	anim->decreaseCount(hero_stats->animations);
	animationSet = NULL;
	loadAnimations();
	stats.cur_state = StatBlock::ENTITY_STANCE;

	// base stats
	for (int i=0; i<Stats::COUNT; ++i) {
		stats.starting[i] = std::max(stats.starting[i], charmed_stats->starting[i]);
	}

	loadSoundsFromStatBlock(charmed_stats);
	loadStepFX("NULL");

	stats.applyEffects();

	transform_pos = stats.pos;
	transform_map = wmap->getFilename();
}

void Avatar::untransform(PlayerInputLocks& locks) {
	// calling a transform power locks the actionbar, so we unlock it here
	locks.unlockActionBar();

	// For timed transformations, move the player to the last valid tile when untransforming
	if (usePerPlayerCollision())
		wmap->collider.unblockPlayer(stats.pos.x, stats.pos.y, id);
	else
		wmap->collider.unblock(stats.pos.x, stats.pos.y);
	if (!wmap->collider.isValidPosition(stats.pos.x, stats.pos.y, MapCollision::MOVE_NORMAL, MapCollision::COLLIDE_TYPE_HERO)) {
		logMsg(msg->get("Transformation expired. You have been moved back to a safe place."), MSG_NORMAL);
		if (transform_map != wmap->getFilename()) {
			wmap->teleportation = true;
			wmap->teleport_mapname = transform_map;
			wmap->teleport_destination.x = floorf(transform_pos.x) + 0.5f;
			wmap->teleport_destination.y = floorf(transform_pos.y) + 0.5f;
			transform_map = "";
		}
		else {
			stats.pos.x = floorf(transform_pos.x) + 0.5f;
			stats.pos.y = floorf(transform_pos.y) + 0.5f;
		}
	}
	if (usePerPlayerCollision())
		wmap->collider.blockPlayer(stats.pos.x, stats.pos.y, id);
	else
		wmap->collider.block(stats.pos.x, stats.pos.y, !MapCollision::IS_ALLY);

	stats.transformed = false;
	transform_triggered = true;
	stats.transform_type = "";
	revertPowers = true;
	stats.effects.clearEffects();

	// revert some hero stats to last saved
	stats.speed = hero_stats->speed;
	stats.movement_type = hero_stats->movement_type;
	stats.humanoid = hero_stats->humanoid;
	stats.animations = hero_stats->animations;
	stats.effects = hero_stats->effects;
	stats.powers_list = hero_stats->powers_list;
	stats.powers_passive = hero_stats->powers_passive;
	stats.animations = hero_stats->animations;
	stats.layer_reference_order = hero_stats->layer_reference_order;
	stats.layer_def = hero_stats->layer_def;
	stats.animation_slots = hero_stats->animation_slots;

	anim->decreaseCount(charmed_stats->animations);
	animationSet = NULL;
	loadAnimations();
	stats.cur_state = StatBlock::ENTITY_STANCE;

	// This is a bit of a hack.
	// In order to switch to the stance animation, we can't already be in a stance animation
	setAnimation("run");

	for (int i=0; i<Stats::COUNT; ++i) {
		stats.starting[i] = hero_stats->starting[i];
	}

	loadSounds();
	loadStepFX(stats.sfx_step);

	delete charmed_stats;
	delete hero_stats;
	charmed_stats = NULL;
	hero_stats = NULL;

	stats.applyEffects();
	stats.untransform_on_hit = false;
}

void Avatar::checkTransform(PlayerInputLocks& locks) {
	// handle transformation
	if (stats.transform_type != "" && stats.transform_type != "untransform" && stats.transformed == false)
		transform(locks);
	if (stats.transform_type != "" && stats.transform_duration == 0)
		untransform(locks);
}

void Avatar::logMsg(const std::string& str, int type) {
	log_msg.push(std::pair<std::string, int>(str, type));
}

// isLowHp returns true if health is below set threshold
bool Avatar::isLowHp() {
	if (stats.hp == 0)
		return false;
	float hp_one_perc = std::max(stats.get(Stats::HP_MAX), 1.f) / 100.0f;
	return stats.hp/hp_one_perc < static_cast<float>(settings->low_hp_threshold);
}

// isDroppedToLowHp returns true only if player hp just dropped below threshold
bool Avatar::isDroppedToLowHp() {
	float hp_one_perc = std::max(stats.get(Stats::HP_MAX), 1.f) / 100.0f;
	return (stats.hp/hp_one_perc < static_cast<float>(settings->low_hp_threshold)) && (prev_hp/hp_one_perc >= static_cast<float>(settings->low_hp_threshold));
}

bool Avatar::isLowHpMessageEnabled() {
	return settings->low_hp_warning_type == settings->LHP_WARN_TEXT ||
		settings->low_hp_warning_type == settings->LHP_WARN_TEXT_CURSOR ||
		settings->low_hp_warning_type == settings->LHP_WARN_TEXT_SOUND ||
		settings->low_hp_warning_type == settings->LHP_WARN_ALL;
}

bool Avatar::isLowHpSoundEnabled() {
	return settings->low_hp_warning_type == settings->LHP_WARN_SOUND ||
		settings->low_hp_warning_type == settings->LHP_WARN_TEXT_SOUND ||
		settings->low_hp_warning_type == settings->LHP_WARN_CURSOR_SOUND ||
		settings->low_hp_warning_type == settings->LHP_WARN_ALL;
}

bool Avatar::isLowHpCursorEnabled() {
	return settings->low_hp_warning_type == settings->LHP_WARN_CURSOR ||
		settings->low_hp_warning_type == settings->LHP_WARN_TEXT_CURSOR ||
		settings->low_hp_warning_type == settings->LHP_WARN_CURSOR_SOUND ||
		settings->low_hp_warning_type == settings->LHP_WARN_ALL;
}

std::string Avatar::getGfxFromType(const std::string& gfx_type) {
	feet_index = -1;
	std::string gfx;

	// P2.3b: this used to read the global pinv, which "worked" only because there was ever one
	// real player. THIS avatar's own inventory, via playerm, is the specific-player-already-in-
	// scope read (kind A) -- "this" avatar is id, so playerm->inventoryFor(id) is always the right
	// inventory regardless of which avatar getGfxFromType() is called on.
	PlayerInventory* inventory = playerm->inventoryFor(id);
	if (inventory) {
		ItemStorage& equipment = inventory->inventory[PlayerInventory::EQUIPMENT];

		for (int i = 0; i < equipment.getSlotNumber(); i++) {
			if (!inventory->isEquipSlotActive(i))
				continue;

			ItemType& equip_item_type = items->getItemType(inventory->slot_type[i]);

			if (items->isValid(equipment[i].item) && gfx_type == equip_item_type.id) {
				gfx = items->items[equipment[i].item]->gfx;
			}
			if (equip_item_type.id == "feet") {
				feet_index = i;
			}
		}
	}

	// special case: if we don't have a head, use the portrait's head
	if (gfx.empty() && gfx_type == "head") {
		gfx = stats.gfx_head;
	}

	// fall back to default if it exists
	if (gfx.empty()) {
		if (Filesystem::fileExists(mods->locate("animations/avatar/" + stats.gfx_base + "/default_" + gfx_type + ".txt")))
			gfx = "default_" + gfx_type;
	}

	return gfx;
}

bool Avatar::isNearMMtarget() {
	return path.empty() && Utils::calcDist(stats.pos, mm_target_desired) <= stats.speed * 2;
}

void Avatar::setDesiredMMTarget(FPoint& target) {
	mm_target = mm_target_desired = target;
}

Avatar::~Avatar() {
	Utils::logInfo("Cleaning up: Avatar");

	delete charmed_stats;
	delete hero_stats;

	unloadSounds();

	for (size_t i = 0; i < power_cooldown_timers.size(); ++i) {
		delete power_cooldown_timers[i];
		delete power_cast_timers[i];
	}

	for (unsigned i=0; i<sound_steps.size(); i++) {
		snd->unload(sound_steps[i]);
	}
}
