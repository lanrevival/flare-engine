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

const uint16_t PROTOCOL_VERSION = 1;

enum MessageType {
	MSG_HELLO = 1,
	MSG_HELLO_OK = 2,
	MSG_REFUSED = 3,
	MSG_PLAYER_COMMAND = 4,
	MSG_SYSTEM_MESSAGE = 5,
	MSG_PLAYER_SNAPSHOT = 6,
	MSG_MAP_SYNC = 7
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

std::string encodePlayerSnapshot(const std::vector<PlayerSnapshotEntry>& players);
bool decodePlayerSnapshot(const std::string& payload, MsgPlayerSnapshot& out);

std::string encodeMapSync(const std::string& map_filename, float spawn_x, float spawn_y);
bool decodeMapSync(const std::string& payload, MsgMapSync& out);

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

} // namespace Net

#endif
