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

#include "NullInputState.h"
#include "Utils.h"

NullInputState::NullInputState() {
	Utils::logInfo("NullInputState: no keyboard, no mouse, no gamepad.");

	// The server must never decide on its own that the user quit.
	done = false;
	lock_all = false;
}

NullInputState::~NullInputState() {
}

void NullInputState::setBind(int action, int type, int bind, std::string *keybind_msg) {
	(void)action; (void)type; (void)bind; (void)keybind_msg;
}

void NullInputState::removeBind(int action, size_t index) {
	(void)action; (void)index;
}

void NullInputState::initJoystick() {
}

void NullInputState::setCommonStrings() {
}

void NullInputState::initBindings() {
}

void NullInputState::hideCursor() {
}

void NullInputState::showCursor() {
}

std::string NullInputState::getJoystickName(int index) {
	(void)index;
	return std::string();
}

std::string NullInputState::getBindingString(int key, bool get_short_string) {
	(void)key; (void)get_short_string;
	return std::string();
}

std::string NullInputState::getBindingStringByIndex(int key, int binding_index, bool get_short_string) {
	(void)key; (void)binding_index; (void)get_short_string;
	return std::string();
}

std::string NullInputState::getGamepadBindingString(int key, bool get_short_string) {
	(void)key; (void)get_short_string;
	return std::string();
}

std::string NullInputState::getMovementString() {
	return std::string();
}

std::string NullInputState::getAttackString() {
	return std::string();
}

int NullInputState::getNumJoysticks() {
	return 0;
}

bool NullInputState::usingMouse() {
	return false;
}

bool NullInputState::usingTouchscreen() {
	return false;
}

void NullInputState::startTextInput() {
}

void NullInputState::stopTextInput() {
}

void NullInputState::joystickRumble(uint16_t low_freq, uint16_t high_freq, uint32_t duration) {
	(void)low_freq; (void)high_freq; (void)duration;
}

void NullInputState::setJoystickLED(Color color) {
	(void)color;
}

void NullInputState::reset() {
}

int NullInputState::getBindFromString(const std::string& bind, int type) {
	(void)bind; (void)type;
	return -1;
}
