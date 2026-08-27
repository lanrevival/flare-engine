/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Stefan Beller
Copyright © 2013 Henrik Andersson
Copyright © 2012-2016 Justin Jacobs

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
 * class CampaignManager
 *
 * Contains data for story mode
 */

#include "Avatar.h"
#include "CampaignManager.h"
#include "CommonIncludes.h"
#include "EngineSettings.h"
#include "EventManager.h"
#include "MapRenderer.h"
#include "Menu.h"
#include "MenuManager.h"
#include "MenuInventory.h"
#include "MessageEngine.h"
#include "PlayerInventory.h"
#include "PlayerManager.h"
#include "Rng.h"
#include "SharedGameResources.h"
#include "SharedResources.h"
#include "StatBlock.h"
#include "UtilsMath.h"
#include "UtilsParsing.h"

// See the header comment on the kind-A methods below. NULL falls back to playerm->local(), the
// pre-P2.2 single-player behavior; a caller migrated to know the real triggering player (today
// just EventManager.cpp) passes it explicitly. Returns NULL only if there is truly no local
// player either (a dedicated server with no local_id set) -- callers must check.
static Avatar* resolvePlayer(Avatar* triggered_by) {
	return triggered_by ? triggered_by : playerm->local();
}

CampaignManager::CampaignManager()
	: bonus_xp(0.0)
	, random_status(0) {
}

StatusID CampaignManager::registerStatus(const std::string& s) {
	if (s.empty())
		return 0;

	StatusID new_id = Utils::hashString(s);

	// check if this status was already registered
	StatusMap::iterator it;
	it = status.find(new_id);
	if (it != status.end())
		return it->first;

	// register a new status
	status[new_id].first = false;
	status[new_id].second = s;
	return new_id;
}

/**
 * Take the savefile campaign= and convert to status array
 */
void CampaignManager::setAll(const std::string& s) {
	std::string str = s + ',';
	std::string token;
	while (!str.empty()) {
		token = Parse::popFirstString(str);
		if (!token.empty())
			setStatus(registerStatus(token));
	}
}

/**
 * Convert status array to savefile campaign= (status csv)
 */
std::string CampaignManager::getAll() {
	std::string output("");

	StatusMap::iterator it;
	for (it = status.begin(); it != status.end(); ++it) {
		if (it->second.first)
			output += it->second.second;

		StatusMap::iterator temp = it;
		++temp;
		if (temp != status.end() && temp->second.first) {
			output += ',';
		}
	}
	return output;
}

bool CampaignManager::checkStatus(const StatusID s) {
	StatusMap::iterator it;
	it = status.find(s);
	if (it != status.end() && it->second.first)
		return true;

	return false;
}

void CampaignManager::setStatus(const StatusID s) {
	// if it's already set, don't set it again
	if (checkStatus(s)) return;

	status[s].first = true;

	// kind C: campaign status is party-wide (D5), so every player's title needs re-checking
	// against it, not just whichever one happens to be local().
	for (size_t i = 0; i < playerm->players.size(); ++i)
		playerm->players[i]->stats.check_title = true;
}

void CampaignManager::unsetStatus(const StatusID s) {
	// if it's already unset, don't unset it again
	if (!checkStatus(s)) return;

	status[s].first = false;

	// kind C -- see setStatus() above.
	for (size_t i = 0; i < playerm->players.size(); ++i)
		playerm->players[i]->stats.check_title = true;
}

void CampaignManager::resetAllStatuses() {
	StatusMap::iterator it;
	for (it = status.begin(); it != status.end(); ++it) {
		it->second.first = false;
	}
}

void CampaignManager::getSetStatusStrings(std::vector<std::string>& status_strings) {
	StatusMap::iterator it;
	for (it = status.begin(); it != status.end(); ++it) {
		if (it->second.first)
			status_strings.push_back(it->second.second);
	}
}

bool CampaignManager::checkCurrency(int quantity, Avatar* triggered_by) {
	Avatar* target = resolvePlayer(triggered_by);
	if (!target) return false;
	PlayerInventory* target_inv = playerm->inventoryFor(target->id);
	if (!target_inv) return false;

	return target_inv->inventory[PlayerInventory::CARRIED].contain(eset->misc.currency_id, quantity);
}

bool CampaignManager::checkItem(ItemStack istack, Avatar* triggered_by) {
	Avatar* target = resolvePlayer(triggered_by);
	if (!target) return false;
	PlayerInventory* target_inv = playerm->inventoryFor(target->id);
	if (!target_inv) return false;

	if (target_inv->inventory[PlayerInventory::CARRIED].contain(istack.item, istack.quantity))
		return true;
	else
		return target_inv->equipmentContain(istack.item, istack.quantity);
}

void CampaignManager::removeCurrency(int quantity, Avatar* triggered_by) {
	Avatar* target = resolvePlayer(triggered_by);
	if (!target) return;
	PlayerInventory* target_inv = playerm->inventoryFor(target->id);
	if (!target_inv) return;

	int max_amount = std::min(quantity, target_inv->currency);

	if (max_amount > 0) {
		target_inv->removeCurrency(max_amount);
		target->logMsg(msg->getv("%d %s removed.", max_amount, eset->loot.currency.c_str()), Avatar::MSG_UNIQUE);
		items->playSound(eset->misc.currency_id);
	}
}

void CampaignManager::removeItem(ItemStack istack, Avatar* triggered_by) {
	if (istack.empty())
		return;

	if (istack.item == eset->misc.currency_id) {
		removeCurrency(istack.quantity, triggered_by);
		return;
	}

	Avatar* target = resolvePlayer(triggered_by);
	if (!target) return;
	PlayerInventory* target_inv = playerm->inventoryFor(target->id);
	if (!target_inv) return;

	int item_count = target_inv->inventory[PlayerInventory::CARRIED].count(istack.item) + target_inv->inventory[PlayerInventory::EQUIPMENT].count(istack.item);
	int max_amount = std::min(item_count, istack.quantity);

	if (target_inv->remove(istack.item, max_amount)) {
		if (max_amount > 1)
			target->logMsg(msg->getv("%s x%d removed.", items->getItemName(istack.item).c_str(), max_amount), Avatar::MSG_UNIQUE);
		else if (max_amount == 1)
			target->logMsg(msg->getv("%s removed.", items->getItemName(istack.item).c_str()), Avatar::MSG_UNIQUE);

		if (max_amount > 0)
			items->playSound(istack.item);
	}
}

void CampaignManager::rewardItem(ItemStack istack, Avatar* triggered_by) {
	if (istack.empty())
		return;

	Avatar* target = resolvePlayer(triggered_by);
	if (!target) return;
	PlayerInventory* target_inv = playerm->inventoryFor(target->id);
	if (!target_inv) return;

	target_inv->add(istack, PlayerInventory::CARRIED, ItemStorage::NO_SLOT, PlayerInventory::ADD_PLAY_SOUND, PlayerInventory::ADD_AUTO_EQUIP);

	if (istack.item == eset->misc.currency_id) {
		target->logMsg(msg->getv("You receive %d %s.", istack.quantity, eset->loot.currency.c_str()), Avatar::MSG_UNIQUE);
	}
	else {
		if (istack.quantity > 1)
			target->logMsg(msg->getv("You receive %s x%d.", items->getItemName(istack.item).c_str(), istack.quantity), Avatar::MSG_UNIQUE);
		else if (istack.quantity == 1)
			target->logMsg(msg->getv("You receive %s.", items->getItemName(istack.item).c_str()), Avatar::MSG_UNIQUE);
	}
}

void CampaignManager::rewardCurrency(int amount, Avatar* triggered_by) {
	ItemStack stack;
	stack.item = eset->misc.currency_id;
	stack.quantity = amount;

	rewardItem(stack, triggered_by);
}

void CampaignManager::rewardXP(float amount, bool show_message, Avatar* triggered_by) {
	Avatar* target = resolvePlayer(triggered_by);
	if (!target) return;

	if (target->block_xp_gain)
		return;

	bonus_xp += (amount * (100.0f + static_cast<float>(target->stats.get(Stats::XP_GAIN)))) / 100.0f;

	int whole_xp = static_cast<int>(bonus_xp);
	target->stats.addXP(whole_xp);
	bonus_xp -= static_cast<float>(whole_xp); // remainder

	target->stats.refresh_stats = true;

	if (show_message)
		target->logMsg(msg->getv("You receive %d XP.", static_cast<int>(amount)), Avatar::MSG_UNIQUE);
}

void CampaignManager::restoreHPMP(const std::string& s, Avatar* triggered_by) {
	Avatar* target = resolvePlayer(triggered_by);
	if (!target) return;

	std::string restore_str = s;
	std::string restore_mode = Parse::popFirstString(restore_str);

	while (!restore_mode.empty()) {
		if (restore_mode == "hp") {
			target->stats.hp = target->stats.get(Stats::HP_MAX);
			target->logMsg(msg->get("HP restored."), Avatar::MSG_UNIQUE);
		}
		else if (restore_mode == "mp") {
			target->stats.mp = target->stats.get(Stats::MP_MAX);
			target->logMsg(msg->get("MP restored."), Avatar::MSG_UNIQUE);
		}
		else if (restore_mode == "hpmp") {
			target->stats.hp = target->stats.get(Stats::HP_MAX);
			target->stats.mp = target->stats.get(Stats::MP_MAX);
			target->logMsg(msg->get("HP and MP restored."), Avatar::MSG_UNIQUE);
		}
		else if (restore_mode == "status") {
			target->stats.effects.clearNegativeEffects(Effect::RESIST_ALL);
			target->logMsg(msg->get("Negative effects removed."), Avatar::MSG_UNIQUE);
		}
		else if (restore_mode == "all") {
			target->stats.hp = target->stats.get(Stats::HP_MAX);
			target->stats.mp = target->stats.get(Stats::MP_MAX);
			target->stats.effects.clearNegativeEffects(Effect::RESIST_ALL);
			target->logMsg(msg->get("HP and MP restored, negative effects removed"), Avatar::MSG_UNIQUE);

			for (size_t i = 0; i < eset->resource_stats.list.size(); ++i) {
				target->stats.resource_stats[i] = target->stats.getResourceStat(i, EngineSettings::ResourceStats::STAT_BASE);
				target->logMsg(eset->resource_stats.list[i].text_log_restore, Avatar::MSG_UNIQUE);
			}
		}
		else {
			for (size_t i = 0; i < eset->resource_stats.list.size(); ++i) {
				if (restore_mode == eset->resource_stats.list[i].ids[EngineSettings::ResourceStats::STAT_BASE]) {
					target->stats.resource_stats[i] = target->stats.getResourceStat(i, EngineSettings::ResourceStats::STAT_BASE);
					target->logMsg(eset->resource_stats.list[i].text_log_restore, Avatar::MSG_UNIQUE);
				}
			}
		}

		restore_mode = Parse::popFirstString(restore_str);
	}
}

bool CampaignManager::checkAllRequirements(const EventComponent& ec, Avatar* triggered_by) {
	if (ec.type == EventComponent::REQUIRES_STATUS) {
		if (checkStatus(ec.status))
			return true;
	}
	else if (ec.type == EventComponent::REQUIRES_NOT_STATUS) {
		if (!checkStatus(ec.status))
			return true;
	}
	else if (ec.type == EventComponent::REQUIRES_CURRENCY) {
		if (checkCurrency(ec.data[0].Int, triggered_by))
			return true;
	}
	else if (ec.type == EventComponent::REQUIRES_NOT_CURRENCY) {
		if (!checkCurrency(ec.data[0].Int, triggered_by))
			return true;
	}
	else if (ec.type == EventComponent::REQUIRES_ITEM) {
		if (checkItem(ItemStack(ec.id, ec.data[0].Int), triggered_by))
			return true;
	}
	else if (ec.type == EventComponent::REQUIRES_NOT_ITEM) {
		if (!checkItem(ItemStack(ec.id, ec.data[0].Int), triggered_by))
			return true;
	}
	else if (ec.type == EventComponent::REQUIRES_LEVEL) {
		Avatar* target = resolvePlayer(triggered_by);
		if (target && target->stats.level >= ec.data[0].Int)
			return true;
	}
	else if (ec.type == EventComponent::REQUIRES_NOT_LEVEL) {
		Avatar* target = resolvePlayer(triggered_by);
		if (target && target->stats.level < ec.data[0].Int)
			return true;
	}
	else if (ec.type == EventComponent::REQUIRES_CLASS) {
		Avatar* target = resolvePlayer(triggered_by);
		if (target && target->stats.character_class == ec.s)
			return true;
	}
	else if (ec.type == EventComponent::REQUIRES_NOT_CLASS) {
		Avatar* target = resolvePlayer(triggered_by);
		if (target && target->stats.character_class != ec.s)
			return true;
	}
	else if (ec.type == EventComponent::REQUIRES_TILE) {
		size_t index = static_cast<size_t>(distance(wmap->layernames_hashed.begin(), find(wmap->layernames_hashed.begin(), wmap->layernames_hashed.end(), ec.id)));
		if (mapr && index < wmap->layers.size() && ec.data[0].Int >= 0 && ec.data[0].Int < wmap->w && ec.data[1].Int >= 0 && ec.data[1].Int < wmap->h)
			if (wmap->layers[index][ec.data[0].Int][ec.data[1].Int] == static_cast<unsigned short>(ec.data[2].Int))
				return true;
	}
	else if (ec.type == EventComponent::REQUIRES_NOT_TILE) {
		size_t index = static_cast<size_t>(distance(wmap->layernames_hashed.begin(), find(wmap->layernames_hashed.begin(), wmap->layernames_hashed.end(), ec.id)));
		if (mapr && index < wmap->layers.size() && ec.data[0].Int >= 0 && ec.data[0].Int < wmap->w && ec.data[1].Int >= 0 && ec.data[1].Int < wmap->h)
			if (wmap->layers[index][ec.data[0].Int][ec.data[1].Int] != static_cast<unsigned short>(ec.data[2].Int))
				return true;
	}
	else {
		// Event component is not a requirement check
		// treat it as if the "requirement" was met
		return true;
	}

	// requirement check failed
	return false;
}

bool CampaignManager::checkRequirementsInVector(const std::vector<EventComponent>& ec_vec, Avatar* triggered_by) {
	for (size_t i = 0; i < ec_vec.size(); ++i) {
		if (!checkAllRequirements(ec_vec[i], triggered_by))
			return false;
	}

	return true;
}

void CampaignManager::randomStatusAppend(const StatusID s) {
	if (std::find(random_status_pool.begin(), random_status_pool.end(), s) == random_status_pool.end()) {
		if (random_status_pool.empty())
			random_status = s;

		random_status_pool.push_back(s);
	}
}

void CampaignManager::randomStatusClear() {
	random_status_pool.clear();
	random_status = 0;
}

void CampaignManager::randomStatusRoll() {
	if (random_status_pool.empty())
		return;

	random_status = random_status_pool[sim_rng->range(0, static_cast<int>(random_status_pool.size()) - 1)];
}

void CampaignManager::randomStatusSet() {
	if (random_status_pool.empty())
		return;

	setStatus(random_status);
}

void CampaignManager::randomStatusUnset() {
	if (random_status_pool.empty())
		return;

	unsetStatus(random_status);
}

CampaignManager::~CampaignManager() {
	Utils::logInfo("Cleaning up: CampaignManager");
}
