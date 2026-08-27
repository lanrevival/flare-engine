/*
Copyright © 2026 Flare LAN Co-op contributors

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

#include "PowerBonusState.h"

#include "ActionBarState.h"
#include "SharedGameResources.h"

PowerBonusState::PowerBonusState()
	: actionbar(NULL)
	, ladder()
	, current_cell()
	, bonus_levels() {
}

void PowerBonusState::addGroup(const std::vector<PowerID>& cell_ids) {
	ladder.push_back(cell_ids);
	current_cell.push_back(0);
	bonus_levels.push_back(std::vector< std::pair<size_t, int> >());
}

int PowerBonusState::getBonusLevels(size_t group) const {
	int blevel = 0;
	for (size_t i = 0; i < bonus_levels[group].size(); ++i) {
		if (current_cell[group] >= bonus_levels[group][i].first)
			blevel += bonus_levels[group][i].second;
	}
	return blevel;
}

bool PowerBonusState::resolve(PowerID power_id, size_t& group, size_t& index) const {
	// Powers can not have an id of 0
	if (power_id == 0)
		return false;

	for (size_t g = 0; g < ladder.size(); ++g) {
		for (size_t i = 0; i < ladder[g].size(); ++i) {
			if (ladder[g][i] == power_id) {
				group = g;
				index = i;
				return true;
			}
		}
	}

	return false;
}

void PowerBonusState::clearActionBarBonusLevels() {
	for (size_t i = 0; i < ladder.size(); ++i) {
		if (ladder[i].empty())
			continue;

		int blevel = getBonusLevels(i);
		if (blevel <= 0)
			continue;

		PowerID current_id = ladder[i][current_cell[i]];

		size_t bonus_index = current_cell[i] + static_cast<size_t>(blevel);
		if (bonus_index >= ladder[i].size())
			bonus_index = ladder[i].size() - 1;
		PowerID bonus_id = ladder[i][bonus_index];

		actionbar->addPower(current_id, bonus_id);
	}
}

void PowerBonusState::clearBonusLevels() {
	clearActionBarBonusLevels();

	for (size_t i = 0; i < bonus_levels.size(); ++i) {
		bonus_levels[i].clear();
	}
}

void PowerBonusState::addBonusLevels(PowerID power_id, int levels) {
	size_t group, index;
	if (!resolve(power_id, group, index))
		return;

	std::pair<size_t, int> bonus(index, levels);
	bonus_levels[group].push_back(bonus);
}
