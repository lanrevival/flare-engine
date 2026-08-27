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

#include "WorldHash.h"

#include "Avatar.h"
#include "CampaignManager.h"
#include "Entity.h"
#include "EntityManager.h"
#include "Hazard.h"
#include "HazardManager.h"
#include "Loot.h"
#include "LootManager.h"
#include "Map.h"
#include "MapRenderer.h"
#include "MenuInventory.h"
#include "MenuManager.h"
#include "PlayerInventory.h"
#include "SharedGameResources.h"
#include "StatBlock.h"
#include "Utils.h"

#include <cstdio>
#include <cstring>

namespace {
	const uint64_t FNV_OFFSET_BASIS = 0xCBF29CE484222325ULL;
	const uint64_t FNV_PRIME        = 0x100000001B3ULL;

	// Section tags. Mixed between sections so that moving a value from one section to another
	// changes the digest -- without them, an empty entity list followed by one hazard would
	// hash the same as one entity followed by an empty hazard list.
	enum {
		TAG_HEADER = 1, TAG_PLAYER, TAG_INVENTORY, TAG_ENTITIES,
		TAG_HAZARDS, TAG_LOOT, TAG_CAMPAIGN, TAG_END
	};
}

uint64_t WorldHash::init() {
	return FNV_OFFSET_BASIS;
}

uint64_t WorldHash::mixBytes(uint64_t h, const void* data, size_t len) {
	const unsigned char* p = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < len; ++i) {
		h ^= static_cast<uint64_t>(p[i]);
		h *= FNV_PRIME;
	}
	return h;
}

uint64_t WorldHash::mixU64(uint64_t h, uint64_t v) {
	// Fixed little-endian byte order, not memcpy of the native layout: a digest that changes
	// with the host's endianness is useless for comparing two machines.
	unsigned char b[8];
	for (int i = 0; i < 8; ++i)
		b[i] = static_cast<unsigned char>((v >> (i * 8)) & 0xFF);
	return mixBytes(h, b, 8);
}

uint64_t WorldHash::mixI32(uint64_t h, int32_t v) {
	return mixU64(h, static_cast<uint64_t>(static_cast<uint32_t>(v)));
}

uint64_t WorldHash::mixFloat(uint64_t h, float v) {
	// Bit pattern, never text. Two canonicalisations so that values which compare equal always
	// hash equal:
	//   -0.0 == 0.0 but has a different bit pattern
	//   NaN != NaN, and there are millions of NaN bit patterns
	if (v == 0.0f)
		v = 0.0f;
	uint32_t bits;
	if (v != v)
		bits = 0x7FC00000U;             // one canonical quiet NaN
	else
		memcpy(&bits, &v, sizeof(bits));
	return mixU64(h, static_cast<uint64_t>(bits));
}

uint64_t WorldHash::mixString(uint64_t h, const std::string& s) {
	h = mixU64(h, static_cast<uint64_t>(s.size()));
	if (!s.empty())
		h = mixBytes(h, s.data(), s.size());
	return h;
}

namespace {
	uint64_t mixStatBlock(uint64_t h, const StatBlock& s) {
		h = WorldHash::mixFloat(h, s.pos.x);
		h = WorldHash::mixFloat(h, s.pos.y);
		h = WorldHash::mixFloat(h, s.hp);
		h = WorldHash::mixFloat(h, s.mp);
		h = WorldHash::mixU64(h, static_cast<uint64_t>(s.direction));
		h = WorldHash::mixI32(h, s.alive ? 1 : 0);
		h = WorldHash::mixI32(h, s.corpse ? 1 : 0);
		return h;
	}
}

uint64_t WorldHash::compute(unsigned long tick) {
	uint64_t h = init();

	// --- header ---
	h = mixI32(h, TAG_HEADER);
	h = mixU64(h, static_cast<uint64_t>(tick));
	// wmap, not mapr: getFilename() reads Map-owned data (P1.4a) and wmap is never NULL, client
	// or headless. mapr is NULL on a headless server (P1.4c), so this unconditionally hashed an
	// empty string there -- every tick's digest was blind to which map was even loaded, the same
	// shape of gap as the menu/inv guard fixed alongside the P1.4a-gap commit, just missed then
	// because nothing had made mapr NULL yet to expose it. Found by bisecting a full-corpus
	// digest mismatch down to a state that matched bit-for-bit everywhere else this file hashes.
	h = mixString(h, wmap ? wmap->getFilename() : std::string());

	// --- player ---
	// P2.3b: kind C, not kind A -- this used to hash only the single global pc/pinv, which is
	// exactly the "player singleton" blind spot the comment below already warns against, one level
	// deeper than the reference migration it was written for. playerm->players is kept sorted by
	// id (PlayerManager.h), so this iteration order is stable and reproducible -- required, since
	// every consumer of this digest depends on it. With exactly one player (every corpus fixture
	// today) this is byte-identical to the old pc/pinv-guarded version: same tag positions, same
	// values, same order.
	h = mixI32(h, TAG_PLAYER);
	for (size_t p = 0; p < playerm->players.size(); ++p) {
		h = mixStatBlock(h, playerm->players[p]->stats);
		h = mixU64(h, static_cast<uint64_t>(playerm->players[p]->stats.xp));
		h = mixI32(h, playerm->players[p]->stats.currency);
	}

	// --- inventory ---
	// Covered on purpose. Phase 2 rewrites hundreds of references to the player singleton; a
	// digest that stopped at positions would pass all of it.
	h = mixI32(h, TAG_INVENTORY);
	// Iterates playerm->inventories now, not a single pinv guard -- same P2.3b reasoning as
	// TAG_PLAYER above. The guard used to ask about the menu instead (menu && menu->inv), which
	// happened to hold whenever pinv did because nothing constructed one without the other --
	// until P1.4c, where a headless server builds pinv with no menu at all. That silently would
	// have dropped this whole block, and with it the corpus's only coverage of equipment/inventory
	// contents: found by reading this file while designing P1.4c's server loop, not by a failing
	// digest, because a skipped block still hashes identically to another skipped block.
	for (size_t p = 0; p < playerm->inventories.size(); ++p) {
		PlayerInventory* inventory = playerm->inventories[p];

		// Which equipment set is active, not just what is in the slots. Measured gap: a probe
		// that vanished items from the digest showed contents ARE covered (melee notices a loss
		// in both storage areas), but this scalar was not hashed at all, so a swap between two
		// equally-full sets was invisible.
		h = mixI32(h, static_cast<int32_t>(inventory->active_equipment_set));

		for (int area = 0; area < MenuInventory::CARRIED + 1; ++area) {
			int slots = inventory->inventory[area].getSlotNumber();
			h = mixI32(h, slots);
			for (int i = 0; i < slots; ++i) {
				h = mixU64(h, static_cast<uint64_t>(inventory->inventory[area][i].item));
				h = mixI32(h, inventory->inventory[area][i].quantity);
			}
		}
	}

	// --- entities, in container order ---
	h = mixI32(h, TAG_ENTITIES);
	if (entitym) {
		h = mixU64(h, static_cast<uint64_t>(entitym->entities.size()));
		for (size_t i = 0; i < entitym->entities.size(); ++i) {
			if (!entitym->entities[i])
				continue;
			h = mixStatBlock(h, entitym->entities[i]->stats);
			// Entities only -- the player goes through mixStatBlock() too and the flag is
			// meaningless there. Covered so that an activation divergence shows up on the tick
			// it happens instead of hundreds of ticks later as a position difference.
			h = mixI32(h, entitym->entities[i]->stats.encountered ? 1 : 0);
		}
	}

	// --- hazards ---
	h = mixI32(h, TAG_HAZARDS);
	if (hazards) {
		h = mixU64(h, static_cast<uint64_t>(hazards->h.size()));
		for (size_t i = 0; i < hazards->h.size(); ++i) {
			const Hazard* z = hazards->h[i];
			if (!z)
				continue;
			h = mixFloat(h, z->pos.x);
			h = mixFloat(h, z->pos.y);
			h = mixFloat(h, z->speed.x);
			h = mixFloat(h, z->speed.y);
			h = mixI32(h, z->lifespan);
			h = mixI32(h, z->active ? 1 : 0);
		}
	}

	// --- floor loot ---
	h = mixI32(h, TAG_LOOT);
	if (loot) {
		h = mixU64(h, static_cast<uint64_t>(loot->loot.size()));
		for (std::vector<Loot>::const_iterator it = loot->loot.begin(); it != loot->loot.end(); ++it) {
			h = mixFloat(h, it->pos.x);
			h = mixFloat(h, it->pos.y);
			h = mixU64(h, static_cast<uint64_t>(it->stack.item));
			h = mixI32(h, it->stack.quantity);
		}
	}

	// --- campaign statuses ---
	// std::map, so iteration is key-ordered and deterministic. Quest state must be covered or a
	// refactor that breaks it passes green.
	h = mixI32(h, TAG_CAMPAIGN);
	if (camp) {
		h = mixU64(h, static_cast<uint64_t>(camp->status.size()));
		for (CampaignManager::StatusMap::const_iterator it = camp->status.begin(); it != camp->status.end(); ++it) {
			h = mixU64(h, static_cast<uint64_t>(it->first));
			h = mixI32(h, it->second.first ? 1 : 0);
		}
	}

	h = mixI32(h, TAG_END);
	return h;
}

std::string WorldHash::toString(uint64_t h) {
	char buf[32];
	snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(h));
	return std::string(buf);
}
