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

#include "Animation.h"
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
#include "Stats.h"
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

// P3.7 (players), P3.9 (entities, appended below the player section), P3.11a (players extended
// with mp/xp/level/currency/effects/power cooldown+cast ticks), P3.11b (players extended again
// with inventory content). Field set mirrors Net::PlayerSnapshotEntry/Net::InventoryEntry/
// Net::EntitySnapshotEntry exactly (net/NetProtocol.h) and the construction in
// serverBroadcastSnapshot() (main_server.cpp) / GameStatePlay.cpp's own snapshot build -- so a
// value that diverges here is, by construction, a value that would also diverge on the wire.
// Still excludes hazards/loot/campaign, and also stats.powers_list/ActionBarState (see
// PlayerSnapshotEntry's own header comment for why) -- see WorldHash.h's doc comment on this
// function for why the rest are excluded.
uint64_t WorldHash::computeReplicated(unsigned long tick, int exclude_id) {
	uint64_t h = init();

	h = mixI32(h, TAG_HEADER);
	h = mixU64(h, static_cast<uint64_t>(tick));

	h = mixI32(h, TAG_PLAYER);
	// Sorted by player_net_id, not iterated in playerm->players' own order: that order is
	// construction order, which differs by process (a client's own avatar is always playerm index
	// 0 regardless of its network id; a server's is index 0 only because network id 0 happens to be
	// what it always is). player_net_id -- not av->id -- is what's actually comparable across
	// processes; see Avatar::player_net_id's own comment. Small (<=8 per D3), so a plain
	// insertion-sorted copy is simplest.
	//
	// P3.8b: exclude_id (keyed on av->id, not player_net_id -- see this function's own header
	// comment) is filtered out while building the copy, not after sorting, so it never occupies a
	// slot at all.
	std::vector<Avatar*> by_net_id;
	for (size_t i = 0; i < playerm->players.size(); ++i) {
		Avatar* av = playerm->players[i];
		if (exclude_id >= 0 && av->id == static_cast<uint8_t>(exclude_id))
			continue;
		by_net_id.push_back(av);
	}
	for (size_t i = 1; i < by_net_id.size(); ++i) {
		Avatar* key = by_net_id[i];
		size_t j = i;
		while (j > 0 && by_net_id[j - 1]->player_net_id > key->player_net_id) {
			by_net_id[j] = by_net_id[j - 1];
			--j;
		}
		by_net_id[j] = key;
	}
	for (size_t p = 0; p < by_net_id.size(); ++p) {
		Avatar* av = by_net_id[p];
		h = mixU64(h, static_cast<uint64_t>(av->player_net_id));
		h = mixFloat(h, av->stats.pos.x);
		h = mixFloat(h, av->stats.pos.y);
		h = mixU64(h, static_cast<uint64_t>(av->stats.direction));
		h = mixString(h, av->activeAnimation ? av->activeAnimation->getName() : std::string());
		h = mixFloat(h, av->stats.hp);
		h = mixFloat(h, av->stats.get(Stats::HP_MAX));
		h = mixI32(h, av->stats.alive ? 1 : 0);

		// P3.11a additions below. mp/xp/level/currency are plain scalars, same as hp/hp_max above.
		h = mixFloat(h, av->stats.mp);
		h = mixFloat(h, av->stats.get(Stats::MP_MAX));
		h = mixU64(h, static_cast<uint64_t>(av->stats.xp));
		h = mixI32(h, av->stats.level);
		h = mixI32(h, av->stats.currency);

		// Sorted by id: EffectManager::effect_list's own insertion order isn't guaranteed identical
		// across processes (same "container order isn't a cross-process invariant" reasoning as the
		// player list itself, just above) -- an unsorted mix here could report a false digest
		// divergence for two lists holding the identical set of effects in a different order.
		std::vector<Effect> effects_sorted(av->stats.effects.effect_list);
		for (size_t i = 1; i < effects_sorted.size(); ++i) {
			Effect key = effects_sorted[i];
			size_t j = i;
			while (j > 0 && effects_sorted[j - 1].id > key.id) {
				effects_sorted[j] = effects_sorted[j - 1];
				--j;
			}
			effects_sorted[j] = key;
		}
		h = mixU64(h, static_cast<uint64_t>(effects_sorted.size()));
		for (size_t i = 0; i < effects_sorted.size(); ++i) {
			h = mixString(h, effects_sorted[i].id);
			h = mixFloat(h, effects_sorted[i].magnitude);
			h = mixU64(h, static_cast<uint64_t>(effects_sorted[i].timer.getCurrent()));
		}

		// Indexed by PowerID, not insertion order -- both vectors are sized to the same static,
		// mod-matched powers->powers.size() on every process, so direct index order is already a
		// stable cross-process key with no sort needed (unlike effect_list above). NULL for a
		// reserved-but-unallocated PowerID (see serverBroadcastSnapshot()'s matching comment) --
		// mixed as 0, same as every other process sees for that same always-NULL index.
		for (size_t i = 0; i < av->power_cooldown_timers.size(); ++i)
			h = mixU64(h, av->power_cooldown_timers[i] ? static_cast<uint64_t>(av->power_cooldown_timers[i]->getCurrent()) : 0);
		for (size_t i = 0; i < av->power_cast_timers.size(); ++i)
			h = mixU64(h, av->power_cast_timers[i] ? static_cast<uint64_t>(av->power_cast_timers[i]->getCurrent()) : 0);

		// stats.powers_list and ActionBarState's hotkeys are deliberately NOT mixed in here -- see
		// PlayerSnapshotEntry's own header comment (net/NetProtocol.h) for the MenuPowers
		// auto-unlock divergence this plan found (a 100% digest mismatch, bisected to exactly this
		// field) and left as a follow-up rather than fix in this plan's own scope.

		// P3.11b. Field set mirrors Net::InventoryEntry exactly (net/NetProtocol.h). Indexed by
		// slot, not sorted -- both EQUIPMENT and CARRIED are plain arrays sized identically by mod
		// data (mod_hash handshake) on every process, already a stable cross-process key, same
		// reasoning as the cooldown/cast arrays just above (unlike effect_list, which needed a
		// sort). currency is NOT mixed again here -- av->stats.currency, already mixed above, is
		// the same value PlayerInventory::recomputeCurrency() keeps it in sync with.
		PlayerInventory* inv = playerm->inventoryFor(av->id);
		if (inv) {
			for (int s = 0; s < inv->MAX_EQUIPPED; ++s) {
				h = mixU64(h, static_cast<uint64_t>(inv->inventory[PlayerInventory::EQUIPMENT][s].item));
				h = mixI32(h, inv->inventory[PlayerInventory::EQUIPMENT][s].quantity);
			}
			for (int s = 0; s < inv->MAX_CARRIED; ++s) {
				h = mixU64(h, static_cast<uint64_t>(inv->inventory[PlayerInventory::CARRIED][s].item));
				h = mixI32(h, inv->inventory[PlayerInventory::CARRIED][s].quantity);
			}
			h = mixI32(h, static_cast<int32_t>(inv->active_equipment_set));
		}
	}

	// P3.9. Field set mirrors Net::EntitySnapshotEntry exactly (net/NetProtocol.h). Sorted by
	// net_id for the same cross-process reason the player section above is -- see this function's
	// own header comment and WorldHash.h's doc comment. NPCs (stats.npc) are excluded: they are not
	// wire-replicated at all, general position/hp/state included -- same exclusion
	// EntityManager::handleNewMap()'s own delete loop already applies to entities. P3.11c added
	// per-player TalkState (PlayerManager.h) so a dialogue node's REWARD_ITEM/SET_STATUS/etc. runs
	// exactly once, server-side, correctly attributed -- it did NOT add general NPC entity
	// replication (position/hp/animation), which remains this same open gap, unrelated in scope
	// (NPCManager.h/.cpp are not in that plan's Files in scope). TalkState itself is deliberately
	// NOT mixed into this digest -- see PlayerInventory's own inventory section below for the
	// precedent this follows: which dialogue node is currently being rendered is presentation state
	// two mirrors need not agree on bit-for-bit, only the node's *effects* (items/status/currency),
	// which already flow through the already-digested channels just above (matches
	// stats.powers_list/ActionBarState hotkeys' own exclusion reasoning, a few lines up).
	h = mixI32(h, TAG_ENTITIES);
	if (entitym) {
		std::vector<Entity*> entities_by_net_id;
		for (size_t i = 0; i < entitym->entities.size(); ++i) {
			Entity* e = entitym->entities[i];
			if (!e || e->stats.npc)
				continue;
			entities_by_net_id.push_back(e);
		}
		for (size_t i = 1; i < entities_by_net_id.size(); ++i) {
			Entity* key = entities_by_net_id[i];
			size_t j = i;
			while (j > 0 && entities_by_net_id[j - 1]->net_id > key->net_id) {
				entities_by_net_id[j] = entities_by_net_id[j - 1];
				--j;
			}
			entities_by_net_id[j] = key;
		}
		for (size_t p = 0; p < entities_by_net_id.size(); ++p) {
			Entity* e = entities_by_net_id[p];
			h = mixU64(h, static_cast<uint64_t>(e->net_id));
			h = mixFloat(h, e->stats.pos.x);
			h = mixFloat(h, e->stats.pos.y);
			h = mixU64(h, static_cast<uint64_t>(e->stats.direction));
			h = mixI32(h, e->stats.cur_state);
			h = mixString(h, e->activeAnimation ? e->activeAnimation->getName() : std::string());
			h = mixFloat(h, e->stats.hp);
			h = mixFloat(h, e->stats.get(Stats::HP_MAX));
			h = mixI32(h, e->stats.alive ? 1 : 0);
			h = mixI32(h, e->stats.corpse ? 1 : 0);
		}
	}

	// P3.10. Field set mirrors Net::HazardSnapshotEntry exactly. Sorted by net_id, same
	// cross-process reason as the player/entity sections above.
	h = mixI32(h, TAG_HAZARDS);
	if (hazards) {
		std::vector<Hazard*> hazards_by_net_id;
		for (size_t i = 0; i < hazards->h.size(); ++i) {
			if (hazards->h[i])
				hazards_by_net_id.push_back(hazards->h[i]);
		}
		for (size_t i = 1; i < hazards_by_net_id.size(); ++i) {
			Hazard* key = hazards_by_net_id[i];
			size_t j = i;
			while (j > 0 && hazards_by_net_id[j - 1]->net_id > key->net_id) {
				hazards_by_net_id[j] = hazards_by_net_id[j - 1];
				--j;
			}
			hazards_by_net_id[j] = key;
		}
		for (size_t p = 0; p < hazards_by_net_id.size(); ++p) {
			Hazard* z = hazards_by_net_id[p];
			h = mixU64(h, static_cast<uint64_t>(z->net_id));
			h = mixFloat(h, z->pos.x);
			h = mixFloat(h, z->pos.y);
			h = mixU64(h, static_cast<uint64_t>(z->direction));
			h = mixI32(h, z->lifespan);
			h = mixI32(h, z->delay_frames);
		}
	}

	// P3.10. Field set mirrors Net::LootSnapshotEntry exactly. Sorted by net_id, same reason.
	h = mixI32(h, TAG_LOOT);
	if (loot) {
		std::vector<const Loot*> loot_by_net_id;
		for (size_t i = 0; i < loot->loot.size(); ++i) {
			loot_by_net_id.push_back(&loot->loot[i]);
		}
		for (size_t i = 1; i < loot_by_net_id.size(); ++i) {
			const Loot* key = loot_by_net_id[i];
			size_t j = i;
			while (j > 0 && loot_by_net_id[j - 1]->net_id > key->net_id) {
				loot_by_net_id[j] = loot_by_net_id[j - 1];
				--j;
			}
			loot_by_net_id[j] = key;
		}
		for (size_t p = 0; p < loot_by_net_id.size(); ++p) {
			const Loot* ld = loot_by_net_id[p];
			h = mixU64(h, static_cast<uint64_t>(ld->net_id));
			h = mixFloat(h, ld->pos.x);
			h = mixFloat(h, ld->pos.y);
			h = mixI32(h, ld->stack.quantity);
			h = mixI32(h, ld->on_ground ? 1 : 0);
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
