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

#ifndef NULLINPUTSTATE_H
#define NULLINPUTSTATE_H

#include "InputState.h"

/** An InputState that reads no hardware.
 *
 * Used by the dedicated server. SDLInputState calls SDL_ShowCursor, SDL_StartTextInput,
 * SDL_NumJoysticks and SDL_GameControllerOpen, all of which need the SDL video subsystem;
 * this class makes no SDL call at all.
 *
 * It never sets 'done'. There is no window to close, so a stray 'done' would make the
 * server exit on its own with no explanation.
 *
 * NOTE: handle() is deliberately NOT overridden. InputState::handle() is pure bookkeeping
 * -- it clears un_press[] and resets key states, with no SDL calls -- so inheriting it is
 * both correct and avoids a C++98 signature-mismatch risk on a non-pure virtual.
 *
 * @class NullInputState
 * @see InputState
 */
class NullInputState : public InputState {
public:
	NullInputState();
	virtual ~NullInputState();

	void setBind(int action, int type, int bind, std::string *keybind_msg);
	void removeBind(int action, size_t index);
	void initJoystick();
	void setCommonStrings();
	void initBindings();
	void hideCursor();
	void showCursor();
	std::string getJoystickName(int index);
	std::string getBindingString(int key, bool get_short_string = !InputState::GET_SHORT_STRING);
	std::string getBindingStringByIndex(int key, int binding_index, bool get_short_string = !InputState::GET_SHORT_STRING);
	std::string getGamepadBindingString(int key, bool get_short_string = !InputState::GET_SHORT_STRING);
	std::string getMovementString();
	std::string getAttackString();
	int getNumJoysticks();
	bool usingMouse();
	bool usingTouchscreen();
	void startTextInput();
	void stopTextInput();
	void joystickRumble(uint16_t low_freq, uint16_t high_freq, uint32_t duration);
	void setJoystickLED(Color color);
	void reset();
	int getBindFromString(const std::string& bind, int type);
};

#endif // NULLINPUTSTATE_H
