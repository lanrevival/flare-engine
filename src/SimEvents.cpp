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

#include "SimEvents.h"

SimEventQueue* sim_events = NULL;

SimEvent::SimEvent()
	: type(SimEvent::SFX_HIT)
	, stop(false)
	, candidates()
	, select(false)
	, channel()
	, channel_by_sound(false)
	, pos()
	, use_pos(false)
	, loop(false)
	, cleanup(true) {
}

SimEvent::SimEvent(int _type)
	: type(_type)
	, stop(false)
	, candidates()
	, select(false)
	, channel()
	, channel_by_sound(false)
	, pos()
	, use_pos(false)
	, loop(false)
	, cleanup(true) {
}

SimEventQueue::SimEventQueue()
	: queue()
	, high_water(0) {
}

void SimEventQueue::push(const SimEvent& e) {
	queue.push_back(e);
	if (queue.size() > high_water)
		high_water = queue.size();
}

void SimEventQueue::pushSound(int type, SoundID sid, const std::string& channel, const FPoint& pos) {
	SimEvent e(type);
	e.candidates.push_back(sid);
	e.channel = channel;
	e.pos = pos;
	e.use_pos = true;
	push(e);
}

void SimEventQueue::pushStop(int type, const std::string& channel) {
	SimEvent e(type);
	e.stop = true;
	e.channel = channel;
	push(e);
}
