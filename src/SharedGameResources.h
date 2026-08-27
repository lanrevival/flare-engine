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

#ifndef SHAREDGAMEOBJECTS_H
#define SHAREDGAMEOBJECTS_H

class CampaignManager;
class EnemyGroupManager;
class EntityManager;
class EventManager;
class HazardManager;
class ItemManager;
class LootManager;
class Map;
class MapRenderer;
class MenuActionBar;
class MenuManager;
class MenuPowers;
class NPCManager;
class PlayerManager;
class PowerManager;
class FogOfWar;
class XPScaling;

/* These objects are created in the GameStatePlay constructor and deleted in the GameStatePlay destructor
*  so can be accessed safely anywhere in between. The objects must not be changed by any other class.
*/
// P2.3b deleted the pc/pinv/pab/pbs compatibility-alias globals that used to live here --
// PlayerManager::get()/inventoryFor()/actionbarFor()/powerbonusFor()/local() (PlayerManager.h) are
// the only way to reach a player's Avatar/PlayerInventory/ActionBarState/PowerBonusState now, for
// any player including local(). See plans/phase2/P2.3b-delete-player-globals.md.
extern PlayerManager *playerm;
extern CampaignManager *camp;
extern EnemyGroupManager *enemyg;
extern EntityManager *entitym;
extern EventManager *eventm;
extern HazardManager *hazards;
extern ItemManager *items;
extern LootManager *loot;
// wmap is the simulation's view of the map: collision, teleport/cutscene/stash/save-game signals,
// map-power activation. mapr is presentation's view: camera, tileset, drawing. On the client they
// are the same object (wmap = mapr, an upcast -- MapRenderer IS-A Map) so nothing has to stay in
// sync. A headless server (P1.4c) constructs a plain Map and points only wmap at it; mapr stays
// NULL, and MapRenderer.cpp is never compiled into flare_sim at all. See P1.4a.
extern Map *wmap;
extern MapRenderer *mapr;
extern MenuActionBar *menu_act;
extern MenuManager *menu;
extern MenuPowers *menu_powers;
extern NPCManager *npcs;
extern PowerManager *powers;
extern FogOfWar *fow;
extern XPScaling *xp_scaling;

#endif // SHAREDGAMEOBJECTS_H
