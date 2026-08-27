/*
Copyright © 2015 Igor Paliychuk
Copyright © 2015 Justin Jacobs

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
 * class SaveLoad
 *
 * Save function for the GameStatePlay.
 */

#ifndef SAVELOAD_H
#define SAVELOAD_H

class ActionBarState;
class Avatar;
class PlayerInventory;
class PowerBonusState;

class SaveLoad {
public:
	const static bool SAVE_STORAGE_ITEMS = true;

	SaveLoad();
	~SaveLoad();

	int getGameSlot() {
		return game_slot;
	}
	void setGameSlot(int slot) {
		game_slot = slot;
	}

	/** The explicit-parameter overloads below are the real implementations (P2.3) -- they read
	 * and write exactly one player's Avatar/PlayerInventory/ActionBarState/PowerBonusState, named
	 * at the call site, never a global. The no-argument overloads are compatibility wrappers for
	 * callers outside P2.3's scope (main_server.cpp, GameStateLoad.cpp, GameStateCutscene.cpp,
	 * GameStateNew.cpp, MenuExit.cpp, SDLInputState.cpp, and the two Platform*.cpp files) that
	 * still assume a single global local player; they forward to the explicit overload using the
	 * pc/pinv/pab/pbs globals (SharedGameResources.h) unchanged. Both must keep working, and both
	 * do exactly the same thing for today's single-player-per-client tree -- the split exists so
	 * this file's own logic is provably off the globals, not because behaviour differs.
	 */
	void saveGame();
	void saveGame(Avatar* avatar, PlayerInventory* inventory, ActionBarState* actionbar, PowerBonusState* powerbonus);
	void loadGame();
	void loadGame(Avatar* avatar, PlayerInventory* inventory, ActionBarState* actionbar, PowerBonusState* powerbonus);
	void loadClass(int index);
	void loadClass(int index, Avatar* avatar, PlayerInventory* inventory, ActionBarState* actionbar, PowerBonusState* powerbonus);
	void saveFOW();
	void saveExtendedItems(bool save_storage_items);
	void saveExtendedItems(bool save_storage_items, PlayerInventory* inventory);

private:
	void loadStash(Avatar* avatar);
	void applyPlayerData(Avatar* avatar, PlayerInventory* inventory, ActionBarState* actionbar, PowerBonusState* powerbonus);
	void loadPowerTree(Avatar* avatar);

	int game_slot;
};

#endif
