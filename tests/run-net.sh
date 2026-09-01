#!/bin/sh
# Launches one flare-server --dedicated and two headless flare --connect clients on loopback,
# lets them run a short scripted session, and asserts every process exited 0 and the server saw
# both peers connect.
#
#   tests/run-net.sh [data-path]
#
# Exit 0 = all three processes exited clean and the server's --dump-players shows 2 connected
# peers. Exit 1 = any process failed, or the server never reported both peers.
#
# This is P3.7's own acceptance test, not a digest-equality check: before P3.8's mirror model
# (D27) a --connect client still fully self-simulates, so client and server world state is not
# expected to agree yet -- see plans/phase3/P3.7-headless-client-and-scripted-clients.md's "Why
# not a digest-equality acceptance criterion". This script only proves the headless client
# machinery itself works end to end: it boots, connects, is driven by a script, and disconnects
# cleanly, all with no window, GPU, or audio device anywhere in the process tree.

set -e
cd "$(dirname "$0")/.."

DATA_PATH="${1:-$FLARE_TEST_DATA_PATH}"
MODS="fantasycore,empyrean_campaign"

if [ -z "$DATA_PATH" ]; then
	echo "usage: $0 <data-path>   (or set FLARE_TEST_DATA_PATH)"
	echo "the data path needs default + the flare-game mods; neither repo is runnable alone"
	exit 2
fi

if [ ! -x ./flare-server ]; then
	echo "FAIL: ./flare-server not built"
	exit 1
fi
if [ ! -x ./flare ]; then
	echo "FAIL: ./flare not built"
	exit 1
fi

OUT="$(mktemp -d)"
# Capture $? at trap entry and re-exit with it explicitly. Both cleanup commands are followed by
# '|| true': under 'set -e', a failing command inside a trap (kill on an already-empty PID
# reliably fails) aborts the trap immediately, so 'exit $ec' below is never reached and the
# script's real result is silently replaced by that failing command's own status -- found by
# tracing a run that printed PASS and still exited 1.
trap 'ec=$?; kill "${SERVER_PID:-}" 2>/dev/null || true; rm -rf "$OUT" || true; exit $ec' INT TERM EXIT

# A save game to load, same mechanism run-replays.sh uses -- make-fixture.sh writes under $HOME.
FIXTURE_HOME="$OUT/fixture_home"
mkdir -p "$FIXTURE_HOME/.config/flare"
: > "$FIXTURE_HOME/.config/flare/settings.txt"
HOME="$FIXTURE_HOME" ./tests/make-fixture.sh > /dev/null

PORT=$((37800 + (RANDOM % 1000)))
SERVER_HOME="$OUT/server_home"
mkdir -p "$SERVER_HOME/.config/flare" "$SERVER_HOME/.local/share/flare/saves"
: > "$SERVER_HOME/.config/flare/settings.txt"
cp -r "$FIXTURE_HOME/.local/share/flare/saves/empyrean" "$SERVER_HOME/.local/share/flare/saves/empyrean"

echo "starting server on port $PORT"
# --max-ticks is deliberately SHORTER than the clients' -- --dump-players only prints at
# construction (before any guest connects) and at THIS process's own exit, never mid-session, so
# the exit dump has to land while both scripted clients are still connected. The script
# (tests/scripts/p3.7-two-players.txt) disconnects at tick 400; 200 leaves a comfortable margin
# for both the connection handshake and normal tick-rate jitter between three independent
# processes.
HOME="$SERVER_HOME" ./flare-server --dedicated --port="$PORT" --data-path="$DATA_PATH" \
	--mods="$MODS" --load-slot=1 --max-players=4 --max-ticks=200 --dump-players \
	> "$OUT/server.out" 2> "$OUT/server.err" &
SERVER_PID=$!

listening=""
for i in $(seq 1 100); do
	# Utils::logInfo goes through SDL_LogMessageV, which lands on stderr, not stdout -- check both
	# so this does not depend on that plumbing detail.
	if grep -q "listening on port" "$OUT/server.out" "$OUT/server.err" 2>/dev/null; then
		listening=1
		break
	fi
	sleep 0.05
done
if [ -z "$listening" ]; then
	echo "FAIL: server never reported listening"
	echo "--- server.err ---"; cat "$OUT/server.err"
	exit 1
fi

fail=0
client_pids=""
for n in 1 2; do
	CLIENT_HOME="$OUT/client${n}_home"
	mkdir -p "$CLIENT_HOME/.config/flare" "$CLIENT_HOME/.local/share/flare/saves"
	: > "$CLIENT_HOME/.config/flare/settings.txt"
	cp -r "$FIXTURE_HOME/.local/share/flare/saves/empyrean" "$CLIENT_HOME/.local/share/flare/saves/empyrean"

	HOME="$CLIENT_HOME" ./flare --headless --data-path="$DATA_PATH" --mods="$MODS" \
		--load-slot=1 --connect="127.0.0.1:$PORT" \
		--script=tests/scripts/p3.7-two-players.txt --max-ticks=600 --no-lock-file \
		> "$OUT/client${n}.out" 2> "$OUT/client${n}.err" &
	client_pids="$client_pids $!"
done

client_fail=0
for pid in $client_pids; do
	if ! wait "$pid"; then
		client_fail=1
	fi
done
if [ "$client_fail" != "0" ]; then
	echo "FAIL: a headless client exited non-zero"
	for n in 1 2; do
		echo "--- client${n}.err (tail) ---"; tail -20 "$OUT/client${n}.err"
	done
	fail=1
fi

wait "$SERVER_PID"
server_rc=$?
SERVER_PID=""
if [ "$server_rc" != "0" ]; then
	echo "FAIL: server exited $server_rc"
	echo "--- server.err (tail) ---"; tail -20 "$OUT/server.err"
	fail=1
fi

peers=$(grep -c "^player id=" "$OUT/server.out" || true)
# --dump-players prints once right after construction (just local id 0, before either guest joins)
# and again at exit (every player still known to the server) -- see main_server.cpp's --dump-players
# doc comment. Two guests plus the exit-time local id 0 line is 3; the construction-time line brings
# it to 4. Fewer than that means at least one guest never made it into playerm->players.
if [ "$peers" -lt 3 ]; then
	echo "FAIL: server's --dump-players shows $peers 'player id=' lines, expected at least 3 (2 guests + local id 0 at exit)"
	echo "--- server.out ---"; cat "$OUT/server.out"
	fail=1
fi

if [ "$fail" = "0" ]; then
	echo "PASS: server + 2 headless scripted clients ran, connected, and exited clean"
fi

exit $fail
