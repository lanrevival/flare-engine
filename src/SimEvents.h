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

#ifndef SIMEVENTS_H
#define SIMEVENTS_H

#include "CommonIncludes.h"
#include "Utils.h"

/** Things the simulation did that the presentation layer may want to react to.
 *
 * The simulation appends; it never plays. The client drains the queue once a tick and plays what
 * it finds. A headless server drains and discards. Nothing here is networked -- Phase 3 decides
 * whether a remote player's sounds are replicated or re-derived from snapshots, and until then a
 * SimEvent is strictly local to the process that produced it.
 *
 * WHAT THIS DELIBERATELY IS NOT:
 *
 * It is not a subscriber system. It is a vector that is emptied every tick.
 *
 * It does not carry a chosen sound. A sim class emits the candidate list and the DRAIN picks one,
 * because which of an entity's three hit sounds you hear is a client's business -- two players may
 * be running different sound mods. This is the whole reason 'candidates' is a list rather than the
 * SoundID the sim used to resolve itself.
 *
 * It carries no entity reference. That is not an oversight and not a stub: the queue is drained in
 * the same tick that filled it, and an entity that died this tick may already be gone by then. A
 * borrowed pointer here would be a use-after-free on exactly the events -- SFX_DIE, SFX_CRITDIE --
 * most likely to be emitted by something about to be deleted. So the candidates are COPIED in.
 *
 * SoundIDs are still owned and loaded by the sim classes. Getting sound ASSETS out of the
 * simulation is a separate job; see plans/phase1/P1.2 for why it is not this one.
 */
class SimEvent {
public:
	enum {
		// combat and movement, emitted from the authoritative tick
		SFX_ATTACK = 0,
		SFX_HIT,
		SFX_DIE,
		SFX_CRITDIE,
		SFX_BLOCK,
		SFX_STEP,
		SFX_LEVELUP,
		SFX_LOOT,
		SFX_POWER,
		SFX_HAZARD_HIT,
		SFX_MAP_EVENT,

		// the low-HP warning is a LOOPING CHANNEL, so it needs both edges. The sim reports the
		// transition; it must never report "still low" every tick.
		SFX_LOWHP_START,
		SFX_LOWHP_STOP,

		// client-local UI, driven by a menu rather than by the tick. Never network these.
		SFX_NPC_VOX,

		TYPE_COUNT
	};

	/** Stable short name for a type, for logs and for the coverage assertion in
	 * tests/run-replays.sh. Returns "?" for an out-of-range type rather than indexing past the
	 * end -- this is called from a reporting path and must never be the thing that crashes. */
	static const char* typeName(int type);

	SimEvent();
	explicit SimEvent(int _type);

	int type;

	/** true: silence 'channel' instead of playing anything. */
	bool stop;

	/** Sounds this event could play. The drain chooses; the simulation must not. */
	std::vector<SoundID> candidates;

	/** true: choose with fx_rng, even when there is only one candidate.
	 *
	 * The redundant-looking one-candidate draw is intentional at sites that always drew before.
	 * fx_rng is the non-reproducible stream by construction, so its draw count is not a
	 * correctness property -- but keeping selection sites honest about being selection sites is.
	 */
	bool select;

	/** Empty means "the client's default channel". No sim site wants a literally empty name. */
	std::string channel;

	/** true: append the chosen SoundID to 'channel'. Entities use per-sound channels so that two
	 * different hit sounds do not cut each other off. */
	bool channel_by_sound;

	FPoint pos;

	/** false: the sound is not positioned, and the client substitutes its own no-position value. */
	bool use_pos;

	bool loop;
	bool cleanup;
};

class SimEventQueue {
public:
	SimEventQueue();

	void push(const SimEvent& e);

	/** Convenience for the common "one known sound, positioned, no loop" case. */
	void pushSound(int type, SoundID sid, const std::string& channel, const FPoint& pos);

	/** Convenience for the low-HP pair and anything else that silences a named channel. */
	void pushStop(int type, const std::string& channel);

	const std::vector<SimEvent>& events() const { return queue; }

	void clear() { queue.clear(); }

	/** Largest the queue has been since the process started. If this climbs without bound, some
	 * path is emitting without anyone draining -- which is the failure this getter exists to catch.
	 */
	size_t getHighWater() const { return high_water; }

	/** How many events of this type have been pushed since the process started.
	 *
	 * This exists because P0.5b's 'attack' recording turned out to contain no attack, and nothing
	 * in the test suite could tell. A digest proves the world changed; it cannot say WHICH code
	 * ran. These counters can, and they cost one increment per event. See plans/phase0/P0.5c.
	 */
	unsigned long getCount(int type) const;

	/** Every event pushed since the process started, all types summed.
	 *
	 * The point is liveness, not accounting: a caller that samples this each tick can see the
	 * tick on which the simulation last did anything. See main_server's `last_tick`.
	 */
	unsigned long getTotal() const { return total; }

private:
	std::vector<SimEvent> queue;
	size_t high_water;
	unsigned long counts[SimEvent::TYPE_COUNT];
	unsigned long total;
};

extern SimEventQueue* sim_events;

#endif // SIMEVENTS_H
