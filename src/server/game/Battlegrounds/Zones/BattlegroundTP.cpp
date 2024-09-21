#include "BattlegroundTP.h"
#include "BattlegroundMgr.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"
#include "BattlegroundNode.h"

// these variables aren't used outside of this file, so declare them only here
enum BG_TP_Rewards
{
    BG_TP_WIN = 0,
    BG_TP_FLAG_CAP,
    BG_TP_MAP_COMPLETE,
    BG_TP_REWARD_NUM
};
uint32 BattlegroundTP::GetRandomIronNodePair()
{
    return urand(1, 3);  // Generates a random number between 1 and 3
}

uint32 BG_TP_Honor[BG_HONOR_MODE_NUM][BG_TP_REWARD_NUM] =
{
    {20, 40, 40}, // normal honor
    {60, 40, 80}  // holiday
};

BattlegroundTP::BattlegroundTP()
{
    BgObjects.resize(BG_TP_OBJECT_MAX);
    BgCreatures.resize(BG_CREATURES_MAX_TP);

    StartMessageIds[BG_STARTING_EVENT_SECOND] = BG_TP_TEXT_START_ONE_MINUTE;
    StartMessageIds[BG_STARTING_EVENT_THIRD] = BG_TP_TEXT_START_HALF_MINUTE;
    StartMessageIds[BG_STARTING_EVENT_FOURTH] = BG_TP_TEXT_BATTLE_HAS_BEGUN;

    _flagSpellForceTimer = 0;
    _bothFlagsKept = false;
    _flagDebuffState = 0;
    m_FlagKeepers[TEAM_ALLIANCE].Clear();
    m_FlagKeepers[TEAM_HORDE].Clear();
    m_DroppedFlagGUID[TEAM_ALLIANCE].Clear();
    m_DroppedFlagGUID[TEAM_HORDE].Clear();
    _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_BASE;
    _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_BASE;
    _flagsTimer[TEAM_ALLIANCE] = 0;
    _flagsTimer[TEAM_HORDE] = 0;
    _flagsDropTimer[TEAM_ALLIANCE] = 0;
    _flagsDropTimer[TEAM_HORDE] = 0;
    _lastFlagCaptureTeam = 0;
    m_ReputationCapture = 0;
    m_HonorWinKills = 0;
    m_HonorEndKills = 0;
    _minutesElapsed = 0;
}

void BattlegroundTPScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(2); // Objectives Count
    data << uint32(FlagCaptures);
    data << uint32(FlagReturns);
}


BattlegroundTP::~BattlegroundTP() { }

void BattlegroundTP::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() == STATUS_IN_PROGRESS)
    {
        events.Update(diff);

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
            case TP_EVENT_SPAWN_FLAGS: // Spawn flags in 2,5 seconds after battle started
                SpawnBGObject(BG_TP_OBJECT_A_FLAG, RESPAWN_IMMEDIATELY);
                SpawnBGObject(BG_TP_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
                SendBroadcastText(BG_TP_TEXT_FLAG_PLACED_ALLIANCE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
                SendBroadcastText(BG_TP_TEXT_FLAG_PLACED_HORDE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
                PlaySoundToAll(BG_TP_SOUND_FLAGS_RESPAWNED);
                break;
            case TP_EVENT_CLOSE_BATTLEGROUND: // Close battleground after 27 minutes of its creation
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
            case TP_EVENT_UPDATE_BATTLEGROUND_TIMER:
                _minutesElapsed++;
                UpdateWorldState(BG_TP_STATE_TIMER, 25 - _minutesElapsed);
                events.Repeat(Minutes(1));
                break;
            default:
                break;
            }
        }

        if (_flagState[TEAM_ALLIANCE] == BG_TP_FLAG_STATE_WAIT_RESPAWN)
        {
            _flagsTimer[TEAM_ALLIANCE] -= diff;

            if (_flagsTimer[TEAM_ALLIANCE] < 0)
            {
                _flagsTimer[TEAM_ALLIANCE] = 0;
                RespawnFlag(ALLIANCE, true);
            }
        }

        if (_flagState[TEAM_ALLIANCE] == BG_TP_FLAG_STATE_ON_GROUND)
        {
            _flagsDropTimer[TEAM_ALLIANCE] -= diff;

            if (_flagsDropTimer[TEAM_ALLIANCE] < 0)
            {
                _flagsDropTimer[TEAM_ALLIANCE] = 0;
                RespawnFlagAfterDrop(ALLIANCE);
                _bothFlagsKept = false;
            }
        }

        if (_flagState[TEAM_HORDE] == BG_TP_FLAG_STATE_WAIT_RESPAWN)
        {
            _flagsTimer[TEAM_HORDE] -= diff;

            if (_flagsTimer[TEAM_HORDE] < 0)
            {
                _flagsTimer[TEAM_HORDE] = 0;
                RespawnFlag(HORDE, true);
            }
        }

        if (_flagState[TEAM_HORDE] == BG_TP_FLAG_STATE_ON_GROUND)
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
                    player->CastSpell(player, TP_SPELL_FOCUSED_ASSAULT, true);
                if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[1]))
                    player->CastSpell(player, TP_SPELL_FOCUSED_ASSAULT, true);
                _flagDebuffState = 1;
            }
            else if (_flagDebuffState == 1 && _flagSpellForceTimer >= 15 * MINUTE * IN_MILLISECONDS) //15 minutes
            {
                if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[0]))
                {
                    player->RemoveAurasDueToSpell(TP_SPELL_FOCUSED_ASSAULT);
                    player->CastSpell(player, TP_SPELL_BRUTAL_ASSAULT, true);
                }
                if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[1]))
                {
                    player->RemoveAurasDueToSpell(TP_SPELL_FOCUSED_ASSAULT);
                    player->CastSpell(player, TP_SPELL_BRUTAL_ASSAULT, true);
                }
                _flagDebuffState = 2;
            }
        }
        else
        {
            if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[0]))
            {
                player->RemoveAurasDueToSpell(TP_SPELL_FOCUSED_ASSAULT);
                player->RemoveAurasDueToSpell(TP_SPELL_BRUTAL_ASSAULT);
            }
            if (Player* player = ObjectAccessor::FindPlayer(m_FlagKeepers[1]))
            {
                player->RemoveAurasDueToSpell(TP_SPELL_FOCUSED_ASSAULT);
                player->RemoveAurasDueToSpell(TP_SPELL_BRUTAL_ASSAULT);
            }

            _flagSpellForceTimer = 0; //reset timer.
            _flagDebuffState = 0;
        }
    }
}
void BattlegroundTP::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    Battleground::AddPlayer(player);
    if (!isInBattleground)
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundTPScore(player->GetGUID());
}

void BattlegroundTP::StartingEventCloseDoors()
{
    // Determine which set of iron nodes to spawn
  //  uint32 randomNodeSet = GetRandomIronNodePair();

    for (uint32 i = BG_TP_OBJECT_DOOR_A_1; i <= BG_TP_OBJECT_DOOR_H_5; ++i)
    {
        DoorClose(i);
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);
    }
    for (uint32 i = BG_TP_OBJECT_A_FLAG; i <= BG_TP_OBJECT_BERSERKBUFF_2; ++i)
        SpawnBGObject(i, RESPAWN_ONE_DAY);

  

    UpdateWorldState(BG_TP_STATE_TIMER_ACTIVE, 1);
    UpdateWorldState(BG_TP_STATE_TIMER, 25);
}

void BattlegroundTP::StartingEventOpenDoors()
{
    // Determine which set of iron nodes to spawn
  //  uint32 randomNodeSet = GetRandomIronNodePair();

    for (uint32 i = BG_TP_OBJECT_DOOR_A_1; i <= BG_TP_OBJECT_DOOR_A_4; ++i)
        DoorOpen(i);
    for (uint32 i = BG_TP_OBJECT_DOOR_H_1; i <= BG_TP_OBJECT_DOOR_H_5; ++i)
        DoorOpen(i);

    for (uint32 i = BG_TP_OBJECT_SPEEDBUFF_1; i <= BG_TP_OBJECT_BERSERKBUFF_2; ++i)
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);


    // Scheduling flag spawn event
    events.ScheduleEvent(TP_EVENT_SPAWN_FLAGS, Seconds(2) + Milliseconds(500));

    // Scheduling battleground timer update
    events.ScheduleEvent(TP_EVENT_UPDATE_BATTLEGROUND_TIMER, Minutes(1));

    StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_EVENT, TP_EVENT_START_BATTLE);
}

bool BattlegroundTP::SetupBattleground()
{
    if (!AddObject(BG_TP_OBJECT_A_FLAG, BG_OBJECT_A_FLAG_TP_ENTRY, 2118.210f, 191.621f, 44.052f, 5.741259f, 0, 0, 0.9996573f, 0.02617699f, BG_TP_FLAG_RESPAWN_TIME / 1000)
        || !AddObject(BG_TP_OBJECT_H_FLAG, BG_OBJECT_H_FLAG_TP_ENTRY, 1578.337f, 344.0451f, 2.418409f, 2.792518f, 0, 0, 0.984807f, 0.1736523f, BG_TP_FLAG_RESPAWN_TIME / 1000)
        /// Buffs
        || !AddObject(BG_TP_OBJECT_SPEEDBUFF_1, GB_OBJECT_SPEED_BUFF_1_TP_ENTRY, 1544.55f, 303.852f, 0.692371f, 6.265733f, 0, 0, 0, 0, BUFF_RESPAWN_TIME)
        || !AddObject(BG_TP_OBJECT_SPEEDBUFF_2, BG_OBJECTID_SPEEDBUFF_ENTRY, 2175.87f, 226.622f, 43.7629f, 2.60053f, 0, 0, 0, 0, BUFF_RESPAWN_TIME)
        || !AddObject(BG_TP_OBJECT_REGENBUFF_1, GB_OBJECT_FOOD_BUFF_1_TP_ENTRY, 1754.163f, 242.125f, -14.13157f, 1.151916f, 0, 0, 0, 0, BUFF_RESPAWN_TIME)
        || !AddObject(BG_TP_OBJECT_REGENBUFF_2, BG_OBJECTID_REGENBUFF_ENTRY, 1951.18f, 383.795f, -10.5257f, 4.06662f, 0, 0, 0, 0, BUFF_RESPAWN_TIME)
        || !AddObject(BG_TP_OBJECT_BERSERKBUFF_1, BG_OBJECTID_BERSERKERBUFF_ENTRY, 1932.832f, 226.7917f, -17.05979f, 2.44346f, 0, 0, 0, 0, BUFF_RESPAWN_TIME)
        || !AddObject(BG_TP_OBJECT_BERSERKBUFF_2, GB_OBJECT_BERSERKER_BUFF_1_TP_ENTRY, 1737.569f, 435.845f, -8.0863f, 5.51524f, 0, 0, 0, 0, BUFF_RESPAWN_TIME)
        /// Alliance gates
        || !AddObject(BG_TP_OBJECT_DOOR_A_1, BG_OBJECT_DOOR_A_1_TP_ENTRY, 2118.088f, 154.6754f, 43.57089f, 2.609261f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY)
        || !AddObject(BG_TP_OBJECT_DOOR_A_2, BG_OBJECT_DOOR_A_2_TP_ENTRY, 2156.0f, 219.2059f, 43.6256f, 2.609261f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY)
        || !AddObject(BG_TP_OBJECT_DOOR_A_3, BG_OBJECT_DOOR_A_3_TP_ENTRY, 2135.525f, 218.926f, 43.60946f, 5.750861f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY)
        || !AddObject(BG_TP_OBJECT_DOOR_A_4, BG_OBJECT_DOOR_A_4_TP_ENTRY, 2101.42895f, 163.94834f, 54.23286f, 2.603450861f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY)
        /// Horde gates
        || !AddObject(BG_TP_OBJECT_DOOR_H_1, BG_OBJECT_DOOR_H_1_TP_ENTRY, 1556.656f, 313.8f, 1.589001f, 6.178466f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY)
        || !AddObject(BG_TP_OBJECT_DOOR_H_2, BG_OBJECT_DOOR_H_2_TP_ENTRY, 1576.605f, 321.2421f, 1.58989f, 6.178466f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY)
        || !AddObject(BG_TP_OBJECT_DOOR_H_3, BG_OBJECT_DOOR_H_3_TP_ENTRY, 1558.088f, 372.7654f, 1.723727f, 6.178466f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY)
        || !AddObject(BG_TP_OBJECT_DOOR_H_4, BG_OBJECT_DOOR_H_4_TP_ENTRY, 1558.623f, 379.1595f, -6.409669f, 4.607672f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY)
        || !AddObject(BG_TP_OBJECT_DOOR_H_5, BG_OBJECT_DOOR_H_5_TP_ENTRY, 1580.6604f, 365.887115f, 13.491569f, 6.192655f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY))

    {
        TC_LOG_ERROR("misc", "BattleGroundTP: Failed to spawn some objects. Battleground not created!");
        return false;
    }
    uint32 randomSet = GetRandomIronNodePair();

    // Spawn nodes for the battleground with the determined set
    SpawnNodeTP(randomSet);
    for (uint32 i = TP_GRAVEYARD_START_ALLIANCE; i < TP_MAX_GRAVEYARDS; ++i)
    {
        WorldSafeLocsEntry const* grave = sWorldSafeLocsStore.LookupEntry(BG_TP_GraveyardIds[i]);

        if (grave)
        {
            uint8 team = i % 2; ///< If 0 team == TEAM_ALLIANCE else TEAM_HORDE
            uint32 creatureType = team == TEAM_ALLIANCE ? TP_SPIRIT_ALLIANCE : TP_SPIRIT_HORDE;
            float orientation = float(team == TEAM_ALLIANCE ? M_PI : 0);

            if (!AddSpiritGuide(creatureType, grave->Loc.X, grave->Loc.Y, grave->Loc.Z, orientation, TeamId(team)))
            {
                TC_LOG_ERROR("misc", "BatteGroundTP: Failed to spawn spirit guide id: %u. Battleground not created!", grave->ID);
                return false;
            }
        }
        else
        {
            TC_LOG_ERROR("misc", "BatteGroundTP: Failed to find grave %u. Battleground not created!", BG_TP_GraveyardIds[i]);
            return false;
        }
    }

    events.ScheduleEvent(TP_EVENT_CLOSE_BATTLEGROUND, Minutes(27));

    return true;
}


void BattlegroundTP::SpawnNodeTP(uint32 randomSet)
{
    std::vector<NodeInfo> nodes;
    switch (randomSet)
    {
    case 1:
        nodes = {
            {BG_TP_OBJECT_IRON_NODE_1, BG_OBJECT_IRON_NODE_ENTRY, 1775.4104f, 474.384115f, 4.4331f, 6.192655f, NODE_RESPAWN_7},
            {BG_TP_OBJECT_IRON_NODE_2, BG_OBJECT_IRON_NODE_ENTRY, 1867.4704f, 191.057115f, -23.778569f, 11.192655f,  NODE_RESPAWN_7},
            {BG_TP_OBJECT_FADE_NODE_1, BG_OBJECT_FADE_NODE_ENTRY, 1918.2804f, 383.417115f, -16.708569f, 11.192655f, NODE_RESPAWN_7},
            {BG_TP_OBJECT_THORN_NODE_1, BG_OBJECT_THORN_NODE_ENTRY, 1700.4554f, 304.17115f, -5.368569f, 11.192655f, NODE_RESPAWN_7},
            {BG_TP_OBJECT_GOLD_NODE_1, BG_OBJECT_GOLD_NODE_ENTRY, 1834.4104f, 384.401115f, -22.69969f, 11.192655f, NODE_RESPAWN_10}
        };
        break;

    case 2:
        nodes = {
            {BG_TP_OBJECT_IRON_NODE_1, BG_OBJECT_IRON_NODE_ENTRY, 2001.2604f, 401.684115f, -6.9731f, 2.192655f, NODE_RESPAWN_7},
            {BG_TP_OBJECT_IRON_NODE_2, BG_OBJECT_IRON_NODE_ENTRY, 1726.05470f, 187.3f, -16.078569f, 7.192655f,  NODE_RESPAWN_7},
            {BG_TP_OBJECT_FADE_NODE_1, BG_OBJECT_FADE_NODE_ENTRY, 1760.7104f, 226.132115f, -13.988569f, 1.192655f, NODE_RESPAWN_7},
            {BG_TP_OBJECT_THORN_NODE_1, BG_OBJECT_THORN_NODE_ENTRY, 1965.9504f, 274.40115f, -3.378569f, 9.192655f,  NODE_RESPAWN_7},
            {BG_TP_OBJECT_GOLD_NODE_1, BG_OBJECT_GOLD_NODE_ENTRY, 1912.8784f, 178.339115f, -24.868569f, 11.192655f, NODE_RESPAWN_10}

        };
        break;

    case 3:
        nodes = {
            {BG_TP_OBJECT_IRON_NODE_1, BG_OBJECT_IRON_NODE_ENTRY, 1681.4454f, 303.044115f, -4.3831f, 12.192655f,  NODE_RESPAWN_7},
            {BG_TP_OBJECT_IRON_NODE_2, BG_OBJECT_IRON_NODE_ENTRY, 1979.4404f, 224.186115f, 3.008569f, 1.192655f,  NODE_RESPAWN_7},
            {BG_TP_OBJECT_FADE_NODE_1, BG_OBJECT_FADE_NODE_ENTRY, 1942.0084f, 235.90575f, -14.108569f, 11.192655f,  NODE_RESPAWN_7},
            {BG_TP_OBJECT_THORN_NODE_1, BG_OBJECT_THORN_NODE_ENTRY, 1753.1574f, 342.079115f, -15.96785f, 8.192655f, NODE_RESPAWN_7},
            {BG_TP_OBJECT_GOLD_NODE_1, BG_OBJECT_GOLD_NODE_ENTRY, 1838.1604f, 320.217115f, -35.708569f, 4.192655f, NODE_RESPAWN_10}
        };
        break;
    }

    for (const auto& node : nodes) {
        AddObject(node.objectType, node.entry, node.x, node.y, node.z, node.orientation, 0, 0, 0, 0, node.respawnTime);
    }
}


void BattlegroundTP::UpdateNodeStatus()
{
    time_t currentTime = std::time(nullptr);  // Get current time

    for (auto& node : nodes)
    {
        // Check if the node is active and needs to be checked for respawn
        if (node.isActive)
        {
            // If 7 minutes have passed since last respawn, deactivate the node
            if (difftime(currentTime, node.lastRespawnTime) >= 7 * 60)
            {
                node.isActive = false; // Mark node as not active
                // Handle node deactivation (e.g., remove it from the game world)
            }
        }
        else
        {
            // If the node is not active and respawn time has passed, handle respawn
            if (difftime(currentTime, node.lastRespawnTime) >= node.respawnTime)
            {
                HandleNodeRespawn(node);
            }
        }
    }
}

void BattlegroundTP::HandleNodeRespawn(NodeInfo& node)
{
    node.isActive = true; // Mark node as active
    node.lastRespawnTime = std::time(nullptr); // Update last respawn time
    // Handle node reactivation (e.g., add it back to the game world)
}

void BattlegroundTP::NodeOnUpdate(uint32 diff)
{
    // Call UpdateNodeStatus periodically
    UpdateNodeStatus();
}
void BattlegroundTP::Reset()
{
    //call parent's class reset
    Battleground::Reset();

    m_FlagKeepers[TEAM_ALLIANCE].Clear();
    m_FlagKeepers[TEAM_HORDE].Clear();
    m_DroppedFlagGUID[TEAM_ALLIANCE].Clear();
    m_DroppedFlagGUID[TEAM_HORDE].Clear();
    _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_BASE;
    _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_BASE;
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
}

void BattlegroundTP::EndBattleground(uint32 winner)
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

void BattlegroundTP::HandleKillPlayer(Player* player, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    EventPlayerDroppedFlag(player);

    Battleground::HandleKillPlayer(player, killer);
}

void BattlegroundTP::HandleAreaTrigger(Player* player, uint32 trigger)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    switch (trigger)
    {
    case 5904: // Alliance Flag spawn
        if (_flagState[TEAM_HORDE] && !_flagState[TEAM_ALLIANCE])
            if (GetFlagPickerGUID(TEAM_HORDE) == player->GetGUID())
                EventPlayerCapturedFlag(player);
        break;
    case 5905: // Horde Flag spawn
        if (_flagState[TEAM_ALLIANCE] && !_flagState[TEAM_HORDE])
            if (GetFlagPickerGUID(TEAM_ALLIANCE) == player->GetGUID())
                EventPlayerCapturedFlag(player);
        break;
    case 5908: // Horde Tower
    case 5909: // Twin Peak House big
    case 5910: // Horde House
    case 5911: // Twin Peak House small
    case 5914: // Alliance Start right
    case 5916: // Alliance Start
    case 5917: // Alliance Start left
    case 5918: // Horde Start
    case 5920: // Horde Start Front entrance
    case 5921: // Horde Start left Water channel
        break;
    default:
        Battleground::HandleAreaTrigger(player, trigger);
        break;
    }
}

bool BattlegroundTP::UpdatePlayerScore(Player* player, uint32 type, uint32 value, bool doAddHonor)
{
    if (!Battleground::UpdatePlayerScore(player, type, value, doAddHonor))
        return false;

    /// Update Achievements + scores
    switch (type)
    {
    case SCORE_FLAG_CAPTURES:
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BG_OBJECTIVE_CAPTURE, TP_OBJECTIVE_CAPTURE_FLAG);
        break;
    case SCORE_FLAG_RETURNS:
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BG_OBJECTIVE_CAPTURE, TP_OBJECTIVE_RETURN_FLAG);
        break;
    default:
        break;
    }
    return true;
}

void BattlegroundTP::UpdateTeamScore(uint32 team)
{
    if (team == TEAM_ALLIANCE)
        UpdateWorldState(BG_TP_FLAG_CAPTURES_ALLIANCE, GetTeamScore(team));
    else
        UpdateWorldState(BG_TP_FLAG_CAPTURES_HORDE, GetTeamScore(team));
}
WorldSafeLocsEntry const* BattlegroundTP::GetClosestGraveyard(Player* player)
{
    if (!player)
        return NULL;

    uint8 team = player->GetTeamId();

    if (GetStatus() != STATUS_IN_PROGRESS) ///< If battle didn't start yet and player is death (unprobably) revive in flagroom
        return sWorldSafeLocsStore.LookupEntry(BG_TP_GraveyardIds[TP_GRAVEYARD_FLAGROOM_ALLIANCE + team]);

    /// Check if player if is closer to the enemy base than the center
    WorldSafeLocsEntry const* grave_enemy_base = sWorldSafeLocsStore.LookupEntry(BG_TP_GraveyardIds[TP_GRAVEYARD_FLAGROOM_ALLIANCE + (team ^ 1)]);
    WorldSafeLocsEntry const* grave_enemy_middle = sWorldSafeLocsStore.LookupEntry(BG_TP_GraveyardIds[TP_GRAVEYARD_MIDDLE_ALLIANCE + (team ^ 1)]);

    if (player->GetDistance2d(grave_enemy_base->Loc.X, grave_enemy_base->Loc.Y) < player->GetDistance2d(grave_enemy_middle->Loc.X, grave_enemy_middle->Loc.Y))
        return sWorldSafeLocsStore.LookupEntry(BG_TP_GraveyardIds[TP_GRAVEYARD_MIDDLE_ALLIANCE + team]);
    else
        return sWorldSafeLocsStore.LookupEntry(BG_TP_GraveyardIds[TP_GRAVEYARD_START_ALLIANCE + team]);



}

void BattlegroundTP::EventPlayerDroppedFlag(Player* player)
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
                player->RemoveAurasDueToSpell(BG_TP_SPELL_HORDE_FLAG);
            }
        }
        else
        {
            if (!IsAllianceFlagPickedup())
                return;

            if (GetFlagPickerGUID(TEAM_ALLIANCE) == player->GetGUID())
            {
                SetAllianceFlagPicker(ObjectGuid::Empty);
                player->RemoveAurasDueToSpell(BG_TP_SPELL_ALLIANCE_FLAG);
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
            player->RemoveAurasDueToSpell(BG_TP_SPELL_HORDE_FLAG);
            if (_flagDebuffState == 1)
                player->RemoveAurasDueToSpell(TP_SPELL_FOCUSED_ASSAULT);
            else if (_flagDebuffState == 2)
                player->RemoveAurasDueToSpell(TP_SPELL_BRUTAL_ASSAULT);
            _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_GROUND;
            player->CastSpell(player, BG_TP_SPELL_HORDE_FLAG_DROPPED, true);
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
            player->RemoveAurasDueToSpell(BG_TP_SPELL_ALLIANCE_FLAG);
            if (_flagDebuffState == 1)
                player->RemoveAurasDueToSpell(TP_SPELL_FOCUSED_ASSAULT);
            else if (_flagDebuffState == 2)
                player->RemoveAurasDueToSpell(TP_SPELL_BRUTAL_ASSAULT);
            _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_GROUND;
            player->CastSpell(player, BG_TP_SPELL_ALLIANCE_FLAG_DROPPED, true);
            set = true;
        }
    }

    if (set)
    {
        player->CastSpell(player, SPELL_RECENTLY_DROPPED_FLAG, true);
        UpdateFlagState(player->GetTeam(), 1);

        if (player->GetTeam() == ALLIANCE)
        {
            SendBroadcastText(BG_TP_TEXT_HORDE_FLAG_DROPPED, CHAT_MSG_BG_SYSTEM_HORDE, player);
            UpdateWorldState(BG_TP_FLAG_UNK_HORDE, uint32(-1));
        }
        else
        {
            SendBroadcastText(BG_TP_TEXT_ALLIANCE_FLAG_DROPPED, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
            UpdateWorldState(BG_TP_FLAG_UNK_ALLIANCE, uint32(-1));
        }

        _flagsDropTimer[GetTeamIndexByTeamId(player->GetTeam()) ? 0 : 1] = BG_TP_FLAG_DROP_TIME;
    }
}

void BattlegroundTP::EventPlayerClickedOnFlag(Player* player, GameObject* target_obj)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    //alliance flag picked up from base
    if (player->GetTeam() == HORDE && GetFlagState(ALLIANCE) == BG_TP_FLAG_STATE_ON_BASE
        && BgObjects[BG_TP_OBJECT_A_FLAG] == target_obj->GetGUID())
    {
        SendBroadcastText(BG_TP_TEXT_ALLIANCE_FLAG_PICKED_UP, CHAT_MSG_BG_SYSTEM_HORDE, player);
        PlaySoundToAll(BG_TP_SOUND_ALLIANCE_FLAG_PICKED_UP);
        SpawnBGObject(BG_TP_OBJECT_A_FLAG, RESPAWN_ONE_DAY);
        SetAllianceFlagPicker(player->GetGUID());
        _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_PLAYER;
        //update world state to show correct flag carrier
        UpdateFlagState(HORDE, BG_TP_FLAG_STATE_ON_PLAYER);
        UpdateWorldState(BG_TP_FLAG_UNK_ALLIANCE, 1);
        player->CastSpell(player, BG_TP_SPELL_ALLIANCE_FLAG, true);
        player->StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_SPELL_TARGET, BG_TP_SPELL_ALLIANCE_FLAG_PICKED);
        if (_flagState[1] == BG_TP_FLAG_STATE_ON_PLAYER)
            _bothFlagsKept = true;
    }

    //horde flag picked up from base
    if (player->GetTeam() == ALLIANCE && GetFlagState(HORDE) == BG_TP_FLAG_STATE_ON_BASE
        && BgObjects[BG_TP_OBJECT_H_FLAG] == target_obj->GetGUID())
    {
        SendBroadcastText(BG_TP_TEXT_HORDE_FLAG_PICKED_UP, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
        PlaySoundToAll(BG_TP_SOUND_HORDE_FLAG_PICKED_UP);
        SpawnBGObject(BG_TP_OBJECT_H_FLAG, RESPAWN_ONE_DAY);
        SetHordeFlagPicker(player->GetGUID());
        _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_PLAYER;
        //update world state to show correct flag carrier
        UpdateFlagState(ALLIANCE, BG_TP_FLAG_STATE_ON_PLAYER);
        UpdateWorldState(BG_TP_FLAG_UNK_HORDE, 1);
        player->CastSpell(player, BG_TP_SPELL_HORDE_FLAG, true);
        player->StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_SPELL_TARGET, BG_TP_SPELL_HORDE_FLAG_PICKED);
        if (_flagState[0] == BG_TP_FLAG_STATE_ON_PLAYER)
            _bothFlagsKept = true;
    }

    //Alliance flag on ground(not in base) (returned or picked up again from ground!)
    if (GetFlagState(ALLIANCE) == BG_TP_FLAG_STATE_ON_GROUND && player->IsWithinDistInMap(target_obj, 10)
        && target_obj->GetGOInfo()->entry == BG_OBJECT_A_FLAG_GROUND_TP_ENTRY)
    {
        if (player->GetTeam() == ALLIANCE)
        {
            SendBroadcastText(BG_TP_TEXT_ALLIANCE_FLAG_RETURNED, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
            UpdateFlagState(HORDE, BG_TP_FLAG_STATE_WAIT_RESPAWN);
            RespawnFlag(ALLIANCE, false);
            SpawnBGObject(BG_TP_OBJECT_A_FLAG, RESPAWN_IMMEDIATELY);
            PlaySoundToAll(BG_TP_SOUND_FLAG_RETURNED);
            UpdatePlayerScore(player, SCORE_FLAG_RETURNS, 1);
            _bothFlagsKept = false;
        }
        else
        {
            SendBroadcastText(BG_TP_TEXT_ALLIANCE_FLAG_PICKED_UP, CHAT_MSG_BG_SYSTEM_HORDE, player);
            PlaySoundToAll(BG_TP_SOUND_ALLIANCE_FLAG_PICKED_UP);
            SpawnBGObject(BG_TP_OBJECT_A_FLAG, RESPAWN_ONE_DAY);
            SetAllianceFlagPicker(player->GetGUID());
            player->CastSpell(player, BG_TP_SPELL_ALLIANCE_FLAG, true);
            _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_PLAYER;
            UpdateFlagState(HORDE, BG_TP_FLAG_STATE_ON_PLAYER);
            if (_flagDebuffState == 1)
                player->CastSpell(player, TP_SPELL_FOCUSED_ASSAULT, true);
            else if (_flagDebuffState == 2)
                player->CastSpell(player, TP_SPELL_BRUTAL_ASSAULT, true);
            UpdateWorldState(BG_TP_FLAG_UNK_ALLIANCE, 1);
        }
    }

    //Horde flag on ground(not in base) (returned or picked up again)
    if (GetFlagState(HORDE) == BG_TP_FLAG_STATE_ON_GROUND && player->IsWithinDistInMap(target_obj, 10)
        && target_obj->GetGOInfo()->entry == BG_OBJECT_H_FLAG_GROUND_TP_ENTRY)
    {
        if (player->GetTeam() == HORDE)
        {
            SendBroadcastText(BG_TP_TEXT_HORDE_FLAG_RETURNED, CHAT_MSG_BG_SYSTEM_HORDE, player);
            UpdateFlagState(ALLIANCE, BG_TP_FLAG_STATE_WAIT_RESPAWN);
            RespawnFlag(HORDE, false);
            SpawnBGObject(BG_TP_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
            PlaySoundToAll(BG_TP_SOUND_FLAG_RETURNED);
            UpdatePlayerScore(player, SCORE_FLAG_RETURNS, 1);
            _bothFlagsKept = false;
        }
        else
        {
            SendBroadcastText(BG_TP_TEXT_HORDE_FLAG_PICKED_UP, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
            PlaySoundToAll(BG_TP_SOUND_HORDE_FLAG_PICKED_UP);
            SpawnBGObject(BG_TP_OBJECT_H_FLAG, RESPAWN_ONE_DAY);
            SetHordeFlagPicker(player->GetGUID());
            player->CastSpell(player, BG_TP_SPELL_HORDE_FLAG, true);
            _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_PLAYER;
            UpdateFlagState(ALLIANCE, BG_TP_FLAG_STATE_ON_PLAYER);
            if (_flagDebuffState == 1)
                player->CastSpell(player, TP_SPELL_FOCUSED_ASSAULT, true);
            else if (_flagDebuffState == 2)
                player->CastSpell(player, TP_SPELL_BRUTAL_ASSAULT, true);
            UpdateWorldState(BG_TP_FLAG_UNK_HORDE, 1);
        }
    }


}

void BattlegroundTP::EventPlayerCapturedFlag(Player* player)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    uint32 winner = 0;

    player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);
    if (player->GetTeam() == ALLIANCE)
    {
        if (!IsHordeFlagPickedup())
            return;
        SetHordeFlagPicker(ObjectGuid::Empty);              // must be before aura remove to prevent 2 events (drop+capture) at the same time
        // horde flag in base (but not respawned yet)
        _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_WAIT_RESPAWN;
        // Drop Horde Flag from Player
        player->RemoveAurasDueToSpell(BG_TP_SPELL_HORDE_FLAG);
        if (_flagDebuffState == 1)
            player->RemoveAurasDueToSpell(TP_SPELL_FOCUSED_ASSAULT);
        else if (_flagDebuffState == 2)
            player->RemoveAurasDueToSpell(TP_SPELL_BRUTAL_ASSAULT);

        if (GetTeamScore(TEAM_ALLIANCE) < BG_TP_MAX_TEAM_SCORE)
            AddPoint(ALLIANCE, 1);
        PlaySoundToAll(BG_TP_SOUND_FLAG_CAPTURED_ALLIANCE);
    }
    else
    {
        if (!IsAllianceFlagPickedup())
            return;
        SetAllianceFlagPicker(ObjectGuid::Empty);           // must be before aura remove to prevent 2 events (drop+capture) at the same time
        // alliance flag in base (but not respawned yet)
        _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_WAIT_RESPAWN;
        // Drop Alliance Flag from Player
        player->RemoveAurasDueToSpell(BG_TP_SPELL_ALLIANCE_FLAG);
        if (_flagDebuffState == 1)
            player->RemoveAurasDueToSpell(TP_SPELL_FOCUSED_ASSAULT);
        else if (_flagDebuffState == 2)
            player->RemoveAurasDueToSpell(TP_SPELL_BRUTAL_ASSAULT);

        if (GetTeamScore(TEAM_HORDE) < BG_TP_MAX_TEAM_SCORE)
            AddPoint(HORDE, 1);
        PlaySoundToAll(BG_TP_SOUND_FLAG_CAPTURED_HORDE);
    }
    //for flag capture is reward 2 honorable kills
    RewardHonorToTeam(GetBonusHonorFromKill(2), player->GetTeam());

    SpawnBGObject(BG_TP_OBJECT_H_FLAG, BG_TP_FLAG_RESPAWN_TIME);
    SpawnBGObject(BG_TP_OBJECT_A_FLAG, BG_TP_FLAG_RESPAWN_TIME);

    if (player->GetTeam() == ALLIANCE)
        SendBroadcastText(BG_TP_TEXT_CAPTURED_HORDE_FLAG, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
    else
        SendBroadcastText(BG_TP_TEXT_CAPTURED_ALLIANCE_FLAG, CHAT_MSG_BG_SYSTEM_HORDE, player);

    UpdateFlagState(player->GetTeam(), 1);                  // flag state none
    UpdateTeamScore(player->GetTeamId());
    // only flag capture should be updated
    UpdatePlayerScore(player, SCORE_FLAG_CAPTURES, 1);      // +1 flag captures

    // update last flag capture to be used if teamscore is equal
    SetLastFlagCapture(player->GetTeam());

    if (GetTeamScore(TEAM_ALLIANCE) == BG_TP_MAX_TEAM_SCORE)
        winner = ALLIANCE;

    if (GetTeamScore(TEAM_HORDE) == BG_TP_MAX_TEAM_SCORE)
        winner = HORDE;

    if (winner)
    {
        UpdateWorldState(BG_TP_FLAG_UNK_ALLIANCE, 0);
        UpdateWorldState(BG_TP_FLAG_UNK_HORDE, 0);
        UpdateWorldState(BG_TP_FLAG_STATE_ALLIANCE, 1);
        UpdateWorldState(BG_TP_FLAG_STATE_HORDE, 1);
        UpdateWorldState(BG_TP_STATE_TIMER_ACTIVE, 0);

        RewardHonorToTeam(BG_TP_Honor[m_HonorMode][BG_TP_WIN], winner);
        EndBattleground(winner);

    }
    else
    {
        _flagsTimer[GetTeamIndexByTeamId(player->GetTeam()) ? 0 : 1] = BG_TP_FLAG_RESPAWN_TIME;
    }

}

void BattlegroundTP::RemovePlayer(Player* player, ObjectGuid guid, uint32)
{
    // sometimes flag aura not removed :(
    if (IsAllianceFlagPickedup() && m_FlagKeepers[TEAM_ALLIANCE] == guid)
    {
        if (!player)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundTP: Removing offline player who has the FLAG!!");
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
            TC_LOG_ERROR("bg.battleground", "BattlegroundTP: Removing offline player who has the FLAG!!");
            SetHordeFlagPicker(ObjectGuid::Empty);
            RespawnFlag(HORDE, false);
        }
        else
            EventPlayerDroppedFlag(player);
    }
}

void BattlegroundTP::RespawnFlag(uint32 Team, bool captured)
{
    if (Team == ALLIANCE)
    {
        TC_LOG_DEBUG("bg.battleground", "Respawn Alliance flag");
        _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_BASE;
    }
    else
    {
        TC_LOG_DEBUG("bg.battleground", "Respawn Horde flag");
        _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_BASE;
    }

    if (captured)
    {
        //when map_update will be allowed for battlegrounds this code will be useless
        SpawnBGObject(BG_TP_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
        SpawnBGObject(BG_TP_OBJECT_A_FLAG, RESPAWN_IMMEDIATELY);
        SendBroadcastText(BG_TP_TEXT_FLAG_PLACED_ALLIANCE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
        SendBroadcastText(BG_TP_TEXT_FLAG_PLACED_HORDE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
        PlaySoundToAll(BG_TP_SOUND_FLAGS_RESPAWNED);        // flag respawned sound...
    }
    _bothFlagsKept = false;
}

void BattlegroundTP::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(BG_TP_FLAG_CAPTURES_ALLIANCE, GetTeamScore(TEAM_ALLIANCE));
    packet.Worldstates.emplace_back(BG_TP_FLAG_CAPTURES_HORDE, GetTeamScore(TEAM_HORDE));

    if (_flagState[TEAM_ALLIANCE] == BG_TP_FLAG_STATE_ON_GROUND)
        packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_ALLIANCE, uint32(-1)); // ??
    else if (_flagState[TEAM_ALLIANCE] == BG_TP_FLAG_STATE_ON_PLAYER)
        packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_ALLIANCE, 1);
    else
        packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_ALLIANCE, 0);

    if (_flagState[TEAM_HORDE] == BG_TP_FLAG_STATE_ON_GROUND)
        packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_HORDE, uint32(-1)); // ??
    else if (_flagState[TEAM_HORDE] == BG_TP_FLAG_STATE_ON_PLAYER)
        packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_HORDE, 1);
    else
        packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_HORDE, 0);

    packet.Worldstates.emplace_back(BG_TP_FLAG_CAPTURES_MAX, BG_TP_MAX_TEAM_SCORE);

    if (GetStatus() == STATUS_IN_PROGRESS)
    {
        packet.Worldstates.emplace_back(BG_TP_STATE_TIMER_ACTIVE, 1);
        packet.Worldstates.emplace_back(BG_TP_STATE_TIMER, 25 - _minutesElapsed);
    }
    else
        packet.Worldstates.emplace_back(BG_TP_STATE_TIMER_ACTIVE, 0);

    if (_flagState[TEAM_HORDE] == BG_TP_FLAG_STATE_ON_PLAYER)
        packet.Worldstates.emplace_back(BG_TP_FLAG_STATE_HORDE, 2);
    else
        packet.Worldstates.emplace_back(BG_TP_FLAG_STATE_HORDE, 1);

    if (_flagState[TEAM_ALLIANCE] == BG_TP_FLAG_STATE_ON_PLAYER)
        packet.Worldstates.emplace_back(BG_TP_FLAG_STATE_ALLIANCE, 2);
    else
        packet.Worldstates.emplace_back(BG_TP_FLAG_STATE_ALLIANCE, 1);
}
void BattlegroundTP::UpdateFlagState(uint32 team, uint32 value)
{
    switch (value)
    {
        /// Values from sniffs
    case BG_TP_FLAG_STATE_WAIT_RESPAWN:
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_TP_FLAG_UNK_ALLIANCE : BG_TP_FLAG_UNK_HORDE, 0);
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_TP_FLAG_STATE_HORDE : BG_TP_FLAG_STATE_ALLIANCE, 1);
        break;
    case BG_TP_FLAG_STATE_ON_BASE:
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_TP_FLAG_UNK_ALLIANCE : BG_TP_FLAG_UNK_HORDE, 0);
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_TP_FLAG_STATE_HORDE : BG_TP_FLAG_STATE_ALLIANCE, 1);
        break;
    case BG_TP_FLAG_STATE_ON_GROUND:
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_TP_FLAG_UNK_ALLIANCE : BG_TP_FLAG_UNK_HORDE, -1);
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_TP_FLAG_STATE_HORDE : BG_TP_FLAG_STATE_ALLIANCE, BG_TP_FLAG_STATE_ON_GROUND);
        break;
    case BG_TP_FLAG_STATE_ON_PLAYER:
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_TP_FLAG_UNK_ALLIANCE : BG_TP_FLAG_UNK_HORDE, 1);
        UpdateWorldState(team == TEAM_ALLIANCE ? BG_TP_FLAG_STATE_HORDE : BG_TP_FLAG_STATE_ALLIANCE, 2);
        break;
    default:
        break;
    }
}

void BattlegroundTP::RespawnFlagAfterDrop(uint32 team)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    RespawnFlag(team, false);
    if (team == ALLIANCE)
    {
        SpawnBGObject(BG_TP_OBJECT_A_FLAG, RESPAWN_IMMEDIATELY);
        SendBroadcastText(BG_TP_TEXT_FLAG_PLACED_ALLIANCE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
    }
    else
    {
        SpawnBGObject(BG_TP_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
        SendBroadcastText(BG_TP_TEXT_FLAG_PLACED_HORDE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
    }

    PlaySoundToAll(BG_TP_SOUND_FLAGS_RESPAWNED);

    if (GameObject* obj = GetBgMap()->GetGameObject(GetDroppedFlagGUID(team)))
        obj->Delete();
    else
        TC_LOG_ERROR("bg.battleground", "unknown dropped flag (%s)", GetDroppedFlagGUID(team).ToString().c_str());

    SetDroppedFlagGUID(ObjectGuid::Empty, GetTeamIndexByTeamId(team));
    _bothFlagsKept = false;
}

uint32 BattlegroundTP::GetPrematureWinner()
{
    if (GetTeamScore(TEAM_ALLIANCE) > GetTeamScore(TEAM_HORDE))
        return ALLIANCE;
    else if (GetTeamScore(TEAM_HORDE) > GetTeamScore(TEAM_ALLIANCE))
        return HORDE;

    return Battleground::GetPrematureWinner();
}
