/*
Copyright © 2011-2012 Thane Brimhall
Copyright © 2013 Henrik Andersson
Copyright © 2015-2016 Justin Jacobs

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
 * class MessageEngine
 *
 * The MessageEngine class allows translation of messages in FLARE by comparing them to
 * .po files in a format similar to gettext.
 *
 * This class is primarily used for making sure FLARE is flexible and translatable.
 */

#ifndef MESSAGE_ENGINE_H
#define MESSAGE_ENGINE_H

#include "CommonIncludes.h"

/**
 * One typed argument for MessageEngine::getv(key, args) -- the network-safe alternative to the
 * varargs getv() below. A network message can carry a key plus a vector of these (see
 * src/net/NetProtocol.h's MsgSystemMessage), which true C varargs cannot be reconstructed from
 * generically. Only int and string are modelled because those are the only conversions
 * (%d/%u and %s) this codebase's message catalog actually uses.
 */
struct MessageArg {
	enum Type { ARG_INT, ARG_STR } type;
	int i;
	std::string s;
	explicit MessageArg(int _i) : type(ARG_INT), i(_i), s("") {}
	explicit MessageArg(const std::string& _s) : type(ARG_STR), i(0), s(_s) {}
};

class MessageEngine {

private:
	std::map<std::string,std::string> messages;
	std::string unescape(const std::string& _val);
public:
	MessageEngine();
	~MessageEngine();
	std::string get(const std::string& key);
	std::string getv(const std::string key, ...);
	std::string getv(const std::string& key, const std::vector<MessageArg>& args);

	// Substitutes %d/%u/%s tokens in 'format' from 'args', in order, without touching a
	// MessageEngine instance -- 'format' is already the looked-up (and already localized) string.
	// Static and side-effect-free so it's testable without constructing a real MessageEngine (which
	// needs mods/settings/filesystem access to load .po catalogs). Fewer args than tokens: the
	// unmatched trailing token(s) are left literally in the output. Extra args: ignored. Never
	// reads past args.size() -- this exists specifically so malformed/short arg lists from the
	// network can't cause a crash.
	static std::string substitute(const std::string& format, const std::vector<MessageArg>& args);
};

#endif
