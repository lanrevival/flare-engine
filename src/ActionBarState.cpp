/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
Copyright © 2013 Kurt Rinnert
Copyright © 2014 Henrik Andersson
Copyright © 2012-2016 Justin Jacobs
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
 * class ActionBarState
 */

#include "ActionBarState.h"
#include "ActionData.h"
#include "Avatar.h"
#include "EngineSettings.h"
#include "InputState.h"
#include "Map.h"
#include "MessageEngine.h"
#include "PowerManager.h"
#include "Settings.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "StatBlock.h"

ActionBarState::ActionBarState()
	: slots_count(0)
	, updated(false)
	, twostep_slot(-1) {
	// MENU_COUNT is a compile-time constant, unlike slots_count, so this can size itself here
	// rather than waiting for initSlots().
	requires_attention.resize(MENU_COUNT, false);
}

void ActionBarState::initSlots(unsigned _slots_count) {
	slots_count = _slots_count;
	hotkeys.resize(slots_count);
	hotkeys_temp.resize(slots_count);
	hotkeys_mod.resize(slots_count);
	locked.resize(slots_count);
	slot_fail_cooldown.resize(slots_count);
}

void ActionBarState::clearSlot(size_t slot) {
	hotkeys[slot] = 0;
	hotkeys_temp[slot] = 0;
	hotkeys_mod[slot] = 0;
	locked[slot] = false;
	slot_fail_cooldown[slot].setDuration(Settings::SIM_TICK_HZ);
	slot_fail_cooldown[slot].reset(Timer::END);
}

void ActionBarState::addPower(const PowerID id, const PowerID target_id) {
	if (!powers->isValid(id))
		return;

	// some powers are explicitly prevented from being placed on the actionbar
	if (powers->powers[id]->no_actionbar)
		return;

	// can't put passive powers on the action bar
	if (powers->powers[id]->passive)
		return;

	// if we're not replacing an existing power, avoid placing duplicate powers
	if (target_id == 0) {
		for (unsigned i = 0; i < 12; ++i) {
			if (hotkeys[i] == id)
				return;
		}
	}

	// MAIN slots have priority. 10, 11 and 12 are MenuActionBar::SLOT_MAIN1/SLOT_MAIN2/SLOT_MAX --
	// this class does not include MenuActionBar.h, so the literals are a duplicate of those values,
	// the same call PlayerInventory::EQUIPMENT/CARRIED already made. Matches the original loop
	// bounds exactly, including relying on menus/actionbar.txt always defining slot_M1 and slot_M2
	// so slots_count is always >= 12 -- moved, not fixed; that assumption predates this class.
	for (unsigned i = 10; i < 12; ++i) {
		if (hotkeys[i] == target_id) {
			if (target_id == 0 && prevent_changing[i]) {
				continue;
			}
			hotkeys[i] = id;
			updated = true;
			if (target_id == 0)
				return;
		}
	}

	// now try 0-9 slots
	for (unsigned i = 0; i < 10; ++i) {
		if (hotkeys[i] == target_id) {
			if (target_id == 0 && prevent_changing[i]) {
				continue;
			}
			hotkeys[i] = id;
			updated = true;
			if (target_id == 0)
				return;
		}
	}
}

void ActionBarState::set(std::vector<PowerID> power_id, bool skip_empty) {
	for (unsigned i = 0; i < slots_count; i++) {
		if (!powers->isValid(power_id[i]))
			continue;

		if (!powers->powers[power_id[i]] || powers->powers[power_id[i]]->no_actionbar)
			continue;

		if (!skip_empty || hotkeys[i] == 0)
			hotkeys[i] = power_id[i];
	}
	updated = true;
}

// The hotkey-only subset of MenuActionBar::checkAction() -- P1.4c. Ported branch-for-branch from
// that function, in the same order, with every widget-touching branch removed rather than
// adapted:
//   - both slots[i]->checkClick() branches (touchscreen and mouse/joystick clicks) are gone
//     outright -- there is no WidgetSlot here to click.
//   - the twostep-activation *follow-through* stays (it only reads twostep_slot/inpt, both sim),
//     but nothing here can *start* a twostep sequence any more, since only the deleted click
//     branch used to set twostep_slot >= 0. It will therefore always read -1 in practice -- kept
//     anyway, for the same reason the mouse-aim target branch below is kept: defensive
//     correctness for a path that isn't expected to fire, not dead weight.
//   - slots[i]->enabled (a widget field, precomputed each tick by MenuActionBar::set() from
//     values that are all sim reads) is replaced by computing that same formula inline, as
//     slot_enabled, right where it's needed.
//   - the "not enough resources" sound effect (snd->play(sfx_unable_to_cast, ...)) is dropped --
//     sfx_unable_to_cast is a MenuActionBar-owned SoundID with no sim-side equivalent, and the
//     server's SoundManager is already a no-op. pc->logMsg(...), which pushes onto the sim-owned
//     log_msg queue read by checkLog()'s ported fragment, is kept.
//   - mapr->cam.pos, read in the mouse-aim target branch, becomes pc->stats.pos -- see this plan's
//     "Camera-position substitution".
void ActionBarState::checkHotkeyActions(std::vector<ActionData> &action_queue) {
	bool enable_mm_attack = (!settings->mouse_move || inpt->pressing[Input::SHIFT] || inpt->usingTouchscreen());
	// Original: (!inpt->usingTouchscreen() || (!menu->menus_open && menu->touch_controls->
	// checkAllowMain1())) && (...). The OR's second half only ever matters when usingTouchscreen()
	// is true, which the server never sets -- dropped rather than reconstructed without a menu.
	bool enable_main1 = !inpt->usingTouchscreen() && (settings->mouse_move_swap || enable_mm_attack);
	bool enable_main2 = !settings->mouse_move_swap || enable_mm_attack;

	unsigned mm_slot = settings->mouse_move_swap ? 11 : 10;
	bool mouse_move_target = false;
	if (settings->mouse_move) {
		mouse_move_target = pc->mm_target_object == Avatar::MM_TARGET_ENTITY &&
		                    powers->checkCombatRange(powers->checkReplaceByEffect(hotkeys_mod[mm_slot], &pc->stats), &pc->stats, pc->mm_target_object_pos) &&
		                    wmap->collider.lineOfSight(pc->stats.pos.x, pc->stats.pos.y, pc->mm_target_object_pos.x, pc->mm_target_object_pos.y);

		if (mouse_move_target && pc->stats.cur_state == StatBlock::ENTITY_MOVE) {
			pc->stats.cur_state = StatBlock::ENTITY_STANCE;
		}
		else if (!mouse_move_target && pc->mm_target_object == Avatar::MM_TARGET_ENTITY && pc->stats.cur_state == StatBlock::ENTITY_STANCE) {
			pc->stats.cur_state = StatBlock::ENTITY_MOVE;
		}
	}

	for (unsigned i = 0; i < slots_count; i++) {
		ActionData action;
		action.hotkey = i;
		bool have_aim = false;

		if (i == mm_slot && mouse_move_target) {
			action.power = hotkeys_mod[i];
			have_aim = true;
		}
		// part two of two step activation
		else if (static_cast<unsigned>(twostep_slot) == i && ((inpt->pressing[Input::MAIN1] && !inpt->lock[Input::MAIN1]) || (inpt->pressing[Input::MAIN2] && !inpt->lock[Input::MAIN2]))) {
			have_aim = true;
			action.power = hotkeys_mod[i];
			twostep_slot = -1;
			if (inpt->pressing[Input::MAIN1]) inpt->lock[Input::MAIN1] = true;
			if (inpt->pressing[Input::MAIN2]) inpt->lock[Input::MAIN2] = true;
		}
		// pressing hotkey
		else if (i<10 && inpt->pressing[i + Input::BAR_1]) {
			have_aim = inpt->usingMouse();
			action.power = hotkeys_mod[i];
			twostep_slot = -1;
		}
		else if (i==10 && inpt->pressing[Input::MAIN1] && !inpt->lock[Input::MAIN1] && enable_main1 && twostep_slot == -1) {
			have_aim = inpt->usingMouse();
			action.power = hotkeys_mod[10];
			twostep_slot = -1;
		}
		else if (i==11 && inpt->pressing[Input::MAIN2] && !inpt->lock[Input::MAIN2] && enable_main2 && twostep_slot == -1) {
			have_aim = inpt->usingMouse();
			action.power = hotkeys_mod[11];
			twostep_slot = -1;
		}

		// a power slot was activated
		if (powers->isValid(action.power)) {
			const Power* power = powers->powers[action.power];

			// Substitute for slots[i]->enabled -- see this function's header comment.
			bool slot_enabled = pc->stats.canUsePower(hotkeys_mod[i], !StatBlock::CAN_USE_PASSIVE)
			                  && pc->power_cooldown_timers[hotkeys_mod[i]]->isEnd()
			                  && pc->power_cast_timers[hotkeys_mod[i]]->isEnd()
			                  && (twostep_slot == -1 || static_cast<unsigned>(twostep_slot) == i);

			bool not_enough_resources = false;
			if (slot_fail_cooldown[i].isEnd()) {
				if (pc->stats.mp < power->requires_mp) {
					pc->logMsg(msg->get("Not enough MP."), Avatar::MSG_NORMAL);
					not_enough_resources = true;
				}
				for (size_t j = 0; j < eset->resource_stats.list.size(); ++j) {
					if (pc->stats.resource_stats[j] < power->requires_resource_stat[j]) {
						pc->logMsg(eset->resource_stats.list[j].text_log_low, Avatar::MSG_NORMAL);
						not_enough_resources = true;
					}
				}
			}

			if (not_enough_resources) {
				slot_fail_cooldown[i].reset(Timer::BEGIN);
				continue;
			}

			action.instant_item = false;
			if (power->new_state == Power::STATE_INSTANT) {
				for (size_t j = 0; j < power->required_items.size(); ++j) {
					if (power->required_items[j].id > 0 && !power->required_items[j].equipped) {
						action.instant_item = true;
						break;
					}
				}
			}

			// set the target depending on how the power was triggered
			if (have_aim && settings->mouse_aim && (settings->mouse_move || !inpt->usingTouchscreen())) {
				action.target = pc->stats.pos;

				if (power->target_nearest > 0) {
					if (!power->requires_corpse && powers->checkNearestTargeting(power, &pc->stats, false)) {
						action.target = pc->stats.target_nearest->pos;
					}
					else if (power->requires_corpse && powers->checkNearestTargeting(power, &pc->stats, true)) {
						action.target = pc->stats.target_nearest_corpse->pos;
					}
				}
				else if (mouse_move_target) {
					action.target = pc->mm_target_object_pos;
				}
				else {
					if (power->aim_assist)
						action.target = Utils::screenToMap(inpt->mouse.x,  inpt->mouse.y + eset->misc.aim_assist, pc->stats.pos.x, pc->stats.pos.y);
					else
						action.target = Utils::screenToMap(inpt->mouse.x,  inpt->mouse.y, pc->stats.pos.x, pc->stats.pos.y);
				}
			}
			else {
				action.target = Utils::calcVector(pc->stats.pos, pc->stats.direction, pc->stats.melee_range);
			}

			bool can_use_power = slot_enabled &&
				(power->new_state == Power::STATE_INSTANT || (pc->stats.cooldown.isEnd() && pc->stats.cur_state != StatBlock::ENTITY_POWER && pc->stats.cur_state != StatBlock::ENTITY_HIT)) &&
				powers->hasValidTarget(action.power, &pc->stats, action.target);

			// add it to the queue
			if (can_use_power) {
				if (i != mm_slot && !action.instant_item) {
					pc->mm_target_object = Avatar::MM_TARGET_NONE;
				}

				slot_fail_cooldown[i].reset(Timer::BEGIN);
				action_queue.push_back(action);
			}
		}
		else {
			// if we're not triggering an action that is currently in the queue,
			// remove it from the queue
			for (size_t j = action_queue.size(); j > 0; --j) {
				if (!action_queue[j-1].activated_from_inventory && action_queue[j-1].hotkey == i)
					action_queue.erase(action_queue.begin()+(j-1));
			}
		}
	}
}
