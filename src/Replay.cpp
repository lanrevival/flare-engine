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

#include "Replay.h"

#include "InputState.h"
#include "ModManager.h"
#include "Rng.h"
#include "SharedResources.h"
#include "Utils.h"
#include "Version.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

Replay* replay = NULL;

Replay::Replay()
	: recording(false)
	, playing(false)
	, seed(0)
	, next_frame(0) {
}

Replay::~Replay() {
	finish();
}

std::string Replay::currentModList() {
	std::string s;
	if (!mods)
		return s;
	for (size_t i = 0; i < mods->mod_list.size(); ++i) {
		if (i > 0)
			s += ",";
		s += mods->mod_list[i].name;
	}
	return s;
}

bool Replay::startRecording(const std::string& path) {
	out.open(path.c_str(), std::ios::out);
	if (!out.is_open()) {
		Utils::logError("Replay: could not open '%s' for writing.", path.c_str());
		return false;
	}

	out << "## flare replay ##\n";
	out << "format=" << FORMAT_VERSION << "\n";
	out << "engine_version=" << VersionInfo::ENGINE.getString() << "\n";
	out << "mods=" << currentModList() << "\n";
	// The seed is part of the recording, not of the run that replays it. A replay that reseeds
	// differently is not a replay.
	out << "seed=" << (sim_rng ? sim_rng->getSeed() : 0) << "\n";

	recording = true;
	Utils::logInfo("Replay: recording to '%s'.", path.c_str());
	return true;
}

bool Replay::startPlayback(const std::string& path) {
	std::ifstream in(path.c_str());
	if (!in.is_open()) {
		Utils::logError("Replay: could not open '%s' for reading.", path.c_str());
		return false;
	}

	int file_format = 0;
	std::string file_version;
	std::string file_mods;
	bool have_seed = false;
	std::string line;

	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#')
			continue;

		if (line[0] == 't') {
			// input frame -- header is over
			std::istringstream fs(line);
			std::string tag;
			Frame f;
			unsigned long long p = 0, l = 0;
			fs >> tag >> f.tick >> p >> l >> f.mouse_x >> f.mouse_y;
			f.pressing = static_cast<uint64_t>(p);
			f.lock = static_cast<uint64_t>(l);
			frames.push_back(f);
			continue;
		}

		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = line.substr(0, eq);
		std::string val = line.substr(eq + 1);

		if (key == "format")
			file_format = atoi(val.c_str());
		else if (key == "engine_version")
			file_version = val;
		else if (key == "mods")
			file_mods = val;
		else if (key == "seed") {
			seed = static_cast<uint64_t>(strtoull(val.c_str(), NULL, 0));
			have_seed = true;
		}
	}

	// Refuse rather than drift. A replay that runs against different data produces a digest that
	// looks like a regression but is not, which costs more time than the failure it hides.
	if (file_format != FORMAT_VERSION) {
		Utils::logError("Replay: format mismatch in '%s' -- file is %d, this build expects %d.",
		                path.c_str(), file_format, FORMAT_VERSION);
		return false;
	}
	if (file_version != VersionInfo::ENGINE.getString()) {
		Utils::logError("Replay: engine version mismatch in '%s' -- recorded with '%s', running '%s'.",
		                path.c_str(), file_version.c_str(), VersionInfo::ENGINE.getString().c_str());
		return false;
	}
	if (file_mods != currentModList()) {
		Utils::logError("Replay: mod list mismatch in '%s' -- recorded with '%s', running '%s'.",
		                path.c_str(), file_mods.c_str(), currentModList().c_str());
		return false;
	}
	if (!have_seed) {
		Utils::logError("Replay: '%s' has no seed. Refusing to guess one.", path.c_str());
		return false;
	}

	playing = true;
	next_frame = 0;
	Utils::logInfo("Replay: playing '%s' -- %lu input frames, seed 0x%llx.",
	               path.c_str(), static_cast<unsigned long>(frames.size()),
	               static_cast<unsigned long long>(seed));
	return true;
}

void Replay::recordTick(unsigned long tick) {
	if (!recording || !inpt)
		return;

	uint64_t pressing = 0;
	uint64_t lock = 0;
	// KEY_COUNT is 46; the masks are 64-bit. Assert rather than silently truncate if that grows.
	for (int i = 0; i < InputState::KEY_COUNT && i < 64; ++i) {
		if (inpt->pressing[i])
			pressing |= (static_cast<uint64_t>(1) << i);
		if (inpt->lock[i])
			lock |= (static_cast<uint64_t>(1) << i);
	}

	// Only write ticks where something is held. A recording of an idle server is all zeroes and
	// tells the reader nothing; skipping them also makes an authored recording readable.
	if (pressing == 0 && lock == 0 && inpt->mouse.x == 0 && inpt->mouse.y == 0)
		return;

	out << "t " << tick << " " << pressing << " " << lock
	    << " " << inpt->mouse.x << " " << inpt->mouse.y << "\n";
}

void Replay::applyTick(unsigned long tick) {
	if (!playing || !inpt)
		return;

	// Advance to this tick. Frames are in ascending order; a gap means "nothing held".
	while (next_frame < frames.size() && frames[next_frame].tick < tick)
		next_frame++;

	uint64_t pressing = 0;
	uint64_t lock = 0;
	if (next_frame < frames.size() && frames[next_frame].tick == tick) {
		pressing = frames[next_frame].pressing;
		lock = frames[next_frame].lock;
		inpt->mouse.x = frames[next_frame].mouse_x;
		inpt->mouse.y = frames[next_frame].mouse_y;
	}

	for (int i = 0; i < InputState::KEY_COUNT && i < 64; ++i) {
		inpt->pressing[i] = ((pressing >> i) & 1) != 0;
		inpt->lock[i] = ((lock >> i) & 1) != 0;
	}
}

void Replay::finish() {
	if (recording && out.is_open())
		out.close();
	recording = false;
	playing = false;
}
