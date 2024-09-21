#include "BattlegroundTTP.h"
#include "Log.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"
BattlegroundTTP::BattlegroundTTP()
{
    BgObjects.resize(BG_TTP_OBJECT_MAX);

}

void BattlegroundTTP::StartingEventCloseDoors()
{
    for (uint32 i = BG_TTP_OBJECT_DOOR_1; i <= BG_TTP_OBJECT_DOOR_2; ++i)
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);
}

void BattlegroundTTP::StartingEventOpenDoors()
{
    for (uint32 i = BG_TTP_OBJECT_DOOR_1; i <= BG_TTP_OBJECT_DOOR_2; ++i)
        DoorOpen(i);

    for (uint32 i = BG_TTP_OBJECT_BUFF_1; i <= BG_TTP_OBJECT_BUFF_2; ++i)
        SpawnBGObject(i, 60);
}


void BattlegroundTTP::HandleAreaTrigger(Player* /**/, uint32 Trigger)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    switch (Trigger)
    {
        case 4536:                                          // buff trigger?
        case 4537:                                          // buff trigger?
            break;
        default:
            break;
    }
}

void BattlegroundTTP::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(2547, 1); // BATTLEGROUND_BLADES_EDGE_ARENA_SHOW

    Arena::FillInitialWorldStates(packet);
}

bool BattlegroundTTP::SetupBattleground()
{
    // Gates
    if (!AddObject(BG_TTP_OBJECT_DOOR_1, BG_TTP_OBJECT_TYPE_DOOR_1, 502.414f, 633.099f, 380.706f, 0.0308292f, 0.0f, 0.0f, 0.015414f, 0.999881f, RESPAWN_IMMEDIATELY)
        || !AddObject(BG_TTP_OBJECT_DOOR_2, BG_TTP_OBJECT_TYPE_DOOR_2, 632.891f, 633.059f, 380.705f, 3.12778f, 0.0f, 0.0f, 0.999976f, 0.0069045f, RESPAWN_IMMEDIATELY)
    // Buffs
        || !AddObject(BG_TTP_OBJECT_BUFF_1, BG_TTP_OBJECT_TYPE_BUFF_1, 566.788f, 602.743f, 383.68f, 1.5724f, 0.0f, 0.0f, 0.707673f, 0.70654f, 120)
        || !AddObject(BG_TTP_OBJECT_BUFF_2, BG_TTP_OBJECT_TYPE_BUFF_2, 566.661f, 664.311f, 383.681f, 4.66374f, 0.0f, 0.0f, 0.724097f, -0.689698f, 120))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundTTP: Failed to spawn some object!");
        return false;
    }

    return true;
}

void BattlegroundTTP::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    _events.Update(diff);

    while (uint32 eventId = _events.ExecuteEvent())
    {
        switch (eventId)
        {
        case BG_TTP_EVENT_REMOVE_DOORS:
            for (uint32 i = BG_TTP_OBJECT_DOOR_1; i <= BG_TTP_OBJECT_DOOR_2; ++i)
                DelObject(i);
            break;
        default:
            break;
        }
    }
}
