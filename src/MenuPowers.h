/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
Copyright © 2012 Stefan Beller
Copyright © 2014 Henrik Andersson
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
 * class MenuPowers
 */

#ifndef MENU_POWERS_H
#define MENU_POWERS_H

#include "CommonIncludes.h"
#include "Menu.h"
#include "Utils.h"

class Avatar;
class MenuActionBar;
class PowerBonusState;
class StatBlock;
class TooltipData;
class WidgetButton;
class WidgetLabel;
class WidgetSlot;
class WidgetTabControl;

class MenuPowersTab {
public:
	std::string title;
	std::string background;
	bool background_is_menu_size;

	MenuPowersTab()
		: title("")
		, background("")
		, background_is_menu_size(true) {
	}
};

class MenuPowersCell {
public:
	MenuPowersCell();

	PowerID id;
	bool requires_point;

	int requires_level;
	std::vector<int> requires_primary;
	std::vector<PowerID> requires_power;
	std::vector<StatusID> requires_status;
	std::vector<StatusID> requires_not_status;

	bool visible;
	bool visible_check_locked;
	bool visible_check_status;

	int upgrade_level;
	bool passive_on;
	bool is_unlocked;

	size_t group;
	MenuPowersCell* next; // TODO should we also have "parent"?
};

/** current_cell and bonus_levels used to live here. They moved to PowerBonusState in P1.3g, so
 * that PlayerInventory::applyEquipment() can reset and recompute bonus levels without reaching
 * through MenuPowers -- see PowerBonusState.h and plans/phase1/P1.3g-power-bonus-state.md. This
 * group's index into MenuPowers::power_cell is the same index into every PowerBonusState vector;
 * nothing here stores that index because every caller already has it (power_cell[i].foo()).
 */
class MenuPowersCellGroup {
public:
	MenuPowersCellGroup();

	int tab;
	Point pos;

	std::vector<MenuPowersCell> cells;

	WidgetButton* upgrade_button;
};

class MenuPowersClick {
public:
	PowerID drag;
	PowerID unlock;

	MenuPowersClick()
		: drag(0)
		, unlock(0)
	{}
};

class MenuPowers : public Menu {
private:
	static const bool UPGRADE_POWER_ALL_TABS = true;
	static const bool TOOLTIP_SHOW_ACTIVATE_HINT = true;

	void loadGraphics();
	void loadTab(FileParser &infile);
	void loadPower(FileParser &infile);
	void loadUpgrade(FileParser &infile, std::vector<MenuPowersCell>& power_cell_upgrade);

	bool checkRequirements(MenuPowersCell* pcell);
	bool checkRequirementStatus(MenuPowersCell* pcell);
	bool checkUnlocked(MenuPowersCell* pcell);
	bool checkUnlock(MenuPowersCell* pcell);
	bool checkUpgrade(MenuPowersCell* pcell);
	void lockCell(MenuPowersCell* pcell);
	bool isBonusCell(MenuPowersCell* pcell);
	bool isCellVisible(MenuPowersCell* pcell);

	MenuPowersCell* getCellByPowerIndex(PowerID power_index);

	// Ported from MenuPowersCellGroup, which lost these in P1.3g -- see that class's header
	// comment. group is an index into power_cell (and, in lockstep, into every PowerBonusState
	// vector); these still return/consume MenuPowersCell*, so they stay here rather than move to
	// PowerBonusState, which deliberately knows nothing about that widget-adjacent type.
	MenuPowersCell* getCurrent(size_t group);
	MenuPowersCell* getBonusCurrent(size_t group, MenuPowersCell* pcell);

	void upgradePower(MenuPowersCell* pcell, bool ignore_tab);

	int getPointsUsed();

	void createTooltip(TooltipData* tip_data, MenuPowersCell* pcell, PowerID power_index, bool show_unlock_prompt, int tooltip_length);
	void createTooltipInputHint(TooltipData* tip_data, bool enable_activate_msg);
	void renderPowers(int tab_num);

	std::vector<MenuPowersCellGroup> power_cell;
	bool skip_section;

	std::vector<Sprite *> tree_surf;
	WidgetButton *closeButton;

	Point close_pos;
	Rect tab_area;

	int points_left;
	std::vector<MenuPowersTab> tabs;
	std::string default_background;

	WidgetLabel *label_powers;
	WidgetLabel *label_unspent;
	WidgetTabControl *tab_control;

	bool tree_loaded;

	int default_power_tab;

	Point upgrade_button_offset;

	std::vector<MenuPowersCell*> recently_locked_cells;

	std::string tooltip_text_shield;
	std::string tooltip_text_heal;

public:
	enum {
		TOOLTIP_SHORT = 0,
		TOOLTIP_LONG_MENU = 1,
		TOOLTIP_LONG_ALL = 2
	};

	MenuPowers(Avatar* _player, PowerBonusState* _player_powerbonus);
	~MenuPowers();
	void align();

	void loadPowerTree(const std::string &filename);

	void logic();
	void render();

	void renderTooltips(const Point& position);
	MenuPowersClick click(const Point& mouse);
	void clickUnlock(PowerID power_index);

	void setUnlockedPowers();
	void resetToBasePowers();

	bool meetsUsageStats(PowerID power_index);

	std::string getItemBonusPowerReqString(PowerID power_index);

	void createTooltipFromActionBar(TooltipData* tip_data, unsigned slot, int tooltip_length);

	std::vector<WidgetSlot*> slots; // power slot Widgets

	bool newPowerNotification;


	std::vector<TabList> tablist_pow;

	bool isTabListSelected();
	int getSelectedCellIndex();
	void setNextTabList(TabList *tl);
	TabList* getCurrentTabList();
	void defocusTabLists();

	Avatar* player;
	PowerBonusState* player_powerbonus;
};
#endif
