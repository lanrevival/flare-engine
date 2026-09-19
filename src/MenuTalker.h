/*
Copyright © 2011-2012 Clint Bellanger and morris989
Copyright © 2013-2014 Henrik Andersson
Copyright © 2012-2016 Justin Jacobs

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
 * class MenuTalker
 */

#ifndef MENU_TALKER_H
#define MENU_TALKER_H

#include "CommonIncludes.h"
#include "Utils.h"
#include "Widget.h"
#include "net/NetProtocol.h" // Net::MsgTalkCommand -- pending_commands stores it by value (C++98, no incomplete-type container support)

class Avatar;
class CampaignManager;
class NPC;
class StatBlock;
class WidgetButton;
class WidgetLabel;
class WidgetScrollBox;

class MenuTalker : public Menu {
private:
	class Action {
	public:
		static const int NO_NODE = -1;
		static const bool IS_VENDOR = true;

		Action();
		~Action();

		WidgetButton* btn;
		int node_id;
		bool is_vendor;
	};

	void createActionButtons(int node_id);
	void clearActionButtons();
	void createActionBuffer();
	void executeAction(size_t index);
	void nextDialog();
	void setupTabList();
	void addAction(const std::string& label, int node_id, bool is_vendor);

	// P3.11c: NPCs have no net_id (see Net::MsgTalkCommand's own header comment, net/NetProtocol.h)
	// -- this is the mirror-side counterpart of that same npcs->npcs index lookup.
	uint32_t indexOfNpc(NPC* target) const;

	Sprite *portrait;
	std::string hero_name;
	std::string hero_class;

	int dialog_node;
	unsigned int event_cursor;
	bool first_interaction;

	Rect dialog_pos;
	Rect text_pos;
	Point text_offset;
	Rect portrait_he;
	Rect portrait_you;

	std::string font_who;
	std::string font_dialog;

	WidgetLabel *label_name;
	WidgetScrollBox *textbox;

	std::vector<Action> actions;

	Color topic_color_normal;
	Color topic_color_hover;
	Color topic_color_pressed;

	Color trade_color_normal;
	Color trade_color_hover;
	Color trade_color_pressed;

public:
	explicit MenuTalker(Avatar* _player);
	~MenuTalker();
	void align();

	NPC *npc;

	void chooseDialogNode(int requested_node);
	void logic();
	void render();
	void setHero(StatBlock &stats);
	void createBuffer();
	void setNPC(NPC* _npc);

	WidgetButton *advanceButton;
	WidgetButton *closeButton;

	bool npc_from_map;

	Avatar* player;

	// P3.11c: set every tick from GameStatePlay.cpp, same pattern P3.11b established for
	// EntityManager/HazardManager/LootManager/MenuInventory's own mirror_mode fields. When true,
	// chooseDialogNode()/nextDialog()/setNPC() send a Net::MsgTalkCommand instead of calling
	// npc->processEvent()/mutating dialog_node/event_cursor locally -- the server is authoritative
	// for a connected guest's own conversation state.
	bool mirror_mode;
	std::vector<Net::MsgTalkCommand> pending_commands;

	// P3.11c: applies server-authoritative dialog state to this mirror's own rendering --
	// GameStatePlay::netApplyTalkState()'s only way to reach into MenuTalker. Deliberately never
	// sends a network command itself (unlike setNPC()/chooseDialogNode()/nextDialog() in
	// mirror_mode) -- this call IS the network's own answer arriving; sending one back would loop.
	void applyTalkState(NPC* new_npc, int new_dialog_node, unsigned new_event_cursor);
};

#endif
