/*
* This file is part of Project SkyFire https://www.projectskyfire.org. 
* See LICENSE.md file for Copyright information
*/

#ifndef SF_BATTLEGROUNDTV_H
#define SF_BATTLEGROUNDTV_H
#include "Arena.h"
#include "EventMap.h"

enum BattlegroundTVObjectTypes
{
    BG_TV_OBJECT_DOOR_1         = 0,
    BG_TV_OBJECT_DOOR_2         = 1,
    BG_TV_OBJECT_BUFF_1         = 2,
    BG_TV_OBJECT_BUFF_2         = 3,
    BG_TV_OBJECT_MAX            = 4
};

enum BattlegroundTVObjects
{
    BG_TV_OBJECT_TYPE_DOOR_1    = 213196,
    BG_TV_OBJECT_TYPE_DOOR_2    = 213197,
    BG_TV_OBJECT_TYPE_BUFF_1    = 184663,
    BG_TV_OBJECT_TYPE_BUFF_2    = 184664
};


inline constexpr Seconds BG_TV_REMOVE_DOORS_TIMER = 5s;

enum BattlegroundTVEvents
{
    BG_TV_EVENT_REMOVE_DOORS = 1
};


class BattlegroundTV : public Arena
{
    public:
        BattlegroundTV();


        void StartingEventCloseDoors() override;
        void StartingEventOpenDoors() override;

        //void RemovePlayer(Player* player, uint64 guid, uint32 team) override;
        void HandleAreaTrigger(Player* Source, uint32 Trigger) override;
        bool SetupBattleground() override;

        void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;


        /* Scorekeeping */
       // void UpdatePlayerScore(Player* Source, uint32 type, uint32 value, bool doAddHonor = true) override;

private:
    void PostUpdateImpl(uint32 diff) override;

    EventMap _events;
};
#endif
