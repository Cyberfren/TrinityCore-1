#ifndef BATTLEGROUND_TTP_H
#define BATTLEGROUND_TTP_H


#include "Arena.h"
#include "EventMap.h"

enum BattlegroundTTPObjectTypes
{
    BG_TTP_OBJECT_DOOR_1         = 0,
    BG_TTP_OBJECT_DOOR_2         = 1,
    BG_TTP_OBJECT_BUFF_1         = 2,
    BG_TTP_OBJECT_BUFF_2         = 3,
    BG_TTP_OBJECT_MAX            = 4
};

enum BattlegroundTTPObjects
{
    BG_TTP_OBJECT_TYPE_DOOR_1    = 212921,
    BG_TTP_OBJECT_TYPE_DOOR_2    = 212921,
    BG_TTP_OBJECT_TYPE_BUFF_1    = 184663,
    BG_TTP_OBJECT_TYPE_BUFF_2    = 184664
};


inline constexpr Seconds BG_TPP_REMOVE_DOORS_TIMER = 5s;

enum BattlegroundTPPEvents
{
    BG_TTP_EVENT_REMOVE_DOORS = 1
};

class BattlegroundTTP : public Arena
{
public:
    BattlegroundTTP();

    /* inherited from BattlegroundClass */
    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;

    void HandleAreaTrigger(Player* player, uint32 Trigger) override;
    bool SetupBattleground() override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;

private:
    void PostUpdateImpl(uint32 diff) override;

    EventMap _events;
};
#endif
