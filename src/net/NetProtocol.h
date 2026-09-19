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

// Bumped to 6 by P3.11c (NPC dialogue as a server-side state machine): MSG_TALK_CMD/MSG_TALK_STATE
// added, same reasoning as every bump before it.
// Bumped to 5 by P3.11b (inventory mirror and commands): MSG_INVENTORY_CMD/MSG_INVENTORY_SNAPSHOT
// added, same reasoning as every bump before it -- a stale binary would misparse the new message
// types entirely, not just miss new fields.
// Bumped to 4 by P3.11a (full player state and one-shot events): PlayerSnapshotEntry grew several
// fields and MSG_PLAYER_EVENT was added, both incompatible with a stale binary, same reasoning as
// the 3->4 bump before it (P3.10's hazard/loot messages) and the 1->2 bump before that
// (MsgPlayerSnapshot::tick).
const uint16_t PROTOCOL_VERSION = 6;

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
	MSG_LOOT_SNAPSHOT = 13,
	MSG_PLAYER_EVENT = 14,
	MSG_INVENTORY_CMD = 15,
	MSG_INVENTORY_SNAPSHOT = 16,
	MSG_TALK_CMD = 17,
	MSG_TALK_STATE = 18
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

// P3.11a. One active effect (buff/debuff) on a player, enough for a mirror to reconstruct the
// EffectManager::effect_list entry MenuCharacter/status icons read. id/magnitude are static-ish
// per application, so those alone would be enough for cooldowns' own "derive duration client-side"
// trick -- but unlike a power's cooldown (one fixed duration per PowerID, already loaded from mod
// data), an effect's total duration varies per source (item, power rank, ...) with nothing on the
// wire to look it up by, so ticks_total has to travel too. Needed for real, not just tidiness:
// MenuActiveEffects.cpp's own status-icon shrink overlay reads timer.getCurrent()/getDuration() as
// a fraction (`:137,194`) -- sending only remaining ticks and reconstructing a Timer with
// setDuration(remaining) (current==duration==remaining, the same trick that works for cooldowns)
// would make that ratio permanently 1.0, rendering every buff/debuff icon as freshly applied no
// matter how close to expiring it actually is.
struct PlayerEffectEntry {
	std::string id;
	float magnitude;
	uint32_t ticks_remaining;
	uint32_t ticks_total;
};

// One player's server-computed state for one tick -- P3.4, extended by P3.11a. position/direction/
// animation/hp/alive are exactly what the server's own Avatar::logic() (run per-player since P3.3)
// just produced; there is nothing for a receiving client to compute or predict, only apply.
//
// P3.11a added mp/mp_max/xp/level/currency/effects/power cooldown+cast ticks: MenuCharacter reads
// these directly off a live Avatar, and a mirror never runs Avatar::logic() (the only thing that
// used to update them), so without these fields it shows whatever stale values the avatar had at
// creation, forever.
//
// Deliberately does NOT include StatBlock::powers_list or ActionBarState's hotkeys, despite both
// being read by MenuPowers/MenuActionBar and despite an earlier draft of this plan having included
// them: MenuPowers::logic() -> setUnlockedPowers() (MenuPowers.cpp) unconditionally re-derives and
// pushes "auto-unlocked" starter/free powers into player->stats.powers_list every tick, on every
// client (not is_mirror-gated, since nothing routes through a menu server-side to gate) -- a
// pre-existing behavior that was invisible in single-player (menu and simulation share one process,
// so there was never anything to disagree with) but is a genuine, deeper gap once powers_list
// becomes wire data: the dedicated server never runs MenuPowers at all (headless, no menu system),
// so its own powers_list never receives this auto-grant, and a connected client's own unconditional
// re-derivation immediately overwrites whatever the server just sent, every tick, with locally
// invented content the server disagrees with. Found empirically (see this plan's own commit
// history/Notes) as a 100% WorldHash::computeReplicated() digest divergence once powers_list was
// wired up. Fixing it needs MenuPowers' auto-unlock logic ported server-side (the same class of fix
// P2.3b/P3.3 already made for level-up/death/respec) -- out of this plan's scope; see Out of scope.
//
// power_cooldown_ticks/power_cast_ticks carry only *remaining* ticks (Timer::getCurrent()), one
// entry per PowerID, sized to match powers->powers.size() -- never Timer::getDuration(), which the
// receiving client derives itself instead: cooldown duration from the static, mod-matched
// Power::cooldown, cast duration from activeAnimation->getDuration() off whichever animation this
// same entry's own `animation` field already names (see GameStatePlay::netApplySnapshotFields()).
// Sending duration too would just be re-stating what the client's own already-loaded data implies.
// A NULL entry (Avatar allocates a Timer* only for a PowerID powers->isValid(), see Avatar.cpp's
// constructor) is sent/applied/mixed as 0 -- see serverBroadcastSnapshot()'s own comment.
struct PlayerSnapshotEntry {
	PlayerID id;
	float pos_x, pos_y;
	uint8_t direction;
	std::string animation; // Animation::getName(); empty if the avatar has none yet
	float hp;
	float hp_max;
	bool alive;
	float mp;
	float mp_max;
	uint32_t xp;
	int32_t level;
	int32_t currency;
	std::vector<PlayerEffectEntry> effects;
	std::vector<uint32_t> power_cooldown_ticks;
	std::vector<uint32_t> power_cast_ticks;
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

// P3.11a. One-shot notification addressed to a single connected player: level-up, death, respec
// completion, a log message, a combat-text number, or a sound -- everything Avatar::logic() used
// to trigger only for a local hero, fanned out per-recipient from the server at the exact point
// each already-existing flag/queue is consumed (see main_server.cpp's own serverLogic() -- this is
// not a new server-side concept, just a new way of reporting ones that already exist). Sent
// point-to-point (NetworkManager::sendTo), never broadcast: `target` is who this is for, and no
// other peer ever receives it.
enum PlayerEventType {
	PLAYER_EVENT_LEVEL_UP = 1,
	PLAYER_EVENT_DEATH = 2,
	PLAYER_EVENT_RESPEC = 3,
	PLAYER_EVENT_LOG_MESSAGE = 4,
	PLAYER_EVENT_COMBAT_TEXT = 5,
	PLAYER_EVENT_SOUND = 6
};

struct MsgPlayerEvent {
	// Zero-initializes every field: callers only ever set the subset that matters for whichever
	// event_type they're building (see per-field comments below), and encodePlayerEvent() only
	// ever reads that same subset -- but leaving the rest indeterminate would be a real (if
	// harmless) uninitialized-read, not just untidy.
	MsgPlayerEvent();

	PlayerID target;
	uint8_t event_type; // PlayerEventType

	// PLAYER_EVENT_LOG_MESSAGE (uses `text` alone) and PLAYER_EVENT_COMBAT_TEXT's !is_number case
	// (uses `text` for a pre-formatted string, e.g. "miss", matching CombatText::addString) share
	// this one field -- the two event types are never both true, so there is nothing to collide.
	// Log text is already msg->get()/getv()-resolved server-side, same string Avatar::logMsg()'s
	// callers already built -- see NetProtocol.h's file header on why this plan doesn't adopt
	// MsgSystemMessage's key+args reassembly (REFUSED_MOD_MISMATCH already guarantees an identical
	// msg->get() catalog on both ends, so re-resolving client-side buys nothing). log_msg_type is
	// PLAYER_EVENT_LOG_MESSAGE-only: Avatar::MSG_NORMAL or Avatar::MSG_UNIQUE (Avatar.h), exactly
	// what GameStatePlay::checkLog() already expects as its own log_msg queue entries' second field
	// -- see netApplyPlayerEvent(), which reuses that exact queue/drain rather than duplicating it.
	std::string text;
	uint8_t log_msg_type;

	// PLAYER_EVENT_COMBAT_TEXT. is_number selects amount (a floating damage number, matching
	// CombatText::addFloat) vs `text` above (matching CombatText::addString). displaytype is a
	// CombatText::MSG_* value (CombatText.h). source_is_target_itself is D25's own filtering input:
	// true when `target` (the player this event is addressed to) is the one who dealt or received
	// this damage, resolved server-side before sending so no other player's identity ever needs to
	// appear in a payload addressed to a third party.
	float pos_x, pos_y;
	float amount;
	bool is_number;
	uint8_t displaytype;
	bool source_is_target_itself;

	// PLAYER_EVENT_SOUND. sfx_type is a SimEvent::SFX_* value (SimEvents.h). chosen_sound is the
	// SoundID the server already rolled from SimEvent::candidates -- every client that hears the
	// same event hears the same sound, matching D2's determinism intent even though sound itself
	// doesn't feed the digest. use_pos false means "play without positioning" (e.g. a UI/self cue).
	uint8_t sfx_type;
	uint32_t chosen_sound;
	bool use_pos;
};

// P3.11b. Client -> host, forwarded exactly like MSG_PLAYER_COMMAND: intent for one inventory
// mutation, applied by the server against that sender's own PlayerInventory
// (main_server.cpp's own MSG_INVENTORY_CMD decode looks the sender's id up in server_net_players,
// same guard serverSendPlayerEvent() already uses). Never applied to any inventory but the
// sender's own -- there is no field naming which player's inventory this is for, because there is
// only ever one answer.
enum InventoryCommandType {
	INV_CMD_MOVE = 1, // move/swap/merge an item between two (area,slot) pairs; see PlayerInventory::moveItem()
	INV_CMD_DROP = 2  // remove an item from (src_area,src_slot) and drop it on the ground; see PlayerInventory::dropItem()
};

struct MsgInventoryCommand {
	uint8_t cmd_type; // InventoryCommandType

	// INV_CMD_MOVE and INV_CMD_DROP both use src_area/src_slot/quantity; dst_area/dst_slot are
	// INV_CMD_MOVE-only (PlayerInventory::EQUIPMENT or ::CARRIED) and are ignored on decode for
	// INV_CMD_DROP, same "declare fields per-type, read only the relevant subset" shape as
	// MsgPlayerEvent above.
	uint8_t src_area;
	int32_t src_slot;
	uint8_t dst_area;
	int32_t dst_slot;
	int32_t quantity;
};

// P3.11b. One inventory slot on the wire: an ItemID (truncated to uint32_t, same convention
// LootSpawnEntry::item already uses) plus how many. Extended/rolled item identity is fully
// described by ItemID alone -- P3.10 already replicates whatever extended-item definitions a
// guest needs to render a tooltip for dropped loot, so nothing else needs to travel here.
struct InventorySlotEntry {
	uint32_t item; // ItemID
	int32_t quantity;
};

// P3.11b. Host -> all, broadcast every tick like MsgPlayerSnapshot/MsgLootSnapshot -- this
// codebase has no delta-encoding infrastructure anywhere, and matching that existing "just resend
// everything" convention is simpler than inventing versioned point-to-point delivery for one
// subsystem. version increments on every PlayerInventory mutation (see PlayerInventory.h) --
// diagnostic only (--dump-players prints it) and not otherwise load-bearing on receipt: an
// earlier draft of this plan had a receiving client discard an entry whose version was not
// strictly newer than its own copy's, on paper a defensive no-op given this codebase's already-
// ordered, reliable per-connection transport. It was not one in practice -- PlayerInventory::
// version is also bumped by several GameStatePlay.cpp call sites that have nothing to do with the
// network (applyDeathPenalty()'s own per-tick call site chief among them), so a connected
// client's own local counter could race ahead of the server's for reasons with no server
// round-trip at all, then reject every subsequent genuinely-newer broadcast as stale forever --
// found via a 100% WorldHash::computeReplicated() digest mismatch in tests/run-net.sh's own
// combat scenario. GameStatePlay::netApplyInventorySnapshot() applies every entry unconditionally
// instead, same as MSG_PLAYER_SNAPSHOT always has.
//
// currency is deliberately NOT included: PlayerSnapshotEntry::currency (P3.11a) already carries
// it, sourced from the same StatBlock::currency PlayerInventory::recomputeCurrency() keeps in
// sync -- a second copy here would just be two fields that can disagree.
struct InventoryEntry {
	PlayerID id;
	uint32_t version;
	std::vector<InventorySlotEntry> equipment;
	std::vector<InventorySlotEntry> carried;
	uint32_t active_equipment_set;
};

struct MsgInventorySnapshot {
	std::vector<InventoryEntry> players;
};

// P3.11c. Client -> host: drives a connected player's own TalkState (PlayerManager.h) exactly like
// MSG_INVENTORY_CMD drives their own PlayerInventory -- never any other player's.
//
// npc_index identifies the target NPC by its position in NPCManager::npcs, NOT by net_id: NPCs are
// never given one (grep -rn "net_id" src/NPC.cpp src/NPCManager.cpp returns nothing -- EntityManager
// never manages them at all), so the entitym->getEntityByNetId()-style lookup an earlier draft of
// this plan assumed (matching P3.9's own convention for ordinary entities) does not apply here. An
// index into npcs->npcs is what every other NPC-identifying call site in this codebase already uses
// instead (EventComponent::NPC_ID's own ec.data[0].Int, GameStatePlay::npc_id/mapr->npc_id) --
// npcs->npcs is rebuilt deterministically from the same map file's wmap->map_npcs on every process
// (host and every connected client alike, same mod_hash-guaranteed-identical-data trust
// EntitySpawnEntry::type_filename already relies on), so its index is already a stable,
// cross-process identifier with nothing new to invent.
enum TalkCommandType {
	TALK_CMD_START = 1,   // begin/resume talking to npc_index -- resets to the topic list, matching
	                      // MenuTalker::setNPC()+chooseDialogNode(-1)'s own combination
	TALK_CMD_CHOOSE = 2,  // choose node_id (-1 = topic list) -- matches MenuTalker::chooseDialogNode()
	TALK_CMD_ADVANCE = 3, // step the current dialog_node forward one line -- matches MenuTalker::nextDialog()
	TALK_CMD_END = 4      // close the conversation -- matches MenuTalker::setNPC(NULL)
};

struct MsgTalkCommand {
	uint8_t cmd_type;   // TalkCommandType
	uint32_t npc_index; // TALK_CMD_START only; ignored on decode for every other cmd_type
	int32_t node_id;    // TALK_CMD_CHOOSE only; ignored on decode for every other cmd_type
};

// P3.11c. Host -> the interacting client only (point-to-point via NetworkManager::sendTo, same as
// MsgPlayerEvent -- another player's live conversation is not this client's business).
//
// Deliberately carries ONLY state (which NPC, which node, how far into it), never rendered text or
// the topic list itself: unlike MsgPlayerEvent's log/combat text (server-resolved strings with no
// client-side equivalent), a receiving client already has the exact same npc->dialog data this
// entry refers to, loaded independently from the same map file (mod_hash guarantees it matches) --
// the same "static per already-loaded data, sending it again would just restate what loading the
// same file already produces" reasoning EntitySpawnEntry::type_filename's own header comment gives.
// GameStatePlay::netApplyTalkState() hands (npc, dialog_node, event_cursor) to
// MenuTalker::applyTalkState(), which rebuilds the on-screen buffer locally exactly the way
// chooseDialogNode()/nextDialog() already do today, just fed server-derived state instead of
// deriving it from a local processEvent()/processDialog() call of its own.
struct MsgTalkState {
	PlayerID player;      // whose TalkState this is -- a receiving client ignores any entry not its own
	int32_t npc_index;    // -1 = not talking to anyone (TalkState::NO_NPC)
	int32_t dialog_node;  // -1 = topic list, matching MenuTalker::dialog_node's own sentinel
	uint32_t event_cursor;
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

std::string encodePlayerEvent(const MsgPlayerEvent& event);
bool decodePlayerEvent(const std::string& payload, MsgPlayerEvent& out);

std::string encodeInventoryCommand(const MsgInventoryCommand& cmd);
bool decodeInventoryCommand(const std::string& payload, MsgInventoryCommand& out);

std::string encodeInventorySnapshot(const std::vector<InventoryEntry>& players);
bool decodeInventorySnapshot(const std::string& payload, MsgInventorySnapshot& out);

std::string encodeTalkCommand(const MsgTalkCommand& cmd);
bool decodeTalkCommand(const std::string& payload, MsgTalkCommand& out);

std::string encodeTalkState(const MsgTalkState& state);
bool decodeTalkState(const std::string& payload, MsgTalkState& out);

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
