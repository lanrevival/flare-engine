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
 * Nothing calls this yet. P3.3 wires it into the server tick loop. The handshake speaks the
 * versioned binary schema defined in NetProtocol.h (P3.2) -- protocol version, engine version,
 * and a mod-list hash are checked on connect, and a client learns its own server-assigned
 * PlayerID via HELLO_OK. See plans/phase3/P3.2-binary-protocol-with-versioning.md.
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

	// Starts listening on 'port' for up to 'max_players' peers. 'required_mod_hash' is this host's
	// own Net::hashModList() result, checked against every incoming HELLO -- protocol version and
	// engine version are checked too, but against this binary's own compiled-in constants
	// (Net::PROTOCOL_VERSION, VersionInfo::ENGINE), not a parameter. Returns false if the socket
	// could not be created, made non-blocking, bound, or put into listen().
	bool startHost(unsigned short port, unsigned int max_players, uint32_t required_mod_hash);

	// Connects (non-blocking) to a host. Completion is confirmed by the first update() call, not
	// by this call returning -- false here only means the connect() attempt itself could not be
	// started (socket creation failed, address didn't resolve). 'local_mod_hash' is this client's
	// own Net::hashModList() result, sent to the host as part of the HELLO handshake.
	bool startClient(const std::string& host_addr, unsigned short port, const std::string& display_name, uint32_t local_mod_hash);

	// Closes every peer socket and the listen socket (if hosting), releases the platform socket
	// layer. Safe to call more than once.
	void shutdown();

	// Non-blocking. Call once per simulation tick. Host: bounded accept loop, then per-peer
	// recv/send. Client: connect-completion check, then recv/send on the one connection to the
	// host.
	void update();

	// Dequeues the next complete inbound message, if any. Returns false if none are queued.
	bool popPacket(PlayerID* from, std::string* payload);

	// Host only (peers.empty() otherwise means these queues never gain entries). Dequeues the next
	// player id whose handshake just completed / whose peer was just removed. A peer that never
	// completed its handshake never appears in either queue -- see pumpPeer()'s/update()'s own
	// comments at the two push sites. Returns false if none are queued.
	bool popConnected(PlayerID* id);
	bool popDisconnected(PlayerID* id);

	// Host only. Sets the floor below which no accepted peer is ever assigned an id (ids are
	// otherwise reused after a disconnect -- see acceptLoop()'s own comment). Must be called before
	// any peer has connected (silently ignored otherwise) -- typically right after startHost()
	// succeeds, to reserve low ids (e.g. 0) for a player the caller creates itself rather than over
	// the network.
	void seedNextPlayerID(PlayerID id);

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

	// Client only. True once this client's HELLO has been accepted and HELLO_OK decoded -- P3.1
	// left client-side PlayerID genuinely unknowable (peer.id was hardcoded to 0 and documented
	// "meaningless client-side"); this is what closes that gap.
	bool hasLocalPlayerID() const { return has_local_id; }
	PlayerID localPlayerID() const { return local_player_id; }

	// Client only. Empty until a REFUSED frame has been decoded; holds the message key from that
	// frame afterward (never cleared -- a NetworkManager is not reused across connection attempts).
	std::string lastRefusalMessage() const { return last_refusal_key; }

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
		bool handshake_done; // host mode only: true once this peer's HELLO has been accepted.
		                     // NOT inferred from display_name being non-empty -- an intentionally
		                     // empty display name must not be mistaken for "handshake pending".
	};

	bool is_host;
	bool started;
	NetSocket listen_socket;
	unsigned int max_players_cap;
	uint32_t required_mod_hash; // host only: what an incoming HELLO's mod_hash must equal
	std::vector<Peer> peers;
	std::deque<std::pair<PlayerID, std::string> > inbound;
	std::deque<PlayerID> newly_connected;    // host only
	std::deque<PlayerID> newly_disconnected; // host only
	PlayerID next_id;
	bool has_local_id;         // client only
	PlayerID local_player_id;  // client only, valid iff has_local_id
	std::string last_refusal_key; // client only
	uint32_t local_mod_hash_pending; // client only: startClient()'s mod_hash, held for the deferred
	                                  // HELLO send once a pending connect() resolves (pumpPeer())

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
