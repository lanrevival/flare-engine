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

#ifndef SCRIPTEDINPUTSTATE_H
#define SCRIPTEDINPUTSTATE_H

#include "NullInputState.h"

/** An InputState that reads no hardware, driven instead by a small text script.
 *
 * P3.7. Reads no SDL, same as NullInputState (which this inherits everything from except
 * construction and the per-tick driver below). Built for headless `flare --script=<file>`, so a
 * scripted client can exercise the exact same PlayerCommandBuilder::build(cmd, in, cam_pos) call
 * site GameStatePlay.cpp already uses for a real player -- no change to that call site at all.
 *
 * Script format, one instruction per line, '#' starts a comment, blank lines ignored:
 *   <tick> press <key_name>
 *   <tick> release <key_name>
 *   <tick> disconnect
 * <key_name> is looked up against InputState::config_keys[] -- the same names a real keybinding
 * config file uses (see SDLInputState::setCommonStrings() / InputState::config_keys, and P3.6e's
 * "cancel_travel" entry). A malformed line or unknown key name is a load-time hard failure
 * (Utils::logError + Utils::Exit(1)), not a silently skipped line -- a script typo should fail
 * loudly, not run a silently-shorter session. See P4.0's [base]-sentinel lesson for why silent
 * misparses are worth this much caution.
 *
 * @class ScriptedInputState
 * @see NullInputState
 * @see InputState
 */
class ScriptedInputState : public NullInputState {
public:
	explicit ScriptedInputState(const std::string& script_path);
	~ScriptedInputState();

	// Applies every press/release/disconnect entry scheduled for exactly this tick. Called once
	// per simulation tick from main.cpp's mainLoop(), after inpt->handle() and before
	// gswitch->logic() -- so PlayerCommandBuilder::build() sees this tick's state when
	// GameStatePlay::logic() calls it. Not virtual: nothing calls it polymorphically.
	void driveTick(unsigned long tick);

private:
	enum ActionKind { ACTION_PRESS, ACTION_RELEASE, ACTION_DISCONNECT };

	struct ScriptEntry {
		unsigned long tick;
		ActionKind kind;
		int key_index; // unused for ACTION_DISCONNECT
	};

	std::vector<ScriptEntry> entries;
	size_t next_entry;

	void loadScript(const std::string& script_path);
	int lookupKeyIndex(const std::string& key_name) const;
};

#endif // SCRIPTEDINPUTSTATE_H
