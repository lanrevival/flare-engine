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
# slot 1 -- the movement fixture. Deliberately unchanged since P0.5a so that
# smoke.rec and patrol.rec keep meaning what they meant.
# ---------------------------------------------------------------------------
mkdir -p "$SAVE_ROOT/1"
cat > "$SAVE_ROOT/1/avatar.txt" <<'SAVE'
## flare-engine save file ##
name=HashFixture
permadeath=0
class=Brute,
xp=0
build=1,1,1,1
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
# Same place, same kit, seven levels lower. This one is meant to LOSE. It exists
# for the low-HP warning, which is the riskiest thing P1.2 converted: it is a
# looping named audio channel with three transitions, and it is the only sound
# event in the engine that carries state across ticks. A queue that is cleared
# every tick cannot express "this loop is still running", so Avatar keeps the
# edge detector and emits only SFX_LOWHP_START and SFX_LOWHP_STOP. Nothing else
# in the corpus drops the player's health far enough to fire either.
#
# Level 5 is the tuned value, and the window is narrow. At level 3 the player is
# dead inside twenty seconds and the run is over before most of it happens; at
# level 6 the warning never fires at all. At 5 the player survives the whole
# recording, the low-HP loop starts and stops TWICE -- which is what proves the
# edge detector re-arms rather than firing once and latching -- and a critical
# kill and four shield-bash hits fall out of it as well.
# ---------------------------------------------------------------------------
mkdir -p "$SAVE_ROOT/3"
cat > "$SAVE_ROOT/3/avatar.txt" <<'SAVE'
## flare-engine save file ##
name=BeatdownFixture
permadeath=0
class=Brute,
xp=3840
build=7,2,2,4
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

echo "wrote $SAVE_ROOT/{1,2,3}/avatar.txt"
