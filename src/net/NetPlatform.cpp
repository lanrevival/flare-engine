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

#include "NetPlatform.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace NetPlatform {

static int init_refcount = 0;

bool platformInit() {
#ifdef _WIN32
	if (init_refcount == 0) {
		WSADATA wsa_data;
		if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
			return false;
	}
#endif
	init_refcount++;
	return true;
}

void platformShutdown() {
	if (init_refcount == 0)
		return;
	init_refcount--;
#ifdef _WIN32
	if (init_refcount == 0)
		WSACleanup();
#endif
}

NetSocket createTcpSocket() {
	NetSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID)
		return INVALID;

	// LAN co-op, not a bandwidth-constrained link -- disabling Nagle keeps small per-tick packets
	// (a PlayerCommand is a handful of bytes) from being held back waiting to coalesce.
	int nodelay = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

	return s;
}

void closeSocket(NetSocket s) {
	if (s == INVALID)
		return;
#ifdef _WIN32
	closesocket(s);
#else
	close(s);
#endif
}

bool setNonBlocking(NetSocket s) {
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags == -1)
		return false;
	return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool wouldBlock() {
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

bool connectInProgress() {
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EINPROGRESS;
#endif
}

std::string lastErrorString() {
#ifdef _WIN32
	int err = WSAGetLastError();
	char buf[256];
	buf[0] = '\0';
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
	               static_cast<DWORD>(err), 0, buf, sizeof(buf), NULL);
	return std::string(buf);
#else
	return std::string(strerror(errno));
#endif
}

} // namespace NetPlatform
