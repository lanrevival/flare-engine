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

ActionBarState::ActionBarState()
	: slots_count(0)
	, updated(false) {
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
}
