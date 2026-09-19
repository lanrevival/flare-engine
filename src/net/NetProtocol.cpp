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

#include "NetProtocol.h"

#include "ActionData.h"
#include "ModManager.h"
#include "Version.h"

#include <cstdio>
#include <cstring>

namespace Net {

bool g_is_synced_client = false;
uint32_t g_last_synced_tick = 0;

namespace {

void writeU8(std::string& out, uint8_t v) {
	out.push_back(static_cast<char>(v));
}

void writeU16(std::string& out, uint16_t v) {
	out.push_back(static_cast<char>((v >> 8) & 0xFF));
	out.push_back(static_cast<char>(v & 0xFF));
}

void writeU32(std::string& out, uint32_t v) {
	out.push_back(static_cast<char>((v >> 24) & 0xFF));
	out.push_back(static_cast<char>((v >> 16) & 0xFF));
	out.push_back(static_cast<char>((v >> 8) & 0xFF));
	out.push_back(static_cast<char>(v & 0xFF));
}

void writeFloat(std::string& out, float f) {
	uint32_t bits;
	std::memcpy(&bits, &f, sizeof(bits));
	writeU32(out, bits);
}

void writeString(std::string& out, const std::string& s) {
	writeU32(out, static_cast<uint32_t>(s.size()));
	out.append(s);
}

bool readU8(const std::string& payload, size_t& offset, uint8_t& out) {
	if (offset + 1 > payload.size())
		return false;
	out = static_cast<uint8_t>(payload[offset]);
	offset += 1;
	return true;
}

bool readU16(const std::string& payload, size_t& offset, uint16_t& out) {
	if (offset + 2 > payload.size())
		return false;
	out = static_cast<uint16_t>((static_cast<uint8_t>(payload[offset]) << 8)
	                           | static_cast<uint8_t>(payload[offset + 1]));
	offset += 2;
	return true;
}

bool readU32(const std::string& payload, size_t& offset, uint32_t& out) {
	if (offset + 4 > payload.size())
		return false;
	out = (static_cast<uint32_t>(static_cast<uint8_t>(payload[offset])) << 24)
	    | (static_cast<uint32_t>(static_cast<uint8_t>(payload[offset + 1])) << 16)
	    | (static_cast<uint32_t>(static_cast<uint8_t>(payload[offset + 2])) << 8)
	    |  static_cast<uint32_t>(static_cast<uint8_t>(payload[offset + 3]));
	offset += 4;
	return true;
}

bool readFloat(const std::string& payload, size_t& offset, float& out) {
	uint32_t bits;
	if (!readU32(payload, offset, bits))
		return false;
	std::memcpy(&out, &bits, sizeof(out));
	return true;
}

// Length-validated extraction of a byte range whose length was just read from the wire and
// bounds-checked against what remains -- not delimiter scanning. See this plan's validator
// checklist note on why substr() here isn't the ad-hoc-ASCII pattern this plan removes.
bool readString(const std::string& payload, size_t& offset, std::string& out) {
	uint32_t len = 0;
	if (!readU32(payload, offset, len))
		return false;
	if (offset + len > payload.size())
		return false;
	out = payload.substr(offset, len);
	offset += len;
	return true;
}

} // anonymous namespace

std::string encodeHello(const std::string& display_name, uint32_t mod_hash) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_HELLO));
	writeU16(out, PROTOCOL_VERSION);
	writeU16(out, VersionInfo::ENGINE.x);
	writeU16(out, VersionInfo::ENGINE.y);
	writeU16(out, VersionInfo::ENGINE.z);
	writeU32(out, mod_hash);
	writeString(out, display_name);
	return out;
}

bool decodeHello(const std::string& payload, MsgHello& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_HELLO)
		return false;
	return readU16(payload, offset, out.protocol_version)
	    && readU16(payload, offset, out.engine_x)
	    && readU16(payload, offset, out.engine_y)
	    && readU16(payload, offset, out.engine_z)
	    && readU32(payload, offset, out.mod_hash)
	    && readString(payload, offset, out.display_name);
}

std::string encodeHelloOk(PlayerID assigned_id) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_HELLO_OK));
	writeU8(out, assigned_id);
	return out;
}

bool decodeHelloOk(const std::string& payload, MsgHelloOk& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_HELLO_OK)
		return false;
	return readU8(payload, offset, out.assigned_id);
}

std::string encodeRefused(uint8_t reason, const std::string& message_key) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_REFUSED));
	writeU8(out, reason);
	writeString(out, message_key);
	return out;
}

bool decodeRefused(const std::string& payload, MsgRefused& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_REFUSED)
		return false;
	return readU8(payload, offset, out.reason)
	    && readString(payload, offset, out.message_key);
}

std::string encodePlayerCommand(const PlayerCommand& cmd) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_PLAYER_COMMAND));

	uint8_t byte1 = 0;
	if (cmd.move_up)    byte1 |= 0x01;
	if (cmd.move_down)  byte1 |= 0x02;
	if (cmd.move_left)  byte1 |= 0x04;
	if (cmd.move_right) byte1 |= 0x08;
	if (cmd.aim_up)     byte1 |= 0x10;
	if (cmd.aim_down)   byte1 |= 0x20;
	if (cmd.aim_left)   byte1 |= 0x40;
	if (cmd.aim_right)  byte1 |= 0x80;
	writeU8(out, byte1);

	uint8_t byte2 = 0;
	if (cmd.mm_pressed)           byte2 |= 0x01;
	if (cmd.main1_pressed)        byte2 |= 0x02;
	if (cmd.main2_pressed)        byte2 |= 0x04;
	if (cmd.main1_active)         byte2 |= 0x08;
	if (cmd.main2_active)         byte2 |= 0x10;
	if (cmd.shift)                byte2 |= 0x20;
	if (cmd.using_mouse)          byte2 |= 0x40;
	if (cmd.click_consumed_by_ui) byte2 |= 0x80;
	writeU8(out, byte2);

	uint8_t byte3 = 0;
	if (cmd.respawn) byte3 |= 0x01;
	if (cmd.cancel_travel) byte3 |= 0x02;
	writeU8(out, byte3);

	writeFloat(out, cmd.mm_map_target.x);
	writeFloat(out, cmd.mm_map_target.y);
	writeU8(out, static_cast<uint8_t>(static_cast<int8_t>(cmd.equip_set_delta)));

	writeU8(out, static_cast<uint8_t>(cmd.actions.size()));
	for (size_t i = 0; i < cmd.actions.size(); ++i) {
		const ActionData& a = cmd.actions[i];
		writeU32(out, static_cast<uint32_t>(a.power));
		writeU32(out, static_cast<uint32_t>(a.hotkey));
		uint8_t aflags = 0;
		if (a.instant_item)            aflags |= 0x01;
		if (a.activated_from_inventory) aflags |= 0x02;
		writeU8(out, aflags);
		writeFloat(out, a.target.x);
		writeFloat(out, a.target.y);
	}

	return out;
}

bool decodePlayerCommand(const std::string& payload, PlayerCommand& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_PLAYER_COMMAND)
		return false;

	uint8_t byte1, byte2, byte3;
	if (!readU8(payload, offset, byte1) || !readU8(payload, offset, byte2) || !readU8(payload, offset, byte3))
		return false;

	out.move_up    = (byte1 & 0x01) != 0;
	out.move_down  = (byte1 & 0x02) != 0;
	out.move_left  = (byte1 & 0x04) != 0;
	out.move_right = (byte1 & 0x08) != 0;
	out.aim_up     = (byte1 & 0x10) != 0;
	out.aim_down   = (byte1 & 0x20) != 0;
	out.aim_left   = (byte1 & 0x40) != 0;
	out.aim_right  = (byte1 & 0x80) != 0;

	out.mm_pressed           = (byte2 & 0x01) != 0;
	out.main1_pressed        = (byte2 & 0x02) != 0;
	out.main2_pressed        = (byte2 & 0x04) != 0;
	out.main1_active         = (byte2 & 0x08) != 0;
	out.main2_active         = (byte2 & 0x10) != 0;
	out.shift                = (byte2 & 0x20) != 0;
	out.using_mouse           = (byte2 & 0x40) != 0;
	out.click_consumed_by_ui = (byte2 & 0x80) != 0;

	out.respawn = (byte3 & 0x01) != 0;
	out.cancel_travel = (byte3 & 0x02) != 0;

	if (!readFloat(payload, offset, out.mm_map_target.x) || !readFloat(payload, offset, out.mm_map_target.y))
		return false;

	uint8_t delta_byte;
	if (!readU8(payload, offset, delta_byte))
		return false;
	out.equip_set_delta = static_cast<int>(static_cast<int8_t>(delta_byte));

	uint8_t action_count;
	if (!readU8(payload, offset, action_count))
		return false;

	out.actions.clear();
	for (uint8_t i = 0; i < action_count; ++i) {
		ActionData a;
		uint32_t power32, hotkey32;
		uint8_t aflags;
		if (!readU32(payload, offset, power32) || !readU32(payload, offset, hotkey32) || !readU8(payload, offset, aflags))
			return false;
		a.power = static_cast<PowerID>(power32);
		a.hotkey = static_cast<unsigned>(hotkey32);
		a.instant_item = (aflags & 0x01) != 0;
		a.activated_from_inventory = (aflags & 0x02) != 0;
		if (!readFloat(payload, offset, a.target.x) || !readFloat(payload, offset, a.target.y))
			return false;
		out.actions.push_back(a);
	}

	return true;
}

std::string encodeSystemMessage(const std::string& key, const std::vector<MessageArg>& args) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_SYSTEM_MESSAGE));
	writeString(out, key);
	writeU8(out, static_cast<uint8_t>(args.size()));
	for (size_t i = 0; i < args.size(); ++i) {
		if (args[i].type == MessageArg::ARG_INT) {
			writeU8(out, 0);
			writeU32(out, static_cast<uint32_t>(args[i].i));
		}
		else {
			writeU8(out, 1);
			writeString(out, args[i].s);
		}
	}
	return out;
}

bool decodeSystemMessage(const std::string& payload, MsgSystemMessage& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_SYSTEM_MESSAGE)
		return false;
	if (!readString(payload, offset, out.message_key))
		return false;

	uint8_t arg_count;
	if (!readU8(payload, offset, arg_count))
		return false;

	out.args.clear();
	for (uint8_t i = 0; i < arg_count; ++i) {
		uint8_t tag;
		if (!readU8(payload, offset, tag))
			return false;
		if (tag == 0) {
			uint32_t raw;
			if (!readU32(payload, offset, raw))
				return false;
			out.args.push_back(MessageArg(static_cast<int>(raw)));
		}
		else if (tag == 1) {
			std::string s;
			if (!readString(payload, offset, s))
				return false;
			out.args.push_back(MessageArg(s));
		}
		else {
			return false; // unrecognised arg tag -- malformed, not a forward-compat case here
		}
	}

	return true;
}

std::string encodePlayerSnapshot(uint32_t tick, const std::vector<PlayerSnapshotEntry>& players) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_PLAYER_SNAPSHOT));
	writeU32(out, tick);
	writeU8(out, static_cast<uint8_t>(players.size()));
	for (size_t i = 0; i < players.size(); ++i) {
		const PlayerSnapshotEntry& p = players[i];
		writeU8(out, p.id);
		writeFloat(out, p.pos_x);
		writeFloat(out, p.pos_y);
		writeU8(out, p.direction);
		writeString(out, p.animation);
		writeFloat(out, p.hp);
		writeFloat(out, p.hp_max);
		writeU8(out, p.alive ? 1 : 0);

		writeFloat(out, p.mp);
		writeFloat(out, p.mp_max);
		writeU32(out, p.xp);
		writeU32(out, static_cast<uint32_t>(p.level));
		writeU32(out, static_cast<uint32_t>(p.currency));

		writeU16(out, static_cast<uint16_t>(p.effects.size()));
		for (size_t j = 0; j < p.effects.size(); ++j) {
			writeString(out, p.effects[j].id);
			writeFloat(out, p.effects[j].magnitude);
			writeU32(out, p.effects[j].ticks_remaining);
			writeU32(out, p.effects[j].ticks_total);
		}

		writeU16(out, static_cast<uint16_t>(p.power_cooldown_ticks.size()));
		for (size_t j = 0; j < p.power_cooldown_ticks.size(); ++j)
			writeU32(out, p.power_cooldown_ticks[j]);

		writeU16(out, static_cast<uint16_t>(p.power_cast_ticks.size()));
		for (size_t j = 0; j < p.power_cast_ticks.size(); ++j)
			writeU32(out, p.power_cast_ticks[j]);
	}
	return out;
}

bool decodePlayerSnapshot(const std::string& payload, MsgPlayerSnapshot& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_PLAYER_SNAPSHOT)
		return false;

	if (!readU32(payload, offset, out.tick))
		return false;

	uint8_t count;
	if (!readU8(payload, offset, count))
		return false;

	out.players.clear();
	for (uint8_t i = 0; i < count; ++i) {
		PlayerSnapshotEntry p;
		uint8_t alive_byte;
		if (!readU8(payload, offset, p.id)
		    || !readFloat(payload, offset, p.pos_x)
		    || !readFloat(payload, offset, p.pos_y)
		    || !readU8(payload, offset, p.direction)
		    || !readString(payload, offset, p.animation)
		    || !readFloat(payload, offset, p.hp)
		    || !readFloat(payload, offset, p.hp_max)
		    || !readU8(payload, offset, alive_byte))
			return false;
		p.alive = alive_byte != 0;

		uint32_t xp32, level32, currency32;
		if (!readFloat(payload, offset, p.mp)
		    || !readFloat(payload, offset, p.mp_max)
		    || !readU32(payload, offset, xp32)
		    || !readU32(payload, offset, level32)
		    || !readU32(payload, offset, currency32))
			return false;
		p.xp = xp32;
		p.level = static_cast<int32_t>(level32);
		p.currency = static_cast<int32_t>(currency32);

		uint16_t effect_count;
		if (!readU16(payload, offset, effect_count))
			return false;
		p.effects.clear();
		for (uint16_t j = 0; j < effect_count; ++j) {
			PlayerEffectEntry e;
			if (!readString(payload, offset, e.id)
			    || !readFloat(payload, offset, e.magnitude)
			    || !readU32(payload, offset, e.ticks_remaining)
			    || !readU32(payload, offset, e.ticks_total))
				return false;
			p.effects.push_back(e);
		}

		uint16_t cooldown_count;
		if (!readU16(payload, offset, cooldown_count))
			return false;
		p.power_cooldown_ticks.clear();
		for (uint16_t j = 0; j < cooldown_count; ++j) {
			uint32_t v;
			if (!readU32(payload, offset, v))
				return false;
			p.power_cooldown_ticks.push_back(v);
		}

		uint16_t cast_count;
		if (!readU16(payload, offset, cast_count))
			return false;
		p.power_cast_ticks.clear();
		for (uint16_t j = 0; j < cast_count; ++j) {
			uint32_t v;
			if (!readU32(payload, offset, v))
				return false;
			p.power_cast_ticks.push_back(v);
		}

		out.players.push_back(p);
	}

	return true;
}

std::string encodeMapSync(const std::string& map_filename, float spawn_x, float spawn_y) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_MAP_SYNC));
	writeString(out, map_filename);
	writeFloat(out, spawn_x);
	writeFloat(out, spawn_y);
	return out;
}

bool decodeMapSync(const std::string& payload, MsgMapSync& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_MAP_SYNC)
		return false;
	return readString(payload, offset, out.map_filename)
	    && readFloat(payload, offset, out.spawn_x)
	    && readFloat(payload, offset, out.spawn_y);
}

std::string encodeEntitySpawn(const std::vector<EntitySpawnEntry>& entities) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_ENTITY_SPAWN));
	writeU16(out, static_cast<uint16_t>(entities.size()));
	for (size_t i = 0; i < entities.size(); ++i) {
		const EntitySpawnEntry& e = entities[i];
		writeU32(out, e.net_id);
		writeString(out, e.type_filename);
		writeFloat(out, e.pos_x);
		writeFloat(out, e.pos_y);
		writeU8(out, e.direction);
		writeU32(out, static_cast<uint32_t>(e.level));
		writeU8(out, e.hero_ally ? 1 : 0);
		writeU8(out, e.enemy_ally ? 1 : 0);
	}
	return out;
}

bool decodeEntitySpawn(const std::string& payload, MsgEntitySpawn& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_ENTITY_SPAWN)
		return false;

	uint16_t count;
	if (!readU16(payload, offset, count))
		return false;

	out.entities.clear();
	for (uint16_t i = 0; i < count; ++i) {
		EntitySpawnEntry e;
		uint32_t level32;
		uint8_t hero_byte, enemy_byte;
		if (!readU32(payload, offset, e.net_id)
		    || !readString(payload, offset, e.type_filename)
		    || !readFloat(payload, offset, e.pos_x)
		    || !readFloat(payload, offset, e.pos_y)
		    || !readU8(payload, offset, e.direction)
		    || !readU32(payload, offset, level32)
		    || !readU8(payload, offset, hero_byte)
		    || !readU8(payload, offset, enemy_byte))
			return false;
		e.level = static_cast<int>(level32);
		e.hero_ally = hero_byte != 0;
		e.enemy_ally = enemy_byte != 0;
		out.entities.push_back(e);
	}

	return true;
}

std::string encodeEntitySnapshot(const std::vector<EntitySnapshotEntry>& entities) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_ENTITY_SNAPSHOT));
	writeU16(out, static_cast<uint16_t>(entities.size()));
	for (size_t i = 0; i < entities.size(); ++i) {
		const EntitySnapshotEntry& e = entities[i];
		writeU32(out, e.net_id);
		writeFloat(out, e.pos_x);
		writeFloat(out, e.pos_y);
		writeU8(out, e.direction);
		writeU8(out, e.cur_state);
		writeString(out, e.animation);
		writeFloat(out, e.hp);
		writeFloat(out, e.hp_max);
		writeU8(out, e.alive ? 1 : 0);
		writeU8(out, e.corpse ? 1 : 0);
	}
	return out;
}

bool decodeEntitySnapshot(const std::string& payload, MsgEntitySnapshot& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_ENTITY_SNAPSHOT)
		return false;

	uint16_t count;
	if (!readU16(payload, offset, count))
		return false;

	out.entities.clear();
	for (uint16_t i = 0; i < count; ++i) {
		EntitySnapshotEntry e;
		uint8_t alive_byte, corpse_byte;
		if (!readU32(payload, offset, e.net_id)
		    || !readFloat(payload, offset, e.pos_x)
		    || !readFloat(payload, offset, e.pos_y)
		    || !readU8(payload, offset, e.direction)
		    || !readU8(payload, offset, e.cur_state)
		    || !readString(payload, offset, e.animation)
		    || !readFloat(payload, offset, e.hp)
		    || !readFloat(payload, offset, e.hp_max)
		    || !readU8(payload, offset, alive_byte)
		    || !readU8(payload, offset, corpse_byte))
			return false;
		e.alive = alive_byte != 0;
		e.corpse = corpse_byte != 0;
		out.entities.push_back(e);
	}

	return true;
}

std::string encodeHazardSpawn(const std::vector<HazardSpawnEntry>& hazards) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_HAZARD_SPAWN));
	writeU16(out, static_cast<uint16_t>(hazards.size()));
	for (size_t i = 0; i < hazards.size(); ++i) {
		const HazardSpawnEntry& h = hazards[i];
		writeU32(out, h.net_id);
		writeString(out, h.animation_name);
		writeU32(out, h.power_index);
		writeU8(out, h.owner_id);
	}
	return out;
}

bool decodeHazardSpawn(const std::string& payload, MsgHazardSpawn& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_HAZARD_SPAWN)
		return false;

	uint16_t count;
	if (!readU16(payload, offset, count))
		return false;

	out.hazards.clear();
	for (uint16_t i = 0; i < count; ++i) {
		HazardSpawnEntry h;
		if (!readU32(payload, offset, h.net_id)
		    || !readString(payload, offset, h.animation_name)
		    || !readU32(payload, offset, h.power_index)
		    || !readU8(payload, offset, h.owner_id))
			return false;
		out.hazards.push_back(h);
	}

	return true;
}

std::string encodeHazardSnapshot(const std::vector<HazardSnapshotEntry>& hazards) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_HAZARD_SNAPSHOT));
	writeU16(out, static_cast<uint16_t>(hazards.size()));
	for (size_t i = 0; i < hazards.size(); ++i) {
		const HazardSnapshotEntry& h = hazards[i];
		writeU32(out, h.net_id);
		writeFloat(out, h.pos_x);
		writeFloat(out, h.pos_y);
		writeU8(out, h.direction);
		writeU32(out, static_cast<uint32_t>(h.lifespan));
		writeU32(out, static_cast<uint32_t>(h.delay_frames));
	}
	return out;
}

bool decodeHazardSnapshot(const std::string& payload, MsgHazardSnapshot& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_HAZARD_SNAPSHOT)
		return false;

	uint16_t count;
	if (!readU16(payload, offset, count))
		return false;

	out.hazards.clear();
	for (uint16_t i = 0; i < count; ++i) {
		HazardSnapshotEntry h;
		uint32_t lifespan32, delay32;
		if (!readU32(payload, offset, h.net_id)
		    || !readFloat(payload, offset, h.pos_x)
		    || !readFloat(payload, offset, h.pos_y)
		    || !readU8(payload, offset, h.direction)
		    || !readU32(payload, offset, lifespan32)
		    || !readU32(payload, offset, delay32))
			return false;
		h.lifespan = static_cast<int32_t>(lifespan32);
		h.delay_frames = static_cast<int32_t>(delay32);
		out.hazards.push_back(h);
	}

	return true;
}

std::string encodeLootSpawn(const std::vector<LootSpawnEntry>& loot) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_LOOT_SPAWN));
	writeU16(out, static_cast<uint16_t>(loot.size()));
	for (size_t i = 0; i < loot.size(); ++i) {
		const LootSpawnEntry& l = loot[i];
		writeU32(out, l.net_id);
		writeU32(out, l.item);
		writeU8(out, l.dropped_by_hero ? 1 : 0);
	}
	return out;
}

bool decodeLootSpawn(const std::string& payload, MsgLootSpawn& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_LOOT_SPAWN)
		return false;

	uint16_t count;
	if (!readU16(payload, offset, count))
		return false;

	out.loot.clear();
	for (uint16_t i = 0; i < count; ++i) {
		LootSpawnEntry l;
		uint8_t dropped_byte;
		if (!readU32(payload, offset, l.net_id)
		    || !readU32(payload, offset, l.item)
		    || !readU8(payload, offset, dropped_byte))
			return false;
		l.dropped_by_hero = dropped_byte != 0;
		out.loot.push_back(l);
	}

	return true;
}

std::string encodeLootSnapshot(const std::vector<LootSnapshotEntry>& loot) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_LOOT_SNAPSHOT));
	writeU16(out, static_cast<uint16_t>(loot.size()));
	for (size_t i = 0; i < loot.size(); ++i) {
		const LootSnapshotEntry& l = loot[i];
		writeU32(out, l.net_id);
		writeFloat(out, l.pos_x);
		writeFloat(out, l.pos_y);
		writeU32(out, static_cast<uint32_t>(l.quantity));
		writeU8(out, l.on_ground ? 1 : 0);
	}
	return out;
}

bool decodeLootSnapshot(const std::string& payload, MsgLootSnapshot& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_LOOT_SNAPSHOT)
		return false;

	uint16_t count;
	if (!readU16(payload, offset, count))
		return false;

	out.loot.clear();
	for (uint16_t i = 0; i < count; ++i) {
		LootSnapshotEntry l;
		uint32_t quantity32;
		uint8_t on_ground_byte;
		if (!readU32(payload, offset, l.net_id)
		    || !readFloat(payload, offset, l.pos_x)
		    || !readFloat(payload, offset, l.pos_y)
		    || !readU32(payload, offset, quantity32)
		    || !readU8(payload, offset, on_ground_byte))
			return false;
		l.quantity = static_cast<int32_t>(quantity32);
		l.on_ground = on_ground_byte != 0;
		out.loot.push_back(l);
	}

	return true;
}

MsgPlayerEvent::MsgPlayerEvent()
	: target(0)
	, event_type(0)
	, text()
	, log_msg_type(0)
	, pos_x(0.f), pos_y(0.f)
	, amount(0.f)
	, is_number(false)
	, displaytype(0)
	, source_is_target_itself(false)
	, sfx_type(0)
	, chosen_sound(0)
	, use_pos(false)
{
}

std::string encodePlayerEvent(const MsgPlayerEvent& event) {
	std::string out;
	writeU8(out, static_cast<uint8_t>(MSG_PLAYER_EVENT));
	writeU8(out, event.target);
	writeU8(out, event.event_type);

	switch (event.event_type) {
		case PLAYER_EVENT_LOG_MESSAGE:
			writeString(out, event.text);
			writeU8(out, event.log_msg_type);
			break;
		case PLAYER_EVENT_COMBAT_TEXT:
			writeFloat(out, event.pos_x);
			writeFloat(out, event.pos_y);
			writeFloat(out, event.amount);
			writeU8(out, event.is_number ? 1 : 0);
			writeString(out, event.text);
			writeU8(out, event.displaytype);
			writeU8(out, event.source_is_target_itself ? 1 : 0);
			break;
		case PLAYER_EVENT_SOUND:
			writeU8(out, event.sfx_type);
			writeU32(out, event.chosen_sound);
			writeFloat(out, event.pos_x);
			writeFloat(out, event.pos_y);
			writeU8(out, event.use_pos ? 1 : 0);
			break;
		case PLAYER_EVENT_LEVEL_UP:
		case PLAYER_EVENT_DEATH:
		case PLAYER_EVENT_RESPEC:
			// no payload -- the recipient's own next MSG_PLAYER_SNAPSHOT already carries the new
			// level/stats this event announces; see this struct's own header comment on why
			// level-up's log text and sound travel as separate events instead. (RESPEC's own
			// powers_list/actionbar rebuild isn't wire-replicated at all -- see
			// PlayerSnapshotEntry's own header comment on the powers_list gap this plan found.)
			break;
		default:
			break;
	}

	return out;
}

bool decodePlayerEvent(const std::string& payload, MsgPlayerEvent& out) {
	size_t offset = 0;
	uint8_t type;
	if (!readU8(payload, offset, type) || type != MSG_PLAYER_EVENT)
		return false;

	if (!readU8(payload, offset, out.target) || !readU8(payload, offset, out.event_type))
		return false;

	switch (out.event_type) {
		case PLAYER_EVENT_LOG_MESSAGE:
			return readString(payload, offset, out.text) && readU8(payload, offset, out.log_msg_type);
		case PLAYER_EVENT_COMBAT_TEXT: {
			uint8_t is_number_byte, source_byte;
			if (!readFloat(payload, offset, out.pos_x)
			    || !readFloat(payload, offset, out.pos_y)
			    || !readFloat(payload, offset, out.amount)
			    || !readU8(payload, offset, is_number_byte)
			    || !readString(payload, offset, out.text)
			    || !readU8(payload, offset, out.displaytype)
			    || !readU8(payload, offset, source_byte))
				return false;
			out.is_number = is_number_byte != 0;
			out.source_is_target_itself = source_byte != 0;
			return true;
		}
		case PLAYER_EVENT_SOUND: {
			uint8_t use_pos_byte;
			if (!readU8(payload, offset, out.sfx_type)
			    || !readU32(payload, offset, out.chosen_sound)
			    || !readFloat(payload, offset, out.pos_x)
			    || !readFloat(payload, offset, out.pos_y)
			    || !readU8(payload, offset, use_pos_byte))
				return false;
			out.use_pos = use_pos_byte != 0;
			return true;
		}
		case PLAYER_EVENT_LEVEL_UP:
		case PLAYER_EVENT_DEATH:
		case PLAYER_EVENT_RESPEC:
			return true; // no payload
		default:
			return false; // unrecognised event type -- malformed, not a forward-compat case here
	}
}

uint8_t peekMessageType(const std::string& payload) {
	if (payload.empty())
		return 0;
	return static_cast<uint8_t>(payload[0]);
}

std::string debugDump(const std::string& payload) {
	uint8_t type = peekMessageType(payload);
	char header[64];

	switch (type) {
		case MSG_HELLO: {
			MsgHello m;
			if (!decodeHello(payload, m))
				return "<truncated:HELLO>";
			snprintf(header, sizeof(header), "HELLO proto=%u engine=%u.%u.%u mod_hash=0x%08x name=\"",
			         static_cast<unsigned>(m.protocol_version), static_cast<unsigned>(m.engine_x),
			         static_cast<unsigned>(m.engine_y), static_cast<unsigned>(m.engine_z), m.mod_hash);
			return std::string(header) + m.display_name + "\"";
		}
		case MSG_HELLO_OK: {
			MsgHelloOk m;
			if (!decodeHelloOk(payload, m))
				return "<truncated:HELLO_OK>";
			snprintf(header, sizeof(header), "HELLO_OK id=%u", static_cast<unsigned>(m.assigned_id));
			return std::string(header);
		}
		case MSG_REFUSED: {
			MsgRefused m;
			if (!decodeRefused(payload, m))
				return "<truncated:REFUSED>";
			snprintf(header, sizeof(header), "REFUSED reason=%u key=\"", static_cast<unsigned>(m.reason));
			return std::string(header) + m.message_key + "\"";
		}
		case MSG_PLAYER_COMMAND: {
			PlayerCommand cmd;
			if (!decodePlayerCommand(payload, cmd))
				return "<truncated:PLAYER_COMMAND>";
			snprintf(header, sizeof(header), "PLAYER_COMMAND actions=%u", static_cast<unsigned>(cmd.actions.size()));
			return std::string(header);
		}
		case MSG_SYSTEM_MESSAGE: {
			MsgSystemMessage m;
			if (!decodeSystemMessage(payload, m))
				return "<truncated:SYSTEM_MESSAGE>";
			snprintf(header, sizeof(header), "SYSTEM_MESSAGE args=%u key=\"", static_cast<unsigned>(m.args.size()));
			return std::string(header) + m.message_key + "\"";
		}
		case MSG_PLAYER_SNAPSHOT: {
			MsgPlayerSnapshot m;
			if (!decodePlayerSnapshot(payload, m))
				return "<truncated:PLAYER_SNAPSHOT>";
			snprintf(header, sizeof(header), "PLAYER_SNAPSHOT players=%u", static_cast<unsigned>(m.players.size()));
			return std::string(header);
		}
		case MSG_MAP_SYNC: {
			MsgMapSync m;
			if (!decodeMapSync(payload, m))
				return "<truncated:MAP_SYNC>";
			snprintf(header, sizeof(header), "MAP_SYNC spawn=(%.1f,%.1f) map=\"", static_cast<double>(m.spawn_x), static_cast<double>(m.spawn_y));
			return std::string(header) + m.map_filename + "\"";
		}
		case MSG_ENTITY_SPAWN: {
			MsgEntitySpawn m;
			if (!decodeEntitySpawn(payload, m))
				return "<truncated:ENTITY_SPAWN>";
			snprintf(header, sizeof(header), "ENTITY_SPAWN entities=%u", static_cast<unsigned>(m.entities.size()));
			return std::string(header);
		}
		case MSG_ENTITY_SNAPSHOT: {
			MsgEntitySnapshot m;
			if (!decodeEntitySnapshot(payload, m))
				return "<truncated:ENTITY_SNAPSHOT>";
			snprintf(header, sizeof(header), "ENTITY_SNAPSHOT entities=%u", static_cast<unsigned>(m.entities.size()));
			return std::string(header);
		}
		default:
			return payload.empty() ? "<empty>" : "<unknown>";
	}
}

uint32_t hashModList(const std::vector<Mod>& mods) {
	uint32_t hash = 2166136261u;
	for (size_t i = 0; i < mods.size(); ++i) {
		std::string entry = mods[i].name + "@";
		if (mods[i].version) {
			char vbuf[32];
			snprintf(vbuf, sizeof(vbuf), "%u.%u.%u",
			         static_cast<unsigned>(mods[i].version->x),
			         static_cast<unsigned>(mods[i].version->y),
			         static_cast<unsigned>(mods[i].version->z));
			entry += vbuf;
		}
		entry += ";";
		for (size_t c = 0; c < entry.size(); ++c) {
			hash ^= static_cast<uint8_t>(entry[c]);
			hash *= 16777619u;
		}
	}
	return hash;
}

} // namespace Net
