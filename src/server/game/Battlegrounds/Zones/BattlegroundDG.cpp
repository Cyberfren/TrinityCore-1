
#include "BattlegroundDG.h"
#include "Player.h"
#include "BattlegroundMgr.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "Random.h"
#include "Util.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldStatePackets.h"
//#include "Object.h"
#include "ObjectAccessor.h"

enum BG_DG_Rewards
{
    BG_DG_WIN = 0,
    BG_DG_FLAG_CAP,
    BG_DG_MAP_COMPLETE,
    BG_DG_REWARD_NUM
};

uint32 BG_DG_Honor[BG_HONOR_MODE_NUM][BG_DG_REWARD_NUM] =
{
    {20, 40, 40}, // normal honor
    {60, 40, 80}  // holiday
};


BattlegroundDG::BattlegroundDG()
{
    BgObjects.resize(BG_DG_OBJECT_MAX);
    BgCreatures.resize(BG_DG_CREATURES_MAX);

    StartMessageIds[BG_STARTING_EVENT_SECOND] = BG_DG_TEXT_START_ONE_MINUTE;
    StartMessageIds[BG_STARTING_EVENT_THIRD] = BG_DG_TEXT_START_HALF_MINUTE;
    StartMessageIds[BG_STARTING_EVENT_FOURTH] = BG_DG_TEXT_BATTLE_HAS_BEGUN;

    _flagSpellForceTimer = 0;
    _bothFlagsKept = false;
    _flagDebuffState = 0;
    m_FlagKeepers[TEAM_ALLIANCE].Clear();
    m_FlagKeepers[TEAM_HORDE].Clear();
    m_DroppedFlagGUID[TEAM_ALLIANCE].Clear();
    m_DroppedFlagGUID[TEAM_HORDE].Clear();
    _flagState[TEAM_ALLIANCE] = BG_DG_FLAG_STATE_ON_BASE;
    _flagState[TEAM_HORDE] = BG_DG_FLAG_STATE_ON_BASE;
    _flagsTimer[TEAM_ALLIANCE] = 0;
    _flagsTimer[TEAM_HORDE] = 0;
    _flagsDropTimer[TEAM_ALLIANCE] = 0;
    _flagsDropTimer[TEAM_HORDE] = 0;
    _lastFlagCaptureTeam = 0;
    m_ReputationCapture = 0;
    m_HonorWinKills = 0;
    m_HonorEndKills = 0;
    _minutesElapsed = 0;




    m_IsInformedNearVictory = false;
   // m_BuffChange = true;
    //BgObjects.resize(BG_BFG_OBJECT_MAX);
   // BgCreatures.resize(BG_BFG_ALL_NODES_COUNT + 5);//+5 for aura triggers

    for (uint8 i = 0; i < BG_DG_DYNAMIC_NODES_COUNT; ++i)
    {
        m_Nodes[i] = 0;
        m_prevNodes[i] = 0;
        m_NodeTimers[i] = 0;
        m_BannerTimers[i].timer = 0;
        m_BannerTimers[i].type = 0;
        m_BannerTimers[i].teamIndex = 0;
    }

    for (uint8 i = 0; i < PVP_TEAMS_COUNT; ++i)
    {
        m_lastTick[i] = 0;
        m_HonorScoreTics[i] = 0;
        //m_TeamScores500Disadvantage[i] = false;
    }

    m_HonorTics = 0;
}

void BattlegroundDGScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(4);
    data << uint32(BasesAssaulted);
    data << uint32(BasesDefended);
    data << uint32(FlagsCaptured);
    data << uint32(FlagsReturned);
}


BattlegroundDG::~BattlegroundDG() { }

void BattlegroundDG::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() == STATUS_IN_PROGRESS)
    {
        events.Update(diff);

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
            case DG_EVENT_SPAWN_FLAGS: // Spawn flags in 2,5 seconds after battle started
                SpawnBGObject(BG_DG_OBJECT_A_FLAG, RESPAWN_IMMEDIATELY);
                SpawnBGObject(BG_DG_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
                SendBroadcastText(BG_DG_TEXT_FLAG_PLACED_ALLIANCE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
                SendBroadcastText(BG_DG_TEXT_FLAG_PLACED_HORDE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
                PlaySoundToAll(BG_DG_SOUND_FLAGS_RESPAWNED);
                break;
            case DG_EVENT_CLOSE_BATTLEGROUND: // Close battleground after 27 minutes of its creation
                if (GetTeamScore(TEAM_ALLIANCE) == 0)
                {
                    if (GetTeamScore(TEAM_HORDE) == 0)        // No one scored - result is tie
                        EndBattleground(0);
                    else                                 // Horde has more points and thus wins
                        EndBattleground(HORDE);


                }
                else if (GetTeamScore(TEAM_HORDE) == 0)
                    EndBattleground(ALLIANCE);           // Alliance has > 0, Horde has 0, alliance wins
                else if (GetTeamScore(TEAM_HORDE) == GetTeamScore(TEAM_ALLIANCE)) // Team score equal, winner is team that scored the last flag
                    EndBattleground(_lastFlagCaptureTeam);
                else if (GetTeamScore(TEAM_HORDE) > GetTeamScore(TEAM_ALLIANCE))  // Last but not least, check who has the higher score
                    EndBattleground(HORDE);
                else
                    EndBattleground(ALLIANCE);
                break;
            case DG_EVENT_UPDATE_BATTLEGROUND_TIMER:
                _minutesElapsed++;
                UpdateWorldState(BG_DG_STATE_TIMER, 25 - _minutesElapsed);
                events.Repeat(Minutes(1));
                break;
            default:
                break;
            }
        }

        if (_flagState[TEAM_ALLIANCE] == BG_DG_FLAG_STATE_WAIT_RESPAWN)
        {
            _flagsTimer[TEAM_ALLIANCE] -= diff;

            if (_flagsTimer[TEAM_ALLIANCE] < 0)
            {
                _flagsTimer[TEAM_ALLIANCE] = 0;
                RespawnFlag(ALLIANCE, true);
            }
        }

        if (_flagState[TEAM_ALLIANCE] == BG_DG_FLAG_STATE_ON_GROUND)
        {
            _flagsDropTimer[TEAM_ALLIANCE] -= diff;

            if (_flagsDropTimer[TEAM_ALLIANCE] < 0)
            {
                _flagsDropTimer[TEAM_ALLIANCE] = 0;
                RespawnFlagAfterDrop(ALLIANCE);
                _bothFlagsKept = false;
            }
        }

        if (_flagState[TEAM_HORDE] == BG_DG_FLAG_STATE_WAIT_RESPAWN)
        {
            _flagsTimer[TEAM_HORDE] -= diff;

            if (_flagsTimer[TEAM_HORDE] < 0)
            {
                _flagsTimer[TEAM_HORDE] = 0;
                RespawnFlag(HORDE, true);
            }
        }

        if (_flagState[TEAM_HORDE] == BG_DG_FLAG_STATE_ON_GROUND)
        {
            _flagsDropTimer[TEAM_HORDE] -= diff;

            if (_flagsDropTimer[TEAM_HORDE] < 0)
            {
                _flagsDropTimer[TEAM_HORDE] = 0;
                RespawnFlagAfterDrop(HORDE);
                _bothFlagsKept = false;
            }
        }

        if (_bothFlagsKept)
        {
            _flagSpellForceTimer += diff;
            if (_flagDebuffState == 0 && _flagSpellForceTimer >= 10 * MINUTE * IN_MILLISECONDS)  //10 minutes
            {
                if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[0]))
                    player->CastSpell(player, DG_SPELL_FOCUSED_ASSAULT, true);
                if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[1]))
                    player->CastSpell(player, DG_SPELL_FOCUSED_ASSAULT, true);
                _flagDebuffState = 1;
            }
            else if (_flagDebuffState == 1 && _flagSpellForceTimer >= 15 * MINUTE * IN_MILLISECONDS) //15 minutes
            {
                if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[0]))
                {
                    player->RemoveAurasDueToSpell(DG_SPELL_FOCUSED_ASSAULT);
                    player->CastSpell(player, DG_SPELL_BRUTAL_ASSAULT, true);
                }
                if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[1]))
                {
                    player->RemoveAurasDueToSpell(DG_SPELL_FOCUSED_ASSAULT);
                    player->CastSpell(player, DG_SPELL_BRUTAL_ASSAULT, true);
                }
                _flagDebuffState = 2;
            }
        }
        else
        {
            if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[0]))
            {
                player->RemoveAurasDueToSpell(DG_SPELL_FOCUSED_ASSAULT);
                player->RemoveAurasDueToSpell(DG_SPELL_BRUTAL_ASSAULT);
            }
            if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[1]))
            {
                player->RemoveAurasDueToSpell(DG_SPELL_FOCUSED_ASSAULT);
                player->RemoveAurasDueToSpell(DG_SPELL_BRUTAL_ASSAULT);
            }

            _flagSpellForceTimer = 0; //reset timer.
            _flagDebuffState = 0;
        }



        int team_points[PVP_TEAMS_COUNT] = { 0, 0 };

        for (int node = 0; node < BG_DG_DYNAMIC_NODES_COUNT; ++node)
        {
            // 3 sec delay to spawn new banner instead previous despawned one
            if (m_BannerTimers[node].timer)
            {
                if (m_BannerTimers[node].timer > diff)
                    m_BannerTimers[node].timer -= diff;
                else
                {
                    m_BannerTimers[node].timer = 0;
                    _CreateBanner(node, m_BannerTimers[node].type, m_BannerTimers[node].teamIndex, false);
                }
            }

            // 1-minute to occupy a node from contested state
            if (m_NodeTimers[node])
            {
                if (m_NodeTimers[node] > diff)
                    m_NodeTimers[node] -= diff;
                else
                {
                    m_NodeTimers[node] = 0;
                    // Change from contested to occupied !
                    uint8 teamIndex = m_Nodes[node] - 1;
                    m_prevNodes[node] = m_Nodes[node];
                    m_Nodes[node] += 2;
                    // burn current contested banner
                    _DelBanner(node, BG_DG_NODE_TYPE_CONTESTED, teamIndex);
                    // create new occupied banner
                    _CreateBanner(node, BG_DG_NODE_TYPE_OCCUPIED, teamIndex, true);
                    _SendNodeUpdate(node);
                    _NodeOccupied(node, (teamIndex == TEAM_ALLIANCE) ? ALLIANCE : HORDE);
                    // Message to chatlog

                    if (teamIndex == TEAM_ALLIANCE)
                    {
                        SendBroadcastText(DGNodes[node].TextAllianceTaken, CHAT_MSG_BG_SYSTEM_ALLIANCE);
                        PlaySoundToAll(BG_DG_SOUND_NODE_CAPTURED_ALLIANCE);
                    }
                    else
                    {
                        SendBroadcastText(DGNodes[node].TextHordeTaken, CHAT_MSG_BG_SYSTEM_HORDE);
                        PlaySoundToAll(BG_DG_SOUND_NODE_CAPTURED_HORDE);
                    }
                }
            }

            for (int team = 0; team < PVP_TEAMS_COUNT; ++team)
                if (m_Nodes[node] == team + BG_DG_NODE_TYPE_OCCUPIED)
                    ++team_points[team];
        }

        // Accumulate points
        for (int team = 0; team < PVP_TEAMS_COUNT; ++team)
        {
            int points = team_points[team];
            if (!points)
                continue;

            m_lastTick[team] += diff;

            if (m_lastTick[team] > BG_DG_TickIntervals[points])
            {
                m_lastTick[team] -= BG_DG_TickIntervals[points];
                m_TeamScores[team] += BG_DG_TickPoints[points];
                m_HonorScoreTics[team] += BG_DG_TickPoints[points];

                if (m_HonorScoreTics[team] >= m_HonorTics)
                {
                    RewardHonorToTeam(GetBonusHonorFromKill(1), (team == TEAM_ALLIANCE) ? ALLIANCE : HORDE);
                    m_HonorScoreTics[team] -= m_HonorTics;
                }

                if (!m_IsInformedNearVictory && m_TeamScores[team] > BG_DG_WARNING_NEAR_VICTORY_SCORE)
                {
                    if (team == TEAM_ALLIANCE)
                        SendBroadcastText(BG_DG_TEXT_ALLIANCE_NEAR_VICTORY, CHAT_MSG_BG_SYSTEM_NEUTRAL);
                    else
                        SendBroadcastText(BG_DG_TEXT_HORDE_NEAR_VICTORY, CHAT_MSG_BG_SYSTEM_NEUTRAL);
                    PlaySoundToAll(BG_DG_SOUND_NEAR_VICTORY);
                    m_IsInformedNearVictory = true;
                }

                if (m_TeamScores[team] > BG_DG_MAX_TEAM_SCORE)
                    m_TeamScores[team] = BG_DG_MAX_TEAM_SCORE;

                if (team == TEAM_ALLIANCE)
                    UpdateWorldState(BG_DG_OP_RESOURCES_ALLY, m_TeamScores[team]);
                else
                    UpdateWorldState(BG_DG_OP_RESOURCES_HORDE, m_TeamScores[team]);

            }
        }

        // Test win condition

        if (m_TeamScores[TEAM_ALLIANCE] >= BG_DG_MAX_TEAM_SCORE) {
            // UpdateWorldState(1842, 0);
         //    UpdateWorldState(1846, 0);
         //    UpdateWorldState(1845, 0);
            EndBattleground(ALLIANCE);
        }
        else if (m_TeamScores[TEAM_HORDE] >= BG_DG_MAX_TEAM_SCORE) {
            //   UpdateWorldState(1842, 0);
           //    UpdateWorldState(1846, 0);
           //    UpdateWorldState(1845, 0);
            EndBattleground(HORDE);
        }
    }
}




void BattlegroundDG::EventPlayerCapturedFlag(Player* player)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    uint32 winner = 0;
    uint8 ally = 0, horde = 0;
    for (uint8 i = 0; i < BG_DG_DYNAMIC_NODES_COUNT; ++i)
        if (m_Nodes[i] == BG_DG_NODE_STATUS_ALLY_OCCUPIED)
            ++ally;
        else if (m_Nodes[i] == BG_DG_NODE_STATUS_HORDE_OCCUPIED)
            ++horde;

    player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);
    if (player->GetTeam() == ALLIANCE)
    {
        if (!IsHordeFlagPickedup())
            return;
        AddPoint(ALLIANCE, 75);
        SetHordeFlagPicker(ObjectGuid::Empty);                         // horde flag in base (but not respawned yet)
        _flagState[TEAM_HORDE] = BG_DG_FLAG_STATE_WAIT_RESPAWN;
        // Drop Horde Flag from Player
        player->RemoveAurasDueToSpell(BG_DG_SPELL_HORDE_FLAG);
        PlaySoundToAll(BG_DG_SOUND_FLAG_CAPTURED_ALLIANCE);
        if (_flagDebuffState == 1)
            player->RemoveAurasDueToSpell(DG_SPELL_FOCUSED_ASSAULT);
        else if (_flagDebuffState == 2)
            player->RemoveAurasDueToSpell(DG_SPELL_BRUTAL_ASSAULT);

        if (GetTeamScore(TEAM_ALLIANCE) < BG_DG_MAX_TEAM_SCORE)
        {
            switch (ally)
            {
            case 3:
                AddPoint(ALLIANCE, 225);
                break;
            case 2:
                AddPoint(ALLIANCE, 150);
                break;
            case 1:
                AddPoint(ALLIANCE, 100);
                break;
            }
        }
        UpdateWorldState(BG_DG_OP_RESOURCES_ALLY, m_TeamScores[0]);
    }
    else
    {
        if (!IsAllianceFlagPickedup())
            return;
        AddPoint(HORDE, 100);
        SetAllianceFlagPicker(ObjectGuid::Empty);                                                      // alliance flag in base (but not respawned yet)
        _flagState[TEAM_ALLIANCE] = BG_DG_FLAG_STATE_WAIT_RESPAWN;
        // Drop Alliance Flag from Player
        player->RemoveAurasDueToSpell(BG_DG_SPELL_ALLIANCE_FLAG);
        if (_flagDebuffState == 1)
            player->RemoveAurasDueToSpell(DG_SPELL_FOCUSED_ASSAULT);
        else if (_flagDebuffState == 2)
            player->RemoveAurasDueToSpell(DG_SPELL_BRUTAL_ASSAULT);
        PlaySoundToAll(BG_DG_SOUND_FLAG_CAPTURED_HORDE);
        if (GetTeamScore(TEAM_HORDE) < BG_DG_MAX_TEAM_SCORE)
        {
            switch (horde)
            {
            case 3:
                AddPoint(HORDE, 300);
                break;
            case 2:
                AddPoint(HORDE, 225);
                break;
            case 1:
                AddPoint(HORDE, 175);
                break;
            }
        }
        UpdateWorldState(BG_DG_OP_RESOURCES_HORDE, m_TeamScores[1]);
     
    }
    //for flag capture is reward 2 honorable kills
    RewardHonorToTeam(GetBonusHonorFromKill(2), player->GetTeam());

    SpawnBGObject(BG_DG_OBJECT_H_FLAG, BG_DG_FLAG_RESPAWN_TIME);
    SpawnBGObject(BG_DG_OBJECT_A_FLAG, BG_DG_FLAG_RESPAWN_TIME);

    if (player->GetTeam() == ALLIANCE)
        SendBroadcastText(BG_DG_TEXT_CAPTURED_HORDE_FLAG, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
    else
        SendBroadcastText(BG_DG_TEXT_CAPTURED_ALLIANCE_FLAG, CHAT_MSG_BG_SYSTEM_HORDE, player);

    UpdateFlagState(player->GetTeam(), 1);                  // flag state none
    // only flag capture should be updated
    UpdatePlayerScore(player, SCORE_FLAG_CAPTURES, 1);
    // update last flag capture to be used if teamscore is equal
    //SetLastFlagCapture(player->GetTeam());

    if (GetTeamScore(TEAM_ALLIANCE) == BG_DG_MAX_TEAM_SCORE)
        winner = ALLIANCE;

    if (GetTeamScore(TEAM_HORDE) == BG_DG_MAX_TEAM_SCORE)
        winner = HORDE;

    if (winner)
    {
        UpdateWorldState(BG_DG_FLAG_UNK_ALLIANCE, 0);
        UpdateWorldState(BG_DG_FLAG_UNK_HORDE, 0);
        UpdateWorldState(BG_DG_FLAG_STATE_ALLIANCE, 1);
        UpdateWorldState(BG_DG_FLAG_STATE_HORDE, 1);
        UpdateWorldState(BG_DG_STATE_TIMER_ACTIVE, 0);

        RewardHonorToTeam(BG_DG_Honor[m_HonorMode][BG_DG_WIN], winner);
        EndBattleground(winner);

    }
    else
    {
        _flagsTimer[GetTeamIndexByTeamId(player->GetTeam()) ? 0 : 1] = BG_DG_FLAG_RESPAWN_TIME;
    }

}
bool BattlegroundDG::UpdatePlayerScore(Player* player, uint32 type, uint32 value, bool doAddHonor)
{
    if (!Battleground::UpdatePlayerScore(player, type, value, doAddHonor))
        return false;

    switch (type)
    {
    case SCORE_BASES_ASSAULTED:
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BG_OBJECTIVE_CAPTURE, DG_OBJECTIVE_ASSAULT_BASE);
        break;
    case SCORE_BASES_DEFENDED:
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BG_OBJECTIVE_CAPTURE, DG_OBJECTIVE_DEFEND_BASE);
        break;
    case SCORE_FLAG_CAPTURES:
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BG_OBJECTIVE_CAPTURE, DG_OBJECTIVE_CAPTURE_FLAG);
        break;
    case SCORE_FLAG_RETURNS:
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BG_OBJECTIVE_CAPTURE, DG_OBJECTIVE_RETURN_FLAG);
        break;
    default:
        break;
    }
    return true;
}


void BattlegroundDG::_CreateBanner(uint8 node, uint8 type, uint8 teamIndex, bool delay)
{
    // Just put it into the queue
    if (delay)
    {
        m_BannerTimers[node].timer = 2000;
        m_BannerTimers[node].type = type;
        m_BannerTimers[node].teamIndex = teamIndex;
        return;
    }

    uint8 obj = node * 8 + type + teamIndex;

    SpawnBGObject(obj, RESPAWN_IMMEDIATELY);

    // handle aura with banner
    if (!type)
        return;
    obj = node * 8 + ((type == BG_DG_NODE_TYPE_OCCUPIED) ? (5 + teamIndex) : 7);
    SpawnBGObject(obj, RESPAWN_IMMEDIATELY);
}

void BattlegroundDG::_DelBanner(uint8 node, uint8 type, uint8 teamIndex)
{
    uint8 obj = node * 8 + type + teamIndex;
    SpawnBGObject(obj, RESPAWN_ONE_DAY);

    // handle aura with banner
    if (!type)
        return;
    obj = node * 8 + ((type == BG_DG_NODE_TYPE_OCCUPIED) ? (5 + teamIndex) : 7);
    SpawnBGObject(obj, RESPAWN_ONE_DAY);
}

void BattlegroundDG::_SendNodeUpdate(uint8 node)
{
    // Send node owner state update to refresh map icons on client
    const uint8 plusArray[] = { 0, 2, 3, 0, 1 };

    if (m_prevNodes[node])
        UpdateWorldState(BG_DG_OP_NODESTATES[node] + plusArray[m_prevNodes[node]], 0);
    else
        UpdateWorldState(BG_DG_OP_NODEICONS[node], 0);

    UpdateWorldState(BG_DG_OP_NODESTATES[node] + plusArray[m_Nodes[node]], 1);

    // How many bases each team owns
    uint8 ally = 0, horde = 0;
    for (uint8 i = 0; i < BG_DG_DYNAMIC_NODES_COUNT; ++i)
        if (m_Nodes[i] == BG_DG_NODE_STATUS_ALLY_OCCUPIED)
            ++ally;
        else if (m_Nodes[i] == BG_DG_NODE_STATUS_HORDE_OCCUPIED)
            ++horde;

    UpdateWorldState(BG_DG_OP_OCCUPIED_BASES_ALLY, ally);
    UpdateWorldState(BG_DG_OP_OCCUPIED_BASES_HORDE, horde);
}

void BattlegroundDG::_NodeOccupied(uint8 node, Team team)
{
    

    if (node >= BG_DG_DYNAMIC_NODES_COUNT)//only dynamic nodes, no start points
        return;

    uint8 capturedNodes = 0;
    for (uint8 i = 0; i < BG_DG_DYNAMIC_NODES_COUNT; ++i)
        if (m_Nodes[i] == uint32(GetTeamIndexByTeamId(team)) + BG_DG_NODE_TYPE_OCCUPIED && !m_NodeTimers[i])
            ++capturedNodes;


    //add bonus honor aura trigger creature when node is accupied
    //cast bonus aura (+50% honor in 25yards)
    //aura should only apply to players who have accupied the node, set correct faction for trigger
   
}

void BattlegroundDG::_NodeDeOccupied(uint8 node)
{
    //only dynamic nodes, no start points
    if (node >= BG_DG_DYNAMIC_NODES_COUNT)
        return;


    // buff object isn't despawned
}

void BattlegroundDG::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    Battleground::AddPlayer(player);
    if (!isInBattleground)
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundDGScore(player->GetGUID());
}

void BattlegroundDG::StartingEventCloseDoors()
{
    for (uint32 i = BG_DG_OBJECT_BUFF_NORTH; i <= BG_DG_OBJECT_BUFF_WEST; ++i)
        SpawnBGObject(i, RESPAWN_ONE_DAY);
    
    for (uint32 i = BG_DG_OBJECT_A_FLAG; i <= BG_DG_OBJECT_CART_HORDE_GROUND; ++i)
        SpawnBGObject(i, RESPAWN_ONE_DAY);
    for (int object = BG_DG_OBJECT_BANNER_NEUTRAL; object < BG_DG_DYNAMIC_NODES_COUNT * 8; ++object)
        SpawnBGObject(object, RESPAWN_ONE_DAY);
    for (uint32 i = BG_DG_OBJECT_GATE_1; i <= BG_DG_OBJECT_GATE_4; ++i)
    {
        DoorClose(i);
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);
    }

    UpdateWorldState(BG_DG_STATE_TIMER_ACTIVE, 1);
    UpdateWorldState(BG_DG_STATE_TIMER, 25);
}

void BattlegroundDG::StartingEventOpenDoors()
{


    for (int banner = BG_DG_OBJECT_BANNER_NEUTRAL, i = 0; i < 3; banner += 8, ++i)
        SpawnBGObject(banner, RESPAWN_IMMEDIATELY);

    for (uint32 i = BG_DG_OBJECT_GATE_1; i <= BG_DG_OBJECT_GATE_4; ++i)
    {       DoorOpen(i);
    SpawnBGObject(i, RESPAWN_ONE_DAY);
    }


    for (uint32 i = BG_DG_OBJECT_BUFF_NORTH; i <= BG_DG_OBJECT_BUFF_WEST; ++i)
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);
    // players joining later are not eligibles

    events.ScheduleEvent(DG_EVENT_UPDATE_BATTLEGROUND_TIMER, Minutes(1));
    StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_EVENT, DG_EVENT_START_BATTLE);
    events.ScheduleEvent(DG_EVENT_SPAWN_FLAGS, Seconds(2) + Milliseconds(500));
}


bool BattlegroundDG::SetupBattleground()
{
    if (!AddObject(BG_DG_OBJECT_A_FLAG, BG_OBJECT_A_FLAG_DG_ENTRY, -241.8901f, 208.462f, 133.819f, 133.741259f, 0, 0, 0.9996573f, 0.02617699f, BG_DG_FLAG_RESPAWN_TIME / 1000)
        || !AddObject(BG_DG_OBJECT_H_FLAG, BG_OBJECT_H_FLAG_DG_ENTRY, -91.6f, 791.363f, 133.82f, 2.792518f, 0, 0, 0.984807f, 0.1736523f, BG_DG_FLAG_RESPAWN_TIME / 1000))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundDG: Failed to spawn carts. Battleground not created!");
        return false;
    }
    for (int i = 0; i < BG_DG_DYNAMIC_NODES_COUNT; ++i)
    {
        if (!AddObject(BG_DG_OBJECT_BANNER_NEUTRAL + 8 * i, BG_DG_OBJECTID_NODE_BANNER_0 + i, BG_DG_NodePositions[i], 0, 0, std::sin(BG_DG_NodePositions[i].GetOrientation() / 2), std::cos(BG_DG_NodePositions[i].GetOrientation() / 2), RESPAWN_ONE_DAY)
            || !AddObject(BG_DG_OBJECT_BANNER_CONT_A + 8 * i, BG_DG_OBJECTID_BANNER_CONT_A, BG_DG_NodePositions[i], 0, 0, std::sin(BG_DG_NodePositions[i].GetOrientation() / 2), std::cos(BG_DG_NodePositions[i].GetOrientation() / 2), RESPAWN_ONE_DAY)
            || !AddObject(BG_DG_OBJECT_BANNER_CONT_H + 8 * i, BG_DG_OBJECTID_BANNER_CONT_H, BG_DG_NodePositions[i], 0, 0, std::sin(BG_DG_NodePositions[i].GetOrientation() / 2), std::cos(BG_DG_NodePositions[i].GetOrientation() / 2), RESPAWN_ONE_DAY)
            || !AddObject(BG_DG_OBJECT_BANNER_ALLY + 8 * i, BG_DG_OBJECTID_BANNER_A, BG_DG_NodePositions[i], 0, 0, std::sin(BG_DG_NodePositions[i].GetOrientation() / 2), std::cos(BG_DG_NodePositions[i].GetOrientation() / 2), RESPAWN_ONE_DAY)
            || !AddObject(BG_DG_OBJECT_BANNER_HORDE + 8 * i, BG_DG_OBJECTID_BANNER_H, BG_DG_NodePositions[i], 0, 0, std::sin(BG_DG_NodePositions[i].GetOrientation() / 2), std::cos(BG_DG_NodePositions[i].GetOrientation() / 2), RESPAWN_ONE_DAY)
            || !AddObject(BG_DG_OBJECT_AURA_ALLY + 8 * i, BG_DG_OBJECTID_AURA_A, BG_DG_NodePositions[i], 0, 0, std::sin(BG_DG_NodePositions[i].GetOrientation() / 2), std::cos(BG_DG_NodePositions[i].GetOrientation() / 2), RESPAWN_ONE_DAY)
            || !AddObject(BG_DG_OBJECT_AURA_HORDE + 8 * i, BG_DG_OBJECTID_AURA_H, BG_DG_NodePositions[i], 0, 0, std::sin(BG_DG_NodePositions[i].GetOrientation() / 2), std::cos(BG_DG_NodePositions[i].GetOrientation() / 2), RESPAWN_ONE_DAY)
            || !AddObject(BG_DG_OBJECT_AURA_CONTESTED + 8 * i, BG_DG_OBJECTID_AURA_C, BG_DG_NodePositions[i], 0, 0, std::sin(BG_DG_NodePositions[i].GetOrientation() / 2), std::cos(BG_DG_NodePositions[i].GetOrientation() / 2), RESPAWN_ONE_DAY))
        {
            TC_LOG_ERROR("sql.sql", "BatteGroundAB: Failed to spawn some object Battleground not created!");
            return false;
        }

    }
    // Doors
    if (!AddObject(BG_DG_OBJECT_GATE_1, BG_DG_OBJECTID_GATE, BG_DG_DoorPositions[0][0], BG_DG_DoorPositions[0][1], BG_DG_DoorPositions[0][2], BG_DG_DoorPositions[0][3], BG_DG_DoorPositions[0][4], BG_DG_DoorPositions[0][5], BG_DG_DoorPositions[0][6], BG_DG_DoorPositions[0][7], RESPAWN_IMMEDIATELY)
        || !AddObject(BG_DG_OBJECT_GATE_2, BG_DG_OBJECTID_GATE, BG_DG_DoorPositions[1][0], BG_DG_DoorPositions[1][1], BG_DG_DoorPositions[1][2], BG_DG_DoorPositions[1][3], BG_DG_DoorPositions[1][4], BG_DG_DoorPositions[1][5], BG_DG_DoorPositions[1][6], BG_DG_DoorPositions[1][7], RESPAWN_IMMEDIATELY)
        || !AddObject(BG_DG_OBJECT_GATE_3, BG_DG_OBJECTID_GATE_H, BG_DG_DoorPositions[2][0], BG_DG_DoorPositions[2][1], BG_DG_DoorPositions[2][2], BG_DG_DoorPositions[2][3], BG_DG_DoorPositions[2][4], BG_DG_DoorPositions[2][5], BG_DG_DoorPositions[2][6], BG_DG_DoorPositions[2][7], RESPAWN_IMMEDIATELY)
        || !AddObject(BG_DG_OBJECT_GATE_4, BG_DG_OBJECTID_GATE_H, BG_DG_DoorPositions[3][0], BG_DG_DoorPositions[3][1], BG_DG_DoorPositions[3][2], BG_DG_DoorPositions[3][3], BG_DG_DoorPositions[3][4], BG_DG_DoorPositions[3][5], BG_DG_DoorPositions[3][6], BG_DG_DoorPositions[3][7], RESPAWN_IMMEDIATELY)
        )
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundDG: Failed to spawn door object. Battleground not created!");
        return false;
    }
     
    //buffs

    if (!AddObject(BG_DG_OBJECT_BUFF_NORTH, BG_OBJECTID_BERSERKERBUFF_ENTRY, 96.05209f, 426.0920f, 111.1858f, 4.059507f, 0, 0, 0, 0, BUFF_RESPAWN_TIME)
        || !AddObject(BG_DG_OBJECT_BUFF_SOUTH, BG_OBJECTID_BERSERKERBUFF_ENTRY, -402.7372f, 609.76011f, 113.49582f, 5.458313f, 0, 0, 0, 0, BUFF_RESPAWN_TIME)
        || !AddObject(BG_DG_OBJECT_BUFF_EAST, BG_OBJECTID_SPEEDBUFF_ENTRY, -93.55556f, 375.3403f, 135.5808f, 5.516745f, 0, 0, 0, 0, BUFF_RESPAWN_TIME)
        || !AddObject(BG_DG_OBJECT_BUFF_WEST, BG_OBJECTID_SPEEDBUFF_ENTRY, -239.4358f, 624.6111f, 135.6253f, 2.562323f, 0, 0, 0, 0, BUFF_RESPAWN_TIME))
    //    || !AddObject(BG_DG_OBJECT_BUFF_LAVA_H, BG_OBJECTID_LAVABUFF_ENTRY_H, -124.657f, 727.24411f, 117.5753f, 2.562323f, 0, 0, 0, 0, BUFF_RESPAWN_TIME)
     //   || !AddObject(BG_DG_OBJECT_BUFF_LAVA_A, BG_OBJECTID_LAVABUFF_ENTRY_A, -189.42001f, 275.4541f, 115.82253f, 2.562323f, 0, 0, 0, 0, BUFF_RESPAWN_TIME))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundDG: Failed to spawn buff object. Battleground not created!");
        return false;
    }
   /* for (int i = 0; i < MAX_BUFFS_DG; i++)
    {
        if (!AddObject(BG_DG_OBJECT_BUFF_NORTH + i, Buff_Entries[urand(0, 2)], BG_DG_BuffPositions[i][0], BG_DG_BuffPositions[i][1], BG_DG_BuffPositions[i][2], BG_DG_BuffPositions[i][3], 0, 0, std::sin(BG_DG_BuffPositions[i][3] / 2), std::cos(BG_DG_BuffPositions[i][3] / 2), BUFF_RESPAWN_TIME))
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundDG: Failed to spawn buff object. Battleground not created!");
            return false;
        }
    }*/


    for (uint32 i = BG_DG_SPIRIT_NORTHERN_A; i < DG_MAX_GRAVEYARDS; ++i)
    {
        WorldSafeLocsEntry const* grave = sWorldSafeLocsStore.LookupEntry(BG_DG_GraveYards[i]);

        if (grave)
        {
            uint8 team = i % 2; ///< If 0 team == TEAM_ALLIANCE else TEAM_HORDE
            uint32 creatureType = team == TEAM_ALLIANCE ? BG_DG_SPIRIT_ALLIANCE : BG_DG_SPIRIT_HORDE;
            float orientation = float(team == TEAM_ALLIANCE ? M_PI : 0);

            if (!AddSpiritGuide(creatureType, grave->Loc.X, grave->Loc.Y, grave->Loc.Z, orientation, TeamId(team)))
            {
                TC_LOG_ERROR("misc", "BatteGroundTP: Failed to spawn spirit guide id: %u. Battleground not created!", grave->ID);
                return false;
            }
        }
        else
        {
            TC_LOG_ERROR("misc", "BatteGroundTP: Failed to find grave %u. Battleground not created!", BG_DG_GraveYards[i]);
            return false;
        }
    }

    return true;
}
void BattlegroundDG::Reset()
{
    //call parent's class reset
    Battleground::Reset();

    m_FlagKeepers[TEAM_ALLIANCE].Clear();
    m_FlagKeepers[TEAM_HORDE].Clear();
    m_DroppedFlagGUID[TEAM_ALLIANCE].Clear();
    m_DroppedFlagGUID[TEAM_HORDE].Clear();
    _flagState[TEAM_ALLIANCE] = BG_DG_FLAG_STATE_ON_BASE;
    _flagState[TEAM_HORDE] = BG_DG_FLAG_STATE_ON_BASE;
    m_TeamScores[TEAM_ALLIANCE] = 0;
    m_TeamScores[TEAM_HORDE] = 0;

    if (sBattlegroundMgr->IsBGWeekend(GetTypeID()))
    {
        m_ReputationCapture = 45;
        m_HonorWinKills = 3;
        m_HonorEndKills = 4;
    }
    else
    {
        m_ReputationCapture = 35;
        m_HonorWinKills = 1;
        m_HonorEndKills = 2;
    }
    _minutesElapsed = 0;
    _lastFlagCaptureTeam = 0;
    _bothFlagsKept = false;
    _flagDebuffState = 0;
    _flagSpellForceTimer = 0;
    _flagsDropTimer[TEAM_ALLIANCE] = 0;
    _flagsDropTimer[TEAM_HORDE] = 0;
    _flagsTimer[TEAM_ALLIANCE] = 0;
    _flagsTimer[TEAM_HORDE] = 0;






    m_TeamScores[TEAM_ALLIANCE] = 0;
    m_TeamScores[TEAM_HORDE] = 0;
    m_lastTick[TEAM_ALLIANCE] = 0;
    m_lastTick[TEAM_HORDE] = 0;
    m_HonorScoreTics[TEAM_ALLIANCE] = 0;
    m_HonorScoreTics[TEAM_HORDE] = 0;
    m_IsInformedNearVictory = false;
}

void BattlegroundDG::EndBattleground(uint32 winner)
{
    // Win reward
    if (winner == ALLIANCE)
        RewardHonorToTeam(GetBonusHonorFromKill(m_HonorWinKills), ALLIANCE);
    if (winner == HORDE)
        RewardHonorToTeam(GetBonusHonorFromKill(m_HonorWinKills), HORDE);
    // Complete map_end rewards (even if no team wins)
    RewardHonorToTeam(GetBonusHonorFromKill(m_HonorEndKills), ALLIANCE);
    RewardHonorToTeam(GetBonusHonorFromKill(m_HonorEndKills), HORDE);

    Battleground::EndBattleground(winner);
}

void BattlegroundDG::HandleKillPlayer(Player* player, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    EventPlayerDroppedFlag(player);

    Battleground::HandleKillPlayer(player, killer);
}

void BattlegroundDG::HandleAreaTrigger(Player* player, uint32 triggerId)
{
    switch (triggerId)
    {
    case 9012: // Alliance cart spawn
        if (_flagState[TEAM_HORDE] && !_flagState[TEAM_ALLIANCE])
            if (GetFlagPickerGUID(TEAM_HORDE) == player->GetGUID())
                EventPlayerCapturedFlag(player);
        break;
    case 9013: // Horde cart spawn
        if (_flagState[TEAM_ALLIANCE] && !_flagState[TEAM_HORDE])
            if (GetFlagPickerGUID(TEAM_ALLIANCE) == player->GetGUID())
                EventPlayerCapturedFlag(player);
        break;
    case 9139: // behind the wood on the spawn building alliance on the right
    case 9140: // inside building
  //  case 9159: // buff location
   // case 9160: // buff location
  //  case 9161: // buff location
  //  case 9162: // buff location
    case 9299: // on the roof
    case 9301: // on the roof
    case 9302: // flying => should tp outside the mine when triggered
    case 9303: // flying => should tp outside the mine when triggered
        TC_LOG_DEBUG("bg.battleground", "BattlegroundDG : Handled AreaTrigger(ID : %u) have been activated by Player %s (ID : %u)",
            triggerId, player->GetName().c_str(), player->GetGUID());
        break;
    default:
        Battleground::HandleAreaTrigger(player, triggerId);
        break;
    }
}

void BattlegroundDG::UpdateTeamScore(int team, int32 value)
{
    m_TeamScores[team] += value;
    m_TeamScores[team] = std::min(int32(BG_DG_MAX_TEAM_SCORE), m_TeamScores[team]);
    m_TeamScores[team] = std::max(0, m_TeamScores[team]);

    UpdateWorldState(team == TEAM_ALLIANCE ? BG_DG_OP_RESOURCES_ALLY : BG_DG_OP_RESOURCES_HORDE, m_TeamScores[team]);

    if (m_TeamScores[team] == BG_DG_MAX_TEAM_SCORE)
    {
        if (!GetTeamScore(team == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE))
            EndBattleground(team == TEAM_ALLIANCE ? ALLIANCE : HORDE);
    }

    else if (!m_IsInformedNearVictory && m_TeamScores[team] > BG_DG_OP_RESOURCES_WARNING)
    {
        SendMessageToAll(team == TEAM_ALLIANCE ? BG_DG_TEXT_ALLIANCE_NEAR_VICTORY : BG_DG_TEXT_HORDE_NEAR_VICTORY, CHAT_MSG_BG_SYSTEM_NEUTRAL);
        PlaySoundToAll((team == TEAM_ALLIANCE) ? BG_DG_SOUND_NEAR_VICTORY : BG_DG_SOUND_NEAR_VICTORY);
        m_IsInformedNearVictory = true;
    }
}


WorldSafeLocsEntry const* BattlegroundDG::GetClosestGraveyard(Player* player)
{
    if (!player)
        return NULL;

    uint8 team = player->GetTeamId();

    if (GetStatus() != STATUS_IN_PROGRESS) ///< If battle didn't start yet and player is death (unprobably) revive in flagroom
        return sWorldSafeLocsStore.LookupEntry(BG_DG_GraveYards[BG_DG_SPIRIT_NORTHERN_A + team]);

    /// Check if player if is closer to the enemy base than the center
    WorldSafeLocsEntry const* grave_enemy_base = sWorldSafeLocsStore.LookupEntry(BG_DG_GraveYards[BG_DG_SPIRIT_NORTHERN_A + (team ^ 1)]);
    WorldSafeLocsEntry const* grave_enemy_middle = sWorldSafeLocsStore.LookupEntry(BG_DG_GraveYards[BG_DG_SPIRIT_SOUTHERN_A + (team ^ 1)]);

    if (player->GetDistance2d(grave_enemy_base->Loc.X, grave_enemy_base->Loc.Y) < player->GetDistance2d(grave_enemy_middle->Loc.X, grave_enemy_middle->Loc.Y))
        return sWorldSafeLocsStore.LookupEntry(BG_DG_GraveYards[BG_DG_SPIRIT_NORTHERN_A + team]);
    else
        return sWorldSafeLocsStore.LookupEntry(BG_DG_GraveYards[BG_DG_SPIRIT_SOUTHERN_A + team]);

}



void BattlegroundDG::EventPlayerDroppedFlag(Player* player)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
    {
        // if not running, do not cast things at the dropper player (prevent spawning the "dropped" flag), neither send unnecessary messages
        // just take off the aura
        if (player->GetTeam() == ALLIANCE)
        {
            if (!IsHordeFlagPickedup())
                return;

            if (GetFlagPickerGUID(TEAM_HORDE) == player->GetGUID())
            {
                SetHordeFlagPicker(ObjectGuid::Empty);
                player->RemoveAurasDueToSpell(BG_DG_SPELL_HORDE_FLAG);
            }
        }
        else
        {
            if (!IsAllianceFlagPickedup())
                return;

            if (GetFlagPickerGUID(TEAM_ALLIANCE) == player->GetGUID())
            {
                SetAllianceFlagPicker(ObjectGuid::Empty);
                player->RemoveAurasDueToSpell(BG_DG_SPELL_ALLIANCE_FLAG);
            }
        }
        return;
    }

    bool set = false;

    if (player->GetTeam() == ALLIANCE)
    {
        if (!IsHordeFlagPickedup())
            return;
        if (GetFlagPickerGUID(TEAM_HORDE) == player->GetGUID())
        {
            SetHordeFlagPicker(ObjectGuid::Empty);
            player->RemoveAurasDueToSpell(BG_DG_SPELL_HORDE_FLAG);
            if (_flagDebuffState == 1)
                player->RemoveAurasDueToSpell(DG_SPELL_FOCUSED_ASSAULT);
            else if (_flagDebuffState == 2)
                player->RemoveAurasDueToSpell(DG_SPELL_BRUTAL_ASSAULT);
            _flagState[TEAM_HORDE] = BG_DG_FLAG_STATE_ON_GROUND;
            player->CastSpell(player, BG_DG_SPELL_HORDE_FLAG_DROPPED, true);
            set = true;
        }
    }
    else
    {
        if (!IsAllianceFlagPickedup())
            return;
        if (GetFlagPickerGUID(TEAM_ALLIANCE) == player->GetGUID())
        {
            SetAllianceFlagPicker(ObjectGuid::Empty);
            player->RemoveAurasDueToSpell(BG_DG_SPELL_ALLIANCE_FLAG);
            if (_flagDebuffState == 1)
                player->RemoveAurasDueToSpell(DG_SPELL_FOCUSED_ASSAULT);
            else if (_flagDebuffState == 2)
                player->RemoveAurasDueToSpell(DG_SPELL_BRUTAL_ASSAULT);
            _flagState[TEAM_ALLIANCE] = BG_DG_FLAG_STATE_ON_GROUND;
            player->CastSpell(player, BG_DG_SPELL_ALLIANCE_FLAG_DROPPED, true);
            set = true;
        }
    }

    if (set)
    {
        player->CastSpell(player, SPELL_RECENTLY_DROPPED_FLAG, true);
        UpdateFlagState(player->GetTeam(), 1);

        if (player->GetTeam() == ALLIANCE)
        {
            SendBroadcastText(BG_DG_TEXT_HORDE_FLAG_DROPPED, CHAT_MSG_BG_SYSTEM_HORDE, player);
            UpdateWorldState(BG_DG_FLAG_UNK_HORDE, uint32(-1));
        }
        else
        {
            SendBroadcastText(BG_DG_TEXT_ALLIANCE_FLAG_DROPPED, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
            UpdateWorldState(BG_DG_FLAG_UNK_ALLIANCE, uint32(-1));
        }

        _flagsDropTimer[GetTeamIndexByTeamId(player->GetTeam()) ? 0 : 1] = BG_DG_FLAG_DROP_TIME;
    }
}

void BattlegroundDG::EventPlayerClickedOnFlag(Player* player, GameObject* target_obj)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    //alliance flag picked up from base
    if (player->GetTeam() == HORDE && GetFlagState(ALLIANCE) == BG_DG_FLAG_STATE_ON_BASE
        && BgObjects[BG_DG_OBJECT_A_FLAG] == target_obj->GetGUID())
    {
        SendBroadcastText(BG_DG_TEXT_ALLIANCE_FLAG_PICKED_UP, CHAT_MSG_BG_SYSTEM_HORDE, player);
        PlaySoundToAll(BG_DG_SOUND_ALLIANCE_FLAG_PICKED_UP);
        SpawnBGObject(BG_DG_OBJECT_A_FLAG, RESPAWN_ONE_DAY);
        SetAllianceFlagPicker(player->GetGUID());
        _flagState[TEAM_ALLIANCE] = BG_DG_FLAG_STATE_ON_PLAYER;
        //update world state to show correct flag carrier
        UpdateFlagState(HORDE, BG_DG_FLAG_STATE_ON_PLAYER);
        UpdateWorldState(BG_DG_FLAG_UNK_ALLIANCE, 1);
        player->CastSpell(player, BG_DG_SPELL_ALLIANCE_FLAG, true);
        player->StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_SPELL_TARGET, BG_DG_SPELL_ALLIANCE_FLAG_PICKED);
        if (_flagState[1] == BG_DG_FLAG_STATE_ON_PLAYER)
            _bothFlagsKept = true;
    }

    //horde flag picked up from base
    if (player->GetTeam() == ALLIANCE && GetFlagState(HORDE) == BG_DG_FLAG_STATE_ON_BASE
        && BgObjects[BG_DG_OBJECT_H_FLAG] == target_obj->GetGUID())
    {
        SendBroadcastText(BG_DG_TEXT_HORDE_FLAG_PICKED_UP, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
        PlaySoundToAll(BG_DG_SOUND_HORDE_FLAG_PICKED_UP);
        SpawnBGObject(BG_DG_OBJECT_H_FLAG, RESPAWN_ONE_DAY);
        SetHordeFlagPicker(player->GetGUID());
        _flagState[TEAM_HORDE] = BG_DG_FLAG_STATE_ON_PLAYER;
        //update world state to show correct flag carrier
        UpdateFlagState(ALLIANCE, BG_DG_FLAG_STATE_ON_PLAYER);
        UpdateWorldState(BG_DG_FLAG_UNK_HORDE, 1);
        player->CastSpell(player, BG_DG_SPELL_HORDE_FLAG, true);
        player->StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_SPELL_TARGET, BG_DG_SPELL_HORDE_FLAG_PICKED);
        if (_flagState[0] == BG_DG_FLAG_STATE_ON_PLAYER)
            _bothFlagsKept = true;
    }

    //Alliance flag on ground(not in base) (returned or picked up again from ground!)
    if (GetFlagState(ALLIANCE) == BG_DG_FLAG_STATE_ON_GROUND && player->IsWithinDistInMap(target_obj, 10)
        && target_obj->GetGOInfo()->entry == BG_OBJECT_A_FLAG_GROUND_DG_ENTRY)
    {
        if (player->GetTeam() == ALLIANCE)
        {
            SendBroadcastText(BG_DG_TEXT_ALLIANCE_FLAG_RETURNED, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
            UpdateFlagState(HORDE, BG_DG_FLAG_STATE_WAIT_RESPAWN);
            RespawnFlag(ALLIANCE, false);
            SpawnBGObject(BG_DG_OBJECT_A_FLAG, RESPAWN_IMMEDIATELY);
            PlaySoundToAll(BG_DG_SOUND_FLAG_RETURNED);
            UpdatePlayerScore(player, SCORE_FLAG_RETURNS, 1);
            _bothFlagsKept = false;
        }
        else
        {
            SendBroadcastText(BG_DG_TEXT_ALLIANCE_FLAG_PICKED_UP, CHAT_MSG_BG_SYSTEM_HORDE, player);
            PlaySoundToAll(BG_DG_SOUND_ALLIANCE_FLAG_PICKED_UP);
            SpawnBGObject(BG_DG_OBJECT_A_FLAG, RESPAWN_ONE_DAY);
            SetAllianceFlagPicker(player->GetGUID());
            player->CastSpell(player, BG_DG_SPELL_ALLIANCE_FLAG, true);
            _flagState[TEAM_ALLIANCE] = BG_DG_FLAG_STATE_ON_PLAYER;
            UpdateFlagState(HORDE, BG_DG_FLAG_STATE_ON_PLAYER);
            if (_flagDebuffState == 1)
                player->CastSpell(player, DG_SPELL_FOCUSED_ASSAULT, true);
            else if (_flagDebuffState == 2)
                player->CastSpell(player, DG_SPELL_BRUTAL_ASSAULT, true);
            UpdateWorldState(BG_DG_FLAG_UNK_ALLIANCE, 1);
        }
    }

    //Horde flag on ground(not in base) (returned or picked up again)
    if (GetFlagState(HORDE) == BG_DG_FLAG_STATE_ON_GROUND && player->IsWithinDistInMap(target_obj, 10)
        && target_obj->GetGOInfo()->entry == BG_OBJECT_H_FLAG_GROUND_DG_ENTRY)
    {
        if (player->GetTeam() == HORDE)
        {
            SendBroadcastText(BG_DG_TEXT_HORDE_FLAG_RETURNED, CHAT_MSG_BG_SYSTEM_HORDE, player);
            UpdateFlagState(ALLIANCE, BG_DG_FLAG_STATE_WAIT_RESPAWN);
            RespawnFlag(HORDE, false);
            SpawnBGObject(BG_DG_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
            PlaySoundToAll(BG_DG_SOUND_FLAG_RETURNED);
            UpdatePlayerScore(player, SCORE_FLAG_RETURNS, 1);
            _bothFlagsKept = false;
        }
        else
        {
            SendBroadcastText(BG_DG_TEXT_HORDE_FLAG_PICKED_UP, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
            PlaySoundToAll(BG_DG_SOUND_HORDE_FLAG_PICKED_UP);
            SpawnBGObject(BG_DG_OBJECT_H_FLAG, RESPAWN_ONE_DAY);
            SetHordeFlagPicker(player->GetGUID());
            player->CastSpell(player, BG_DG_SPELL_HORDE_FLAG, true);
            _flagState[TEAM_HORDE] = BG_DG_FLAG_STATE_ON_PLAYER;
            UpdateFlagState(ALLIANCE, BG_DG_FLAG_STATE_ON_PLAYER);
            if (_flagDebuffState == 1)
                player->CastSpell(player, DG_SPELL_FOCUSED_ASSAULT, true);
            else if (_flagDebuffState == 2)
                player->CastSpell(player, DG_SPELL_BRUTAL_ASSAULT, true);
            UpdateWorldState(BG_DG_FLAG_UNK_HORDE, 1);
        }
    }


    uint8 node = BG_DG_NODE_STABLES;
    GameObject* obj = GetBgMap()->GetGameObject(BgObjects[node * 8 + 7]);
    while ((node < BG_DG_DYNAMIC_NODES_COUNT) && ((!obj) || (!player->IsWithinDistInMap(obj, 10))))
    {
        ++node;
        obj = GetBgMap()->GetGameObject(BgObjects[node * 8 + BG_DG_OBJECT_AURA_CONTESTED]);
    }

    if (node == BG_DG_DYNAMIC_NODES_COUNT)
    {
        // this means our player isn't close to any of banners - maybe cheater ??
        return;
    }

    TeamId teamIndex = GetTeamIndexByTeamId(player->GetTeam());

    // Check if player really could use this banner, not cheated
    if (!(m_Nodes[node] == 0 || teamIndex == uint8(m_Nodes[node] % 2)))
        return;

    player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);
    uint32 sound = 0;
    // If node is neutral, change to contested
    if (m_Nodes[node] == BG_DG_NODE_TYPE_NEUTRAL)
    {
        UpdatePlayerScore(player, SCORE_BASES_ASSAULTED, 1);
        m_prevNodes[node] = m_Nodes[node];
        m_Nodes[node] = teamIndex + 1;
        // burn current neutral banner
        _DelBanner(node, BG_DG_NODE_TYPE_NEUTRAL, 0);
        // create new contested banner
        _CreateBanner(node, BG_DG_NODE_TYPE_CONTESTED, teamIndex, true);
        _SendNodeUpdate(node);
        m_NodeTimers[node] = BG_DG_FLAG_CAPTURING_TIME;

        if (teamIndex == TEAM_ALLIANCE)
            SendBroadcastText(DGNodes[node].TextAllianceClaims, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
        else
            SendBroadcastText(DGNodes[node].TextHordeClaims, CHAT_MSG_BG_SYSTEM_HORDE, player);

        sound = BG_DG_SOUND_NODE_CLAIMED;
    }
    // If node is contested
    else if ((m_Nodes[node] == BG_DG_NODE_STATUS_ALLY_CONTESTED) || (m_Nodes[node] == BG_DG_NODE_STATUS_HORDE_CONTESTED))
    {
        // If last state is NOT occupied, change node to enemy-contested
        if (m_prevNodes[node] < BG_DG_NODE_TYPE_OCCUPIED)
        {
            UpdatePlayerScore(player, SCORE_BASES_ASSAULTED, 1);
            m_prevNodes[node] = m_Nodes[node];
            m_Nodes[node] = uint8(teamIndex) + BG_DG_NODE_TYPE_CONTESTED;
            // burn current contested banner
            _DelBanner(node, BG_DG_NODE_TYPE_CONTESTED, !teamIndex);
            // create new contested banner
            _CreateBanner(node, BG_DG_NODE_TYPE_CONTESTED, teamIndex, true);
            _SendNodeUpdate(node);
            m_NodeTimers[node] = BG_DG_FLAG_CAPTURING_TIME;

            if (teamIndex == TEAM_ALLIANCE)
                SendBroadcastText(DGNodes[node].TextAllianceAssaulted, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
            else
                SendBroadcastText(DGNodes[node].TextHordeAssaulted, CHAT_MSG_BG_SYSTEM_HORDE, player);
        }
        // If contested, change back to occupied
        else
        {
            UpdatePlayerScore(player, SCORE_BASES_DEFENDED, 1);
            m_prevNodes[node] = m_Nodes[node];
            m_Nodes[node] = uint8(teamIndex) + BG_DG_NODE_TYPE_OCCUPIED;
            // burn current contested banner
            _DelBanner(node, BG_DG_NODE_TYPE_CONTESTED, !teamIndex);
            // create new occupied banner
            _CreateBanner(node, BG_DG_NODE_TYPE_OCCUPIED, teamIndex, true);
            _SendNodeUpdate(node);
            m_NodeTimers[node] = 0;
            _NodeOccupied(node, (teamIndex == TEAM_ALLIANCE) ? ALLIANCE : HORDE);

            if (teamIndex == TEAM_ALLIANCE)
                SendBroadcastText(DGNodes[node].TextAllianceDefended, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
            else
                SendBroadcastText(DGNodes[node].TextHordeDefended, CHAT_MSG_BG_SYSTEM_HORDE, player);
        }
        sound = (teamIndex == TEAM_ALLIANCE) ? BG_DG_SOUND_NODE_ASSAULTED_ALLIANCE : BG_DG_SOUND_NODE_ASSAULTED_HORDE;
    }
    // If node is occupied, change to enemy-contested
    else
    {
        UpdatePlayerScore(player, SCORE_BASES_ASSAULTED, 1);
        m_prevNodes[node] = m_Nodes[node];
        m_Nodes[node] = uint8(teamIndex) + BG_DG_NODE_TYPE_CONTESTED;
        // burn current occupied banner
        _DelBanner(node, BG_DG_NODE_TYPE_OCCUPIED, !teamIndex);
        // create new contested banner
        _CreateBanner(node, BG_DG_NODE_TYPE_CONTESTED, teamIndex, true);
        _SendNodeUpdate(node);
        _NodeDeOccupied(node);
        m_NodeTimers[node] = BG_DG_FLAG_CAPTURING_TIME;

        if (teamIndex == TEAM_ALLIANCE)
            SendBroadcastText(DGNodes[node].TextAllianceAssaulted, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
        else
            SendBroadcastText(DGNodes[node].TextHordeAssaulted, CHAT_MSG_BG_SYSTEM_HORDE, player);

        sound = (teamIndex == TEAM_ALLIANCE) ? BG_DG_SOUND_NODE_ASSAULTED_ALLIANCE : BG_DG_SOUND_NODE_ASSAULTED_HORDE;
    }

    // If node is occupied again, send "X has taken the Y" msg.
    if (m_Nodes[node] >= BG_DG_NODE_TYPE_OCCUPIED)
    {
        if (teamIndex == TEAM_ALLIANCE)
            SendBroadcastText(DGNodes[node].TextAllianceTaken, CHAT_MSG_BG_SYSTEM_ALLIANCE);
        else
            SendBroadcastText(DGNodes[node].TextHordeTaken, CHAT_MSG_BG_SYSTEM_HORDE);
    }
    PlaySoundToAll(sound);
}


void BattlegroundDG::RemovePlayer(Player* player, ObjectGuid guid, uint32)
{
    // sometimes flag aura not removed :(
    if (IsAllianceFlagPickedup() && m_FlagKeepers[TEAM_ALLIANCE] == guid)
    {
        if (!player)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundDG: Removing offline player who has the FLAG!!");
            SetAllianceFlagPicker(ObjectGuid::Empty);
            RespawnFlag(ALLIANCE, false);
        }
        else
            EventPlayerDroppedFlag(player);
    }
    if (IsHordeFlagPickedup() && m_FlagKeepers[TEAM_HORDE] == guid)
    {
        if (!player)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundDG: Removing offline player who has the FLAG!!");
            SetHordeFlagPicker(ObjectGuid::Empty);
            RespawnFlag(HORDE, false);
        }
        else
            EventPlayerDroppedFlag(player);
    }
}

void BattlegroundDG::RespawnFlag(uint32 Team, bool captured)
{
    if (Team == ALLIANCE)
    {
        TC_LOG_DEBUG("bg.battleground", "Respawn Alliance flag");
        _flagState[TEAM_ALLIANCE] = BG_DG_FLAG_STATE_ON_BASE;
    }
    else
    {
        TC_LOG_DEBUG("bg.battleground", "Respawn Horde flag");
        _flagState[TEAM_HORDE] = BG_DG_FLAG_STATE_ON_BASE;
    }

    if (captured)
    {
        //when map_update will be allowed for battlegrounds this code will be useless
        SpawnBGObject(BG_DG_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
        SpawnBGObject(BG_DG_OBJECT_A_FLAG, RESPAWN_IMMEDIATELY);
        SendBroadcastText(BG_DG_TEXT_FLAG_PLACED_ALLIANCE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
        SendBroadcastText(BG_DG_TEXT_FLAG_PLACED_HORDE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
        PlaySoundToAll(BG_DG_SOUND_FLAGS_RESPAWNED);        // flag respawned sound...
    }
    _bothFlagsKept = false;
}

void BattlegroundDG::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{


    const uint8 plusArray[] = { 0, 2, 3, 0, 1 };

    // Node icons
    for (uint8 node = 0; node < BG_DG_DYNAMIC_NODES_COUNT; ++node)
        packet.Worldstates.emplace_back(BG_DG_OP_NODEICONS[node], (m_Nodes[node] == 0) ? 1 : 0);

    // Node occupied states
    for (uint8 node = 0; node < BG_DG_DYNAMIC_NODES_COUNT; ++node)
        for (uint8 itr = 1; itr < BG_DG_DYNAMIC_NODES_COUNT; ++itr)
            packet.Worldstates.emplace_back(BG_DG_OP_NODESTATES[node] + plusArray[itr], (m_Nodes[node] == itr) ? 1 : 0);

    // How many bases each team owns
    int32 ally = 0, horde = 0;
    for (uint8 node = 0; node < BG_DG_DYNAMIC_NODES_COUNT; ++node)
        if (m_Nodes[node] == BG_DG_NODE_STATUS_ALLY_OCCUPIED)
            ++ally;
        else if (m_Nodes[node] == BG_DG_NODE_STATUS_HORDE_OCCUPIED)
            ++horde;

    packet.Worldstates.emplace_back(BG_DG_OP_OCCUPIED_BASES_ALLY, ally);
    packet.Worldstates.emplace_back(BG_DG_OP_OCCUPIED_BASES_HORDE, horde);

    // Team scores
    packet.Worldstates.emplace_back(BG_DG_OP_RESOURCES_MAX, BG_DG_MAX_TEAM_SCORE);
    packet.Worldstates.emplace_back(BG_DG_OP_RESOURCES_WARNING, BG_DG_WARNING_NEAR_VICTORY_SCORE);
    packet.Worldstates.emplace_back(BG_DG_OP_RESOURCES_ALLY, m_TeamScores[TEAM_ALLIANCE]);
    packet.Worldstates.emplace_back(BG_DG_OP_RESOURCES_HORDE, m_TeamScores[TEAM_HORDE]);

    // other unknown BG_DG_UNK_01
    packet.Worldstates.emplace_back(1861, 2);

    packet.Worldstates.emplace_back(BG_DG_FLAG_CAPTURES_ALLIANCE, GetTeamScore(TEAM_ALLIANCE));
    packet.Worldstates.emplace_back(BG_DG_FLAG_CAPTURES_HORDE, GetTeamScore(TEAM_HORDE));

    if (_flagState[TEAM_ALLIANCE] == BG_DG_FLAG_STATE_ON_GROUND)
        packet.Worldstates.emplace_back(BG_DG_FLAG_UNK_ALLIANCE, uint32(-1)); // ??
    else if (_flagState[TEAM_ALLIANCE] == BG_DG_FLAG_STATE_ON_PLAYER)
        packet.Worldstates.emplace_back(BG_DG_FLAG_UNK_ALLIANCE, 1);
    else
        packet.Worldstates.emplace_back(BG_DG_FLAG_UNK_ALLIANCE, 0);

    if (_flagState[TEAM_HORDE] == BG_DG_FLAG_STATE_ON_GROUND)
        packet.Worldstates.emplace_back(BG_DG_FLAG_UNK_HORDE, uint32(-1)); // ??
    else if (_flagState[TEAM_HORDE] == BG_DG_FLAG_STATE_ON_PLAYER)
        packet.Worldstates.emplace_back(BG_DG_FLAG_UNK_HORDE, 1);
    else
        packet.Worldstates.emplace_back(BG_DG_FLAG_UNK_HORDE, 0);

    packet.Worldstates.emplace_back(BG_DG_FLAG_CAPTURES_MAX, BG_DG_MAX_TEAM_SCORE);

    if (GetStatus() == STATUS_IN_PROGRESS)
    {
        packet.Worldstates.emplace_back(BG_DG_STATE_TIMER_ACTIVE, 1);
        packet.Worldstates.emplace_back(BG_DG_STATE_TIMER, 25 - _minutesElapsed);
    }
    else
        packet.Worldstates.emplace_back(BG_DG_STATE_TIMER_ACTIVE, 0);

    if (_flagState[TEAM_HORDE] == BG_DG_FLAG_STATE_ON_PLAYER)
        packet.Worldstates.emplace_back(BG_DG_FLAG_STATE_HORDE, 2);
    else
        packet.Worldstates.emplace_back(BG_DG_FLAG_STATE_HORDE, 1);

    if (_flagState[TEAM_ALLIANCE] == BG_DG_FLAG_STATE_ON_PLAYER)
        packet.Worldstates.emplace_back(BG_DG_FLAG_STATE_ALLIANCE, 2);
    else
        packet.Worldstates.emplace_back(BG_DG_FLAG_STATE_ALLIANCE, 1);
}
void BattlegroundDG::UpdateFlagState(uint32 team, uint32 value)
{
    switch (value)
    {
        /// Values from sniffs
    case BG_DG_FLAG_STATE_WAIT_RESPAWN:
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_DG_FLAG_UNK_ALLIANCE : BG_DG_FLAG_UNK_HORDE, 0);
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_DG_FLAG_STATE_HORDE : BG_DG_FLAG_STATE_ALLIANCE, 1);
        break;
    case BG_DG_FLAG_STATE_ON_BASE:
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_DG_FLAG_UNK_ALLIANCE : BG_DG_FLAG_UNK_HORDE, 0);
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_DG_FLAG_STATE_HORDE : BG_DG_FLAG_STATE_ALLIANCE, 1);
        break;
    case BG_DG_FLAG_STATE_ON_GROUND:
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_DG_FLAG_UNK_ALLIANCE : BG_DG_FLAG_UNK_HORDE, -1);
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_DG_FLAG_STATE_HORDE : BG_DG_FLAG_STATE_ALLIANCE, BG_DG_FLAG_STATE_ON_GROUND);
        break;
    case BG_DG_FLAG_STATE_ON_PLAYER:
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_DG_FLAG_UNK_ALLIANCE : BG_DG_FLAG_UNK_HORDE, 1);
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_DG_FLAG_STATE_HORDE : BG_DG_FLAG_STATE_ALLIANCE, 2);
        break;
    default:
        break;
    }
}

void BattlegroundDG::RespawnFlagAfterDrop(uint32 team)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    RespawnFlag(team, false);
    if (team == ALLIANCE)
    {
        SpawnBGObject(BG_DG_OBJECT_A_FLAG, RESPAWN_IMMEDIATELY);
        SendBroadcastText(BG_DG_TEXT_FLAG_PLACED_ALLIANCE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
    }
    else
    {
        SpawnBGObject(BG_DG_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
        SendBroadcastText(BG_DG_TEXT_FLAG_PLACED_HORDE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
    }

    PlaySoundToAll(BG_DG_SOUND_FLAGS_RESPAWNED);

    if (GameObject* obj = GetBgMap()->GetGameObject(GetDroppedFlagGUID(team)))
        obj->Delete();
    else
        TC_LOG_ERROR("bg.battleground", "unknown dropped flag (%s)", GetDroppedFlagGUID(team).ToString().c_str());

    SetDroppedFlagGUID(ObjectGuid::Empty, GetTeamIndexByTeamId(team));
    _bothFlagsKept = false;
}

uint32 BattlegroundDG::GetPrematureWinner()
{
    if (GetTeamScore(TEAM_ALLIANCE) > GetTeamScore(TEAM_HORDE))
        return ALLIANCE;
    else if (GetTeamScore(TEAM_HORDE) > GetTeamScore(TEAM_ALLIANCE))
        return HORDE;

    return Battleground::GetPrematureWinner();
}
