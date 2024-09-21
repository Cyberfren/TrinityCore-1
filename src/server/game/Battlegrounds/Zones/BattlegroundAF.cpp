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

#include "BattlegroundAF.h"
#include "Log.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"

BattlegroundAF::BattlegroundAF()
{
    BgObjects.resize(BG_AF_OBJECT_MAX);
}

void BattlegroundAF::StartingEventCloseDoors()
{
    for (uint32 i = BG_AF_OBJECT_DOOR_1; i <= BG_AF_OBJECT_DOOR_2; ++i)
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);
}

void BattlegroundAF::StartingEventOpenDoors()
{
    for (uint32 i = BG_AF_OBJECT_DOOR_1; i <= BG_AF_OBJECT_DOOR_2; ++i)
        DoorOpen(i);

    for (uint32 i = BG_AF_OBJECT_BUFF_1; i <= BG_AF_OBJECT_BUFF_2; ++i)
        SpawnBGObject(i, 60);
}

void BattlegroundAF::HandleAreaTrigger(Player* Source, uint32 Trigger)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    switch (Trigger)
    {
    case 4536:
    case 4537:
        break;
    default:
        Battleground::HandleAreaTrigger(Source, Trigger);
        break;
    }
}

void BattlegroundAF::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(0xE1A, 1);
    Arena::FillInitialWorldStates(packet);
}

bool BattlegroundAF::SetupBattleground()
{
    // Gates
    if (!AddObject(BG_AF_OBJECT_DOOR_1, BG_AF_OBJECT_TYPE_DOOR_1, 3548.0f, 5585.18f, 323.617f, 4.78249f, 0.0f, 0.0f, 0.0f, RESPAWN_IMMEDIATELY)
        || !AddObject(BG_AF_OBJECT_DOOR_2, BG_AF_OBJECT_TYPE_DOOR_2, 3540.02f, 5487.5f, 323.943f, 1.59691f, 0.0f, 0.0f, 0.0f, RESPAWN_IMMEDIATELY)
        // Buffs
        || !AddObject(BG_AF_OBJECT_BUFF_1, BG_AF_OBJECT_TYPE_BUFF_1, 3504.79f, 5499.64f, 325.824f, 0.822952f, 0.0f, 0.0f, 0.0f, 120)
        || !AddObject(BG_AF_OBJECT_BUFF_2, BG_AF_OBJECT_TYPE_BUFF_2, 3574.51f, 5576.24f, 326.795f, 4.08236f, 0.0f, 0.70068f, -0.0f, 120))
    {
        TC_LOG_ERROR("sql.sql", "BatteGroundAF: Failed to spawn some object!");
        return false;
    }

    return true;
}

void BattlegroundAF::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    _events.Update(diff);

    while (uint32 eventId = _events.ExecuteEvent())
    {
        switch (eventId)
        {
        case BG_AF_EVENT_REMOVE_DOORS:
            for (uint32 i = BG_AF_OBJECT_DOOR_1; i <= BG_AF_OBJECT_DOOR_2; ++i)
                DelObject(i);
            break;
        default:
            break;
        }
    }
}
