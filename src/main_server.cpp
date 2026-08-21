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
 * This target exists so that the engine can be linked without main.cpp, which is a
 * precondition for running the simulation with no window, no GPU and no audio device.
 *
 * The headless run loop is NOT implemented here yet; it is added in P0.2 along with the
 * null render and sound devices. Until then this binary only reports its version, so that
 * the target, its link line and its CI coverage can be established and kept working.
 */

#include <cstdio>

#include "CommonIncludes.h"
#include "Settings.h"
#include "SharedResources.h"
#include "Version.h"

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

// Defined in main.cpp for the client build. The server has no GameSwitcher, but several
// translation units reference this symbol, so it must be provided here too.
class GameSwitcher;
GameSwitcher *gswitch = NULL;

int main(int argc, char *argv[]) {
	settings = new Settings();

	bool print_version = false;
	bool print_help = false;

	for (int i = 1; i < argc; i++) {
		std::string arg = std::string(argv[i]);
		if (arg == "--version")
			print_version = true;
		else if (arg == "--help")
			print_help = true;
		else {
			printf("'%s' is not a valid command line option. Try '--help' for a list of valid options.\n", argv[i]);
			delete settings;
			return 1;
		}
	}

	if (print_help) {
		printf("Command line options:\n"
		       "--help                   Prints this message.\n"
		       "--version                Prints the release version.\n");
		delete settings;
		return 0;
	}

	if (print_version) {
		printf("%s\n", VersionInfo::createVersionStringFull().c_str());
		delete settings;
		return 0;
	}

	printf("flare-server: the headless run loop is not implemented yet (added in P0.2).\n");
	printf("Try '--help' for a list of valid options.\n");

	delete settings;
	return 1;
}
