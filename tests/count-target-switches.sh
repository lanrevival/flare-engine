#!/bin/sh
# Reads --dump-ai-targets output on stdin and counts, per hostile entity, how often its target
# (the `target=` field) changes, bucketed into non-overlapping 100-tick windows. Written for
# P2.4's AC4: oscillating aggro ("nearest player" recomputed every tick, flipping between two
# near-equidistant players) passes AC3 (aggro spreads across players) with flying colours and
# looks terrible in play -- this is what actually catches it.
#
#   ./flare-server ... --dump-ai-targets | ./tests/count-target-switches.sh
#
# Input line shape (main_server.cpp's serverDumpAiTargets()):
#   tick=T entity=I target_los=0|1 target=<player id>|none
#
# Output: one line per entity per 100-tick window that saw at least one switch --
#   entity=I window=W switches=S
# followed by a summary line --
#   max_switches_per_window=M
#
# Exit 0 if the max switches in any single window, for any entity, is below 3 (AC4's threshold).
# Exit 1 otherwise. An entity/window with zero switches is not printed (a quiet enemy is not
# news); the summary line is always printed, even if input was empty (M=0, exit 0).

awk '
BEGIN {
	FS = " ";
	max_switches = 0;
}
{
	tick = 0; entity = ""; target = "";
	for (i = 1; i <= NF; i++) {
		if (split($i, kv, "=") == 2) {
			if (kv[1] == "tick") tick = kv[2] + 0;
			else if (kv[1] == "entity") entity = kv[2];
			else if (kv[1] == "target") target = kv[2];
		}
	}
	if (entity == "") next;

	window = int((tick - 1) / 100); # ticks 1..100 -> window 0, 101..200 -> window 1, ...

	# Flush the previous window tally for this entity the moment we see a later window --
	# windows are visited in non-decreasing tick order (the dump is written one line per tick),
	# so once we move past a window for an entity, its count is final.
	key = entity;
	if ((key in cur_window) && cur_window[key] != window) {
		if (switches[key] > 0) {
			printf "entity=%s window=%d switches=%d\n", key, cur_window[key], switches[key];
			if (switches[key] > max_switches) max_switches = switches[key];
		}
		switches[key] = 0;
	}
	cur_window[key] = window;

	if ((key in last_target) && last_target[key] != target) {
		switches[key]++;
	}
	last_target[key] = target;
}
END {
	# Flush whatever window each entity ended on.
	for (key in cur_window) {
		if (switches[key] > 0) {
			printf "entity=%s window=%d switches=%d\n", key, cur_window[key], switches[key];
			if (switches[key] > max_switches) max_switches = switches[key];
		}
	}
	printf "max_switches_per_window=%d\n", max_switches;
	exit (max_switches < 3) ? 0 : 1;
}
'
