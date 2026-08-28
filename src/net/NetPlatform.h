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
 * Portable BSD-socket wrapper. The only file in src/net/ that touches a raw socket -- everything
 * else in NetworkManager.cpp goes through these calls so the platform split lives in one place.
 *
 * No engine dependency on purpose (no CommonIncludes.h, no SDL): this is plumbing that has nothing
 * to do with the simulation and should be usable/testable in total isolation from it. See
 * plans/phase3/P3.1-transport-and-peer-identity.md.
 */

#ifndef NET_PLATFORM_H
#define NET_PLATFORM_H

#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET NetSocket;
#else
typedef int NetSocket;
#endif

namespace NetPlatform {

#ifdef _WIN32
const NetSocket INVALID = INVALID_SOCKET;
#else
const NetSocket INVALID = -1;
#endif

// Ref-counted: WSAStartup/WSACleanup on Windows are paired per call, so two independent
// NetworkManagers in the same process (a host and a client under test, say) don't tear down
// sockets the other one still owns. No-ops on POSIX.
bool platformInit();
void platformShutdown();

NetSocket createTcpSocket();
void closeSocket(NetSocket s);

// Non-blocking mode -- required before accept()/connect()/recv()/send() are called anywhere in
// NetworkManager. See the outline's own instruction (plans/phase3/OUTLINE.md): no select()/poll(),
// just per-tick non-blocking calls on every socket.
bool setNonBlocking(NetSocket s);

// True if the last socket call failed only because it would have blocked (EWOULDBLOCK/EAGAIN on
// POSIX, WSAEWOULDBLOCK on Windows) -- i.e. "try again next tick", not a real error.
bool wouldBlock();

// True if the last connect() call is still in progress (EINPROGRESS on POSIX, WSAEWOULDBLOCK on
// Windows) -- distinct from wouldBlock() because a non-blocking connect() reports completion via
// a later writable check, not via a repeated connect() call.
bool connectInProgress();

std::string lastErrorString();

} // namespace NetPlatform

#endif
