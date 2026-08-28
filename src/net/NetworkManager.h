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
 * class NetworkManager
 *
 * Length-framed TCP transport, non-blocking, driven by one update() call per simulation tick (no
 * select()/poll() event loop -- see plans/phase3/OUTLINE.md's explicit instruction not to
 * "modernise" this; at a handful of peers the syscall cost of per-tick recv() is negligible).
 *
 * Peer identity is PlayerManager::PlayerID, assigned by this class at accept() time and never
 * taken from packet content -- popPacket()'s 'from' is always the id NetworkManager itself
 * assigned to the socket the bytes arrived on. This is the fix for the LAN reference build's own
 * documented flaw ("source peer is not tracked at this layer"); see this plan's "Sequencing note"
 * for why this is a fresh implementation rather than a literal port.
 *
 * Nothing calls this yet. P3.3 wires it into the server tick loop; P3.2 replaces the placeholder
 * HELLO/REFUSED frames used here with a real versioned schema. See
 * plans/phase3/P3.1-transport-and-peer-identity.md.
 */

#ifndef NET_NETWORKMANAGER_H
#define NET_NETWORKMANAGER_H

#include "NetPlatform.h"
#include "PlayerManager.h" // PlayerID

#include <deque>
#include <string>
#include <vector>

namespace Net {

class NetworkManager {
public:
	static const size_t MAX_PACKET_BYTES = 65536;
	static const unsigned int MAX_ACCEPTS_PER_TICK = 4;

	NetworkManager();
	~NetworkManager();

	// Starts listening on 'port' for up to 'max_players' peers. Returns false if the socket could
	// not be created, made non-blocking, bound, or put into listen().
	bool startHost(unsigned short port, unsigned int max_players);

	// Connects (non-blocking) to a host. Completion is confirmed by the first update() call, not
	// by this call returning -- false here only means the connect() attempt itself could not be
	// started (socket creation failed, address didn't resolve).
	bool startClient(const std::string& host_addr, unsigned short port, const std::string& display_name);

	// Closes every peer socket and the listen socket (if hosting), releases the platform socket
	// layer. Safe to call more than once.
	void shutdown();

	// Non-blocking. Call once per simulation tick. Host: bounded accept loop, then per-peer
	// recv/send. Client: connect-completion check, then recv/send on the one connection to the
	// host.
	void update();

	// Dequeues the next complete inbound message, if any. Returns false if none are queued.
	bool popPacket(PlayerID* from, std::string* payload);

	// Host only. Queues 'payload' for delivery to peer 'to' / every connected peer. No-op if this
	// NetworkManager is a client (a client has exactly one peer -- the host -- addressed
	// implicitly by whatever protocol layer sits above this one).
	void sendTo(PlayerID to, const std::string& payload);
	void broadcast(const std::string& payload);

	// Client only. Queues 'payload' for the host.
	void sendToHost(const std::string& payload);

	bool isHost() const { return is_host; }
	bool isConnected() const;
	size_t peerCount() const { return peers.size(); }
	std::string displayNameFor(PlayerID id) const;

private:
	struct Peer {
		NetSocket socket;
		PlayerID id;
		std::string display_name;
		std::string recv_buffer;
		std::string send_buffer;
		bool connecting; // client mode only: true while the non-blocking connect() is unresolved
		bool alive;      // false once a close/error/protocol-violation has been observed; removed
		                 // at the end of the update() that set this, never removed mid-tick
	};

	bool is_host;
	bool started;
	NetSocket listen_socket;
	unsigned int max_players_cap;
	std::vector<Peer> peers;
	std::deque<std::pair<PlayerID, std::string> > inbound;
	PlayerID next_id;

	void acceptLoop();
	void pumpPeer(Peer& peer);
	void removePeer(size_t index);
	std::string disambiguatedName(const std::string& base, PlayerID id) const;

	static void appendFramed(std::string& buffer, const std::string& payload);
	// Pulls complete frames out of the front of 'buffer', appending each to 'out'. Returns false
	// (and leaves 'buffer' alone) the moment a length prefix exceeds MAX_PACKET_BYTES -- the
	// caller drops that peer, since this indicates either corruption or something malicious rather
	// than a slow trickle of a legitimate frame.
	static bool extractFrames(std::string& buffer, std::vector<std::string>& out);
};

} // namespace Net

#endif
