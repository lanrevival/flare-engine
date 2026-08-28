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

#include "NetworkManager.h"
#include "NetProtocol.h"
#include "Version.h"

#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace Net {

NetworkManager::NetworkManager()
	: is_host(false)
	, started(false)
	, listen_socket(NetPlatform::INVALID)
	, max_players_cap(0)
	, required_mod_hash(0)
	, peers()
	, inbound()
	, next_id(0)
	, has_local_id(false)
	, local_player_id(0)
	, last_refusal_key()
	, local_mod_hash_pending(0)
{
}

NetworkManager::~NetworkManager() {
	shutdown();
}

bool NetworkManager::startHost(unsigned short port, unsigned int max_players, uint32_t required_mod_hash_) {
	if (started)
		return false;
	if (!NetPlatform::platformInit())
		return false;

	listen_socket = NetPlatform::createTcpSocket();
	if (listen_socket == NetPlatform::INVALID) {
		NetPlatform::platformShutdown();
		return false;
	}

	int reuse = 1;
	setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	if (bind(listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
	    || listen(listen_socket, static_cast<int>(max_players) + 1) != 0
	    || !NetPlatform::setNonBlocking(listen_socket)) {
		NetPlatform::closeSocket(listen_socket);
		listen_socket = NetPlatform::INVALID;
		NetPlatform::platformShutdown();
		return false;
	}

	is_host = true;
	started = true;
	max_players_cap = max_players;
	required_mod_hash = required_mod_hash_;
	return true;
}

bool NetworkManager::startClient(const std::string& host_addr, unsigned short port, const std::string& display_name, uint32_t local_mod_hash) {
	if (started)
		return false;
	if (!NetPlatform::platformInit())
		return false;

	NetSocket s = NetPlatform::createTcpSocket();
	if (s == NetPlatform::INVALID) {
		NetPlatform::platformShutdown();
		return false;
	}
	if (!NetPlatform::setNonBlocking(s)) {
		NetPlatform::closeSocket(s);
		NetPlatform::platformShutdown();
		return false;
	}

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, host_addr.c_str(), &addr.sin_addr) != 1) {
		NetPlatform::closeSocket(s);
		NetPlatform::platformShutdown();
		return false;
	}

	int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	if (rc != 0 && !NetPlatform::connectInProgress() && !NetPlatform::wouldBlock()) {
		NetPlatform::closeSocket(s);
		NetPlatform::platformShutdown();
		return false;
	}

	Peer peer;
	peer.socket = s;
	peer.id = 0; // the host is the only "peer" a client tracks; not this client's real PlayerID --
	             // see localPlayerID(), populated once HELLO_OK arrives.
	peer.display_name = display_name;
	peer.connecting = (rc != 0); // rare but possible: loopback can complete connect() synchronously
	peer.alive = true;
	peer.handshake_done = false; // unused client-side (has_local_id is the client's own gate)
	peers.push_back(peer);

	local_mod_hash_pending = local_mod_hash;
	if (!peer.connecting)
		appendFramed(peers.back().send_buffer, Net::encodeHello(display_name, local_mod_hash));

	is_host = false;
	started = true;
	return true;
}

void NetworkManager::shutdown() {
	if (!started)
		return;

	for (size_t i = 0; i < peers.size(); ++i)
		NetPlatform::closeSocket(peers[i].socket);
	peers.clear();

	if (listen_socket != NetPlatform::INVALID) {
		NetPlatform::closeSocket(listen_socket);
		listen_socket = NetPlatform::INVALID;
	}

	NetPlatform::platformShutdown();
	started = false;
}

bool NetworkManager::isConnected() const {
	if (is_host)
		return started;
	return started && !peers.empty() && !peers[0].connecting && peers[0].alive;
}

void NetworkManager::acceptLoop() {
	for (unsigned int i = 0; i < MAX_ACCEPTS_PER_TICK; ++i) {
		sockaddr_in addr;
		socklen_t addr_len = sizeof(addr);
		NetSocket s = accept(listen_socket, reinterpret_cast<sockaddr*>(&addr), &addr_len);
		if (s == NetPlatform::INVALID)
			return; // wouldBlock() -- nothing pending; anything else, nothing useful to do either

		NetPlatform::setNonBlocking(s);

		if (peers.size() >= max_players_cap) {
			// Refuse politely: still drains the accept queue (so a refused connection doesn't
			// wedge the next legitimate one behind it), but consumes no PlayerID and no peer slot.
			std::string refusal;
			appendFramed(refusal, Net::encodeRefused(Net::REFUSED_SERVER_FULL, "Server is full."));
			send(s, refusal.data(), static_cast<int>(refusal.size()), 0);
			NetPlatform::closeSocket(s);
			continue;
		}

		Peer peer;
		peer.socket = s;
		peer.id = next_id++;
		peer.display_name = "";
		peer.connecting = false; // inbound connections are already established by accept()
		peer.alive = true;
		peer.handshake_done = false;
		peers.push_back(peer);
	}
}

void NetworkManager::pumpPeer(Peer& peer) {
	// Client mode, connect() still pending: a socket is writable once the non-blocking connect()
	// resolves (success or failure) -- SO_ERROR then distinguishes the two. This is a one-socket,
	// zero-timeout poll of a single fd, not the per-peer select()-based event loop the outline
	// says not to build; see NetworkManager.h's class comment.
	if (peer.connecting) {
		fd_set write_set;
		FD_ZERO(&write_set);
		FD_SET(peer.socket, &write_set);
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		if (select(static_cast<int>(peer.socket) + 1, NULL, &write_set, NULL, &tv) <= 0)
			return; // still pending

		int err = 0;
		socklen_t err_len = sizeof(err);
		getsockopt(peer.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &err_len);
		peer.connecting = false;
		if (err != 0) {
			peer.alive = false; // connect() failed -- caller (update()) removes this peer
			return;
		}
		appendFramed(peer.send_buffer, Net::encodeHello(peer.display_name, local_mod_hash_pending));
		return; // let the next tick's pumpPeer() do the first real recv/send on this socket
	}

	char buf[4096];
	for (;;) {
		int n = static_cast<int>(recv(peer.socket, buf, sizeof(buf), 0));
		if (n > 0) {
			peer.recv_buffer.append(buf, static_cast<size_t>(n));
			continue;
		}
		if (n == 0) {
			peer.alive = false; // orderly close -- caller (update()) removes this peer
			break;
		}
		if (!NetPlatform::wouldBlock())
			peer.alive = false; // real error -- caller removes this peer
		break;
	}

	std::vector<std::string> frames;
	if (!extractFrames(peer.recv_buffer, frames)) {
		peer.alive = false; // oversized length prefix -- treat like any other protocol violation
		return;
	}
	for (size_t i = 0; i < frames.size(); ++i) {
		if (is_host && !peer.handshake_done) {
			// The handshake sets the name shown for this PlayerID and validates the connecting
			// build; it never changes what PlayerID the message is attributed to. peer.id was
			// assigned at accept() and is never touched by anything read off the wire.
			Net::MsgHello hello;
			bool decoded = Net::peekMessageType(frames[i]) == Net::MSG_HELLO && Net::decodeHello(frames[i], hello);

			uint8_t reason = 0;
			std::string reason_key;
			if (!decoded) {
				reason = Net::REFUSED_MALFORMED;
				reason_key = "Malformed connection request.";
			}
			else if (hello.protocol_version != Net::PROTOCOL_VERSION
			         || VersionInfo::ENGINE != Version(hello.engine_x, hello.engine_y, hello.engine_z)) {
				reason = Net::REFUSED_VERSION_MISMATCH;
				reason_key = "This server requires a different game version.";
			}
			else if (hello.mod_hash != required_mod_hash) {
				reason = Net::REFUSED_MOD_MISMATCH;
				reason_key = "This server's mods do not match yours.";
			}

			if (reason != 0) {
				appendFramed(peer.send_buffer, Net::encodeRefused(reason, reason_key));
				peer.alive = false; // update() closes the socket after flushing send_buffer below
				break; // don't process any further frames from a peer being refused
			}

			peer.display_name = hello.display_name;
			peer.handshake_done = true;
			appendFramed(peer.send_buffer, Net::encodeHelloOk(peer.id));
			continue;
		}

		if (!is_host && !has_local_id) {
			// Expect the host's first reply to be HELLO_OK (accepted) or REFUSED (rejected).
			uint8_t type = Net::peekMessageType(frames[i]);
			if (type == Net::MSG_HELLO_OK) {
				Net::MsgHelloOk ok_msg;
				if (Net::decodeHelloOk(frames[i], ok_msg)) {
					local_player_id = ok_msg.assigned_id;
					has_local_id = true;
				}
				else {
					peer.alive = false;
				}
			}
			else if (type == Net::MSG_REFUSED) {
				Net::MsgRefused refused;
				if (Net::decodeRefused(frames[i], refused))
					last_refusal_key = refused.message_key;
				peer.alive = false;
			}
			else {
				peer.alive = false; // protocol violation: expected a handshake reply first
			}
			continue;
		}

		inbound.push_back(std::make_pair(peer.id, frames[i]));
	}

	// Flush send_buffer even if this same call just set alive=false (e.g. a REFUSED frame queued
	// above, right before refusing the peer) -- the socket is still open until update() removes it
	// at the end of this tick, and a small refusal frame virtually always clears in one write() on
	// a LAN/loopback socket. This is best-effort, not guaranteed: if the write partially blocks, the
	// remaining bytes never get a second tick to go out, since a !alive peer is removed before
	// pumpPeer() runs again. Attempting send() on an already-broken socket (alive went false because
	// of a real recv() error above) is harmless -- it fails immediately and re-sets alive=false, a
	// no-op.
	if (!peer.send_buffer.empty()) {
		int n = static_cast<int>(send(peer.socket, peer.send_buffer.data(), static_cast<int>(peer.send_buffer.size()), 0));
		if (n > 0)
			peer.send_buffer.erase(0, static_cast<size_t>(n));
		else if (n < 0 && !NetPlatform::wouldBlock())
			peer.alive = false;
	}
}

void NetworkManager::removePeer(size_t index) {
	NetPlatform::closeSocket(peers[index].socket);
	peers.erase(peers.begin() + static_cast<std::vector<Peer>::difference_type>(index));
}

void NetworkManager::update() {
	if (!started)
		return;

	if (is_host)
		acceptLoop();

	for (size_t i = peers.size(); i > 0; --i) {
		pumpPeer(peers[i - 1]);
		// Any complete frames pumpPeer() received before the disconnect are already in 'inbound'
		// (extractFrames() runs unconditionally, even on the same call that sets alive=false) --
		// an incomplete trailing frame in recv_buffer, if any, cannot be completed by a peer that
		// just went away, so it is safe to drop the whole peer here. 'connecting' peers are never
		// touched by this check -- see pumpPeer()'s own early-return while a connect() is pending.
		if (!peers[i - 1].alive)
			removePeer(i - 1);
	}
}

bool NetworkManager::popPacket(PlayerID* from, std::string* payload) {
	if (inbound.empty())
		return false;
	*from = inbound.front().first;
	*payload = inbound.front().second;
	inbound.pop_front();
	return true;
}

void NetworkManager::sendTo(PlayerID to, const std::string& payload) {
	if (!is_host)
		return;
	for (size_t i = 0; i < peers.size(); ++i) {
		if (peers[i].id == to) {
			appendFramed(peers[i].send_buffer, payload);
			return;
		}
	}
}

void NetworkManager::broadcast(const std::string& payload) {
	if (!is_host)
		return;
	for (size_t i = 0; i < peers.size(); ++i)
		appendFramed(peers[i].send_buffer, payload);
}

void NetworkManager::sendToHost(const std::string& payload) {
	if (is_host || peers.empty())
		return;
	appendFramed(peers[0].send_buffer, payload);
}

std::string NetworkManager::displayNameFor(PlayerID id) const {
	for (size_t i = 0; i < peers.size(); ++i) {
		if (peers[i].id == id)
			return disambiguatedName(peers[i].display_name, id);
	}
	return "";
}

std::string NetworkManager::disambiguatedName(const std::string& base, PlayerID id) const {
	bool collision = false;
	for (size_t i = 0; i < peers.size(); ++i) {
		if (peers[i].id != id && peers[i].display_name == base) {
			collision = true;
			break;
		}
	}
	if (!collision)
		return base;

	std::ostringstream oss;
	oss << base << "#" << static_cast<unsigned int>(id);
	return oss.str();
}

void NetworkManager::appendFramed(std::string& buffer, const std::string& payload) {
	uint32_t len = static_cast<uint32_t>(payload.size());
	char header[4];
	header[0] = static_cast<char>((len >> 24) & 0xFF);
	header[1] = static_cast<char>((len >> 16) & 0xFF);
	header[2] = static_cast<char>((len >> 8) & 0xFF);
	header[3] = static_cast<char>(len & 0xFF);
	buffer.append(header, 4);
	buffer.append(payload);
}

bool NetworkManager::extractFrames(std::string& buffer, std::vector<std::string>& out) {
	for (;;) {
		if (buffer.size() < 4)
			return true;

		uint32_t len = (static_cast<uint32_t>(static_cast<unsigned char>(buffer[0])) << 24)
		             | (static_cast<uint32_t>(static_cast<unsigned char>(buffer[1])) << 16)
		             | (static_cast<uint32_t>(static_cast<unsigned char>(buffer[2])) << 8)
		             | static_cast<uint32_t>(static_cast<unsigned char>(buffer[3]));

		if (len > MAX_PACKET_BYTES)
			return false;
		if (buffer.size() < 4 + len)
			return true; // frame not fully arrived yet

		out.push_back(buffer.substr(4, len));
		buffer.erase(0, 4 + len);
	}
}

} // namespace Net
