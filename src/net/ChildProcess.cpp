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

#include "ChildProcess.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace Net {

ChildProcess::ChildProcess()
#ifdef _WIN32
	: process_handle(NULL)
#else
	: pid(0)
#endif
{
}

ChildProcess::~ChildProcess() {
	terminate();
}

bool ChildProcess::spawn(const std::string& exe_path, const std::vector<std::string>& args, const std::string& log_path) {
#ifdef _WIN32
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	HANDLE log_handle = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
	                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (log_handle == INVALID_HANDLE_VALUE)
		return false;

	std::string cmdline = "\"" + exe_path + "\"";
	for (size_t i = 0; i < args.size(); ++i)
		cmdline += " \"" + args[i] + "\"";
	std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
	cmdline_buf.push_back('\0');

	STARTUPINFOA si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = log_handle;
	si.hStdError = log_handle;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));

	BOOL ok = CreateProcessA(exe_path.c_str(), &cmdline_buf[0], NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
	CloseHandle(log_handle);
	if (!ok)
		return false;

	CloseHandle(pi.hThread);
	process_handle = pi.hProcess;
	return true;
#else
	int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (log_fd < 0)
		return false;

	std::vector<char*> argv;
	argv.push_back(const_cast<char*>(exe_path.c_str()));
	for (size_t i = 0; i < args.size(); ++i)
		argv.push_back(const_cast<char*>(args[i].c_str()));
	argv.push_back(NULL);

	pid_t child = fork();
	if (child < 0) {
		close(log_fd);
		return false;
	}
	if (child == 0) {
		// Child: redirect stdout/stderr to the log file, then exec. Only async-signal-safe calls
		// (dup2/close/execvp) run between fork() and execvp() -- the well-known "never do anything
		// else between fork and exec in a process that might have other threads" rule.
		dup2(log_fd, STDOUT_FILENO);
		dup2(log_fd, STDERR_FILENO);
		close(log_fd);
		execvp(exe_path.c_str(), &argv[0]);
		// execvp() only returns on failure -- this is the child's own private diagnostic, written
		// to the now-redirected stderr, distinct from spawn()'s own false-return path above (which
		// means the OS-level fork()/open() itself failed, not this).
		fprintf(stderr, "ChildProcess: execvp(%s) failed: %s\n", exe_path.c_str(), strerror(errno));
		_exit(127);
	}

	close(log_fd);
	pid = child;
	return true;
#endif
}

bool ChildProcess::waitForLogLine(const std::string& log_path, const std::string& marker, unsigned timeout_ms) {
	const unsigned poll_interval_ms = 50;
	unsigned waited_ms = 0;
	for (;;) {
		FILE* f = fopen(log_path.c_str(), "r");
		if (f) {
			char line[1024];
			while (fgets(line, sizeof(line), f)) {
				if (strstr(line, marker.c_str())) {
					fclose(f);
					return true;
				}
			}
			fclose(f);
		}

		if (!isRunning())
			return false;
		if (waited_ms >= timeout_ms)
			return false;

#ifdef _WIN32
		Sleep(poll_interval_ms);
#else
		usleep(poll_interval_ms * 1000);
#endif
		waited_ms += poll_interval_ms;
	}
}

bool ChildProcess::isRunning() {
#ifdef _WIN32
	if (!process_handle)
		return false;
	DWORD code = 0;
	if (!GetExitCodeProcess(process_handle, &code)) {
		CloseHandle(process_handle);
		process_handle = NULL;
		return false;
	}
	if (code != STILL_ACTIVE) {
		CloseHandle(process_handle);
		process_handle = NULL;
		return false;
	}
	return true;
#else
	if (pid == 0)
		return false;
	int status = 0;
	pid_t r = waitpid(pid, &status, WNOHANG);
	if (r == 0)
		return true; // still running
	// r == pid (exited/signalled) or r == -1 (e.g. ECHILD -- already reaped elsewhere): either way
	// this pid is no longer ours to track.
	pid = 0;
	return false;
#endif
}

void ChildProcess::terminate() {
#ifdef _WIN32
	if (!process_handle)
		return;
	TerminateProcess(process_handle, 0);
	WaitForSingleObject(process_handle, 2000);
	CloseHandle(process_handle);
	process_handle = NULL;
#else
	if (pid == 0)
		return;
	kill(pid, SIGTERM);
	int status = 0;
	for (int i = 0; i < 20; ++i) { // ~1s budget, 50ms steps
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			pid = 0;
			return;
		}
		usleep(50000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	pid = 0;
#endif
}

std::string ChildProcess::findSiblingExecutable(const std::string& sibling_name) {
	std::string self_path;

#ifdef _WIN32
	char buf[MAX_PATH];
	DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
	if (len == 0 || len == MAX_PATH)
		return std::string();
	self_path.assign(buf, len);
#elif defined(__APPLE__)
	char buf[4096];
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) != 0)
		return std::string();
	self_path.assign(buf);
#elif defined(__linux__)
	char buf[4096];
	ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (len <= 0)
		return std::string();
	self_path.assign(buf, static_cast<size_t>(len));
#else
	// No portable self-exe lookup on this platform. None of this project's non-desktop targets
	// (Android/iOS/GCW0/Emscripten) build flare-server at all -- see main_server.cpp's own top
	// comment -- so --host simply becomes unavailable here rather than guessing.
	(void)sibling_name;
	return std::string();
#endif

	size_t slash = self_path.find_last_of('/');
#ifdef _WIN32
	size_t backslash = self_path.find_last_of('\\');
	if (backslash != std::string::npos && (slash == std::string::npos || backslash > slash))
		slash = backslash;
#endif
	if (slash == std::string::npos)
		return std::string();

	std::string dir = self_path.substr(0, slash + 1);
#ifdef _WIN32
	std::string candidate = dir + sibling_name + ".exe";
	DWORD attrs = GetFileAttributesA(candidate.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
		return std::string();
	return candidate;
#else
	std::string candidate = dir + sibling_name;
	if (access(candidate.c_str(), X_OK) != 0)
		return std::string();
	return candidate;
#endif
}

} // namespace Net
