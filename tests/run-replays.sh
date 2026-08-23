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

if [ -z "$DATA_PATH" ]; then
	echo "usage: $0 <data-path>   (or set FLARE_TEST_DATA_PATH)"
	echo "the data path needs default + the flare-game mods; neither repo is runnable alone"
	exit 2
fi

if [ ! -x ./flare-server ]; then
	echo "FAIL: ./flare-server not built"
	exit 1
fi

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

fail=0
while read -r name ticks slot requires; do
	case "$name" in ''|\#*) continue ;; esac

	golden="$DIR/$name.$TAG.hash"

	out=$(./flare-server --headless --data-path="$DATA_PATH" --mods="$MODS" --load-slot="$slot" \
	        --replay="$DIR/$name.rec" --max-ticks="$ticks" --hash 2>/dev/null || true)

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
			fail=1
			continue
		fi
	fi

	# LIVENESS, checked before the digest.
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
	died=$(echo "$events" | tr ' ' '\n' | sed -n 's/^died_tick=//p')
	if [ -n "$died" ] && [ "$died" != "0" ]; then
		echo "FAIL $name: the player died at tick $died of $ticks"
		echo "     the rest of the recording is a corpse, and every 'requires' entry still"
		echo "     passes because each one fired before the death. Re-tune the fixture in"
		echo "     tests/make-fixture.sh -- do NOT lower the tick budget in $DIR/MANIFEST,"
		echo "     and do NOT weaken the 'requires' column."
		fail=1
		continue
	fi

	if [ -z "$got" ]; then
		echo "FAIL $name: no digest produced (replay refused, or the server died)"
		fail=1
	elif [ ! -f "$golden" ]; then
		# Print the digest rather than swallowing it, so one CI run is enough to record a new
		# platform's goldens. Still a failure: an unrecorded platform is untested, not passing.
		echo "FAIL $name: no golden for this platform -- digest is $got"
		echo "     to record it: echo $got > $golden"
		fail=1
	elif [ "$got" != "$(cat "$golden")" ]; then
		echo "FAIL $name: expected $(cat "$golden")  got $got"
		echo "     bisect with: ./flare-server --headless --data-path=$DATA_PATH --mods=$MODS \\"
		echo "                    --load-slot=$slot --replay=$DIR/$name.rec --max-ticks=$ticks --hash-every=1"
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

if [ "$fail" -eq 0 ]; then
	echo "all replays match"
else
	echo "REPLAY MISMATCH -- the simulation changed"
fi
exit $fail
