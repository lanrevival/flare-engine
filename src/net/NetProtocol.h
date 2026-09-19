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

/**
 * Net::NetProtocol
 *
 * Fixed-width binary message schema carried inside NetworkManager's opaque payload strings.
 * Replaces the placeholder "HELLO <name>" / "REFUSED server full" ASCII NetworkManager (P3.1)
 * used to send -- see plans/phase3/P3.2-binary-protocol-with-versioning.md.
 *
 * Every encode* returns a std::string ready to hand to NetworkManager::sendTo/broadcast/
 * sendToHost. Every decode* takes what popPacket() returned and reports success/failure via its
 * bool return -- false means truncated or malformed input, which callers must treat as a protocol
 * violation, never as "wait for more bytes" (popPacket() already delivered one complete frame).
 *
 * All multi-byte fields are explicit big-endian, written by hand (see NetProtocol.cpp) -- the
 * same convention NetworkManager::appendFramed already used for its length prefix, extended here
 * to every field so the wire format doesn't depend on host byte order. No atof, no locale-
 * sensitive parsing anywhere in this file's implementation.
 */

#ifndef NET_NETPROTOCOL_H
#define NET_NETPROTOCOL_H

#include "MessageEngine.h" // MessageArg
#include "PlayerCommand.h"
#include "PlayerManager.h" // PlayerID

#include <stdint.h>
#include <string>
#include <vector>

class Mod;

namespace Net {

// Bumped to 3 by P3.10 (hazard/loot replication): four new message types were added, incompatible
// with a stale binary, same reasoning as the 1->2 bump before it (MsgPlayerSnapshot::tick).
const uint16_t PROTOCOL_VERSION = 3;

enum MessageType {
	MSG_HELLO = 1,
	MSG_HELLO_OK = 2,
	MSG_REFUSED = 3,
	MSG_PLAYER_COMMAND = 4,
	MSG_SYSTEM_MESSAGE = 5,
	MSG_PLAYER_SNAPSHOT = 6,
	MSG_MAP_SYNC = 7,
	MSG_ENTITY_SPAWN = 8,
	MSG_ENTITY_SNAPSHOT = 9,
	MSG_HAZARD_SPAWN = 10,
	MSG_HAZARD_SNAPSHOT = 11,
	MSG_LOOT_SPAWN = 12,
	MSG_LOOT_SNAPSHOT = 13
};

enum RefusalReason {
	REFUSED_SERVER_FULL = 1,
	REFUSED_VERSION_MISMATCH = 2,
	REFUSED_MOD_MISMATCH = 3,
	REFUSED_MALFORMED = 4
};

struct MsgHello {
	uint16_t protocol_version;
	uint16_t engine_x, engine_y, engine_z;
	uint32_t mod_hash;
	std::string display_name;
};

struct MsgHelloOk {
	PlayerID assigned_id;
};

struct MsgRefused {
	uint8_t reason;
	std::string message_key;
};

struct MsgSystemMessage {
	std::string message_key;
	std::vector<MessageArg> args;
};

// One player's server-computed state for one tick -- P3.4. position/direction/animation/hp/alive
// are exactly what the server's own Avatar::logic() (run per-player since P3.3) just produced;
// there is nothing for a receiving client to compute or predict, only apply.
struct PlayerSnapshotEntry {
	PlayerID id;
	float pos_x, pos_y;
	uint8_t direction;
	std::string animation; // Animation::getName(); empty if the avatar has none yet
	float hp;
	float hp_max;
	bool alive;
};

struct MsgPlayerSnapshot {
	// The server's own tick counter at the moment this broadcast was built (serverMainLoop()'s
	// total_ticks). Added by the mirror-tick-sync bugfix: a client's own local loop iterates at
	// its own wall-clock pace (SDL_Delay-throttled, not locked to the server), so two independently
	// scheduled --connect/--host clients sampling "their own local tick 60" were in fact sampling
	// two DIFFERENT real server broadcasts whenever their own pacing drifted apart even slightly --
	// invisible for a player once movement stops (the mirrored value stops changing, so any nearby
	// broadcast reads the same), but permanent for anything that never stops changing (a wandering
	// entity's position/direction), which is why the disclosed P3.9 divergence never resolved no
	// matter how long the quiet tail ran. Both peers now key their own periodic digest sample on
	// THIS value (main.cpp's periodic hash block) instead of their own local tick counter, so a
	// sample only ever compares two peers' views of the SAME real broadcast.
	uint32_t tick;
	std::vector<PlayerSnapshotEntry> players;
};

// P3.5a. Sent exactly once per peer, right after that peer is provisioned server/host-side -- not a
// recurring sync and not a response to later party travel (P3.6's job). spawn_x/spawn_y are always
// a concrete position (the same spawn_pos serverProvisionPlayer()/netHostProvisionPeer() already
// computed for that same player id), never a sentinel.
struct MsgMapSync {
	std::string map_filename;
	float spawn_x, spawn_y;
};

// P3.9. Sent once per entity per client -- on join, for everything already alive, and again the
// tick any later entity is created -- never a recurring sync for an already-announced id (see
// main_server.cpp's own server_announced_entities set). name/lifeform/speed are deliberately NOT
// included: all three are static per type_filename, loaded by StatBlock::load() inside
// EntityManager::loadEntityPrototype(), and the mod_hash handshake already guarantees a client
// loads the identical prototype file -- sending them would just be re-stating what loading the
// same file already produces. level IS sent because D15 party-average scaling makes it genuinely
// instance-specific, not derivable from type_filename alone. hero_ally/enemy_ally are sent so a
// mirror client's own handleNewMap() delete loop (EntityManager.cpp) can tell a persistent ally
// apart from an ordinary enemy the same way the server does -- see
// plans/phase3/P3.9-entity-replication.md's Why for what breaks without this.
struct EntitySpawnEntry {
	uint32_t net_id;
	std::string type_filename;
	float pos_x, pos_y;
	uint8_t direction;
	int level;
	bool hero_ally;
	bool enemy_ally;
};

struct MsgEntitySpawn {
	std::vector<EntitySpawnEntry> entities;
};

// P3.9. Per-tick full dump of every replicated (non-NPC) entity's mutable state -- same "full
// dump every tick, absence is despawn" contract as MsgPlayerSnapshot, same reasoning (D27: no
// deltas). cur_state (StatBlock::ENTITY_* enum) is included even though 'animation' is too,
// because EntityManager::entityFocus()/getNearestEntity() -- both already called unconditionally
// from GameStatePlay's ungated UI code, not gated by is_mirror -- switch on cur_state directly
// (ENTITY_DEAD/ENTITY_CRITDEAD), and that isn't reliably recoverable from an animation name alone.
struct EntitySnapshotEntry {
	uint32_t net_id;
	float pos_x, pos_y;
	uint8_t direction;
	uint8_t cur_state;
	std::string animation;
	float hp, hp_max;
	bool alive;
	bool corpse;
};

struct MsgEntitySnapshot {
	std::vector<EntitySnapshotEntry> entities;
};

// P3.10. No owner (a monster-sourced hazard, or one this scan couldn't attribute) -- PlayerID's
// own valid range is small (D3: up to 8 players), so 0xFF is a safe sentinel.
const PlayerID NO_HAZARD_OWNER = 0xFF;

// P3.10. Spawn-time-only fields for a Hazard -- same "spawn once, snapshot every tick" split
// P3.9 established for entities. animation_name/power_index let a mirror resolve the same
// visuals the server has (loadAnimation()/powers->powers[power_index]) without sending a full
// stat dump, the same trust EntitySpawnEntry::type_filename already relies on (mod_hash
// handshake guarantees matching mod data on both ends).
struct HazardSpawnEntry {
	uint32_t net_id;
	std::string animation_name;
	uint32_t power_index;
	PlayerID owner_id;
};

struct MsgHazardSpawn {
	std::vector<HazardSpawnEntry> hazards;
};

// P3.10. Full per-tick dump, same "absence is despawn" contract as MsgEntitySnapshot. delay_frames
// is included because Hazard::addRenderable() gates rendering on it (delay_frames == 0) -- without
// it a wound-up trap would render as already-active on every mirror.
struct HazardSnapshotEntry {
	uint32_t net_id;
	float pos_x, pos_y;
	uint8_t direction;
	int32_t lifespan;
	int32_t delay_frames;
};

struct MsgHazardSnapshot {
	std::vector<HazardSnapshotEntry> hazards;
};

// P3.10. Spawn-time-only fields for a Loot. dropped_by_hero gates LootManager's own auto-pickup
// eligibility check and never changes after creation, so it belongs here rather than in the
// per-tick snapshot.
struct LootSpawnEntry {
	uint32_t net_id;
	uint32_t item; // ItemID
	bool dropped_by_hero;
};

struct MsgLootSpawn {
	std::vector<LootSpawnEntry> loot;
};

// P3.10. Full per-tick dump. quantity is here, not spawn-only: LootManager::addLoot()'s
// same-position merge path changes an existing stack's quantity without creating a new net_id.
// on_ground lets a mirror know when a flying-loot animation has landed (see
// GameStatePlay::netApplyLootSnapshot()'s own comment on why this needs local animation advance).
struct LootSnapshotEntry {
	uint32_t net_id;
	float pos_x, pos_y;
	int32_t quantity;
	bool on_ground;
};

struct MsgLootSnapshot {
	std::vector<LootSnapshotEntry> loot;
};

std::string encodeHello(const std::string& display_name, uint32_t mod_hash);
bool decodeHello(const std::string& payload, MsgHello& out);

std::string encodeHelloOk(PlayerID assigned_id);
bool decodeHelloOk(const std::string& payload, MsgHelloOk& out);

std::string encodeRefused(uint8_t reason, const std::string& message_key);
bool decodeRefused(const std::string& payload, MsgRefused& out);

std::string encodePlayerCommand(const PlayerCommand& cmd);
bool decodePlayerCommand(const std::string& payload, PlayerCommand& out);

std::string encodeSystemMessage(const std::string& key, const std::vector<MessageArg>& args);
bool decodeSystemMessage(const std::string& payload, MsgSystemMessage& out);

std::string encodePlayerSnapshot(uint32_t tick, const std::vector<PlayerSnapshotEntry>& players);
bool decodePlayerSnapshot(const std::string& payload, MsgPlayerSnapshot& out);

std::string encodeMapSync(const std::string& map_filename, float spawn_x, float spawn_y);
bool decodeMapSync(const std::string& payload, MsgMapSync& out);

std::string encodeEntitySpawn(const std::vector<EntitySpawnEntry>& entities);
bool decodeEntitySpawn(const std::string& payload, MsgEntitySpawn& out);

std::string encodeEntitySnapshot(const std::vector<EntitySnapshotEntry>& entities);
bool decodeEntitySnapshot(const std::string& payload, MsgEntitySnapshot& out);

std::string encodeHazardSpawn(const std::vector<HazardSpawnEntry>& hazards);
bool decodeHazardSpawn(const std::string& payload, MsgHazardSpawn& out);

std::string encodeHazardSnapshot(const std::vector<HazardSnapshotEntry>& hazards);
bool decodeHazardSnapshot(const std::string& payload, MsgHazardSnapshot& out);

std::string encodeLootSpawn(const std::vector<LootSpawnEntry>& loot);
bool decodeLootSpawn(const std::string& payload, MsgLootSpawn& out);

std::string encodeLootSnapshot(const std::vector<LootSnapshotEntry>& loot);
bool decodeLootSnapshot(const std::string& payload, MsgLootSnapshot& out);

// Reads just the message-type byte, without decoding anything else -- callers switch on this
// before picking a decode*(). Returns 0 (not a valid MessageType) if payload is empty.
uint8_t peekMessageType(const std::string& payload);

// Human-readable one-line rendering of any encoded message, for logging/diagnosis only -- never
// used for wire transmission. Returns "<empty>", "<unknown>", or "<truncated:TYPE>" rather than
// crashing on malformed input, same fuzz-safety guarantee as the decode* functions.
std::string debugDump(const std::string& payload);

// FNV-1a 32-bit over "name@x.y.z;" for each mod in 'mods', in the order given (load order is
// meaningful -- a reordering is a legitimate mismatch). A robustness check against an accidental
// version/mod mismatch (D2's LAN trust model), not a security hash.
uint32_t hashModList(const std::vector<Mod>& mods);

// Set by GameStatePlay::netSyncPlayers() the first time (and every time) a MSG_PLAYER_SNAPSHOT
// lands; read by main.cpp's own periodic --hash-replicated block. A --connect/--host client's own
// game loop paces itself off wall-clock time (SDL_Delay in mainLoop()), not off the server's tick
// counter, so two independently-scheduled clients sampling "their own local tick 60" were really
// sampling whatever broadcast each happened to have most recently applied -- the SAME real moment
// only by coincidence. See MsgPlayerSnapshot::tick's own comment for the bug this caused and
// g_last_synced_tick lets main.cpp sample by the server's own tick number instead, so a sample
// only ever compares two peers' views of the identical broadcast. Never set on single-player or
// --dedicated (both read these as g_is_synced_client == false and keep sampling by local tick).
extern bool g_is_synced_client;
extern uint32_t g_last_synced_tick;

} // namespace Net

#endif
