#!/bin/sh
# Replays every recording and compares its world digest against the committed golden file.
#
#   tests/run-replays.sh [data-path]
#
# Exit 0 = every recording matched. Exit 1 = at least one moved, or a replay was refused.
#
# A moved digest means the simulation changed. That is the point: Phases 1 and 2 are almost
# entirely behaviour-preserving refactors, and this is how they are checked. If a change is
# intended, re-record the goldens deliberately and say so in the commit -- do not silently
# regenerate them to make this pass.
#
# NOTE these recordings only mean something because they contain motion. A recording of an idle
# server produces a digest that no change to the simulation can move; see plans/phase0/P0.5a.

set -e
cd "$(dirname "$0")/.."

DATA_PATH="${1:-$FLARE_TEST_DATA_PATH}"
MODS="fantasycore,empyrean_campaign"
DIR="tests/replays"

# Goldens are PER-PLATFORM, and that is a finding rather than a precaution -- but not the finding
# it was first recorded as.
#
# It was originally read as "the divergence is the architecture, not the compiler". That reading
# rested on Linux digests produced by a job in which the fixture step below silently did nothing,
# so the Linux runs hashed an empty world with no map loaded while the macOS runs hashed a real
# one. smoke.Linux-x86_64-clang.hash was 0x8751e5eefd8e093b, which is exactly what this server
# prints for a run with no save game at all. Two compilers agreeing on the digest of nothing says
# nothing about codegen. P0.5c fixed the fixture step; the numbers below are from after that.
#
# MEASURED, on a corpus that does load a world:
#   gcc vs clang on one machine  -- identical digests. The divergence is not codegen.
#   macOS/arm64 vs Linux/x86-64  -- diverge behaviourally: 'melee' kills two enemies on one and
#                                   nothing on the other. The coverage block below catches it.
#
# Those two runners differ in both OS and ISA, so which one moved is UNATTRIBUTED. The CI matrix
# now carries an arm64 Linux runner to split them; see .github/workflows/main.yml.
#
# Keep FLARE_GOLDEN_SUFFIX set to the compiler. uname -m is already in the tag, so the four
# combinations land in four distinct files and none of them can quietly overwrite another.
TAG="$(uname -s)-$(uname -m)${FLARE_GOLDEN_SUFFIX:+-$FLARE_GOLDEN_SUFFIX}"
echo "platform tag: $TAG"

# Is this platform RECORDED? Counted, not configured, and deliberately not a flag: a switch that
# turns the digest comparison off is a switch someone eventually flips on the platform that needs
# it. The rule is all-or-nothing per platform --
#
#   no goldens at all  -> unrecorded. Digests are printed as information. Coverage and liveness
#                         still hard-fail, because those ask whether the game BEHAVED, which is a
#                         meaningful question on a platform with nothing to compare against.
#   some goldens       -> recorded. Every row must have one and match it. A missing golden here
#                         means a partial recording, i.e. somebody forgot one, which is an error.
#
# This exists because the digest is a SAME-MACHINE regression test, not a determinism requirement.
# Cross-machine determinism was abandoned when the arm64 Linux runner showed the divergence is both
# the ISA and the OS; Phase 3 uses an authoritative host. Linux goldens stay unrecorded on purpose,
# because 'melee' does not melee there and a golden for that would rebuild the P0.5c mistake.
golden_count=$(ls "$DIR"/*."$TAG".hash 2>/dev/null | wc -l | tr -d ' ')
if [ "$golden_count" = "0" ]; then
	goldens_required=0
	echo "platform is UNRECORDED -- digests are informational; coverage and liveness still enforced"
else
	goldens_required=1
fi

if [ -z "$DATA_PATH" ]; then
	echo "usage: $0 <data-path>   (or set FLARE_TEST_DATA_PATH)"
	echo "the data path needs default + the flare-game mods; neither repo is runnable alone"
	exit 2
fi

if [ ! -x ./flare-server ]; then
	echo "FAIL: ./flare-server not built"
	exit 1
fi

# A HOME OF ITS OWN.
#
# The simulation reads the user's config file. Measured, not assumed: one line in
# ~/.config/flare/settings.txt --
#
#   auto_equip=0
#
# -- makes the 'autoequip' row below report equipped=0 and digest 0xa3b5c21e37e4adef, which is
# BYTE FOR BYTE the digest produced by a build with MenuInventory::add()'s auto-equip branch
# deliberately sabotaged. A config file and a broken engine are indistinguishable from here.
#
# This is the P0.5d problem again in a different coat: something machine-local reaching the
# simulation. There the route was the window size, and the fix was to cut the route. Here the
# route is legitimate -- auto_equip really is a player preference -- so the fix is to stop the
# corpus asking the developer's account what the player prefers. Every row runs against the
# shipped defaults, and an empty settings.txt is what says so.
#
# This also owns the save games, because make-fixture.sh writes under $HOME.
FLARE_TEST_HOME="$(mktemp -d)"
mkdir -p "$FLARE_TEST_HOME/.config/flare"
: > "$FLARE_TEST_HOME/.config/flare/settings.txt"
trap 'rm -rf "$FLARE_TEST_HOME"' INT TERM
export HOME="$FLARE_TEST_HOME"

# The fixtures decide which map loads, where the player stands and how hard they hit. Without them
# the server sits on the title screen and there is no world to digest. The MANIFEST's 'slot'
# column picks which of the three each recording boots from.
#
# NOT `|| true`, and no longer a path outside the repository. It used to be both, so in CI this
# step silently did nothing, no save was ever written, and every replay hashed an empty world in
# which no map had loaded. A setup step that is allowed to fail quietly is not a setup step.
if ! ./tests/make-fixture.sh > /dev/null; then
	echo "FAIL: tests/make-fixture.sh did not run; there is no world to hash"
	exit 1
fi

# Test-only mods live in the repo and are linked into the data path, the same way CI assembles
# 'default' and the flare-game mods. Keeping the source of truth in the tree is the point: a mod
# generated into a scratch directory is one nobody can review, and P0.5c is what happens when a
# setup step is invisible. Only MANIFEST rows that name a mod in their 'mods' column load it.
for m in tests/mods/*/; do
	[ -d "$m" ] || continue
	name=$(basename "$m")
	if [ ! -e "$DATA_PATH/mods/$name" ]; then
		if ! ln -s "$(cd "$m" && pwd)" "$DATA_PATH/mods/$name"; then
			echo "FAIL: could not link test mod '$name' into $DATA_PATH/mods"
			exit 1
		fi
	fi
done

fail=0
while read -r name rec ticks slot mods survives equipped carried equipset requires; do
	case "$name" in ''|\#*) continue ;; esac

	golden="$DIR/$name.$TAG.hash"

	# Every behavioural check below records into this and NONE of them short-circuits the rest.
	# They used to 'continue' on the first failure, and that hid real findings: 'beatdown' fails
	# its coverage requirement on Linux, so its gear numbers were never printed -- and they differ
	# too (macOS loots nothing and ends with 0 carried slots, x86-64 Linux loots twice and ends
	# with 1). One divergence was masking another. The digest is still skipped when any of them
	# fails, because a hex mismatch on top of a known behavioural failure tells nobody anything.
	row_fail=0

	# The 'mods' column is EXTRA mods appended to the default list, or '-' for none. Order
	# matters: later mods override earlier ones, so a test override has to land last.
	row_mods="$MODS"
	[ "$mods" = "-" ] || row_mods="$MODS,$mods"

	out=$(./flare-server --headless --data-path="$DATA_PATH" --mods="$row_mods" --load-slot="$slot" \
	        --replay="$DIR/$rec.rec" --max-ticks="$ticks" --hash 2>/dev/null || true)

	got=$(echo "$out" | grep '^0x' || true)
	events=$(echo "$out" | grep '^simevents' || true)

	# COVERAGE, checked before the digest.
	#
	# A digest says the world changed. It cannot say which code ran, and that is not a
	# theoretical gap: P0.5b's recording named 'attack' pressed the wrong key against an empty
	# action bar, on a server whose enemy AI was switched off by encounter_dist = 0, and its
	# digest matched its golden on three platforms for two plans. Every combat sound path in
	# the engine was covered by nothing and the suite was green. See plans/phase0/P0.5c.
	if [ -n "$requires" ] && [ "$requires" != "-" ]; then
		missing=""
		for want in $(echo "$requires" | tr ',' ' '); do
			n=$(echo "$events" | tr ' ' '\n' | sed -n "s/^$want=//p")
			if [ -z "$n" ] || [ "$n" = "0" ]; then
				missing="$missing $want"
			fi
		done
		if [ -n "$missing" ]; then
			echo "FAIL $name: required simulation events never fired:$missing"
			echo "     got: $events"
			echo "     the recording no longer does what its name says. Fix the recording or"
			echo "     the fixture -- do NOT weaken the 'requires' column in $DIR/MANIFEST."
			row_fail=1
		fi
	fi

	# LIVENESS, checked before the digest. Enforced in BOTH directions.
	#
	# The coverage block above asks whether an event EVER fired. It cannot see a recording whose
	# player dies a third of the way in and leaves a corpse for the rest: every required event
	# has already fired by then. Measured when this check was added -- beatdown died at 1186 of
	# 2956 and patrol at 1101 of 2000, and both were green.
	#
	# The gate is survival, not activity. An earlier version of this check required the last
	# simulation event to land in the final third; it was wrong, and the measurement that killed
	# it is in plans/phase0/P0.5e -- smoke's last event is at tick 378 of 600 while its world
	# goes on changing to the last tick. Events stopping is not the world stopping. Dying is
	# specific, it is what the fixtures claim, and it cannot be satisfied by lowering a number.
	#
	# P0.5e checked one direction, and that turned out to exclude real code from the suite. The
	# death penalty -- currency loss, XP loss, and a random item destroyed with sim_rng -- lives
	# in MenuInventory::logic() and runs only when the player dies, so 'no recording may die'
	# meant it could never execute under test. P1.3b moves that code out of the menus, and a
	# survives=no row is what makes the move verifiable. Both directions are enforced because a
	# fixture that stops dying disarms its test exactly as silently as one that starts.
	# GEAR, checked before the digest, for the same reason coverage is: a digest can only say
	# "different", and P1.3's own risk note says the way this refactor goes wrong is that
	# "a subtle error means players lose gear".
	#
	# Not redundant with the digest. Equipment contents are hashed, so a lost item does move the
	# golden -- but so does everything else, and the person reading a red run gets a hex number
	# and no idea which of the twenty things in the digest moved. This says the word.
	#
	# It is also the ONLY check that can fail when auto-equip breaks on the 'autoequip' row, in
	# the sense that matters: the row exists because P1.3d measured that the rest of the corpus
	# cannot see equipping at all. Forcing every equipment slot disabled moved no digest on any
	# of the six recordings that predate it.
	got_equipped=$(echo "$events" | tr ' ' '\n' | sed -n 's/^equipped=//p')
	[ -n "$got_equipped" ] || got_equipped="?"

	if [ "$equipped" != "$got_equipped" ]; then
		echo "FAIL $name: $got_equipped equipment slots filled at the end, and this row says $equipped"
		echo "     got: $events"
		echo "     gear arriving or falling out is what P1.3 says the danger is. Find out which"
		echo "     slot changed -- do NOT edit the number in $DIR/MANIFEST to match."
		row_fail=1
	fi

	# CARRIED, checked with 'equipped' rather than after it, because the PAIR is the check and
	# neither number is worth much alone. P1.3d-4 moves the item storage out of the menus, and the
	# way that goes wrong is one copy of the data quietly becoming two. Measured on the 'invdrag'
	# row below, by sabotaging MenuInventory::activate() in each direction:
	#
	#   healthy, the item moves     equipped=1  carried=0     <- the only combination that passes
	#   the item is COPIED          equipped=1  carried=1     <- caught by this check
	#   the item is LOST in transit equipped=0  carried=0     <- caught by the check above
	#
	# A digest moves in all three cases and cannot tell them apart. These two numbers can.
	got_carried=$(echo "$events" | tr ' ' '\n' | sed -n 's/^carried=//p')
	[ -n "$got_carried" ] || got_carried="?"

	if [ "$carried" != "$got_carried" ]; then
		echo "FAIL $name: $got_carried carried slots filled at the end, and this row says $carried"
		echo "     got: $events"
		echo "     read this together with 'equipped' above: both rising is an item duplicated,"
		echo "     both falling is one destroyed. Do NOT edit the number in $DIR/MANIFEST."
		row_fail=1
	fi

	# WHICH SET, checked with the count above, because they fail differently. A refactor that
	# loses an item moves 'equipped'; one that loses track of which set is live moves this and
	# leaves 'equipped' untouched, and the player is standing there in nothing.
	#
	# The 'equipswap' row is the only one that changes it, and it is worth saying what that row
	# measured: with set 2 empty, one tick of EQUIPMENT_SWAP does not merely move a scalar -- the
	# same recording with that one bit cleared takes 13 steps instead of 14 and its last event
	# lands two ticks earlier, because a character wearing nothing moves differently.
	got_equipset=$(echo "$events" | tr ' ' '\n' | sed -n 's/^equipset=//p')
	[ -n "$got_equipset" ] || got_equipset="?"

	if [ "$equipset" != "$got_equipset" ]; then
		echo "FAIL $name: equipment set $got_equipset active at the end, and this row says $equipset"
		echo "     got: $events"
		echo "     the character is wearing a different half of its equipment than this row"
		echo "     expects -- do NOT edit the number in $DIR/MANIFEST to match."
		row_fail=1
	fi

	died=$(echo "$events" | tr ' ' '\n' | sed -n 's/^died_tick=//p')
	[ -n "$died" ] || died=0

	if [ "$survives" = "yes" ] && [ "$died" != "0" ]; then
		echo "FAIL $name: the player died at tick $died of $ticks, and this row says survives=yes"
		echo "     the rest of the recording is a corpse, and every 'requires' entry still"
		echo "     passes because each one fired before the death. Re-tune the fixture in"
		echo "     tests/make-fixture.sh -- do NOT lower the tick budget in $DIR/MANIFEST,"
		echo "     do NOT weaken the 'requires' column, and do NOT flip this row to survives=no."
		row_fail=1
	fi

	if [ "$survives" = "no" ] && [ "$died" = "0" ]; then
		echo "FAIL $name: the player survived $ticks ticks, and this row says survives=no"
		echo "     this row exists to reach code that only runs on death -- the death penalty,"
		echo "     now MenuInventory::applyDeathPenalty(). A fixture that stopped dying has"
		echo "     disarmed it silently,"
		echo "     which is the same failure as a survives=yes row that started dying."
		echo "     Re-tune the fixture in tests/make-fixture.sh; do NOT flip this row to yes."
		row_fail=1
	fi

	if [ "$row_fail" != "0" ]; then
		fail=1
		continue
	fi

	if [ -z "$got" ]; then
		echo "FAIL $name: no digest produced (replay refused, or the server died)"
		fail=1
	elif [ "$goldens_required" = "0" ]; then
		# Unrecorded platform. The digest is printed rather than swallowed, so one CI run is still
		# enough to record this platform's goldens should anyone want to. Not a failure: there is
		# nothing here to regress against, and the checks that CAN fail on this platform --
		# coverage and liveness -- already ran above and passed.
		echo "info $name  $got  (no golden for $TAG)"
	elif [ ! -f "$golden" ]; then
		echo "FAIL $name: no golden, but this platform has $golden_count others -- partial recording"
		echo "     to record it: echo $got > $golden"
		fail=1
	elif [ "$got" != "$(cat "$golden")" ]; then
		echo "FAIL $name: expected $(cat "$golden")  got $got"
		echo "     bisect with: ./flare-server --headless --data-path=$DATA_PATH --mods=$row_mods \\"
		echo "                    --load-slot=$slot --replay=$DIR/$rec.rec --max-ticks=$ticks --hash-every=1"
		fail=1
	else
		echo "ok   $name  $got"
	fi
done < "$DIR/MANIFEST"

# RESOLUTION INVARIANCE.
#
# A golden file cannot express "the monitor must not matter" -- it can only record one monitor's
# answer. Until P0.5d, encounter_dist was derived from view_w/view_h, so it did, and the goldens
# in this directory silently encoded the machine that recorded them: the same recording gave
# 0x4630f00a4eedf12f with no config file and 0x4b876e551cb7be0d at 1920x1080. The two resolutions
# below are the extremes of the measured range (encounter_dist 10.770 and 19.849).
#
# Two resolutions, one recording, one digest. If this fails, something view-derived has been
# wired back into the simulation; find it, do NOT delete this check.
echo "resolution invariance:"
inv_prev=""
for res in 1280x720 5120x1440; do
	h="$(mktemp -d)"
	mkdir -p "$h/.config/flare"
	printf 'fullscreen=0\nresolution_w=%s\nresolution_h=%s\n' "${res%x*}" "${res#*x}" \
		> "$h/.config/flare/settings.txt"
	HOME="$h" ./tests/make-fixture.sh > /dev/null
	inv=$(HOME="$h" ./flare-server --headless --data-path="$DATA_PATH" --mods="$MODS" \
	        --load-slot=2 --replay="$DIR/melee.rec" --max-ticks=600 --hash 2>/dev/null \
	      | grep '^0x' || true)
	rm -rf "$h"
	echo "  $res  $inv"
	if [ -z "$inv" ]; then
		echo "FAIL: no digest at $res"
		fail=1
	elif [ -n "$inv_prev" ] && [ "$inv" != "$inv_prev" ]; then
		echo "FAIL: the simulation depends on the window size"
		fail=1
	fi
	inv_prev="$inv"
done

# Kept on failure, deliberately. Utils::logInfo writes to $HOME/.config/flare/flare_log.txt and
# nowhere else, so deleting this directory on a red run throws away the only detailed account of
# what the server did.
if [ "$fail" -eq 0 ]; then
	rm -rf "$FLARE_TEST_HOME"
else
	echo "server log: $FLARE_TEST_HOME/.config/flare/flare_log.txt"
fi

if [ "$fail" -eq 0 ]; then
	if [ "$goldens_required" = "0" ]; then
		echo "behaviour OK on $TAG -- every recording ran, covered its required events and ended as"
		echo "the manifest says. No goldens recorded for this platform, so nothing was compared."
	else
		echo "all replays match"
	fi
else
	echo "REPLAY MISMATCH -- the simulation changed"
fi
exit $fail
