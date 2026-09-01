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

#include "ScriptedInputState.h"

#include "Utils.h"
#include "UtilsFileSystem.h"
#include "UtilsParsing.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

ScriptedInputState::ScriptedInputState(const std::string& script_path)
	: NullInputState()
	, entries()
	, next_entry(0) {
	Utils::logInfo("ScriptedInputState: driving input from '%s'.", script_path.c_str());
	loadScript(script_path);
}

ScriptedInputState::~ScriptedInputState() {
}

int ScriptedInputState::lookupKeyIndex(const std::string& key_name) const {
	for (int i = 0; i < KEY_COUNT_USER; ++i) {
		if (config_keys[i] == key_name)
			return i;
	}
	return -1;
}

void ScriptedInputState::loadScript(const std::string& script_path) {
	std::ifstream infile(script_path.c_str());
	if (!infile.is_open()) {
		Utils::logError("ScriptedInputState: could not open script '%s'.", script_path.c_str());
		Utils::Exit(1);
	}

	std::string line;
	int line_num = 0;
	while (std::getline(infile, line)) {
		++line_num;

		size_t hash_pos = line.find('#');
		if (hash_pos != std::string::npos)
			line = line.substr(0, hash_pos);

		// trim whitespace
		size_t start = line.find_first_not_of(" \t\r\n");
		if (start == std::string::npos)
			continue; // blank or comment-only line
		size_t end = line.find_last_not_of(" \t\r\n");
		line = line.substr(start, end - start + 1);

		std::istringstream iss(line);
		std::string tick_str, action_str, key_str;
		iss >> tick_str >> action_str;

		bool tick_ok = !tick_str.empty();
		for (size_t i = 0; tick_ok && i < tick_str.size(); ++i)
			tick_ok = isdigit(static_cast<unsigned char>(tick_str[i])) != 0;

		if (!tick_ok || action_str.empty()) {
			Utils::logError("ScriptedInputState: %s:%d: malformed line '%s'.", script_path.c_str(), line_num, line.c_str());
			Utils::Exit(1);
		}

		ScriptEntry entry;
		entry.tick = strtoul(tick_str.c_str(), NULL, 10);
		entry.key_index = -1;

		if (action_str == "disconnect") {
			entry.kind = ACTION_DISCONNECT;
		}
		else if (action_str == "press" || action_str == "release") {
			iss >> key_str;
			int key_index = key_str.empty() ? -1 : lookupKeyIndex(key_str);
			if (key_index < 0) {
				Utils::logError("ScriptedInputState: %s:%d: unknown key '%s'.", script_path.c_str(), line_num, key_str.c_str());
				Utils::Exit(1);
			}
			entry.kind = (action_str == "press") ? ACTION_PRESS : ACTION_RELEASE;
			entry.key_index = key_index;
		}
		else {
			Utils::logError("ScriptedInputState: %s:%d: unknown action '%s'.", script_path.c_str(), line_num, action_str.c_str());
			Utils::Exit(1);
		}

		entries.push_back(entry);
	}

	// Stable sort by tick: within the same tick, entries fire in file order, which lets a script
	// author write 'press X' and 'release X' at the same tick and get a well-defined outcome
	// (release wins) without a redundant one-tick gap.
	for (size_t i = 1; i < entries.size(); ++i) {
		size_t j = i;
		while (j > 0 && entries[j - 1].tick > entries[j].tick) {
			ScriptEntry tmp = entries[j - 1];
			entries[j - 1] = entries[j];
			entries[j] = tmp;
			--j;
		}
	}
}

void ScriptedInputState::driveTick(unsigned long tick) {
	while (next_entry < entries.size() && entries[next_entry].tick == tick) {
		const ScriptEntry& entry = entries[next_entry];
		switch (entry.kind) {
			case ACTION_PRESS:
				pressing[entry.key_index] = true;
				break;
			case ACTION_RELEASE:
				pressing[entry.key_index] = false;
				break;
			case ACTION_DISCONNECT:
				done = true;
				break;
		}
		++next_entry;
	}
}
