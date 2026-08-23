/*
Copyright © 2026 Flare LAN Revival contributors

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

#include "PlayerCommand.h"

#include "InputState.h"
#include "Settings.h"
#include "SharedResources.h"
#include "Utils.h"

PlayerCommand::PlayerCommand()
	: move_up(false), move_down(false), move_left(false), move_right(false)
	, aim_up(false), aim_down(false), aim_left(false), aim_right(false)
	, mm_pressed(false)
	, mm_map_target()
	, main1_pressed(false), main2_pressed(false)
	, main1_active(false), main2_active(false)
	, shift(false)
	, using_mouse(false)
	, mouse_screen()
	, respawn(false)
	, actions() {
}

PlayerInputLocks::PlayerInputLocks() {
	for (int i = 0; i < InputState::KEY_COUNT; ++i)
		lock[i] = false;
}

void PlayerInputLocks::copyFrom(const InputState& in) {
	for (int i = 0; i < InputState::KEY_COUNT; ++i)
		lock[i] = in.lock[i];
}

void PlayerInputLocks::copyTo(InputState& in) const {
	for (int i = 0; i < InputState::KEY_COUNT; ++i)
		in.lock[i] = lock[i];
}

void PlayerInputLocks::unlockActionBar() {
	lock[Input::BAR_1] = false;
	lock[Input::BAR_2] = false;
	lock[Input::BAR_3] = false;
	lock[Input::BAR_4] = false;
	lock[Input::BAR_5] = false;
	lock[Input::BAR_6] = false;
	lock[Input::BAR_7] = false;
	lock[Input::BAR_8] = false;
	lock[Input::BAR_9] = false;
	lock[Input::BAR_0] = false;
	lock[Input::MAIN1] = false;
	lock[Input::MAIN2] = false;
	lock[Input::MENU_ACTIVATE] = false;
}

void PlayerCommandBuilder::build(PlayerCommand& cmd, const InputState& in, const FPoint& cam_pos) {
	// 'pressing && !lock' is the engine's idiom for "held, and the UI has not claimed it".
	// Resolved once here rather than 35 times inside the simulation.
	cmd.move_up    = in.pressing[Input::UP]    && !in.lock[Input::UP];
	cmd.move_down  = in.pressing[Input::DOWN]  && !in.lock[Input::DOWN];
	cmd.move_left  = in.pressing[Input::LEFT]  && !in.lock[Input::LEFT];
	cmd.move_right = in.pressing[Input::RIGHT] && !in.lock[Input::RIGHT];

	cmd.aim_up    = in.pressing[Input::AIM_UP]    && !in.lock[Input::AIM_UP];
	cmd.aim_down  = in.pressing[Input::AIM_DOWN]  && !in.lock[Input::AIM_DOWN];
	cmd.aim_left  = in.pressing[Input::AIM_LEFT]  && !in.lock[Input::AIM_LEFT];
	cmd.aim_right = in.pressing[Input::AIM_RIGHT] && !in.lock[Input::AIM_RIGHT];

	const int mm_key = settings->mouse_move_swap ? Input::MAIN2 : Input::MAIN1;
	cmd.mm_pressed = in.pressing[mm_key];

	// The camera is presentation. Convert here so the simulation never asks where it is.
	cmd.mm_map_target = Utils::screenToMap(in.mouse.x, in.mouse.y, cam_pos.x, cam_pos.y);

	cmd.main1_pressed = in.pressing[Input::MAIN1];
	cmd.main2_pressed = in.pressing[Input::MAIN2];
	cmd.main1_active  = in.pressing[Input::MAIN1] && !in.lock[Input::MAIN1];
	cmd.main2_active  = in.pressing[Input::MAIN2] && !in.lock[Input::MAIN2];

	cmd.shift = in.pressing[Input::SHIFT];
	cmd.using_mouse = const_cast<InputState&>(in).usingMouse();
	cmd.mouse_screen = in.mouse;

	// actions are filled by the caller from menu->act->checkAction(); they are the one part of
	// player intent the engine already modelled as a command list.
}
