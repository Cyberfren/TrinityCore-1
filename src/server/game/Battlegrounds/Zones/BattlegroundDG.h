
#ifndef BATTLEGROUND_DG_H
#define BATTLEGROUND_DG_H

#include "Battleground.h"
#include "BattlegroundScore.h"
#include "Object.h"
#include "EventMap.h"

enum BG_DG_BattlegroundNodes
{
    BG_DG_NODE_STABLES = 0,
    BG_DG_NODE_BLACKSMITH = 1,
    BG_DG_NODE_FARM = 2,
    BG_DG_DYNAMIC_NODES_COUNT = 3
};


const uint32 BG_DG_OP_NODEICONS[3] = { 1842, 1846, 1845 };
const uint32 BG_DG_OP_NODESTATES[3] = { 1767, 1782, 1772 };

enum BG_DG_TimerOrScore
{
    BG_DG_FLAG_RESPAWN_TIME = 23000,
    BG_DG_FLAG_DROP_TIME = 10000,
    BG_DG_SPELL_FORCE_TIME = 600000,
    BG_DG_SPELL_BRUTAL_TIME = 900000,
    BG_DG_MAX_TEAM_SCORE = 2000,
    BG_DG_WARNING_NEAR_VICTORY_SCORE = 1800,
    BG_DG_FLAG_CAPTURING_TIME = 60000,
};

enum BG_DG_BroadcastTexts
{
    BG_DG_TEXT_START_ONE_MINUTE = 41186,
    BG_DG_TEXT_START_HALF_MINUTE = 41187,
    BG_DG_TEXT_BATTLE_HAS_BEGUN = 10014,

    BG_DG_TEXT_CAPTURED_HORDE_FLAG = 9801,
    BG_DG_TEXT_CAPTURED_ALLIANCE_FLAG = 9802,
    BG_DG_TEXT_FLAG_PLACED_HORDE = 9803,
    BG_DG_TEXT_FLAG_PLACED_ALLIANCE = 9803,
    BG_DG_TEXT_ALLIANCE_FLAG_PICKED_UP = 9804,
    BG_DG_TEXT_ALLIANCE_FLAG_DROPPED = 9805,
    BG_DG_TEXT_HORDE_FLAG_PICKED_UP = 9807,
    BG_DG_TEXT_HORDE_FLAG_DROPPED = 9806,
    BG_DG_TEXT_ALLIANCE_FLAG_RETURNED = 9808,
    BG_DG_TEXT_HORDE_FLAG_RETURNED = 9809,




    BG_DG_TEXT_ALLIANCE_NEAR_VICTORY = 10598,
    BG_DG_TEXT_HORDE_NEAR_VICTORY = 10599,
};

enum BG_DG_Sound
{
    BG_DG_SOUND_FLAG_CAPTURED_ALLIANCE = 8173,
    BG_DG_SOUND_FLAG_CAPTURED_HORDE = 8213,
    BG_DG_SOUND_FLAG_PLACED = 8232,
    BG_DG_SOUND_FLAG_RETURNED = 8192,
    BG_DG_SOUND_HORDE_FLAG_PICKED_UP = 8212,
    BG_DG_SOUND_ALLIANCE_FLAG_PICKED_UP = 8174,
    BG_DG_SOUND_FLAGS_RESPAWNED = 8232,



    BG_DG_SOUND_NODE_CLAIMED = 8192,
    BG_DG_SOUND_NODE_CAPTURED_ALLIANCE = 8173,
    BG_DG_SOUND_NODE_CAPTURED_HORDE = 8213,
    BG_DG_SOUND_NODE_ASSAULTED_ALLIANCE = 8212,
    BG_DG_SOUND_NODE_ASSAULTED_HORDE = 8174,
    BG_DG_SOUND_NEAR_VICTORY = 8456
};
enum BG_DG_SpellId
{
    BG_DG_SPELL_HORDE_FLAG = 23333,
    BG_DG_SPELL_HORDE_FLAG_DROPPED = 23334,
    BG_DG_SPELL_HORDE_FLAG_PICKED = 61266,    // fake spell, does not exist but used as timer start event
    BG_DG_SPELL_ALLIANCE_FLAG = 23335,
    BG_DG_SPELL_ALLIANCE_FLAG_DROPPED = 23336,
    BG_DG_SPELL_ALLIANCE_FLAG_PICKED = 61265,    // fake spell, does not exist but used as timer start event
    BG_DG_SPELL_FOCUSED_ASSAULT = 46392,
    BG_DG_SPELL_BRUTAL_ASSAULT = 46393
};


#define MAX_BUFFS_DG 4

const float BG_DG_BuffPositions[MAX_BUFFS_DG][4] =
{
    {  96.05209f, 426.0920f, 111.1858f, 4.059507f },    // Nourrish
    { -93.55556f, 375.3403f, 135.5808f, 5.516745f },    // Berserk
    { -428.2292f, 581.0121f, 110.9582f, 6.058313f },    // Nourrish
    { -239.4358f, 624.6111f, 135.6253f, 2.562323f }     // Berserk
};

const float BG_DG_DoorPositions[4][8] =
{
    //      x,          y,          z,          o,     rot0, rot1, rot2, rot3
 //   { -263.434998f, 218.514999f, 132.054993f, 4.683630f, 0.f, 0.f, 0.f, 0.f },
  //  { -213.848007f, 201.164001f, 132.382004f, 3.978350f, 0.f, 0.f, 0.f, 0.f },
  //  { -119.559998f, 799.192993f, 132.414001f, 0.793560f, 0.f, 0.f, 0.f, 0.f },
  //  { -70.1034010f, 781.851013f, 132.164993f, 1.594670f, 0.f, 0.f, 0.f, 0.f }
      { -262.1527f, 224.7687f, 129.56f,  4.6f, 0.f, 0.f, 0.f, 0.f },
      { -210.0439f, 206.7399f, 130.27618f, 4.1f, 0.f, 0.f, 0.f, 0.f },
      { -70.8575f, 768.69304f,  126.469f, 1.84f, 0.f, 0.f, 0.f, 0.f },
      { -130.9073f, 787.6644f, 126.368f, 1.17670f, 0.f, 0.f, 0.f, 0.f }
};

struct DGNodeInfo
{
    uint32 NodeId;
    uint32 TextAllianceAssaulted;
    uint32 TextHordeAssaulted;
    uint32 TextAllianceTaken;
    uint32 TextHordeTaken;
    uint32 TextAllianceDefended;
    uint32 TextHordeDefended;
    uint32 TextAllianceClaims;
    uint32 TextHordeClaims;
};

DGNodeInfo const DGNodes[BG_DG_DYNAMIC_NODES_COUNT] =
{
    { BG_DG_NODE_STABLES,     80036, 80037, 80038, 80039, 80040, 80041, 80042, 80043 },  //center   
    { BG_DG_NODE_BLACKSMITH,  80028, 80029, 80030, 80031, 80032, 80033, 80034, 80035 },  //southern
    { BG_DG_NODE_FARM,        80020, 80021, 80022, 80023, 80024, 80025, 80026, 80027 }  //northern
};

const uint32 BG_DG_TickIntervals[4] = { 0, 8000, 3000, 1000 };
const uint32 BG_DG_TickPoints[4] = { 0, 10,  10,   30 };
/// To Do: Find what unk world states means and rename
enum BG_DG_WorldStates
{
    BG_DG_FLAG_UNK_ALLIANCE = 1545,
    BG_DG_FLAG_UNK_HORDE = 1546,
    //    FLAG_UNK                      = 1547,
    BG_DG_FLAG_CAPTURES_ALLIANCE = 1581,
    BG_DG_FLAG_CAPTURES_HORDE = 1582,
    BG_DG_FLAG_CAPTURES_MAX = 1601,
    BG_DG_FLAG_STATE_HORDE = 2338,
    BG_DG_FLAG_STATE_ALLIANCE = 2339,
    BG_DG_STATE_TIMER = 4248,
    BG_DG_STATE_TIMER_ACTIVE = 4247,


    BG_DG_OP_OCCUPIED_BASES_HORDE = 1778,
    BG_DG_OP_OCCUPIED_BASES_ALLY = 1779,
    BG_DG_OP_RESOURCES_ALLY = 1776,
    BG_DG_OP_RESOURCES_HORDE = 1777,
    BG_DG_OP_RESOURCES_MAX = 1780,
    BG_DG_OP_RESOURCES_WARNING = 1955,



};
const Position BG_DG_NodePositions[BG_DG_DYNAMIC_NODES_COUNT] =
{
    {-397.77f, 574.36f, 111.05f, 5.1f},    // Goblin mine
    {-167.50f, 499.05f, 92.63f, 5.1f},    // Central mine
    {57.39f, 427.17f, 111.49f, 5.1f}     // Pandaren mine
};
enum BG_DG_ObjectTypes
{
    BG_DG_OBJECT_BANNER_NEUTRAL = 0,
    BG_DG_OBJECT_BANNER_CONT_A = 1,
    BG_DG_OBJECT_BANNER_CONT_H = 2,
    BG_DG_OBJECT_BANNER_ALLY = 3,
    BG_DG_OBJECT_BANNER_HORDE = 4,
    BG_DG_OBJECT_AURA_ALLY = 5,
    BG_DG_OBJECT_AURA_HORDE = 6,
    BG_DG_OBJECT_AURA_CONTESTED = 7,
    BG_DG_OBJECT_GATE_1 = 24,
    BG_DG_OBJECT_GATE_2 = 25,
    BG_DG_OBJECT_GATE_3 = 26,
    BG_DG_OBJECT_GATE_4 = 27,
    BG_DG_OBJECT_A_FLAG = 28,
    BG_DG_OBJECT_H_FLAG = 29,
    BG_DG_OBJECT_CART_ALLY_GROUND = 30,
    BG_DG_OBJECT_CART_HORDE_GROUND = 31,
    BG_DG_OBJECT_BUFF_NORTH = 32,
    BG_DG_OBJECT_BUFF_SOUTH = 33,
    BG_DG_OBJECT_BUFF_EAST = 34,
    BG_DG_OBJECT_BUFF_WEST = 35,
   // BG_DG_OBJECT_BUFF_LAVA_H = 36,
 //   BG_DG_OBJECT_BUFF_LAVA_A = 37,
    BG_DG_OBJECT_MAX = 36
};

enum BG_DG_ObjectEntry
{

    BG_DG_OBJECTID_BANNER_A = 180058,
    BG_DG_OBJECTID_BANNER_CONT_A = 180059,
    BG_DG_OBJECTID_BANNER_H = 180060,
    BG_DG_OBJECTID_BANNER_CONT_H = 180061,
    BG_DG_OBJECTID_AURA_A = 180100,
    BG_DG_OBJECTID_AURA_H = 180101,
    BG_DG_OBJECTID_AURA_C = 180102,
  //  BG_DG_OBJECTID_GATE = 180255,
  //  BG_DG_OBJECTID_GATE_H = 180256,
    BG_DG_OBJECTID_GATE = 180259,
    BG_DG_OBJECTID_GATE_H = 180260,
    BG_OBJECT_A_FLAG_DG_ENTRY = 179830,
    BG_OBJECT_H_FLAG_DG_ENTRY = 179831,
    BG_OBJECT_A_FLAG_GROUND_DG_ENTRY = 179785,
    BG_OBJECT_H_FLAG_GROUND_DG_ENTRY = 179786,
    BG_OBJECT_SPEED_BUFF_EAST = 179899,
    BG_OBJECT_SPEED_BUFF_WEST = 179899,
    BG_OBJECT_BERSERKER_BUFF_EAST = 179907,
    BG_OBJECT_BERSERKER_BUFF_WEST = 179907,
    BG_DG_OBJECTID_NODE_BANNER_0 = 180087,       // Stables banner
    BG_DG_OBJECTID_NODE_BANNER_1 = 180088,       // Blacksmith banner
    BG_DG_OBJECTID_NODE_BANNER_2 = 180089,
    BG_OBJECTID_LAVABUFF_ENTRY_A = 179986,
    BG_OBJECTID_LAVABUFF_ENTRY_H = 179986
};


enum BG_DG_FlagState
{
    BG_DG_FLAG_STATE_ON_BASE = 0,
    BG_DG_FLAG_STATE_WAIT_RESPAWN,
    BG_DG_FLAG_STATE_ON_PLAYER,
    BG_DG_FLAG_STATE_ON_GROUND,
};

enum BG_DG_CreatureTypes
{
    BG_DG_SPIRIT_ALLIANCE = 0,
    BG_DG_SPIRIT_HORDE = 1,
    BG_DG_CREATURES_MAX = 2
};

enum BG_DG_GraveyardsIds
{
    BG_DG_SPIRIT_NORTHERN_A = 0,
    BG_DG_SPIRIT_NORTHERN_H = 1,
    BG_DG_SPIRIT_SOUTHERN_A = 2,
    BG_DG_SPIRIT_SOUTHERN_H = 3,
    DG_MAX_GRAVEYARDS = 4
};

const uint32 BG_DG_GraveYards[DG_MAX_GRAVEYARDS] = { 4546, 4489, 4488, 4545 };


enum BG_DG_CarrierDebuffs
{
    DG_SPELL_FOCUSED_ASSAULT = 46392,
    DG_SPELL_BRUTAL_ASSAULT = 46393
};

enum BG_DG_Objectives
{
    DG_OBJECTIVE_ASSAULT_BASE = 122,
    DG_OBJECTIVE_DEFEND_BASE = 123,
    DG_OBJECTIVE_CAPTURE_FLAG = 290,
    DG_OBJECTIVE_RETURN_FLAG = 291
};

enum BG_DG_Events
{
    DG_EVENT_SPAWN_FLAGS = 1,
    DG_EVENT_CLOSE_BATTLEGROUND,
    DG_EVENT_UPDATE_BATTLEGROUND_TIMER,
    DG_EVENT_RESPAWN_ALLIANCE_FLAG,
    DG_EVENT_RESPAWN_HORDE_FLAG,
};
enum BG_DG_NodeStatus
{
    BG_DG_NODE_TYPE_NEUTRAL = 0,
    BG_DG_NODE_TYPE_CONTESTED = 1,
    BG_DG_NODE_STATUS_ALLY_CONTESTED = 1,
    BG_DG_NODE_STATUS_HORDE_CONTESTED = 2,
    BG_DG_NODE_TYPE_OCCUPIED = 3,
    BG_DG_NODE_STATUS_ALLY_OCCUPIED = 3,
    BG_DG_NODE_STATUS_HORDE_OCCUPIED = 4
};
struct BG_DG_BannerTimer
{
    uint32      timer;
    uint8       type;
    uint8       teamIndex;
};
#define DG_EVENT_START_BATTLE 8563
struct BattlegroundDGScore final : public BattlegroundScore
{
    friend class BattlegroundDG;

protected:
    BattlegroundDGScore(ObjectGuid playerGuid) : BattlegroundScore(playerGuid), BasesAssaulted(0), BasesDefended(0), FlagsCaptured(0), FlagsReturned(0) { }

    void UpdateScore(uint32 type, uint32 value) override
    {
        switch (type)
        {
        case SCORE_BASES_ASSAULTED:
            BasesAssaulted += value;
            break;
        case SCORE_BASES_DEFENDED:
            BasesDefended += value;
            break;
        case SCORE_FLAG_CAPTURES:
            FlagsCaptured += value;
            break;
        case SCORE_FLAG_RETURNS:
            FlagsReturned += value;
            break;
        default:
            BattlegroundScore::UpdateScore(type, value);
            break;
        }
    }

    void BuildObjectivesBlock(WorldPacket& data) final override;

    uint32 GetAttr1() const final override { return BasesAssaulted; }
    uint32 GetAttr2() const final override { return BasesDefended; }
    uint32 GetAttr3() const final override { return FlagsCaptured; }
    uint32 GetAttr4() const final override { return FlagsReturned; }

    uint32 BasesAssaulted;
    uint32 BasesDefended;
    uint32 FlagsCaptured;
    uint32 FlagsReturned;
};


class BattlegroundDG : public Battleground
{
public:
    BattlegroundDG();
    ~BattlegroundDG();


    void AddPlayer(Player* player) override;
    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;


    ObjectGuid GetFlagPickerGUID(int32 team) const override
    {
        if (team == TEAM_ALLIANCE || team == TEAM_HORDE)
            return m_FlagKeepers[team];
        return ObjectGuid::Empty;
    }
    void SetAllianceFlagPicker(ObjectGuid guid) { m_FlagKeepers[TEAM_ALLIANCE] = guid; }
    void SetHordeFlagPicker(ObjectGuid guid) { m_FlagKeepers[TEAM_HORDE] = guid; }
    bool IsAllianceFlagPickedup() const { return !m_FlagKeepers[TEAM_ALLIANCE].IsEmpty(); }
    bool IsHordeFlagPickedup() const { return !m_FlagKeepers[TEAM_HORDE].IsEmpty(); }
    void RespawnFlag(uint32 Team, bool captured);
    void RespawnFlagAfterDrop(uint32 Team);
    uint8 GetFlagState(uint32 team) { return _flagState[GetTeamIndexByTeamId(team)]; }

    virtual void SendRemoveWorldStates(Player* /*player*/) { }
    void EventPlayerCapturedFlag(Player* player);
    void EventPlayerDroppedFlag(Player* player) override;
    void EventPlayerClickedOnFlag(Player* player, GameObject* target_obj) override;
    void RemovePlayer(Player* player, ObjectGuid guid, uint32 team) override;
    void HandleAreaTrigger(Player* player, uint32 trigger) override;
    void HandleKillPlayer(Player* player, Player* killer) override;
    bool SetupBattleground() override;
    void Reset() override;
    void EndBattleground(uint32 winner) override;
   // GameObjectTemplate const* GetGOInfo() const { return m_goInfo; }
    WorldSafeLocsEntry const* GetClosestGraveyard(Player* player) override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;
    void UpdateFlagState(uint32 team, uint32 value);
    void SetLastFlagCapture(uint32 team) { _lastFlagCaptureTeam = team; }
    void UpdateTeamScore(int team, int32 value);
    bool UpdatePlayerScore(Player* player, uint32 type, uint32 value, bool doAddHonor = true) override;
    void SetDroppedFlagGUID(ObjectGuid guid, int32 team = -1) override
    {
        if (team == TEAM_ALLIANCE || team == TEAM_HORDE)
            m_DroppedFlagGUID[team] = guid;
    }

    ObjectGuid GetDroppedFlagGUID(uint32 TeamID) { return m_DroppedFlagGUID[GetTeamIndexByTeamId(TeamID)]; }

    void AddPoint(uint32 TeamID, uint32 Points = 1) { m_TeamScores[GetTeamIndexByTeamId(TeamID)] += Points; }
    void SetTeamPoint(uint32 TeamID, uint32 Points = 0) { m_TeamScores[GetTeamIndexByTeamId(TeamID)] = Points; }
    void RemovePoint(uint32 TeamID, uint32 Points = 1) { m_TeamScores[GetTeamIndexByTeamId(TeamID)] -= Points; }
    uint32 GetPrematureWinner() override;

private:
    void _CreateBanner(uint8 node, uint8 type, uint8 teamIndex, bool delay);
    void _DelBanner(uint8 node, uint8 type, uint8 teamIndex);
    void _SendNodeUpdate(uint8 node);
    
    /// @todo working, scripted peons spawning
    void _NodeOccupied(uint8 node, Team team);
    void _NodeDeOccupied(uint8 node);
    //float negativeAllyValue;
  //  float negativeHordeValue;
  //  GameObjectTemplate const* m_goInfo;

   uint8                   m_Nodes[BG_DG_DYNAMIC_NODES_COUNT];
    uint8                   m_prevNodes[BG_DG_DYNAMIC_NODES_COUNT];
   BG_DG_BannerTimer      m_BannerTimers[BG_DG_DYNAMIC_NODES_COUNT];
   uint32                  m_NodeTimers[BG_DG_DYNAMIC_NODES_COUNT];
    uint32                  m_lastTick[PVP_TEAMS_COUNT];
    uint32                  m_HonorScoreTics[PVP_TEAMS_COUNT];
    bool                    m_IsInformedNearVictory;
    uint32                  m_HonorTics;



    ObjectGuid m_FlagKeepers[2];                            // 0 - alliance, 1 - horde
    ObjectGuid m_DroppedFlagGUID[2];
    uint8 _flagState[2];                               // for checking flag state
    int32 _flagsTimer[2];
    int32 _flagsDropTimer[2];
    uint32 _lastFlagCaptureTeam;                       // Winner is based on this if score is equal

    uint32 m_ReputationCapture;
    uint32 m_HonorWinKills;
    uint32 m_HonorEndKills;
    int32 _flagSpellForceTimer;
    bool _bothFlagsKept;
    uint8 _flagDebuffState;                            // 0 - no debuffs, 1 - focused assault, 2 - brutal assault
    uint8 _minutesElapsed;

    void PostUpdateImpl(uint32 diff) override;

    EventMap events;
};

#endif


