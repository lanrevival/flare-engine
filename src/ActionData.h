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

#ifndef ACTIONDATA_H
#define ACTIONDATA_H

#include "Utils.h"
#include "Stats.h"

/** One power activation queued for a tick.
 *
 * Lifted verbatim out of Avatar.h so that PlayerCommand can hold a vector of these without
 * Avatar.h and PlayerCommand.h including each other. The engine already modelled power
 * activation as a command list; this is the half of player intent that was always a value.
 */
class ActionData {
public:
	PowerID power;
	unsigned hotkey;
	bool instant_item;
	bool activated_from_inventory;
	FPoint target;

	ActionData()
		: power(0)
		, hotkey(0)
		, instant_item(false)
		, activated_from_inventory(false)
		, target(FPoint()) {
	}
};

#endif // ACTIONDATA_H
