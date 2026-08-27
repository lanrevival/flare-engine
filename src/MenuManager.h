/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2013 Henrik Andersson
Copyright © 2013 Kurt Rinnert
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
 * class MenuManager
 */

#ifndef MENU_MANAGER_H
#define MENU_MANAGER_H

#include "CommonIncludes.h"
#include "ItemManager.h"

class ActionBarState;
class Avatar;
class Menu;
class MenuActionBar;
class MenuActiveEffects;
class MenuBook;
class MenuCharacter;
class MenuConfirm;
class MenuDevConsole;
class MenuEnemy;
class MenuExit;
class MenuGameOver;
class MenuHUDLog;
class MenuInventory;
class MenuLog;
class MenuMiniMap;
class MenuNumPicker;
class MenuPowers;
class MenuRegionTitle;
class MenuStash;
class MenuStatBar;
class MenuTalker;
class MenuTouchControls;
class MenuVendor;
class PlayerInventory;
class PowerBonusState;
class StatBlock;
class Subtitles;
class WidgetSlot;

class MenuManager {
private:
	enum {
		DRAG_SRC_NONE = 0,
		DRAG_SRC_POWERS = 1,
		DRAG_SRC_INVENTORY = 2,
		DRAG_SRC_ACTIONBAR = 3,
		DRAG_SRC_VENDOR = 4,
		DRAG_SRC_STASH = 5
	};

	enum {
		ACTION_SRC_NONE = 0,
		ACTION_SRC_POWERS = 1,
		ACTION_SRC_INVENTORY = 2,
		ACTION_SRC_ACTIONBAR = 3,
		ACTION_SRC_VENDOR = 4,
		ACTION_SRC_STASH = 5,
	};

	enum {
		ACTION_PICKER_ACTIONBAR_SELECT = 0,
		ACTION_PICKER_ACTIONBAR_CLEAR = 1,
		ACTION_PICKER_ACTIONBAR_USE = 2,
	};

	enum {
		ACTION_PICKER_POWERS_SELECT = 0,
		ACTION_PICKER_POWERS_UPGRADE = 1,
	};

	enum {
		ACTION_PICKER_INVENTORY_SELECT = 0,
		ACTION_PICKER_INVENTORY_ACTIVATE = 1,
		ACTION_PICKER_INVENTORY_ACTIONBAR = 2,
		ACTION_PICKER_INVENTORY_DROP = 3,
		ACTION_PICKER_INVENTORY_SELL = 4,
		ACTION_PICKER_INVENTORY_STASH = 5,
	};

	enum {
		ACTION_PICKER_STASH_SELECT = 0,
		ACTION_PICKER_STASH_TRANSFER = 1,
	};

	enum {
		ACTION_PICKER_VENDOR_BUY = 0,
	};

	enum {
		DRAG_POST_ACTION_NONE = 0,
		DRAG_POST_ACTION_DROP = 1,
		DRAG_POST_ACTION_BUY = 2,
		DRAG_POST_ACTION_SELL = 3,
		DRAG_POST_ACTION_STASH = 4,
	};

	void handleKeyboardTooltips();

	bool key_lock;

	bool mouse_dragging;
	bool keyboard_dragging;
	bool sticky_dragging;
	ItemStack drag_stack;
	PowerID drag_power;
	int drag_src;
	WidgetSlot *drag_icon;

	bool done;

	bool act_drag_hover;
	Point keydrag_pos;

	int action_src;
	Point action_picker_target;
	std::map<size_t, unsigned> action_picker_map;
	int drag_post_action;

	void renderIcon(int x, int y);
	void setDragIcon(int icon_id, int overlay_id);
	void setDragIconItem(ItemStack stack);

	void handleKeyboardNavigation();
	void dragAndDropWithKeyboard();
	void pushMatchingItemsOf(const Point& hov_pos);

	bool isTabListSelected();

	void showActionPicker(Menu* src_menu, const Point& target);

	void actionPickerStartDrag();

public:
	/** Reads playerm->local()/inventoryFor()/actionbarFor()/powerbonusFor() exactly once, right
	 * here at construction (playerm->setLocal(0) has already run by the time GameStatePlay
	 * constructs this) -- not a global lookup at every use site. See P2.3. */
	explicit MenuManager();
	MenuManager(const MenuManager &copy); // not implemented
	~MenuManager();

	/** Re-points MenuManager's own bindings and every menu that can be re-pointed after
	 * construction (a plain pointer member, not a reference). MenuActionBar cannot be re-pointed
	 * this way -- its hotkeys/locked/etc. are C++ references bound once, at construction, to
	 * whatever ActionBarState* it was built with (P1.3e-a's shape) -- so a new local action bar
	 * needs a new MenuActionBar, not a call to this. Exposed for reconnect (P2.5); only called
	 * once, at startup, for now. */
	void setPlayer(Avatar* _player, PlayerInventory* _player_inventory, ActionBarState* _player_actionbar, PowerBonusState* _player_powerbonus);
	void alignAll();
	void logic();
	void render();
	void closeAll();
	void closeLeft();
	void closeRight();
	void resetDrag();
	void defocusLeft();
	void defocusRight();

	std::vector<Menu*> menus;
	MenuInventory *inv;
	MenuPowers *pow;
	MenuCharacter *chr;
	MenuLog *questlog;
	MenuHUDLog *hudlog;
	MenuActionBar *act;
	MenuBook *book;
	MenuStatBar *hp;
	MenuStatBar *mp;
	MenuStatBar *xp;
	std::vector<MenuStatBar*> resource_statbars;
	MenuMiniMap *mini;
	MenuNumPicker *num_picker;
	MenuEnemy *enemy;
	MenuVendor *vendor;
	MenuTalker *talker;
	MenuExit *exit;
	MenuActiveEffects *effects;
	MenuStash *stash;
	MenuGameOver *game_over;
	MenuConfirm *action_picker;
	MenuRegionTitle *region_title;

	MenuDevConsole *devconsole;
	MenuTouchControls *touch_controls;

	Subtitles *subtitles;

	/** The UI would like the world to stop. A REQUEST, not a command.
	 *
	 * It used to be called 'pause' and GameStatePlay::isPaused() returned it directly, so any
	 * menu could halt the simulation. eset->misc.menus_pause defaults to false and Empyrean does
	 * not set it, but the exit menu, the dev console, an open book and the two item pickers all
	 * pause unconditionally -- so in co-op one player pressing Escape would have frozen the world
	 * for everyone, and on a headless server a menu object nobody renders could stop the game.
	 *
	 * The simulation decides whether to honour it. See GameStatePlay::isPaused().
	 *
	 * Client-local reads that mean "a menu owns this client's input right now" -- the equipment
	 * swap hotkey in MenuInventory, the region title in logic() -- keep reading this directly and
	 * keep today's behaviour. For them "a menu is up" is exactly the question being asked.
	 */
	bool pause_requested;
	bool menus_open;
	std::queue<ItemStack> drop_stack;

	bool isDragging();
	bool requestingExit() {
		return done;
	}
	bool isNPCMenuVisible();
	void showExitMenu();

	Avatar* player;
	PlayerInventory* player_inventory;
	ActionBarState* player_actionbar;
	PowerBonusState* player_powerbonus;
};

#endif
