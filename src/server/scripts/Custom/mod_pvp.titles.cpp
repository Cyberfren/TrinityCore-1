#include "Player.h"
#include "DBCStores.h"
#include "Define.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Unit.h"
struct PvPTitles
{
    uint32 RequiredKills;
    uint32 TitleId;
};

struct PvPTitleData
{
    uint32 TitleId[2];
};

enum Ranks
{                             //    A                           H
    RANK_ONE            = 0,  // Private                 &    Scout
    RANK_TWO            = 1,  // Corporal                &    Grunt
    RANK_THREE          = 2,  // Sergeant                &    Sergeant
    RANK_FOUR           = 3,  // Master Sergeant         &    Senior Sergeant
    RANK_FIVE           = 4,  // Sergeant Major          &    First Sergeant
    RANK_SIX            = 5,  // Knight                  &    Stone Guard
    RANK_SEVEN          = 6,  // Knight Lieutenant       &    Blood Guard
    RANK_EIGHT          = 7,  // Knight Captain          &    Legionnaire
    RANK_NINE           = 8,  // Knight Champion         &    Centurion
    RANK_TEN            = 9,  // Lieutenant Commander    &    Champion
    RANK_ELEVEN         = 10, // Commander               &    Lieutenant General
    RANK_TWELVE         = 11, // Marshal                 &    General
    RANK_THIRTEEN       = 12, // Field Marshal           &    Warlord
    RANK_FOURTEEN       = 13  // Grand Marshal           &    High Warlord
};

enum RankReqKillDefault
{
    RANK_ONE_HK_COUNT        = 50,    // Private                 &    Scout
    RANK_TWO_HK_COUNT        = 100,   // Corporal                &    Grunt
    RANK_THREE_HK_COUNT      = 500,   // Sergeant                &    Sergeant
    RANK_FOUR_HK_COUNT       = 1000,  // Master Sergeant         &    Senior Sergeant
    RANK_FIVE_HK_COUNT       = 2000,  // Sergeant Major          &    First Sergeant
    RANK_SIX_HK_COUNT        = 4000,  // Knight                  &    Stone Guard
    RANK_SEVEN_HK_COUNT      = 5000,  // Knight Lieutenant       &    Blood Guard
    RANK_EIGHT_HK_COUNT      = 10000,  // Knight Captain          &    Legionnaire
    RANK_NINE_HK_COUNT       = 30000,  // Knight Champion         &    Centurion
    RANK_TEN_HK_COUNT        = 40000, // Lieutenant Commander    &    Champion
    RANK_ELEVEN_HK_COUNT     = 60000, // Commander               &    Lieutenant General
    RANK_TWELVE_HK_COUNT     = 70000, // Marshal                 &    General
    RANK_THIRTEEN_HK_COUNT   = 80000, // Field Marshal           &    Warlord
    RANK_FOURTEEN_HK_COUNT   = 100000  // Grand Marshal           &    High Warlord
};

enum Titles
{
    // Alliance
    PRIVATE                  = 1,
    CORPORAL                 = 2,
    SERGEANT                 = 3,
    MASTER_SERGEANT          = 4,
    SERGEANT_MAJOR           = 5,
    KNIGHT                   = 6,
    KNIGHT_LIEUTENANT        = 7,
    KNIGHT_CAPTAIN           = 8,
    KNIGHT_CHAMPION          = 9,
    LIEUTENANT_COMMANDER     = 10,
    COMMANDER                = 11,
    MARSHAL                  = 12,
    FIELD_MARSHAL            = 13,
    GRAND_MARSHAL            = 14,
    
    // Horde
    SCOUT                    = 15,
    GRUNT                    = 16,
    SERGEANT_H               = 17,
    SENIOR_SERGEANT          = 18,
    FIRST_SERGEANT           = 19,
    STONE_GUARD              = 20,
    BLOOD_GUARD              = 21,
    LEGIONNAIRE              = 22,
    CENTURION                = 23,
    CHAMPION                 = 24,
    LIEUTENANT_GENERAL       = 25,
    GENERAL                  = 26,
    WARLORD                  = 27,
    HIGH_WARLORD             = 28
};

enum CleanUpTitlesModes
{
    CLEAN_UP_NONE             = 0, // Disabled.
    CLEAN_UP_REMOVE_ALL       = 1, // Remove all titles on login.
    CLEAN_UP_REMOVE_INVALID   = 2  // Remove titles if players no longer meet the requirements on login.
};

PvPTitleData const TitleData[14] =
{
    { PRIVATE,              SCOUT              },
    { CORPORAL,             GRUNT              },
    { SERGEANT,             SERGEANT_H         },
    { MASTER_SERGEANT,      SENIOR_SERGEANT    },
    { SERGEANT_MAJOR,       FIRST_SERGEANT     },
    { KNIGHT,               STONE_GUARD        },
    { KNIGHT_LIEUTENANT,    BLOOD_GUARD        },
    { KNIGHT_CAPTAIN,       LEGIONNAIRE        },
    { KNIGHT_CHAMPION,      CENTURION          },
    { LIEUTENANT_COMMANDER, CHAMPION           },
    { COMMANDER,            LIEUTENANT_GENERAL },
    { MARSHAL,              GENERAL            },
    { FIELD_MARSHAL,        WARLORD            },
    { GRAND_MARSHAL,        HIGH_WARLORD       }
};

class PVPTitles : public PlayerScript
{
public:
    PVPTitles() : PlayerScript("PVPTitles") { }


    void OnPVPKill(Player* killer, Player* killed) override
    {
       
            if (killer->GetGUID() == killed->GetGUID())
                return;

            AwardEarnedTitles(killer);
        
    }

    void AwardEarnedTitles(Player* me)
    {
        TeamId teamId = me->GetTeamId();  // Get the player's faction: 0 for Alliance, 1 for Horde
        uint32 kills = me->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);

        // Define the required HKs (honorable kills) for each rank
        const uint32 requiredKills[14] = {
            RANK_ONE_HK_COUNT,    RANK_TWO_HK_COUNT,    RANK_THREE_HK_COUNT,
            RANK_FOUR_HK_COUNT,   RANK_FIVE_HK_COUNT,   RANK_SIX_HK_COUNT,
            RANK_SEVEN_HK_COUNT,  RANK_EIGHT_HK_COUNT,  RANK_NINE_HK_COUNT,
            RANK_TEN_HK_COUNT,    RANK_ELEVEN_HK_COUNT, RANK_TWELVE_HK_COUNT,
            RANK_THIRTEEN_HK_COUNT, RANK_FOURTEEN_HK_COUNT
        };

        for (uint8 rank = 0; rank < 14; ++rank)
        {
            // Use teamId to select the correct title for Alliance (teamId == 0) or Horde (teamId == 1)
            uint32 titleId = TitleData[rank].TitleId[teamId];

            // Check if the player has earned enough kills for the rank and doesn't already have the title
            if (kills >= requiredKills[rank] && !me->HasTitle(titleId))
            {
                me->SetTitle(sCharTitlesStore.LookupEntry(titleId));  // Assign the title
            }
        }
    }
};

void AddPvpTitlesScripts() 
{
    new PVPTitles();
}
