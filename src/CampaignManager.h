/*
Copyright © 2011-2012 Clint Bellanger
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


#ifndef CAMPAIGN_MANAGER_H
#define CAMPAIGN_MANAGER_H

#include "CommonIncludes.h"
#include "ItemManager.h"
#include "Utils.h"

class Avatar;
class EventComponent;
class StatBlock;

class CampaignManager {
	// Reads private simulation state to digest it. A diagnostic should not force the
	// class to widen its public API for everyone else. See WorldHash.h.
	friend class WorldHash;

public:
	typedef std::map<StatusID, std::pair<bool, std::string> > StatusMap;

	CampaignManager();
	~CampaignManager();

	StatusID registerStatus(const std::string& s);
	void setAll(const std::string& s);
	std::string getAll();
	bool checkStatus(const StatusID s);
	void setStatus(const StatusID s);
	void unsetStatus(const StatusID s);
	void resetAllStatuses();
	void getSetStatusStrings(std::vector<std::string>& status_strings);

	// P2.2 kind-A migration: these all act on a specific player's inventory/stats -- a quest
	// reward, a toll-gate currency check, an HP/MP restore trigger. `triggered_by` is that
	// player. It defaults to NULL, which resolves to playerm->local() inside the .cpp --
	// identical to the pre-P2.2 single-player behavior, and *only* because every caller this
	// plan does not touch (NPC.cpp, Entity.cpp, QuestLog.cpp, Map.cpp, SaveLoad.cpp,
	// GameStatePlay.cpp, main_server.cpp, and StatBlock.cpp's on-kill XP grant) is itself still
	// single-player-shaped and out of scope here -- see the P2.2 report for the full list.
	// EventManager.cpp (P2.2 step 6b) is the one caller migrated to pass the real triggering
	// player, since map events are the case the plan calls out explicitly (a toll gate must
	// charge the player who walked up to it, not playerm->local()).
	bool checkCurrency(int quantity, Avatar* triggered_by = NULL);
	bool checkItem(ItemStack istack, Avatar* triggered_by = NULL);
	void removeCurrency(int quantity, Avatar* triggered_by = NULL);
	void removeItem(ItemStack istack, Avatar* triggered_by = NULL);
	void rewardItem(ItemStack istack, Avatar* triggered_by = NULL);
	void rewardCurrency(int amount, Avatar* triggered_by = NULL);
	void rewardXP(float amount, bool show_message, Avatar* triggered_by = NULL);
	void restoreHPMP(const std::string& s, Avatar* triggered_by = NULL);
	bool checkAllRequirements(const EventComponent& ec, Avatar* triggered_by = NULL);
	bool checkRequirementsInVector(const std::vector<EventComponent>& ec_vec, Avatar* triggered_by = NULL);

	void randomStatusAppend(const StatusID s);
	void randomStatusClear();
	void randomStatusRoll();
	void randomStatusSet();
	void randomStatusUnset();

	std::queue<ItemStack> drop_stack;

	float bonus_xp;		// Fractional XP points not yet awarded (e.g. killing 1 XP enemies with a +25% ring)

	static const bool XP_SHOW_MSG = true;

private:
	StatusMap status;

	std::vector<StatusID> random_status_pool;
	StatusID random_status;
};


#endif
