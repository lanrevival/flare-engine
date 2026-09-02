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
 * Minimal child-process wrapper: spawn one process with its stdout+stderr redirected to a log
 * file, poll that file for a marker line, and terminate the process on request or on destruction.
 * This is the entire interface --host (GameStatePlay::netHostSpawnAndConnect()) needs: it never
 * talks to the child except through its own log file and its exit status, the same way a human
 * operator launching a second terminal would.
 *
 * No engine dependency on purpose, matching NetPlatform.h's own precedent in this directory: usable
 * and testable in total isolation from the simulation. See
 * plans/phase3/P3.8b-host-becomes-a-child-process.md.
 */

#ifndef NET_CHILD_PROCESS_H
#define NET_CHILD_PROCESS_H

#include <string>
#include <vector>

namespace Net {

class ChildProcess {
public:
	ChildProcess();
	~ChildProcess();

	// Launches exe_path with args (NOT including exe_path itself, which becomes argv[0]),
	// redirecting the child's stdout AND stderr to log_path (truncated first). False if the OS-level
	// spawn call itself fails (log file couldn't be opened, fork()/CreateProcessA failed) -- this is
	// distinct from a later exec failure inside an already-forked child, which instead shows up as
	// the child exiting almost immediately (see isRunning()) with its own diagnostic line written
	// into log_path.
	bool spawn(const std::string& exe_path, const std::vector<std::string>& args, const std::string& log_path);

	// Polls log_path (re-opened and re-read from the top each call -- the file is being actively
	// appended to by the child, and at this size there is no reason to bother with seek/tail
	// bookkeeping) for a line containing marker, sleeping 50ms between checks, up to timeout_ms
	// total. Returns false immediately, without waiting out the rest of the budget, the moment the
	// child is no longer running -- a dead child will never produce a line it hasn't already written.
	bool waitForLogLine(const std::string& log_path, const std::string& marker, unsigned timeout_ms);

	// Non-blocking liveness check -- reaps the child if it has already exited (POSIX: waitpid
	// WNOHANG; Windows: GetExitCodeProcess against STILL_ACTIVE), so a caller polling this in a loop
	// cannot leak a zombie by never checking again after the child dies.
	bool isRunning();

	// Idempotent: a second call, or a call when nothing was ever spawned (or it already exited), is
	// a no-op. SIGTERM then, if still alive after ~1s, SIGKILL (POSIX) / TerminateProcess (Windows)
	// -- there is no graceful-shutdown protocol to ask flare-server for beyond the SIGTERM/SIGINT it
	// already handles (serverSignalHandler(), main_server.cpp), the same signal a human operator
	// would send.
	void terminate();

	// Resolves the absolute path to a file named sibling_name (e.g. "flare-server", or
	// "flare-server.exe" on Windows -- the extension is added here, not by the caller) in the same
	// directory as the CURRENTLY RUNNING executable. Both binaries this is ever used to find are
	// always built into the same output directory (CMakeLists.txt's Add_Executable calls for
	// flare/flare-server both land in CMAKE_CURRENT_BINARY_DIR, no per-target subdirectory), which is
	// also exactly how tests/run-net.sh already runs them. Empty string if the running executable's
	// own path could not be determined on this platform, or if sibling_name does not exist next to
	// it.
	static std::string findSiblingExecutable(const std::string& sibling_name);

private:
	// Not implemented -- this class owns an OS process handle, which cannot be safely copied.
	ChildProcess(const ChildProcess&);
	ChildProcess& operator=(const ChildProcess&);

#ifdef _WIN32
	void* process_handle; // HANDLE, kept as void* so this header does not need <windows.h>
#else
	int pid; // 0 means "nothing spawned, or already reaped"
#endif
};

} // namespace Net

#endif
