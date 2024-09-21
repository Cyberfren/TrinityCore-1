/*
 * Copyright (C) 2022 BfaCore Reforged
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __BATTLEGROUNDAF_H
#define __BATTLEGROUNDAF_H

#include "Arena.h"
#include "EventMap.h"

enum BattlegroundAFObjectTypes
{
    BG_AF_OBJECT_DOOR_1 = 0,
    BG_AF_OBJECT_DOOR_2 = 1,
    BG_AF_OBJECT_BUFF_1 = 2,
    BG_AF_OBJECT_BUFF_2 = 3,
    BG_AF_OBJECT_MAX = 4
};

enum BattlegroundAFObjects
{
    BG_AF_OBJECT_TYPE_DOOR_1 = 250431,
    BG_AF_OBJECT_TYPE_DOOR_2 = 250430,
    BG_AF_OBJECT_TYPE_BUFF_1 = 184663,
    BG_AF_OBJECT_TYPE_BUFF_2 = 184664
};
inline constexpr Seconds BG_AF_REMOVE_DOORS_TIMER = 5s;

enum BattlegroundAFEvents
{
    BG_AF_EVENT_REMOVE_DOORS = 1
};

class BattlegroundAF : public Arena
{
public:
    BattlegroundAF();
    /* inherited from BattlegroundClass */
    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;

    void HandleAreaTrigger(Player* Source, uint32 Trigger) override;
    bool SetupBattleground() override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;

private:
    void PostUpdateImpl(uint32 diff) override;

    EventMap _events;
};

#endif
