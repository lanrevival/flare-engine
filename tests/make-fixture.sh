#!/bin/sh
# Creates the save games the replay corpus boots from. Run by tests/run-replays.sh.
#
# THIS LIVES IN THE REPO, and that is the whole point. It used to live beside the plans, outside
# the tree, and tests/run-replays.sh invoked it as `plans/artifacts/... || true`. That path does
# not exist in a fresh checkout, so in CI the script silently did not run, no save game was ever
# written, and every replay booted a server that sat on the title screen with no map loaded. The
# committed Linux goldens are digests of that empty world: smoke.Linux-x86_64-clang.hash is
# 0x8751e5eefd8e093b, which is exactly what this server prints for a run with no save at all.
# Two compilers agreeing on the digest of nothing is not evidence that codegen is deterministic.
# See plans/phase0/P0.5c.
#
# There is no other way in. flare-server has no UI, character creation is a client-only menu
# state (P0.3b put it on fx_rng for exactly that reason), and without a save the server sits on
# the title screen forever -- where there is no world to hash.
#
# The save format is plain key=value text (SaveLoad.cpp), so it can be written by hand. These are
# TEST INPUTS, not golden files: they decide which map loads, where the player stands, what is on
# the action bar and how hard the player hits. Changing any of that changes every world hash
# derived from it.
#
# The map choice is load-bearing. arrival.txt was tried first and is USELESS as a fixture: it
# contains zero enemies, so nothing in the world moves, and a digest taken over it is identical
# whatever you do to the simulation -- a 1% change to movement speed did not move it, and neither
# did any RNG seed. abandoned_mines.txt has 18 enemy groups with area spawns.
#
# P0.5c: that last sentence was not enough on its own. Until P0.5c the server ran with
# encounter_dist = 0, and EntityBehavior::logic() returns immediately for any enemy further from
# the player than that -- so all fifty enemies stood on their spawn tiles doing nothing, in every
# recording, for the whole run. Having enemies on the map is necessary and was not sufficient.

set -e
SAVE_ROOT="${HOME}/.local/share/flare/saves/empyrean"

# ---------------------------------------------------------------------------
# slot 1 -- the movement fixture, for smoke.rec and patrol.rec.
#
# It was a naked level 1 from P0.5a until P0.5e, and P0.5e's new died_tick
# report showed why that was wrong: the player was killed at tick 1101 of
# patrol's 2000, so 45% of that recording was a corpse walking nowhere. The
# corpus was green throughout, because patrol only requires map_event and step
# and both had already fired.
#
# Level 7 is the tuned value, measured against patrol at its full 2000 ticks:
#
#   level 1  (xp=0,     build=1,1,1,1)      died at 1101
#   level 5  (xp=3840,  build=7,2,2,4)      died at 1567
#   level 6  (xp=7936,  build=14,2,2,7)     died at 1876
#   level 7  (xp=16128, build=24,3,3,10)    survives; last event at 1959
#
# Still no equipment and no powers: the point of this fixture is a character
# that WALKS, and levels were the smallest knob that let it finish the route.
# Changing the spawn instead would have sent patrol.rec's fixed input somewhere
# else entirely.
# ---------------------------------------------------------------------------
mkdir -p "$SAVE_ROOT/1"
cat > "$SAVE_ROOT/1/avatar.txt" <<'SAVE'
## flare-engine save file ##
name=HashFixture
permadeath=0
class=Brute,
xp=16128
build=24,3,3,10
currency=0
equipped_quantity=
equipped=
carried_quantity=
carried=
spawn=maps/abandoned_mines.txt,76,71
actionbar=0,0,0,0,0,0,0,0,0,0,0,0
powers=
campaign=
time_played=0
engine_version=1.15.52
SAVE

# ---------------------------------------------------------------------------
# slot 2 -- the combat fixture, for melee.rec.
#
# Everything here is tuned, and none of it is what a player would actually have:
#
#   spawn      58,69 sits inside the goblin group's area spawn box, which is
#              location=54,62,6,12 in maps/abandoned_mines.txt. Slot 1's spawn is
#              nineteen tiles from the nearest enemy, which is why nothing ever
#              happened there.
#   equipped   the Brute class kit from engine/classes.txt, in equipment-slot
#              order: hands, artifact, ring, ring, main, off, head, chest, legs,
#              feet -- then the same ten again for the second equipment set.
#   actionbar  slot 10 is MAIN1 and slot 11 is MAIN2 (MenuActionBar.h:78-79).
#              1 = Swing, 2 = Shield Bash. Shield Bash is not decoration: it is
#              the only warrior power carrying BOTH soundfx and soundfx_hit, so
#              it is the only route this data set has to SFX_POWER and
#              SFX_HAZARD_HIT.
#   xp         twelve below the level-12 threshold in engine/xp_table.txt
#              (524032), so that the first kill crosses it and SFX_LEVELUP fires.
#              Deliberate, fragile, and worth it: without it that path has no
#              coverage at all.
#   build      strong enough to win the fight. A level-1 Brute in this spot dies
#              in about twenty seconds, and a dead player respawns on another map,
#              which ends the run early and takes the combat coverage with it.
# ---------------------------------------------------------------------------
mkdir -p "$SAVE_ROOT/2"
cat > "$SAVE_ROOT/2/avatar.txt" <<'SAVE'
## flare-engine save file ##
name=CombatFixture
permadeath=0
class=Brute,
xp=524020
build=50,5,4,15
currency=0
equipped_quantity=1,0,0,0,1,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0
equipped=7,0,0,0,8,11,0,4,5,6,0,0,0,0,0,0,0,0,0,0
carried_quantity=
carried=
spawn=maps/abandoned_mines.txt,58,69
actionbar=0,0,0,0,0,0,0,0,0,0,1,2
powers=2,7
campaign=
time_played=0
engine_version=1.15.52
SAVE

# ---------------------------------------------------------------------------
# slot 3 -- the beatdown fixture, for beatdown.rec.
#
# Same place, same kit, forty-four levels lower. This one is meant to STRUGGLE. It exists
# for the low-HP warning, which is the riskiest thing P1.2 converted: it is a
# looping named audio channel with three transitions, and it is the only sound
# event in the engine that carries state across ticks. A queue that is cleared
# every tick cannot express "this loop is still running", so Avatar keeps the
# edge detector and emits only SFX_LOWHP_START and SFX_LOWHP_STOP. Nothing else
# in the corpus drops the player's health far enough to fire either.
#
# Level 6 is the tuned value, and the window is narrow. P0.5d widened the enemy
# activation radius from 10.770 tiles to a fixed 12, one more enemy wakes, and
# every bound below moved by a level. Measured against beatdown at its full
# 2956 ticks:
#
#   level 5  (xp=3840, build=7,2,2,4)    died at 1186 -- 59% of the run a corpse
#   level 6  (xp=7936, build=10,2,2,5)   died at 1436
#   level 6  (xp=7936, build=12,2,2,6)   survives, one low-HP cycle
#   level 6  (xp=7936, build=14,2,2,7)   survives, one low-HP cycle  <-- chosen
#   level 7  (xp=16128, build=24,3,3,10) survives, NO low-HP, NO hazard_hit
#
# At level 6 the player survives to the last tick, takes 68 attacks and 16 hits,
# eats one enemy hazard and drops into the low-HP warning once.
#
# ONE cycle, not two. Until P0.5e this comment claimed the loop started and
# stopped TWICE and that the second cycle was what proved the edge detector
# re-arms rather than firing once and latching. That was true at level 5 before
# P0.5d and is not true now, and no build in the surviving neighbourhood brings
# it back. The re-arm property is currently NOT covered by anything. Do not put
# lowhp_start=2 in the MANIFEST to paper over it -- a requirement that flickers
# gets weakened rather than investigated. See plans/phase0/P0.5e.
# ---------------------------------------------------------------------------
mkdir -p "$SAVE_ROOT/3"
cat > "$SAVE_ROOT/3/avatar.txt" <<'SAVE'
## flare-engine save file ##
name=BeatdownFixture
permadeath=0
class=Brute,
xp=7936
build=14,2,2,7
currency=0
equipped_quantity=1,0,0,0,1,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0
equipped=7,0,0,0,8,11,0,4,5,6,0,0,0,0,0,0,0,0,0,0
carried_quantity=
carried=
spawn=maps/abandoned_mines.txt,58,69
actionbar=0,0,0,0,0,0,0,0,0,0,1,2
powers=2,7
campaign=
time_played=0
engine_version=1.15.52
SAVE

# ---------------------------------------------------------------------------
# slot 4 -- the DEATH fixture, for death.rec (which is patrol.rec's input again).
#
# This exists because of a gap found while verifying P1.3: the death penalty --
# currency loss, XP loss and random item destruction -- does not live in the
# simulation. It lives in MenuInventory::logic(), and it draws from sim_rng.
# P1.3b moves it out. Nothing could verify that move, because the penalty only
# runs when the player dies and P0.5e made a dying player a hard FAIL on every
# recording in the corpus. The two checks are each correct and together they
# excluded the code from ever executing under test.
#
# So this fixture is the one that is SUPPOSED to die, and the MANIFEST's new
# 'survives' column says so out loud rather than exempting it quietly.
#
# It is deliberately the naked level 1 that P0.5e retired from slot 1, on slot
# 1's spawn, replaying slot 1's input. P0.5e measured that character dying at
# tick 1101 of patrol's 2000, so the death is already a known quantity rather
# than something newly tuned. The budget below is 1400: comfortably past 1101,
# and short enough not to spend 800 ticks watching a corpse.
#
#   currency=1000  is the whole point. SaveLoad.cpp:545 turns this field into
#                  1000 real currency items in CARRIED, and MenuInventory
#                  recomputes pc->stats.currency from that stack every tick.
#                  engine/death_penalty.txt removes 50% on death, so a run that
#                  applies the penalty ends on 500 and one that does not ends on
#                  1000. Both numbers are hashed -- as the scalar AND as the
#                  stack quantity -- so P1.3b cannot silently drop the penalty.
#
# LIMIT, stated rather than relied on: of the three penalty branches, the shipped
# mod data enables only currency. xp_total, xp_current_level and random_item are
# all off in mods/default/engine/death_penalty.txt, so the XP arithmetic and the
# sim_rng->index() item-destruction draw stay uncovered. Do NOT edit shipped game
# data to fix that; the way in is a test-only mod that overrides death_penalty.txt
# with every branch enabled, which needs a 'mods' column here. Worth doing in
# P1.3b, when that code is the thing being moved.
# ---------------------------------------------------------------------------
mkdir -p "$SAVE_ROOT/4"
cat > "$SAVE_ROOT/4/avatar.txt" <<'SAVE'
## flare-engine save file ##
name=DeathFixture
permadeath=0
class=Brute,
xp=0
build=1,1,1,1
currency=1000
equipped_quantity=
equipped=
carried_quantity=
carried=
spawn=maps/abandoned_mines.txt,76,71
actionbar=0,0,0,0,0,0,0,0,0,0,0,0
powers=
campaign=
time_played=0
engine_version=1.15.52
SAVE

# ---------------------------------------------------------------------------
# slot 5 -- the FULL death penalty fixture, for deathfull.rec.
#
# Slot 4 dies under the shipped configuration, which enables exactly one of the
# three penalty branches. This one runs with tests/mods/test_deathpenalty
# appended, which turns all three on, so the XP arithmetic and the
# sim_rng->index() item-destruction draw are covered too. P1.3b moves that code;
# an uncovered branch is where a copy-paste error hides.
#
# Same naked level 1 on the same spawn replaying the same input, so the death
# itself is the known quantity from slot 4. What differs is what there is to lose:
#
#   currency=1000  25% goes on death under the test mod, so 1000 -> 750.
#   carried=4,5    two ordinary items from the Brute kit, plus the 1000 currency
#                  items, gives the random-item draw a list of THREE to choose
#                  from. With only currency carried the list has one entry and
#                  sim_rng->index(1) is 0 every time -- the call would execute
#                  without the choice ever being exercised.
#   xp=<tuned>     xp_total=10 removes a tenth of it. Tuned by measurement below,
#                  because XP is what sets the level and the level is what decides
#                  whether this character still dies inside the budget.
# ---------------------------------------------------------------------------
mkdir -p "$SAVE_ROOT/5"
cat > "$SAVE_ROOT/5/avatar.txt" <<'SAVE'
## flare-engine save file ##
name=DeathFullFixture
permadeath=0
class=Brute,
xp=200
build=1,1,1,1
currency=1000
equipped_quantity=
equipped=
carried_quantity=1,1
carried=4,5
spawn=maps/abandoned_mines.txt,76,71
actionbar=0,0,0,0,0,0,0,0,0,0,0,0
powers=
campaign=
time_played=0
engine_version=1.15.52
SAVE

# ---------------------------------------------------------------------------
# slot 6 -- the AUTO-EQUIP fixture, for autoequip.rec.
#
# This exists because of a gap found while verifying P1.3d: the corpus does not exercise
# equipping AT ALL. Three measurements, none of which could be argued with:
#
#   forcing every equipment slot disabled          moved no digest, on any recording
#   counting enabled/disabled slots at exit        20 enabled, 0 disabled, all six recordings
#   a naked level-50 fixture replaying melee.rec   kills, loots, and still moves nothing,
#                                                  because getEquipSlotFromItem() returns -1 for
#                                                  everything that drops there
#
# So MenuInventory::add()'s auto-equip branch -- the one that reads equip_slot_enabled and
# decides whether an incoming item goes to EQUIPMENT or CARRIED -- had no coverage, in a plan
# whose own stated failure mode is "a subtle error means players lose gear".
#
# The way in is a map event, not a loot drop. CampaignManager::rewardItem() is the only caller
# that passes ADD_AUTO_EQUIP from map data (CampaignManager.cpp:188), and a map event is
# something a test mod can author outright instead of hoping the enemy loot tables cooperate.
#
#   spawn      tests/mods/test_autoequip/maps/test_autoequip.txt, an empty 16x16 room whose one
#              event grants item 4. Nothing else is on that map on purpose.
#   naked      equipped= is empty, so the chest slot is free and the shirt has somewhere to go.
#              This is what makes the row's assertion sharp: equipped goes 0 -> 1, and if the
#              auto-equip decision stops working the item lands in CARRIED and it stays 0.
#   currency=0 nothing else to lose, so the digest moves for one reason only.
#
# Item 4 is the Cloth Shirt (items/categories/level_1.txt), item_type=chest, level=1. Level
# matters: applyEquipment() strips gear the character cannot meet the requirements for, and a
# level 1 character that instantly unequips what it was just handed would report equipped=0 and
# look exactly like a broken auto-equip branch.
# ---------------------------------------------------------------------------
mkdir -p "$SAVE_ROOT/6"
cat > "$SAVE_ROOT/6/avatar.txt" <<'SAVE'
## flare-engine save file ##
name=AutoEquipFixture
permadeath=0
class=Brute,
xp=0
build=1,1,1,1
currency=0
equipped_quantity=
equipped=
carried_quantity=
carried=
spawn=maps/test_autoequip.txt,8,8
actionbar=0,0,0,0,0,0,0,0,0,0,0,0
powers=
campaign=
time_played=0
engine_version=1.15.52
SAVE

echo "wrote $SAVE_ROOT/{1,2,3,4,5,6}/avatar.txt"
