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
#include <cstdlib>

#include "AnimationManager.h"
#include "CombatText.h"
#include "CommonIncludes.h"
#include "DeviceList.h"
#include "EngineSettings.h"
#include "FontEngine.h"
#include "GameSwitcher.h"
#include "InputState.h"
#include "MessageEngine.h"
#include "ModManager.h"
#include "RenderDevice.h"
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
	ServerCmdLineArgs() : mod_list(), load_slot(), data_path(), max_ticks(0) {}
	std::vector<std::string> mod_list;
	std::string load_slot;
	std::string data_path;
	unsigned long max_ticks;
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
static unsigned long serverMainLoop(unsigned long max_ticks) {
	bool done = false;
	unsigned long total_ticks = 0;

	float seconds_per_frame = 1.f/static_cast<float>(settings->max_frames_per_sec);

	uint64_t prev_ticks = SDL_GetPerformanceCounter();
	uint64_t logic_ticks = SDL_GetPerformanceCounter();

	while (!done) {
		int loops = 0;
		uint64_t now_ticks = SDL_GetPerformanceCounter();

		while (now_ticks >= logic_ticks && loops < settings->max_frames_per_sec) {
			if (gswitch->isLoadingFrame()) {
				logic_ticks = now_ticks;
				break;
			}

			// No SDL_PumpEvents(): there is no event queue worth pumping without a window.
			// inpt->handle() is still called because NullInputState inherits InputState's
			// bookkeeping, which the game logic expects to have run.
			inpt->handle();

			gswitch->logic();
			inpt->resetScroll();

			total_ticks++;

			done = gswitch->done || shutdown_requested;
			if (max_ticks > 0 && total_ticks >= max_ticks)
				done = true;

			logic_ticks += static_cast<uint64_t>(seconds_per_frame * static_cast<float>(SDL_GetPerformanceFrequency()));
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
		if (getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter()) < seconds_per_frame) {
			int32_t delay_ms = static_cast<int32_t>((seconds_per_frame - getSecondsElapsed(prev_ticks, SDL_GetPerformanceCounter())) * 1000.f);
			if (delay_ms > 0)
				SDL_Delay(delay_ms);
		}
		prev_ticks = SDL_GetPerformanceCounter();
	}

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

	unsigned long ticks = serverMainLoop(args.max_ticks);

	if (shutdown_requested)
		Utils::logInfo("main_server: shutdown requested, stopping.");
	Utils::logInfo("main_server: simulated %lu logic ticks.", ticks);

	serverCleanup();

	delete settings;
	return 0;
}
