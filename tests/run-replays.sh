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

# Goldens are PER-PLATFORM, and that is a finding rather than a convenience.
#
# The same recording produces a different digest on macOS/arm64 than on Linux/x86-64 -- all three
# recordings differ, measured in CI. The simulation is deterministic on a given build and not
# bit-identical across builds. Floating point is why: different libm implementations for sin/cos/
# sqrt/pow, and different freedom to contract multiply-add.
#
# Set FLARE_GOLDEN_SUFFIX to the compiler (gcc/clang) so that a compiler-level divergence is
# distinguishable from an architecture-level one. That distinction matters a great deal for
# Phase 3: if two compilers on ONE machine disagree, lockstep between differently-built peers is
# impossible and the design has to send state rather than inputs.
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

# The fixture decides which map loads. Without it the server sits on the title screen and there
# is no world to digest.
plans/artifacts/P0.5-make-fixture.sh > /dev/null 2>&1 || true

fail=0
while read -r name ticks; do
	case "$name" in ''|\#*) continue ;; esac

	golden="$DIR/$name.$TAG.hash"

	got=$(./flare-server --headless --data-path="$DATA_PATH" --mods="$MODS" --load-slot=1 \
	        --replay="$DIR/$name.rec" --max-ticks="$ticks" --hash 2>/dev/null | grep '^0x' || true)

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
		echo "                    --load-slot=1 --replay=$DIR/$name.rec --max-ticks=$ticks --hash-every=1"
		fail=1
	else
		echo "ok   $name  $got"
	fi
done < "$DIR/MANIFEST"

if [ "$fail" -eq 0 ]; then
	echo "all replays match"
else
	echo "REPLAY MISMATCH -- the simulation changed"
fi
exit $fail
