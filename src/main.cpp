/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2013-2014 Henrik Andersson
Copyright © 2013 Kurt Rinnert
Copyright © 2012-2016 Justin Jacobs

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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <limits.h>

#include "AnimationManager.h"
#include "CombatText.h"
#include "DeviceList.h"
#include "FontDeviceList.h"
#include "EngineSettings.h"
#include "GameSwitcher.h"
#include "InputState.h"
#include "MessageEngine.h"
#include "ModManager.h"
#include "NullInputState.h"
#include "NullRenderDevice.h"
#include "RenderDevice.h"
#include "Rng.h"
#include "SaveLoad.h"
#include "ScriptedInputState.h"
#include "SDLFontEngine.h"
#include "Settings.h"
#include "SharedResources.h"
#include "SimEvents.h"
#include "SoundManager.h"
#include "Stats.h"
#include "TooltipManager.h"
#include "Utils.h"
#include "UtilsFileSystem.h"
#include "UtilsParsing.h"
#include "Version.h"
#include "WorldHash.h"

GameSwitcher *gswitch;

class CmdLineArgs {
public:
	std::string render_device_name;
	std::vector<std::string> mod_list;
};

#define PLATFORM_CPP_INCLUDE

#ifdef _WIN32
#include "PlatformWin32.cpp"
#elif __ANDROID__
#include "PlatformAndroid.cpp"
#elif __IPHONEOS__
#include "PlatformIPhoneOS.cpp"
#elif __GCW0__
#include "PlatformGCW0.cpp"
#elif __EMSCRIPTEN__
#include "PlatformEmscripten.cpp"
bool init_finished = false;
#else
// Linux stuff should work on Mac OSX/BSD/etc, too
#include "PlatformLinux.cpp"
#endif

/**
 * Game initialization.
 */
static void init(const CmdLineArgs& cmd_line_args) {
	/**
	 * Set system paths
	 * PATH_CONF is for user-configurable settings files (e.g. keybindings)
	 * PATH_USER is for user-specific data (e.g. save games)
	 * PATH_DATA is for common game data (e.g. images, music)
	 */
	platform.setPaths();

	// It's *important* that setCustomPathData() runs before setGame() because:
	// 1. We want PATH_CONF to be at the base level if we have to save the custom data path
	// 2. We want to use the custom data path in setGame() when looking for game.txt
	settings->setCustomPathData();
	settings->setGame();

	Utils::lockFileCheck();

	Utils::createLogFile();
	Utils::logInfo(VersionInfo::createVersionStringFull().c_str());

	// log common paths
	Utils::logInfo("main: PATH_CONF = '%s'", settings->path_conf.c_str());
	Utils::logInfo("main: PATH_USER = '%s'", settings->path_user.c_str());
	Utils::logInfo("main: PATH_DATA = '%s'", settings->path_data.c_str());

	// SDL Inits. P3.7: --headless opens no video/audio/controller subsystem at all, the same as
	// flare-server's own serverInit() (main_server.cpp) -- SDL_INIT_TIMER is enough for
	// SDL_GetPerformanceCounter()/SDL_Delay(), both used unconditionally by mainLoop() below.
	if (settings->headless) {
		if ( SDL_Init (SDL_INIT_TIMER) < 0 ) {
			Utils::logError("main: Could not initialize SDL: %s", SDL_GetError());
			Utils::logErrorDialog("main: Could not initialize SDL: %s", SDL_GetError());
			Utils::Exit(1);
		}
	}
	else if ( SDL_Init (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0 ) {
		Utils::logError("main: Could not initialize SDL: %s", SDL_GetError());
		Utils::logErrorDialog("main: Could not initialize SDL: %s", SDL_GetError());
		Utils::Exit(1);
	}

	// Shared Resources set-up

	mods = new ModManager(&(cmd_line_args.mod_list));

	if (!mods->haveFallbackMod()) {
		Utils::logError("main: Could not find the default mod in the following locations:");
		if (Filesystem::pathExists(settings->path_user + "mods")) Utils::logError("%smods/", settings->path_user.c_str());
		if (Filesystem::pathExists(settings->path_data + "mods")) Utils::logError("%smods/", settings->path_data.c_str());
		Utils::logError("A copy of the default mod is in the \"mods\" directory of the flare-engine repo.");
		Utils::logError("The repo is located at: https://github.com/flareteam/flare-engine");
		Utils::logError("Try again after copying the default mod to one of the above directories. Exiting.");
		Utils::logErrorDialog("main: Could not find the 'default' mod in the following locations:\n\n%smods/\n\n%smods/", settings->path_user.c_str(), settings->path_data.c_str());
#if __ANDROID__
		PlatformAndroid::dialogInstallHint();
#endif
		Utils::Exit(1);
	}

	settings->loadSettings();
	// Same reasoning as flare-server's serverInit(): off at the source as well as at the device,
	// so nothing even tries to decode.
	if (settings->headless)
		settings->audio = false;
	settings->logSettings();

	// Two explicit random streams. sim_rng is reproducible and belongs to the simulation;
	// fx_rng is presentation-only and must never be reproducible. See Rng.h.
	// Created here rather than at the soft_reset label so a soft reset reseeds them, matching
	// the surrounding singletons' lifetime.
	sim_rng = new Rng();
	sim_rng->seed(RNG_DEFAULT_SIM_SEED);
	fx_rng = new Rng();
	fx_rng->seed(static_cast<uint64_t>(time(NULL)));

	// What the simulation did, for the presentation layer to react to. Emptied every tick.
	sim_events = new SimEventQueue();
	Utils::logInfo("main: sim_rng seeded with 0x%llx", static_cast<unsigned long long>(sim_rng->getSeed()));

	save_load = new SaveLoad();
	msg = new MessageEngine();
	font = getFontEngine();
	anim = new AnimationManager();
	comb = new CombatText();

	// Load miscellaneous settings
	eset = new EngineSettings();
	eset->load();

	// P3.7: --headless drives input from a script (if given) or nothing at all, never SDLInputState
	// -- getInputManager(bool)'s own headless branch only chooses NullInputState, with no branch for
	// a scripted source, so construct directly here rather than growing that factory's signature for
	// this one caller.
	if (settings->headless)
		inpt = settings->script_path.empty() ? static_cast<InputState*>(new NullInputState())
		                                      : static_cast<InputState*>(new ScriptedInputState(settings->script_path));
	else
		inpt = getInputManager();
	icons = NULL;

	Stats::init();

	// platform-specific default screen size
	platform.setScreenSize();

	// Create render Device and Rendering Context. P3.7: --headless always takes the null render
	// device -- "null" is deliberately absent from createRenderDeviceList() (DeviceList.cpp) so a
	// player can never select it from the video options menu, but it is reachable by name here the
	// same way flare-server reaches it by direct construction.
	if (settings->headless)
		render_device = getRenderDevice("null");
	else if (settings->safe_video)
		render_device = getRenderDevice(settings->render_device_name);
	else if (platform.default_renderer != "")
		render_device = getRenderDevice(platform.default_renderer);
	else if (cmd_line_args.render_device_name != "")
		render_device = getRenderDevice(cmd_line_args.render_device_name);
	else
		render_device = getRenderDevice(settings->render_device_name);

	int status = render_device->createContext();

	if (status == -1) {
		Utils::logError("main: Could not create rendering context: %s", SDL_GetError());
		Utils::logErrorDialog("main: Could not create rendering context: %s", SDL_GetError());
		Utils::Exit(1);
	}

	// reset the reload_graphics flag
	render_device->reloadGraphics();

	snd = getSoundManager(settings->headless);

	tooltipm = new TooltipManager();

	gswitch = new GameSwitcher();
}

static float getSecondsElapsed(uint64_t prev_ticks, uint64_t now_ticks) {
	return (static_cast<float>(now_ticks - prev_ticks) / static_cast<float>(SDL_GetPerformanceFrequency()));
}

// A stall (map load, minimise, a breakpoint) leaves the accumulator in debt. Catching up is
// right; catching up without bound turns a five second pause into a five second fast-forward.
static const int MAX_CATCHUP_TICKS = 5;

static void mainLoop () {
	bool done = false;

	// P3.7. Counts real gswitch->logic() calls, same placement server-side's total_ticks occupies
	// (main_server.cpp's serverMainLoop()) -- meaningful only headless (see the two uses below),
	// but harmless and cheap to maintain unconditionally.
	unsigned long total_ticks = 0;
	ScriptedInputState* scripted_input = settings->headless ? dynamic_cast<ScriptedInputState*>(inpt) : NULL;

	// Two different rates, deliberately. The simulation step is fixed and shared so that every
	// peer agrees on what tick N means; the render frame limit is a user preference and
	// affects nothing but how often we draw.
	const float seconds_per_sim_tick = 1.f/static_cast<float>(Settings::SIM_TICK_HZ);
	const float seconds_per_render_frame = 1.f/static_cast<float>(settings->max_frames_per_sec);

	uint64_t prev_ticks = SDL_GetPerformanceCounter();
	uint64_t logic_ticks = SDL_GetPerformanceCounter();

	float last_fps = -1;

	while ( !done ) {
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
			// Frames where data loading happens (GameState switching and map loading)
			// take a long time, so our loop here will think that the game "lagged" and
			// try to compensate. To prevent this compensation, we mark those frames as
			// "loading frames" and update the logic ticker without actually executing logic.
			if (gswitch->isLoadingFrame()) {
				logic_ticks = now_ticks;
				break;
			}

			SDL_PumpEvents();
			inpt->handle();

			// Skip game logic when minimized
			// *except* if the player closes the window when minimized. We then continue with the logic to properly exit
			if (inpt->window_minimized && !inpt->window_restored && !inpt->done)
				break;

			// P3.7. Applies this tick's scripted press/release/disconnect entries before
			// gswitch->logic() reads them via PlayerCommandBuilder::build() deep inside
			// GameStatePlay::logic() -- same idiom as the replay-driven input below on the server.
			if (scripted_input)
				scripted_input->driveTick(total_ticks + 1);

			gswitch->logic();
			inpt->resetScroll();

			++total_ticks;

			// Per-tick digests make a divergence bisectable, same reasoning and format as
			// main_server.cpp's own hash_every block -- kept directly comparable line-for-line.
			if (settings->headless && settings->hash_replicated && settings->hash_every > 0
			    && total_ticks % settings->hash_every == 0) {
				printf("tick %lu %s\n", total_ticks,
				       WorldHash::toString(WorldHash::computeReplicated(total_ticks)).c_str());
			}

			// Engine done means the user escapes the main game menu.
			// Input done means the user closes the window.
			done = gswitch->done || inpt->done;

			if (settings->max_ticks > 0 && total_ticks >= settings->max_ticks)
				done = true;

			// Same reason as main_server.cpp: 'done' does not end the catch-up loop, so
			// without this the client keeps simulating after the game has said stop.
			if (done)
				break;

			logic_ticks += static_cast<uint64_t>(seconds_per_sim_tick * static_cast<float>(SDL_GetPerformanceFrequency()));
			loops++;

			// When the app is minimized, no logic gets processed.
			// As a result, the delta time when restoring the app is large, so the game will skip frames and appear to be running fast.
			// To counter this, we reset our delta time here when restoring the app
			if (inpt->window_minimized && inpt->window_restored) {
				logic_ticks = now_ticks = SDL_GetPerformanceCounter();
				inpt->window_minimized = inpt->window_restored = false;
				break;
			}

			// don't skip frames if the game is paused
			if (gswitch->isPaused()) {
				logic_ticks = now_ticks;
				break;
			}
		}

		if (!inpt->window_minimized) {
			render_device->blankScreen();
			gswitch->render();

			// display the FPS counter
			if (last_fps != -1) {
				gswitch->showFPS(last_fps);
			}

			render_device->commitFrame();

			// calculate the FPS
			// if the frame completed quickly, we estimate the delay here
			float fps_delay;
			if (getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter()) < seconds_per_render_frame) {
				fps_delay = seconds_per_render_frame;
			} else {
				fps_delay = getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter());
			}
			if (fps_delay != 0) {
				last_fps = (1000.f / fps_delay) / 1000.f;
			} else {
				last_fps = -1;
			}
		}

		// delay quick frames
		// thanks to David Gow: https://davidgow.net/handmadepenguin/ch18.html
		if (getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter()) < seconds_per_render_frame) {
			int32_t delay_ms = static_cast<int32_t>((seconds_per_render_frame - getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter())) * 1000.f);
			if (delay_ms > 0) {
				SDL_Delay(delay_ms);
			}
			while (getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter()) < seconds_per_render_frame) {
				// Waiting...
			}
		}
		prev_ticks = SDL_GetPerformanceCounter();
	}
}

static void cleanup() {
	Utils::lockFileWrite(-1);

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
	delete sim_rng;
	delete fx_rng;
	delete sim_events;
	sim_events = NULL;

	if (render_device)
		render_device->destroyContext();
	delete render_device;

	SDL_Quit();
}

std::string parseArg(const std::string &arg) {
	std::string result = "";

	// arguments must start with '--'
	if (arg.length() > 2 && arg[0] == '-' && arg[1] == '-') {
		for (unsigned i = 2; i < arg.length(); ++i) {
			if (arg[i] == '=') break;
			result += arg[i];
		}
	}

	return result;
}

std::string parseArgValue(const std::string &arg) {
	std::string result = "";
	bool found_equals = false;

	for (unsigned i = 0; i < arg.length(); ++i) {
		if (found_equals) {
			result += arg[i];
		}
		if (arg[i] == '=') found_equals = true;
	}

	return result;
}

#ifdef __EMSCRIPTEN__
void EmscriptenMainLoop() {
	if (!init_finished) {
		if (platform.FSCheckReady()) {
			// browsers don't have command line args, so pass default struct to init
			init(CmdLineArgs());
			init_finished = true;
			emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, static_cast<int>(1000/settings->max_frames_per_sec));
		}
		return;
	}

	SDL_PumpEvents();
	inpt->handle();

	gswitch->logic();
	inpt->resetScroll();

	render_device->blankScreen();
	gswitch->render();
	render_device->commitFrame();
}
#endif

int main(int argc, char *argv[]) {
	settings = new Settings();

	bool debug_event = false;
	bool done = false;
	CmdLineArgs cmd_line_args;

	for (int i = 1 ; i < argc; i++) {
		std::string arg_full = std::string(argv[i]);
		std::string arg = parseArg(arg_full);
		if (arg == "debug-event") {
			debug_event = true;
		}
		else if (arg == "data-path") {
			settings->custom_path_data = parseArgValue(arg_full);
		}
		else if (arg == "save-data-path") {
			settings->custom_path_data_save = true;
		}
		else if (arg == "clear-data-path") {
			settings->custom_path_data_clear = true;
		}
		else if (arg == "ignore-data-path") {
			settings->custom_path_data_ignore = true;
		}
		else if (arg == "version") {
			Utils::logInfo("%s", VersionInfo::createVersionStringFull().c_str());
			done = true;
		}
		else if (arg == "renderer") {
			cmd_line_args.render_device_name = parseArgValue(arg_full);
		}
		else if (arg == "no-audio") {
			settings->audio = false;
		}
		else if (arg == "mods") {
			std::string mod_list_str = parseArgValue(arg_full);
			while (!mod_list_str.empty()) {
				cmd_line_args.mod_list.push_back(Parse::popFirstString(mod_list_str));
			}
		}
		else if (arg == "load-slot") {
			settings->load_slot = parseArgValue(arg_full);
		}
		else if (arg == "load-script") {
			settings->load_script = parseArgValue(arg_full);
		}
		else if (arg == "connect") {
			settings->net_connect_target = parseArgValue(arg_full);
		}
		else if (arg == "host") {
			settings->net_host_port = static_cast<unsigned short>(Parse::toInt(parseArgValue(arg_full)));
		}
		else if (arg == "max-players") {
			settings->net_max_players = Parse::toInt(parseArgValue(arg_full));
		}
		else if (arg == "safe-video") {
			settings->safe_video = true;
		}
		else if (arg == "no-lock-file") {
			settings->no_lock_file = true;
		}
		else if (arg == "headless") {
			settings->headless = true;
		}
		else if (arg == "max-ticks") {
			settings->max_ticks = strtoul(parseArgValue(arg_full).c_str(), NULL, 10);
		}
		else if (arg == "script") {
			settings->script_path = parseArgValue(arg_full);
		}
		else if (arg == "hash-replicated") {
			settings->hash_replicated = true;
		}
		else if (arg == "hash-every") {
			settings->hash_every = strtoul(parseArgValue(arg_full).c_str(), NULL, 10);
		}
		else if (arg == "help") {
			Utils::logInfo("Command line options:\n\
--help                   Prints this message.\n\
--version                Prints the release version.\n\
--data-path=<PATH>       Specifies an exact path to look for mod data.\n\
--save-data-path         Saves the path specified with --data-path to the user's config.\n\
--clear-data-path        Removes the saved data-path from the user's config.\n\
--ignore-data-path       Temporarily ignores any saved data-path in the user's config.\n\
--debug-event            Prints verbose hardware input information.\n\
--renderer=<RENDERER>    Specifies the rendering backend to use.\n\
                         The default is 'sdl'.\n\
--no-audio               Disables sound effects and music.\n\
--mods=<MOD>,...         Starts the game with only these mods enabled.\n\
--load-slot=<SLOT>       Loads a save slot by numerical index.\n\
--load-script=<SCRIPT>   Execute's a script upon loading a saved game.\n\
                         The script path is mod-relative.\n\
--connect=<HOST>:<PORT>  Joins a running dedicated server as a network client.\n\
--host=<PORT>            Spawns a local flare-server and connects to it as a network\n\
                         client, so other players can connect to\n\
                         --connect=<this machine>:<PORT> too.\n\
--max-players=<N>        With --host, the connection cap, 2-8 (D3). Default 8.\n\
--safe-video             Launches with the minimum video settings.\n\
--no-lock-file           Skips the single-instance check, so that more than one copy\n\
                         of Flare can be run at once. Intended for testing.\n\
--headless               Runs with no window, GPU, or audio device -- for scripted/CI use.\n\
--max-ticks=<N>          With --headless, exits cleanly after N simulation ticks.\n\
--script=<FILE>          With --headless, drives input from this script instead of\n\
                         hardware. Requires --headless.\n\
--hash-replicated        With --headless, print a periodic digest of only the fields\n\
                         the network already replicates (see --hash-every).\n\
--hash-every=<N>         With --headless, prints a world digest every N ticks.");
			done = true;
		}
		else {
			Utils::logError("'%s' is not a valid command line option. Try '--help' for a list of valid options.", argv[i]);
		}
	}

	// P3.4c: a GameStatePlay is either a network client or an embedded host, never both -- refuse
	// the combination here rather than letting GameStatePlay::netConnectIfNeeded()/netHostIfNeeded()
	// silently pick one.
	if (!done && !settings->net_connect_target.empty() && settings->net_host_port != 0) {
		Utils::logError("--connect and --host are mutually exclusive.");
		done = true;
	}
	// P3.7: a scripted run with a live SDL input device would silently ignore the script's key
	// state on the very next SDL_PumpEvents()/inpt->handle() -- refuse at startup rather than run
	// a session whose input nobody is actually driving.
	if (!done && !settings->script_path.empty() && !settings->headless) {
		Utils::logError("--script requires --headless.");
		done = true;
	}
	if (!done && settings->net_host_port != 0 && (settings->net_max_players < 2 || settings->net_max_players > 8)) {
		Utils::logError("--max-players=%d out of range (D3: 2-8).", settings->net_max_players);
		done = true;
	}

soft_reset:
	if (!done) {
		// Nothing to seed here any more: the global C library generator is gone. sim_rng and
		// fx_rng are seeded in init(), which runs once per soft reset, so this label needs
		// nothing of its own.
#ifdef __EMSCRIPTEN__
		platform.FSInit();
		emscripten_set_main_loop(EmscriptenMainLoop, settings->max_frames_per_sec, 1);
#else
		init(cmd_line_args);

		if (debug_event)
			inpt->enableEventLog();

		mainLoop();
#endif

		if (gswitch)
			gswitch->saveUserSettings();

		cleanup();
	}

	if (settings->soft_reset) {
		Utils::logInfo("main: Restarting Flare...");
		settings->soft_reset = false;
		done = false;
		cmd_line_args = CmdLineArgs();
		goto soft_reset;
	}

	delete settings;

	return 0;
}
