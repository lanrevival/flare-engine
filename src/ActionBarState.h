/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
Copyright © 2013 Kurt Rinnert
Copyright © 2014 Henrik Andersson
Copyright © 2012-2016 Justin Jacobs
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
 * class ActionBarState
 *
 * What powers a character has bound to which hotkey. This used to be fields on MenuActionBar,
 * which meant a player's bindings were owned by a window -- see PlayerInventory.h for why that's
 * the wrong owner the moment there is more than one player and only one of them has a screen.
 *
 * There is exactly ONE copy of this data. MenuActionBar's `hotkeys`, `hotkeys_temp`, `hotkeys_mod`,
 * `locked`, `prevent_changing`, `requires_attention`, `updated` and `slots_count` are REFERENCES
 * bound to the members below, not copies of them -- MenuActionBar.cpp's ~90 uses of e.g.
 * `hotkeys[i]` still compile and still mean the same storage, unchanged. That is deliberate and it
 * is this class's whole point: two copies desynchronise and the symptom is a hotkey silently
 * reverting.
 *
 * `requires_attention` and `updated` are presentation, not simulation, and they move here anyway:
 * `requires_attention[MENU_LOG]` is serialised as `questlog_dismissed` (SaveLoad.cpp), and the save
 * format must not change, so SaveLoad needs an owner for it that exists without a live MenuManager.
 * See P1.3e-actionbar-state.md's "open question" -- this was a judgment call, not a measurement,
 * and is recorded as one.
 *
 * `prevent_changing` joined in P1.3e-c, one step later than the other seven: it wasn't obviously
 * needed until addPower() turned out to require it (see below) to be genuinely usable without a
 * live MenuActionBar, not just movable in name. It grows INCREMENTALLY, unlike the four sized once
 * by initSlots() -- MenuActionBar::addSlot() resizes it slot-by-slot as menus/actionbar.txt parses,
 * and that code did not need to change at all: resize()/[index]= behave identically through a
 * reference.
 *
 * `twostep_slot` and `slot_fail_cooldown` joined in P1.4c, for a different reason than any of the
 * above: not because a widget got in the way, but because the headless server's hotkey translator
 * (main_server.cpp) needs to persist two-step-activation and per-slot fail-cooldown state ACROSS
 * ticks, the same way MenuActionBar always has, and there is exactly one of each per hotkey slot
 * whether or not anything is drawing them. Sized in initSlots()/cleared in clearSlot() alongside
 * the four PowerID vectors, same pattern.
 *
 * What is NOT here, on purpose: `clear()`, and half of `addPower()`. `clear()` calls
 * MenuActionBar's own clearSlot() (the widget-touching one) for every slot it clears, and its two
 * callers (GameStatePlay's resetGame(), EventManager's class-switch) still go through a live menu
 * today -- moving it isn't needed for anything currently blocked, so it stays, deferred rather than
 * rushed. `checkAction()` itself also stays on MenuActionBar, for the widget-click branches it still
 * owns -- but as of P1.4c it is no longer true that it "never runs on a headless server at all": the
 * hotkey-only subset of what it does now runs server-side too, through checkHotkeyActions() below.
 *
 * addPower()'s split follows a real seam, not the wart PlayerInventory::setEquipSlotEnabled() has:
 * MenuActionBar::addPower(id, target_id)'s two branches are MUTUALLY EXCLUSIVE, not entangled.
 * `id == 0` means "clear whichever slot holds target_id" -- the ONLY branch that touches a widget,
 * via clearSlot() -- and PowerID 0 always means "no power", so powers->isValid(0) is false and the
 * function returns right after that branch either way; the assignment logic below it never runs in
 * the same call. `id != 0` means "place power `id` on a slot" and never touched a widget even in
 * the original, single-class version -- it only ever set hotkeys[i] and relied on the next logic()
 * tick to redraw the icon, same as this class's version does now. So this class implements ONLY the
 * `id != 0` path. It is not a general-purpose addPower() with a documented gap; the `id == 0` path
 * is a DIFFERENT operation that has never been asked of this class, by anything, and isn't
 * implemented speculatively. If a future caller needs it, that is when it gets added and tested --
 * not before, because untested-by-construction code is exactly what this project keeps finding
 * costs more than it saves.
 */

#ifndef ACTION_BAR_STATE_H
#define ACTION_BAR_STATE_H

#include "CommonIncludes.h"
#include "Utils.h"

class ActionData;

class ActionBarState {
public:
	// How many menu buttons requires_attention tracks (character/inventory/powers/log). A
	// duplicate of MenuActionBar::MENU_COUNT's value, not a reference to it -- this class does not
	// include MenuActionBar.h, the same reasoning PlayerInventory::EQUIPMENT/CARRIED already gives.
	static const int MENU_COUNT = 4;

	// Argument to set(). MenuActionBar::SET_SKIP_EMPTY no longer exists -- deleted, not aliased,
	// the same call PlayerInventory::ONLY_EMPTY_SLOTS already made for the same reason.
	static const bool SET_SKIP_EMPTY = true;

	ActionBarState();

	/** Sizes the six per-slot arrays (the four PowerID vectors, plus slot_fail_cooldown since
	 * P1.4c). Called once, from MenuActionBar's constructor, after it has parsed
	 * menus/actionbar.txt and knows how many slots exist -- see PlayerInventory::init()'s header
	 * comment for the same shape of dependency and why it hasn't moved yet (D1).
	 */
	void initSlots(unsigned _slots_count);

	/** Empties one slot: hotkeys[slot], hotkeys_temp[slot], hotkeys_mod[slot] and locked[slot] all
	 * go to their unset values, and slot_fail_cooldown[slot] (since P1.4c) is reset to its
	 * just-cleared duration/state. Nothing else -- MenuActionBar's own clearSlot() calls this and
	 * then does the widget-side reset (icon, cooldown flash, item count) that this class has no way
	 * to do and no need to: the next logic() tick redraws every slot's icon from hotkeys_mod anyway.
	 */
	void clearSlot(size_t slot);

	/** The hotkey-only subset of MenuActionBar::checkAction() -- see this file's header comment.
	 * Reads pc/powers/inpt/wmap directly (all sim globals) instead of taking them as parameters,
	 * matching Map::checkEvents()/activatePower()'s own style. Appends to action_queue exactly like
	 * the original; does not touch slots[]/slot_item_count/anything widget-owned, because none of
	 * that exists without a live MenuActionBar.
	 */
	void checkHotkeyActions(std::vector<ActionData> &action_queue);

	/** Places power `id` on a slot: MAIN1/MAIN2 first, then 0-9, skipping slots menus/actionbar.txt
	 * marked un-droppable (prevent_changing) and, when target_id is 0, skipping duplicates of a
	 * power already on the bar. Only the `id != 0` half of MenuActionBar::addPower() -- see this
	 * file's header comment for why the other half isn't here.
	 */
	void addPower(const PowerID id, const PowerID target_id);

	/** Binds power_id[i] to hotkeys[i] for each valid, non-passive, actionbar-eligible power.
	 * skip_empty leaves an already-occupied slot alone rather than overwriting it. Moved outright
	 * from MenuActionBar -- unlike addPower() and clearSlot(), this never touched a widget even in
	 * the original.
	 */
	void set(std::vector<PowerID> power_id, bool skip_empty);

	unsigned slots_count;
	std::vector<PowerID> hotkeys;       // refers to power_index in PowerManager
	std::vector<PowerID> hotkeys_temp;  // saved here during shapeshifting, restored after
	std::vector<PowerID> hotkeys_mod;   // hotkeys, with item/bonus modifications applied
	std::vector<bool> locked;           // slot can't be dragged out from under a transform
	std::vector<bool> prevent_changing; // slot can't be reassigned at all; a mod-authored constant

	// Sized to MENU_COUNT, not slots_count -- one flag per menu button, not per hotkey slot.
	std::vector<bool> requires_attention;
	bool updated;

	// Since P1.4c. Which slot (if any) is mid two-step activation (a power with
	// STARTING_POS_TARGET or buff_teleport, waiting on its second MAIN1/MAIN2 press) -- -1 means
	// none. One value for the whole action bar, not per-slot, matching MenuActionBar's original.
	int twostep_slot;

	// Since P1.4c. Per-slot "don't retry a just-failed activation every tick" cooldown -- sized in
	// initSlots(), armed in clearSlot(), matching MenuActionBar::slot_fail_cooldown exactly.
	std::vector<Timer> slot_fail_cooldown;
};

#endif
