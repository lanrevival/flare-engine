/*
Copyright © 2026 Flare LAN Revival contributors

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
 * flare-server entry point.
 *
 * Runs the simulation with no window, no GPU and no audio device, using the null devices
 * from DeviceList. This is init() and mainLoop() from main.cpp with everything
 * presentation-shaped removed; it is not shared code yet because the sim/presentation split
 * is P1.4. Until then the duplication is deliberate and the two must be kept in step.
 *
 * There is no networking here. That is Phase 3.
 */

#include <csignal>
#include <cstdio>
#include <ctime>
#include <cstdlib>

#include "AnimationManager.h"
#include "Avatar.h"
#include "PlayerInventory.h"
#include "SharedGameResources.h"
#include "CombatText.h"
#include "CommonIncludes.h"
#include "DeviceList.h"
#include "EngineSettings.h"
#include "FontEngine.h"
#include "GameSwitcher.h"
#include "InputState.h"
#include "MenuInventory.h"
#include "MenuManager.h"
#include "MessageEngine.h"
#include "ModManager.h"
#include "RenderDevice.h"
#include "Replay.h"
#include "SimEvents.h"
#include "WorldHash.h"
#include "Rng.h"
#include "SaveLoad.h"
#include "Settings.h"
#include "SharedResources.h"
#include "SoundManager.h"
#include "Stats.h"
#include "TooltipManager.h"
#include "Utils.h"
#include "UtilsFileSystem.h"
#include "UtilsParsing.h"
#include "Version.h"

#include "NullRenderDevice.h"

#include <SDL.h>

GameSwitcher *gswitch;

class ServerCmdLineArgs {
public:
	ServerCmdLineArgs() : mod_list(), load_slot(), data_path(), max_ticks(0), hash_every(0), hash_at_exit(false), sim_seed(RNG_DEFAULT_SIM_SEED), record_path(), replay_path() {}
	std::vector<std::string> mod_list;
	std::string load_slot;
	std::string data_path;
	unsigned long max_ticks;
	unsigned long hash_every;
	bool hash_at_exit;
	uint64_t sim_seed;
	std::string record_path;
	std::string replay_path;
};

// The Platform implementations are compiled by #include-ing them into the entry point rather
// than as translation units of their own, so the server has to repeat what main.cpp does or
// the global 'platform' instance is never defined. Only the desktop platforms are listed: a
// dedicated server on Android, iOS, GCW0 or Emscripten is not a thing we intend to support.
#define PLATFORM_CPP_INCLUDE

#ifdef _WIN32
#include "PlatformWin32.cpp"
#else
// Linux stuff should work on Mac OSX/BSD/etc, too
#include "PlatformLinux.cpp"
#endif

// Set from a signal handler, so it must be sig_atomic_t and volatile. Nothing else may be
// touched from the handler -- logging or freeing from here is not async-signal-safe.
static volatile sig_atomic_t shutdown_requested = 0;

extern "C" void serverSignalHandler(int sig) {
	(void)sig;
	shutdown_requested = 1;
}

static void serverInit(const ServerCmdLineArgs& args) {
	platform.setPaths();

	settings->setCustomPathData();
	settings->setGame();

	// The server never participates in the single-instance lock: several servers, or a
	// server alongside a client, is the normal case on one machine.
	settings->no_lock_file = true;
	settings->headless = true;

	Utils::createLogFile();
	Utils::logInfo("%s", VersionInfo::createVersionStringFull().c_str());
	Utils::logInfo("flare-server: headless, no window and no audio device.");

	Utils::logInfo("main_server: PATH_CONF = '%s'", settings->path_conf.c_str());
	Utils::logInfo("main_server: PATH_USER = '%s'", settings->path_user.c_str());
	Utils::logInfo("main_server: PATH_DATA = '%s'", settings->path_data.c_str());

	// SDL_INIT_TIMER only. No VIDEO, no AUDIO, no GAMECONTROLLER -- requesting any of those
	// would defeat the entire point of this binary and fail on a machine with no display.
	if (SDL_Init(SDL_INIT_TIMER) < 0) {
		Utils::logError("main_server: Could not initialize SDL: %s", SDL_GetError());
		Utils::Exit(1);
	}

	mods = new ModManager(&(args.mod_list));

	if (!mods->haveFallbackMod()) {
		Utils::logError("main_server: Could not find the default mod in the following locations:");
		Utils::logError("%smods/", settings->path_user.c_str());
		Utils::logError("%smods/", settings->path_data.c_str());
		Utils::logError("A copy of the default mod is in the \"mods\" directory of the flare-engine repo.");
		Utils::Exit(1);
	}

	settings->loadSettings();

	// Audio is off at the source as well as at the device, so nothing even tries to decode.
	settings->audio = false;

	// The server is the simulation authority, so sim_rng's seed is the one that will eventually
	// be handed to joining clients (Phase 3). Fixed for now. See Rng.h.
	sim_rng = new Rng();
	sim_rng->seed(args.sim_seed);
	fx_rng = new Rng();
	fx_rng->seed(static_cast<uint64_t>(time(NULL)));

	// What the simulation did, for the presentation layer to react to. Emptied every tick.
	sim_events = new SimEventQueue();
	Utils::logInfo("main_server: sim_rng seeded with 0x%llx", static_cast<unsigned long long>(sim_rng->getSeed()));

	replay = new Replay();

	save_load = new SaveLoad();
	msg = new MessageEngine();
	font = getFontEngine();
	anim = new AnimationManager();
	comb = new CombatText();

	eset = new EngineSettings();
	eset->load();

	inpt = getInputManager(true);
	icons = NULL;

	Stats::init();

	platform.setScreenSize();

	render_device = getRenderDevice("null");

	// If this ever stops being a NullRenderDevice, the server has quietly regained a
	// dependency on a display. Fail loudly rather than discovering it on a headless box.
	if (dynamic_cast<NullRenderDevice*>(render_device) == NULL) {
		Utils::logError("main_server: --headless requires the null render device, but got something else.");
		Utils::Exit(1);
	}

	if (render_device->createContext() == -1) {
		Utils::logError("main_server: Could not create rendering context.");
		Utils::Exit(1);
	}
	render_device->reloadGraphics();

	snd = getSoundManager(true);

	tooltipm = new TooltipManager();

	gswitch = new GameSwitcher();
}

static void serverCleanup() {
	delete gswitch;

	delete anim;
	delete comb;
	delete font;
	delete inpt;
	delete mods;
	delete msg;
	delete snd;
	delete save_load;
	delete eset;
	delete replay;
	replay = NULL;
	delete sim_rng;
	delete fx_rng;
	delete sim_events;
	sim_events = NULL;
	delete tooltipm;

	if (render_device)
		render_device->destroyContext();
	delete render_device;

	SDL_Quit();
}

static float getSecondsElapsed(uint64_t prev_ticks, uint64_t now_ticks) {
	return (static_cast<float>(now_ticks - prev_ticks) / static_cast<float>(SDL_GetPerformanceFrequency()));
}

/**
 * main.cpp's mainLoop() with every presentation step removed: no blankScreen(), no
 * gswitch->render(), no commitFrame(), no FPS counter. The fixed-step logic ticker is kept
 * exactly as-is; pinning the tick rate is P0.4 and must not be done here.
 */
// Mirrors main.cpp. A stall must not become a fast-forward.
static const int MAX_CATCHUP_TICKS = 5;

// How often the trajectory digest samples the world. 30 ticks is twice a second at
// Settings::SIM_TICK_HZ.
//
// A digest of the FINAL state only is not enough, and that is measured rather than assumed:
// raising the hero's melee damage by one point in engine/stats.txt did not move smoke, patrol or
// melee, even though melee.rec kills two goblins. The extra damage changed when they died, not
// whether -- a dead enemy is at hp 0 either way, so the end state converges and the difference
// disappears. A golden that cannot see a change to weapon damage is not much of a regression
// test for a combat refactor. Sampling the whole run fixes that; --hash-every is still the tool
// for finding WHICH tick diverged.
static const unsigned long TRAJECTORY_SAMPLE_TICKS = 30;

static unsigned long serverMainLoop(unsigned long max_ticks, unsigned long hash_every,
                                    uint64_t* trajectory, unsigned long* last_event_tick,
                                    unsigned long* died_tick) {
	bool done = false;
	unsigned long total_ticks = 0;
	uint64_t traj = WorldHash::init();

	unsigned long prev_ev_total = sim_events->getTotal();
	unsigned long last_ev_tick = 0;
	unsigned long died_at = 0;

	// The server renders nothing, so there is only one rate here: the shared simulation step.
	// --max-fps is accepted and ignored on purpose; see parseServerArgs().
	const float seconds_per_sim_tick = 1.f/static_cast<float>(Settings::SIM_TICK_HZ);

	uint64_t prev_ticks = SDL_GetPerformanceCounter();
	uint64_t logic_ticks = SDL_GetPerformanceCounter();

	while (!done) {
		int loops = 0;
		uint64_t now_ticks = SDL_GetPerformanceCounter();


		// Bound the accumulated debt. Capping work per iteration is not enough on its own --
		// the outer loop simply spins until the debt is repaid, so a five second stall still
		// replays 300 ticks in milliseconds. Clients cannot follow that, so the missed time is
		// dropped rather than simulated. Measured: without this, a 5s SIGSTOP cost 0s of wall
		// clock; with it, it costs 5s.
		const uint64_t max_debt = static_cast<uint64_t>(
			seconds_per_sim_tick * static_cast<float>(MAX_CATCHUP_TICKS)
			* static_cast<float>(SDL_GetPerformanceFrequency()));
		if (now_ticks > logic_ticks + max_debt)
			logic_ticks = now_ticks - max_debt;
		while (now_ticks >= logic_ticks && loops < MAX_CATCHUP_TICKS) {
			if (gswitch->isLoadingFrame()) {
				logic_ticks = now_ticks;
				break;
			}

			// No SDL_PumpEvents(): there is no event queue worth pumping without a window.
			// inpt->handle() is still called because NullInputState inherits InputState's
			// bookkeeping, which the game logic expects to have run.
			inpt->handle();

			// Drive input from the recording before the logic that reads it. Recording
			// happens at the same point so that a record/replay round trip sees the same
			// state at the same moment.
			if (replay && replay->isPlaying())
				replay->applyTick(total_ticks + 1);
			else if (replay && replay->isRecording())
				replay->recordTick(total_ticks + 1);

			gswitch->logic();
			inpt->resetScroll();

			total_ticks++;

			// Liveness. A recording that has stopped simulating still produces a digest and
			// still satisfies every 'requires' entry it satisfied earlier, because those ask
			// whether an event EVER fired. This is the tick it last did. See plans/phase0/P0.5e.
			unsigned long ev_total = sim_events->getTotal();
			if (ev_total != prev_ev_total) {
				prev_ev_total = ev_total;
				last_ev_tick = total_ticks;
			}

			// The tick the player died on, if they did. First transition only -- a corpse does
			// not come back, and the interesting number is when the recording stopped being
			// about a player. See plans/phase0/P0.5e.
			if (died_at == 0 && pc && !pc->stats.alive)
				died_at = total_ticks;

			// Per-tick digests make a divergence bisectable: diff two runs and the first
			// differing line is the exact tick they parted.
			if (hash_every > 0 && total_ticks % hash_every == 0) {
				printf("tick %lu %s\n", total_ticks,
				       WorldHash::toString(WorldHash::compute(total_ticks)).c_str());
			}

			if (total_ticks % TRAJECTORY_SAMPLE_TICKS == 0)
				traj = WorldHash::mixU64(traj, WorldHash::compute(total_ticks));

			done = gswitch->done || shutdown_requested;
			if (max_ticks > 0 && total_ticks >= max_ticks)
				done = true;

			// Leave the catch-up loop immediately. Setting 'done' does not end it -- its
			// condition is the accumulator, not this flag -- so without this break the server
			// simulates one to four extra ticks past the stopping point, and exactly how many
			// depends on wall-clock timing. That made --max-ticks nondeterministic and showed
			// up as a bimodal world digest before the tick-by-tick digest localised it.
			if (done)
				break;

			logic_ticks += static_cast<uint64_t>(seconds_per_sim_tick * static_cast<float>(SDL_GetPerformanceFrequency()));
			loops++;

			if (gswitch->isPaused()) {
				logic_ticks = now_ticks;
				break;
			}
		}

		if (shutdown_requested)
			done = true;

		// Same frame pacing as the client, minus the busy-wait: a dedicated server has no
		// reason to burn a core spinning for sub-millisecond accuracy.
		if (getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter()) < seconds_per_sim_tick) {
			int32_t delay_ms = static_cast<int32_t>((seconds_per_sim_tick - getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter())) * 1000.f);
			if (delay_ms > 0)
				SDL_Delay(delay_ms);
		}
		prev_ticks = SDL_GetPerformanceCounter();
	}

	// Always fold in the final state, whether or not it landed on a sample boundary.
	traj = WorldHash::mixU64(traj, WorldHash::compute(total_ticks));
	if (trajectory)
		*trajectory = traj;
	if (last_event_tick)
		*last_event_tick = last_ev_tick;
	if (died_tick)
		*died_tick = died_at;

	return total_ticks;
}

static void printHelp() {
	printf("Command line options:\n"
	       "--help                   Prints this message.\n"
	       "--version                Prints the release version.\n"
	       "--data-path=<PATH>       Specifies an exact path to look for mod data.\n"
	       "--mods=<MOD>,...         Starts the server with only these mods enabled.\n"
	       "--load-slot=<SLOT>       Loads a save slot by numerical index.\n"
	       "--max-ticks=<N>          Stops after N logic ticks. For testing.\n"
	       "--sim-seed=<N>           Seeds the simulation RNG. Default is fixed.\n"
	       "--record=<FILE>          Records per-tick input to FILE.\n"
	       "--replay=<FILE>          Replays input from FILE. Refuses a version or mod mismatch.\n"
	       "--hash                   Prints a digest of world state at exit.\n"
	       "--hash-every=<N>         Prints a digest every N ticks, for bisecting a divergence.\n"
	       "--max-fps=<N>            Render frame limit. Accepted and ignored: the server\n"
	       "                         renders nothing and always simulates at 60 Hz.\n"
	       "--headless               Accepted and implied; the server is always headless.\n");
}

static std::string parseServerArg(const std::string& arg) {
	if (arg.length() > 2 && arg.substr(0, 2) == "--") {
		size_t eq = arg.find('=');
		if (eq == std::string::npos)
			return arg.substr(2);
		return arg.substr(2, eq - 2);
	}
	return "";
}

static std::string parseServerArgValue(const std::string& arg) {
	size_t eq = arg.find('=');
	if (eq == std::string::npos)
		return "";
	return arg.substr(eq + 1);
}

int main(int argc, char *argv[]) {
	settings = new Settings();

	ServerCmdLineArgs args;
	bool done = false;

	for (int i = 1; i < argc; i++) {
		std::string arg_full = std::string(argv[i]);
		std::string arg = parseServerArg(arg_full);

		if (arg == "version") {
			printf("%s\n", VersionInfo::createVersionStringFull().c_str());
			done = true;
		}
		else if (arg == "help") {
			printHelp();
			done = true;
		}
		else if (arg == "headless") {
			// Accepted for symmetry with the client and for explicit scripts. The server
			// is always headless; there is no way to turn this off.
		}
		else if (arg == "data-path") {
			settings->custom_path_data = parseServerArgValue(arg_full);
		}
		else if (arg == "ignore-data-path") {
			settings->custom_path_data_ignore = true;
		}
		else if (arg == "mods") {
			std::string mod_list_str = parseServerArgValue(arg_full);
			while (!mod_list_str.empty())
				args.mod_list.push_back(Parse::popFirstString(mod_list_str));
		}
		else if (arg == "load-slot") {
			settings->load_slot = parseServerArgValue(arg_full);
		}
		else if (arg == "max-ticks") {
			args.max_ticks = strtoul(parseServerArgValue(arg_full).c_str(), NULL, 10);
		}
		else if (arg == "record") {
			args.record_path = parseServerArgValue(arg_full);
		}
		else if (arg == "replay") {
			args.replay_path = parseServerArgValue(arg_full);
		}
		else if (arg == "sim-seed") {
			// Phase 3 will take this from the host instead. Until then it exists so that the
			// RNG-to-world-state link is testable: two seeds must produce two digests.
			args.sim_seed = strtoull(parseServerArgValue(arg_full).c_str(), NULL, 0);
		}
		else if (arg == "hash") {
			args.hash_at_exit = true;
		}
		else if (arg == "hash-every") {
			args.hash_every = strtoul(parseServerArgValue(arg_full).c_str(), NULL, 10);
		}
		else if (arg == "max-fps") {
			// Accepted so that the sim-rate-vs-render-rate claim can actually be tested: a
			// server run at 30 and at 144 must produce the same tick count in the same wall
			// time. The value is stored and then never read, because the server renders
			// nothing -- which is exactly the property under test.
			settings->max_frames_per_sec = static_cast<unsigned short>(
				strtoul(parseServerArgValue(arg_full).c_str(), NULL, 10));
		}
		else {
			printf("'%s' is not a valid command line option. Try '--help' for a list of valid options.\n", argv[i]);
			delete settings;
			return 1;
		}
	}

	if (done) {
		delete settings;
		return 0;
	}

	signal(SIGINT, serverSignalHandler);
	signal(SIGTERM, serverSignalHandler);

	serverInit(args);

	// After serverInit so the mod list is loaded and can be validated against the recording.
	if (!args.replay_path.empty()) {
		if (!replay->startPlayback(args.replay_path)) {
			// Utils::Exit, not serverCleanup + return: the engine is half-initialised here and
			// ~AnimationManager asserts that every animation has been released, which has not
			// happened yet. This is the engine's own idiom for a fatal startup error, and it is
			// what the --headless assertion above uses.
			Utils::logError("main_server: refusing to replay. Nothing was simulated.");
			Utils::Exit(1);
		}
		// The recording's seed wins. Replaying under a different seed is not a replay.
		sim_rng->seed(replay->getSeed());
		Utils::logInfo("main_server: sim_rng reseeded from replay with 0x%llx",
		               static_cast<unsigned long long>(sim_rng->getSeed()));
	}
	else if (!args.record_path.empty()) {
		if (!replay->startRecording(args.record_path)) {
			Utils::Exit(1);
		}
	}

	uint64_t trajectory = WorldHash::init();
	unsigned long last_event_tick = 0;
	unsigned long died_tick = 0;
	unsigned long ticks = serverMainLoop(args.max_ticks, args.hash_every, &trajectory,
	                                     &last_event_tick, &died_tick);

	replay->finish();

	if (shutdown_requested)
		Utils::logInfo("main_server: shutdown requested, stopping.");
	Utils::logInfo("main_server: simulated %lu logic ticks.", ticks);

	// Reported unconditionally rather than behind a flag: an emit path with no drain is a slow
	// leak, and a leak you have to remember to ask about is one you find in production. This is
	// the deepest the queue ever got, not a total -- it should stay small no matter how long the
	// server ran. See SimEventQueue::getHighWater().
	Utils::logInfo("main_server: sim event queue high water = %lu",
	               static_cast<unsigned long>(sim_events->getHighWater()));

	// stdout and unconditional, for the same reason the high water is: a coverage claim you have
	// to remember to ask for is a coverage claim nobody checks. tests/run-replays.sh parses this
	// line to assert that a recording named 'attack' actually attacks -- P0.5b's did not, and the
	// digest could not tell anyone. Single line, "name=count" pairs, stable names.
	{
		printf("simevents");
		for (int i = 0; i < SimEvent::TYPE_COUNT; ++i)
			printf(" %s=%lu", SimEvent::typeName(i), sim_events->getCount(i));
		// Two liveness fields, on the same line for the same reason the counts are here at all:
		// a claim you have to remember to ask for is a claim nobody checks.
		//
		// died_tick is the gate run-replays.sh enforces (0 = the player survived). A fixture
		// that dies mid-recording leaves a corpse for the rest of the run: P0.5d's beatdown
		// died at 1186 of 2956 and every 'requires' entry still passed, because those ask only
		// whether an event EVER fired.
		//
		// last_tick is diagnostic, not a gate. It is the tick of the last simulation event, and
		// it is a poor liveness measure on its own -- smoke's last event is at 378 of 600 while
		// its world keeps changing to the final tick. It is printed because it is what made the
		// beatdown problem visible. See plans/phase0/P0.5e.
		printf(" last_tick=%lu died_tick=%lu", last_event_tick, died_tick);

		// How many equipment slots still hold something. Third liveness field, and the one that
		// speaks to P1.3's own stated failure mode: "a subtle error means players lose gear".
		//
		// Read through pinv, deliberately, while WorldHash.cpp still reads the same storages
		// through menu->inv. P1.3d-4a made MenuInventory::inventory a POINTER INTO this array
		// rather than a second one, and a commit that got that wrong produces byte-identical
		// goldens -- measured, all nine, so "nothing moved" is that bug's symptom too. One reader
		// on each side of the alias is what makes the pair falsifiable: give the menu its own
		// copy and these counters go to 0 while the digest does not notice.
		// The digest covers equipment contents, but a golden can only say "different", and the
		// obvious way for a refactor of the inventory to go wrong is for gear to quietly stop
		// arriving or quietly fall out. tests/replays/MANIFEST pins this per row, so that is a
		// named number a reviewer can read rather than a hex digest nobody can interpret.
		int equipped = 0;
		if (pinv) {
			int slots = pinv->inventory[PlayerInventory::EQUIPMENT].getSlotNumber();
			for (int i = 0; i < slots; ++i) {
				if (!pinv->inventory[PlayerInventory::EQUIPMENT][i].empty())
					equipped++;
			}
		}
		printf(" equipped=%d", equipped);

		// Which equipment set is live. Separate from the count above because they answer
		// different questions: 'equipped' is how much gear exists, 'equipset' is how much of it
		// the character is actually wearing. MenuInventory::isEquipSlotActive() returns false for
		// every slot when this is 0, so the two numbers can disagree completely.
		printf(" equipset=%d", pinv ? static_cast<int>(pinv->active_equipment_set) : -1);

		// How many carried slots hold something. The COUNTERPART to 'equipped', and the pair is
		// the point: P1.3d-4 moves the item storage out of the menus, and the way that goes wrong
		// is one copy of the data becoming two. A single number cannot see that. Two can --
		// an item that moves from the carried area to an equipment slot must make one go down as
		// the other goes up, and a duplicate shows as both going up.
		int carried = 0;
		if (pinv) {
			int cslots = pinv->inventory[PlayerInventory::CARRIED].getSlotNumber();
			for (int i = 0; i < cslots; ++i) {
				if (!pinv->inventory[PlayerInventory::CARRIED][i].empty())
					carried++;
			}
		}
		printf(" carried=%d", carried);

		// Diagnostic, like last_tick -- NOT pinned in the MANIFEST. It is here because without it
		// a death-penalty failure is unreadable: the penalty's random draw removes ONE UNIT of a
		// randomly chosen stack, so drawing a 1-quantity item empties its slot and drawing the
		// 750-strong currency stack does not. 'carried' alone shows the difference and cannot
		// explain it. Currency is already hashed, so this is not new coverage, only new legibility.
		printf(" currency=%d", pinv ? pinv->currency : -1);


		printf("\n");
	}

	// stdout, not the log: golden-file comparison should not have to parse timestamps.
	//
	// This is the TRAJECTORY digest -- every sample taken during the run folded together, not
	// just the world as it stands now. See TRAJECTORY_SAMPLE_TICKS for why the end state alone
	// was not enough. The final-state digest is still printed underneath it for debugging; the
	// golden files compare the first line, because tests/run-replays.sh greps '^0x'.
	if (args.hash_at_exit) {
		printf("%s\n", WorldHash::toString(trajectory).c_str());
		Utils::logInfo("main_server: final-state digest = %s",
		               WorldHash::toString(WorldHash::compute(ticks)).c_str());
	}

	serverCleanup();

	delete settings;
	return 0;
}
