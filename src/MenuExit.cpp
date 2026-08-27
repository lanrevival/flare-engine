/*
Copyright © 2011-2012 kitano
Copyright © 2013 Kurt Rinnert
Copyright © 2014 Henrik Andersson
Copyright © 2012-2015 Justin Jacobs

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
 * class MenuExit
 */

#include "Avatar.h"
#include "InputState.h"
#include "MapRenderer.h"
#include "MenuConfig.h"
#include "MenuConfirm.h"
#include "MenuExit.h"
#include "SaveLoad.h"
#include "SharedGameResources.h"
#include "SharedResources.h"

MenuExit::MenuExit()
	: Menu()
	, menu_config(new MenuConfig(!MenuConfig::IS_GAME_STATE))
	, exitClicked(false)
	, reload_music(false)
{
	// P2.3b: MenuExit wasn't in P2.3's own file list (a real omission -- see
	// plans/phase2/P2.3b-delete-player-globals.md). Reading playerm->local() directly, rather than
	// adding a constructor parameter, avoids touching MenuManager.cpp's construction call (out of
	// this plan's scope) -- kind A, the local player, same as every other menu. playerm->setLocal()
	// has already run by the time MenuManager constructs this (same ordering P2.3's MenuManager
	// report documents for its own menus).
	menu_config->setHero(playerm->local());
	align();
}

void MenuExit::align() {
	Menu::align();
	menu_config->refreshWidgets();
}

void MenuExit::logic() {
	if (visible) {
		menu_config->logic();
	}

	if (menu_config->reload_music) {
		reload_music = true;
		menu_config->reload_music = false;
	}

	if (menu_config->clicked_pause_continue) {
		visible = false;
		menu_config->clicked_pause_continue = false;
	}
	else if (menu_config->clicked_pause_exit) {
		exitClicked = true;
		menu_config->clicked_pause_exit = false;
	}

	else if (menu_config->clicked_pause_save) {
		visible = false;
		menu_config->clicked_pause_save = false;

		mapr->respawn_point = playerm->local()->stats.pos;
		save_load->saveGame();
	}
}

void MenuExit::render() {
	if (visible) {
		// background
		Menu::render();

		menu_config->render();
	}
}

void MenuExit::disableSave() {
	menu_config->setPauseExitText(!MenuConfig::ENABLE_SAVE_GAME);
	menu_config->setPauseSaveEnabled(!MenuConfig::ENABLE_SAVE_GAME);
}

void MenuExit::handleCancel() {
	if (!visible) {
		menu_config->resetSelectedTab();
		visible = true;
	}
	else {
		if (!menu_config->input_confirm->visible) {
			visible = false;
		}
		else {
			// MenuManager will have locked CANCEL before calling this
			// But we need it unlocked for the input_cofirm dialog. So unlock it here
			inpt->lock[Input::CANCEL] = false;
		}
	}
}

MenuExit::~MenuExit() {
	delete menu_config;
}

