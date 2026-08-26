/*
Copyright © 2013 Stefan Beller
Copyright © 2015 Justin Jacobs

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

#include "ActionBarState.h"
#include "Avatar.h"
#include "CampaignManager.h"
#include "EnemyGroupManager.h"
#include "EntityManager.h"
#include "EventManager.h"
#include "HazardManager.h"
#include "LootManager.h"
#include "MenuActionBar.h"
#include "MenuPowers.h"
#include "NPCManager.h"
#include "PlayerInventory.h"
#include "PowerBonusState.h"
#include "PowerManager.h"
#include "XPScaling.h"
#include "SharedGameResources.h"

Avatar *pc = NULL;
MenuManager *menu = NULL;
CampaignManager *camp = NULL;
EnemyGroupManager *enemyg = NULL;
EntityManager *entitym = NULL;
EventManager *eventm = NULL;
HazardManager *hazards = NULL;
ItemManager *items = NULL;
LootManager *loot = NULL;
Map *wmap = NULL;
MapRenderer *mapr = NULL;
MenuActionBar *menu_act= NULL;
ActionBarState *pab = NULL;
MenuPowers *menu_powers = NULL;
NPCManager *npcs = NULL;
PlayerInventory *pinv = NULL;
PowerBonusState *pbs = NULL;
PowerManager *powers = NULL;
FogOfWar *fow = NULL;
XPScaling *xp_scaling = NULL;
