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

/**
 * class PowerBonusState
 *
 * Temporary bonus levels granted to a power's upgrade chain by equipped items and active sets --
 * "this ring grants +1 to Fireball's upgrade track" -- and which chain position is presently
 * invested. This decides which power id actually fires from a hotkey when a bonus is active
 * (see clearBonusLevels()), so it is simulation state, not a tooltip decoration, even though it
 * used to live on MenuPowers.
 *
 * Found while scoping P1.3d-4b-3: PlayerInventory::applyEquipment() (once it moves) needs to
 * reset and recompute these bonuses on every equipment change, and could not do that through
 * MenuPowers without reintroducing the exact "state reached through a menu" problem P1.3e just
 * removed for the action bar. See plans/phase1/P1.3g-power-bonus-state.md for the full design
 * writeup, including why ActionBarState's reference-member trick does not apply here: that trick
 * binds a reference to an object that already exists at bind time, which works for MenuActionBar
 * (one object, bound once in its constructor) and does not work for MenuPowersCellGroup (an
 * element of a std::vector that is still being resized while power trees load).
 *
 * ladder is a COPY of each group's cell ids, not a reference -- built once, from MenuPowers, right
 * after a power tree finishes loading and before any bonus is ever applied. Nothing mutates a
 * power's id after load, so a stale copy is not a risk for the lifetime of one loaded character.
 * This is what lets this class resolve a PowerID to a group/position without knowing anything
 * about MenuPowersCellGroup or MenuPowersCell -- deliberately: pulling those types in would pull
 * in WidgetButton with them, and this class has to stay linkable into flare_sim (P1.4) without any
 * presentation type in its interface.
 *
 * current_cell and bonus_levels were, until this class existed, fields on MenuPowersCellGroup.
 * They still get written by UI code -- MenuPowers::setUnlockedPowers() advances current_cell as
 * the player invests points, and that function is NOT moving here; it stays exactly where it is,
 * reading and writing through this class's accessors instead of the struct fields it used to own
 * directly. See MenuPowers.cpp for the several call sites that still legitimately touch this
 * class's data from UI code -- that is expected and correct, the same way MenuActionBar legitimately
 * still touches ActionBarState's fields after P1.3e.
 */

#ifndef POWER_BONUS_STATE_H
#define POWER_BONUS_STATE_H

#include "CommonIncludes.h"
#include "Utils.h"

class ActionBarState;

class PowerBonusState {
public:
	// P2.3b. Set once by PlayerManager::create(), alongside owner/actionbar being set on the
	// sibling PlayerInventory/ActionBarState for the SAME player (P2.1's parallel arrays). This is
	// a genuine cross-reference, not a same-object self-call: clearActionBarBonusLevels() below
	// repoints a slot on the player's OWN action bar, a different class this one has never been
	// merged with. See plans/phase2/P2.3b-delete-player-globals.md, step 1's note on this exact
	// call site.
	ActionBarState* actionbar;

	PowerBonusState();

	/** Registers one power-upgrade-chain group, in the same order MenuPowers builds power_cell --
	 * group index i here MUST equal power_cell[i]'s index. Called once per group, from
	 * MenuPowers::loadPowerTree(), after that group's cells (and their ids) are finalised and
	 * before setUnlockedPowers() runs for the first time.
	 */
	void addGroup(const std::vector<PowerID>& cell_ids);

	/** Sum of every recorded bonus that applies at the group's current ladder position. Ported
	 * unchanged from MenuPowersCellGroup::getBonusLevels(), reading ladder/current_cell/bonus_levels
	 * here instead of cells/current_cell/bonus_levels there. MenuPowers' own UI-facing
	 * getBonusLevels()/getBonusCurrent()/getCurrent() (unmoved -- they return MenuPowersCell*, a
	 * widget-adjacent type this class deliberately knows nothing about) call this for the sum and do
	 * their own cell-pointer arithmetic around it.
	 */
	int getBonusLevels(size_t group) const;

	/** Repoints any action bar slot a bonus was pointing at back to its unbonused power, via
	 * actionbar->addPower() directly (actionbar is this same player's own ActionBarState, set
	 * alongside this object by PlayerManager::create() -- P2.3b -- and already independent of any
	 * menu, unchanged from P1.3e). Does NOT clear the bonus records themselves -- MenuPowers::setUnlockedPowers() calls this
	 * alone, before recomputing which powers are unlocked, and the bonus records must survive
	 * that (they're only recomputed from scratch by applyEquipment(), which calls
	 * clearBonusLevels() below instead). Ported unchanged from MenuPowers::clearActionBarBonusLevels().
	 */
	void clearActionBarBonusLevels();

	/** Full bodies moved from MenuPowers::clearBonusLevels()/addBonusLevels() -- see
	 * plans/phase1/P1.3g-power-bonus-state.md. clearBonusLevels() calls clearActionBarBonusLevels()
	 * above, then clears every bonus record -- the same two steps the original did, just no longer
	 * fused into one function it's unsafe to call half of.
	 */
	void clearBonusLevels();
	void addBonusLevels(PowerID power_id, int levels);

	std::vector< std::vector<PowerID> > ladder;
	std::vector<size_t> current_cell;
	std::vector< std::vector< std::pair<size_t, int> > > bonus_levels;

private:
	/** PowerID -> (group, index within ladder[group]), by linear scan of ladder. Ported from
	 * MenuPowers::getCellByPowerIndex(), which does the same scan over cells[].id. Only
	 * addBonusLevels() needs this -- every other consumer already knows its group.
	 */
	bool resolve(PowerID power_id, size_t& group, size_t& index) const;
};

#endif
