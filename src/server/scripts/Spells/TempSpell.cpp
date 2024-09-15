// Symbiosis for TrinityCore 3.3.5
// Project Siegecraft, Lost Chapters


#include "ScriptMgr.h"
#include "Creature.h"
#include "Player.h"
#include "Random.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "log.h"
#include "Unit.h"
#include "TempSpell.h"


enum Specializations
{
    SPEC_NONE,
    CLASS_SPEC_WARRIOR_ARMS,
    CLASS_SPEC_WARRIOR_FURY,
    CLASS_SPEC_WARRIOR_PROTECTION,
    CLASS_SPEC_PALADIN_HOLY,
    CLASS_SPEC_PALADIN_PROTECTION,
    CLASS_SPEC_PALADIN_RETRIBUTION,
    CLASS_SPEC_HUNTER_SURVIVAL,
    CLASS_SPEC_HUNTER_MARKSMANSHIP,
    CLASS_SPEC_HUNTER_RANGER,
    CLASS_SPEC_HUNTER_BEAST_MASTERY,
    CLASS_SPEC_ROGUE_ASSASSINATION,
    CLASS_SPEC_ROGUE_COMBAT,
    CLASS_SPEC_ROGUE_SUBLETY,
    CLASS_SPEC_PRIEST_DISCIPLINE,
    CLASS_SPEC_PRIEST_HOLY,
    CLASS_SPEC_PRIEST_SHADOW,
    CLASS_SPEC_NECROMANCER_PAIN,
    CLASS_SPEC_NECROMANCER_PUPPET,
    CLASS_SPEC_NECROMANCER_DESOLATION,
    CLASS_SPEC_SHAMAN_ELEMENTAL,
    CLASS_SPEC_SHAMAN_ENHANCEMENT,
    CLASS_SPEC_SHAMAN_RESTORATION,
    CLASS_SPEC_MAGE_ARCANE,
    CLASS_SPEC_MAGE_FIRE,
    CLASS_SPEC_MAGE_FROST,
    CLASS_SPEC_WARLOCK_AFFLICTION,
    CLASS_SPEC_WARLOCK_SOUL_REAPING,
    CLASS_SPEC_WARLOCK_DESTRUCTION,
    CLASS_SPEC_WARLOCK_DEMONOLOGY,
    CLASS_SPEC_DRUID_BALANCE,
    CLASS_SPEC_DRUID_FERAL,
    CLASS_SPEC_DRUID_RESTORATION,
    CLASS_SPEC_WITCH_SOUL_BINDING,
    CLASS_SPEC_WITCH_PREDATION,
    CLASS_SPEC_WITCH_VOODOO,
    CLASS_SPEC_MONK_FELLOWSHIP,
    CLASS_SPEC_MONK_DEVOTION,
    CLASS_SPEC_MONK_COMBAT,
    CLASS_SPEC_WARDEN_NATURE,
    CLASS_SPEC_WARDEN_GUARDIAN,
    CLASS_SPEC_WARDEN_WATCHER,
    CLASS_SPEC_BARD_UNITY,
    CLASS_SPEC_BARD_SUBTERFUGE,
    CLASS_SPEC_BARD_JESTER,
    CLASS_WARLOCK_NOSPEC,
    CLASS_WARRIOR_NOSPEC,
    CLASS_MAGE_NOSPEC,
    CLASS_HUNTER_NOSPEC,
    CLASS_SHAMAN_NOSPEC,
    CLASS_PRIEST_NOSPEC,
    CLASS_PALADIN_NOSPEC,
    CLASS_ROGUE_NOSPEC,
    CLASS_DRUID_NOSPEC,
    CLASS_BARD_NOSPEC,
    CLASS_WARDEN_NOSPEC,
    CLASS_NECROMANCER_NOSPEC,
    CLASS_WITCH_NOSPEC,
    CLASS_MONK_NOSPEC,
    SPEC_UNKNOWN
};
std::unordered_set<uint32> WarriorArmsSpells = { 64976, 46924, 29623 };  // endless rage, blade storm, juggernaut
std::unordered_set<uint32> WarriorFurySpells = { 60970, 46917, 46911 };  // Titans Grip, Heroic Fury, Furious Attacks Rank 2
std::unordered_set<uint32> WarriorProtectionSpells = { 46968, 50720, 57499 };  //shockwave, vigilance, warbringer
std::unordered_set<uint32> PaladinHolySpells = { 53563, 31842, 31841 };  //beacon of light, beacon of light, Holy Guidance Rank 5
std::unordered_set<uint32> PaladinProtectionSpells = { 31935, 53696, 53585 };  /// avengers shield, judgements of the just, Guarded by the Light Rank 2
std::unordered_set<uint32> PaladinRetributionSpells = { 53385, 80020, 80269 }; //divine storm, templars verdict, Inquisitors Fist
std::unordered_set<uint32> HunterSurvivalSpells = { 19577, 80409, 80117};  // intim, distracting throw, wranglers net
std::unordered_set<uint32> HunterMarksmanshipSpells = { 80178, 52209, 53216 };  //dispatch, chim, wild quiver
std::unordered_set<uint32> HunterRangerSpells = { 80420, 80148, 80162 }; //harpoon, shared armor, wolverine strikes rank 3
//std::unordered_set<uint32> HunterBeastMasterySpells = { 123, 223, 323 };
std::unordered_set<uint32> RogueAssassinationSpells = { 1329, 80217, 51662 };  // mut, marked for death, hunger for blood
std::unordered_set<uint32> RogueCombatSpells = { 51690, 80212, 32601 };//killing spree, shield smack, surprise attack
std::unordered_set<uint32> RogueSubtletySpells = { 58414, 36554, 51713 }; //filthy tricks, shadow step, shadowdance
std::unordered_set<uint32> PriestDisciplineSpells = { 47508, 33206, 47540 }; //aspiration, pain sup, pennance
std::unordered_set<uint32> PriestHolySpells = { 34861, 724, 63717 }; //circle of healing, light well, serendip rank 3
std::unordered_set<uint32> PriestShadowSpells = { 33371, 47585, 81306 }; //mind melt rank 2,  dispersion, void shift, 
//std::unordered_set<uint32> NecromancerPainSpells = { 150, 250, 350 };
//std::unordered_set<uint32> NecromancerPuppetSpells = { 151, 251, 351 };
//std::unordered_set<uint32> NecromancerDesolationSpells = { 152, 252, 352 };
std::unordered_set<uint32> ShamanElementalSpells = { 51470, 30706, 51490 };//elemental oath, tot of wrath, thundesstorm
std::unordered_set<uint32> ShamanEnhancementSpells = { 51513, 60103, 30823 };//feral spirit, stormlash, shamanistic rage
std::unordered_set<uint32> ShamanRestorationSpells = { 30869, 61295, 974 };//natures blessing, riptide, earth shield
std::unordered_set<uint32> MageArcaneSpells = { 31589, 44425, 12042 };  //slow, barrage, arcane power
std::unordered_set<uint32> MageFireSpells = { 44443, 31661, 44457 }; //firestarter, dragons breath, living bomb
std::unordered_set<uint32> MageFrostSpells = { 44545, 44572, 31687 };//fingers of ronst,  deep freeze, summon water ele
std::unordered_set<uint32> WarlockAfflictionSpells = { 30108, 27243, 48181 }; // seed of corr, haunt, unstable afflic
std::unordered_set<uint32> WarlockSoulReapingSpells = { 80260, 80634, 80458 };// painewaver rank 2, void charge, horrific strike
std::unordered_set<uint32> WarlockDestructionSpells = { 29722, 50796, 80577 };//chaos bolt, incin, fel lightning
//std::unordered_set<uint32> WarlockDemonologySpells = { 183, 283, 383 };
std::unordered_set<uint32> DruidBalanceSpells = { 50516, 80399, 48505 };
std::unordered_set<uint32> DruidFeralSpells = { 22570, 52610, 50334 };
std::unordered_set<uint32> DruidRestorationSpells = { 33891, 48539, 80264 };
/*std::unordered_set<uint32> WitchSoulBindingSpells = {200, 300, 400};
std::unordered_set<uint32> WitchPredationSpells = { 201, 301, 401 };
std::unordered_set<uint32> WitchVoodooSpells = { 202, 302, 402 };
std::unordered_set<uint32> MonkFellowshipSpells = { 210, 310, 410 };
std::unordered_set<uint32> MonkDevotionSpells = { 211, 311, 411 };
std::unordered_set<uint32> MonkCombatSpells = { 212, 312, 412 };
std::unordered_set<uint32> WardenNatureSpells = { 220, 320, 420 };
std::unordered_set<uint32> WardenGuardianSpells = { 221, 321, 421 };
std::unordered_set<uint32> WardenWatcherSpells = { 222, 322, 422 };
std::unordered_set<uint32> BardUnitySpells = { 230, 330, 430 };
std::unordered_set<uint32> BardSubterfugeSpells = { 231, 331, 431 };
std::unordered_set<uint32> BardJesterSpells = { 232, 332, 432 };*/

// Function to check player's specialization

uint32 GetSpec(Player* player) {
    switch (player->GetClass()) {
    case CLASS_WARRIOR:
        if (std::any_of(WarriorArmsSpells.begin(), WarriorArmsSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_WARRIOR_ARMS;
        }
        if (std::any_of(WarriorFurySpells.begin(), WarriorFurySpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_WARRIOR_FURY;
        }
        if (std::any_of(WarriorProtectionSpells.begin(), WarriorProtectionSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_WARRIOR_PROTECTION;
        }
        return CLASS_WARRIOR_NOSPEC;

    case CLASS_PALADIN:
        if (std::any_of(PaladinHolySpells.begin(), PaladinHolySpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_PALADIN_HOLY;
        }
        if (std::any_of(PaladinProtectionSpells.begin(), PaladinProtectionSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_PALADIN_PROTECTION;
        }
        if (std::any_of(PaladinRetributionSpells.begin(), PaladinRetributionSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_PALADIN_RETRIBUTION;
        }
        return CLASS_PALADIN_NOSPEC;

    case CLASS_HUNTER:
        if (std::any_of(HunterSurvivalSpells.begin(), HunterSurvivalSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_HUNTER_SURVIVAL;
        }
        if (std::any_of(HunterMarksmanshipSpells.begin(), HunterMarksmanshipSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_HUNTER_MARKSMANSHIP;
        }
        if (std::any_of(HunterRangerSpells.begin(), HunterRangerSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_HUNTER_RANGER;
        }
        // Uncomment if needed:
        // if (std::any_of(HunterBeastMasterySpells.begin(), HunterBeastMasterySpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
        //     return CLASS_SPEC_HUNTER_BEAST_MASTERY;
        // }
        return CLASS_HUNTER_NOSPEC;

    case CLASS_ROGUE:
        if (std::any_of(RogueAssassinationSpells.begin(), RogueAssassinationSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_ROGUE_ASSASSINATION;
        }
        if (std::any_of(RogueCombatSpells.begin(), RogueCombatSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_ROGUE_COMBAT;
        }
        if (std::any_of(RogueSubtletySpells.begin(), RogueSubtletySpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_ROGUE_SUBLETY;
        }
        return CLASS_ROGUE_NOSPEC;

    case CLASS_PRIEST:
        if (std::any_of(PriestDisciplineSpells.begin(), PriestDisciplineSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_PRIEST_DISCIPLINE;
        }
        if (std::any_of(PriestHolySpells.begin(), PriestHolySpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_PRIEST_HOLY;
        }
        if (std::any_of(PriestShadowSpells.begin(), PriestShadowSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_PRIEST_SHADOW;
        }
        return CLASS_PRIEST_NOSPEC;

    case CLASS_SHAMAN:
        if (std::any_of(ShamanElementalSpells.begin(), ShamanElementalSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_SHAMAN_ELEMENTAL;
        }
        if (std::any_of(ShamanEnhancementSpells.begin(), ShamanEnhancementSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_SHAMAN_ENHANCEMENT;
        }
        if (std::any_of(ShamanRestorationSpells.begin(), ShamanRestorationSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_SHAMAN_RESTORATION;
        }
        return CLASS_SHAMAN_NOSPEC;

    case CLASS_MAGE:
        if (std::any_of(MageFireSpells.begin(), MageFireSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_MAGE_FIRE;
        }
        if (std::any_of(MageFrostSpells.begin(), MageFrostSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_MAGE_FROST;
        }
        if (std::any_of(MageArcaneSpells.begin(), MageArcaneSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_MAGE_ARCANE;
        }
        return CLASS_MAGE_NOSPEC;

    case CLASS_WARLOCK:
        if (std::any_of(WarlockAfflictionSpells.begin(), WarlockAfflictionSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_WARLOCK_AFFLICTION;
        }
        if (std::any_of(WarlockSoulReapingSpells.begin(), WarlockSoulReapingSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_WARLOCK_SOUL_REAPING;
        }
        if (std::any_of(WarlockDestructionSpells.begin(), WarlockDestructionSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_WARLOCK_DESTRUCTION;
        }
        // Uncomment if needed:
        // if (std::any_of(WarlockDemonologySpells.begin(), WarlockDemonologySpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
        //     return CLASS_SPEC_WARLOCK_DEMONOLOGY;
        // }
        return CLASS_WARLOCK_NOSPEC;

    case CLASS_DRUID:
        if (std::any_of(DruidBalanceSpells.begin(), DruidBalanceSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_DRUID_BALANCE;
        }
        if (std::any_of(DruidFeralSpells.begin(), DruidFeralSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_DRUID_FERAL;
        }
        if (std::any_of(DruidRestorationSpells.begin(), DruidRestorationSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); })) {
            return CLASS_SPEC_DRUID_RESTORATION;
        }
        return CLASS_DRUID_NOSPEC;

    default:
        return SPEC_UNKNOWN;  // Or any default value indicating an unknown class
    }

    //  if (std::any_of(NecromancerPainSpells.begin(), NecromancerPainSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
     //     return CLASS_SPEC_NECROMANCER_PAIN;
     // if (std::any_of(NecromancerPuppetSpells.begin(), NecromancerPuppetSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
      //    return CLASS_SPEC_NECROMANCER_PUPPET;
     // if (std::any_of(NecromancerDesolationSpells.begin(), NecromancerDesolationSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
       //   return CLASS_SPEC_NECROMANCER_DESOLATION;

    
    // if (std::any_of(WarlockDemonologySpells.begin(), WarlockDemonologySpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
     //    return CLASS_SPEC_WARLOCK_DEMONOLOGY;
   /* if (std::any_of(WitchSoulBindingSpells.begin(), WitchSoulBindingSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_WITCH_SOUL_BINDING;
    if (std::any_of(WitchPredationSpells.begin(), WitchPredationSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_WITCH_PREDATION;
    if (std::any_of(WitchVoodooSpells.begin(), WitchVoodooSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_WITCH_VOODOO;
    if (std::any_of(MonkFellowshipSpells.begin(), MonkFellowshipSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_MONK_FELLOWSHIP;
    if (std::any_of(MonkDevotionSpells.begin(), MonkDevotionSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_MONK_DEVOTION;
    if (std::any_of(MonkCombatSpells.begin(), MonkCombatSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_MONK_COMBAT;
    if (std::any_of(WardenNatureSpells.begin(), WardenNatureSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_WARDEN_NATURE;
    if (std::any_of(WardenGuardianSpells.begin(), WardenGuardianSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_WARDEN_GUARDIAN;
    if (std::any_of(WardenWatcherSpells.begin(), WardenWatcherSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_WARDEN_WATCHER;
    if (std::any_of(BardUnitySpells.begin(), BardUnitySpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_BARD_UNITY;
    if (std::any_of(BardSubterfugeSpells.begin(), BardSubterfugeSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_BARD_SUBTERFUGE;
    if (std::any_of(BardJesterSpells.begin(), BardJesterSpells.end(), [player](uint32 spellId) { return player->HasSpell(spellId); }))
        return CLASS_SPEC_BARD_JESTER;*/

}

class AuraScript_UnlearnSpellOnAuraRemove : public AuraScript
{
    PrepareAuraScript(AuraScript_UnlearnSpellOnAuraRemove);

    void OnRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
        {
            std::vector<uint32> spellToUnlearn;

            // Check the player's class and assign the correct spells to unlearn
            if (player->GetClass() == CLASS_DRUID)
            {
                spellToUnlearn = { 90915, 90917, 90920, 90921, 90924, 90926, 90928, 90930, 90935, 90934, 90931, 90935, 90939,  90941, 90942, 90943, 90944, 90946, 90947, 90949, 90950, 90951, 90952, 90953, 90954, 90955, 90958, 90959, 90960, 90965   }; // Spell IDs for Druids to unlearn
            }
            else
            {
                spellToUnlearn = { 90916, 90918, 90919, 90922, 90923, 90925, 90927, 90929, 90932, 90933, 90936, 90937, 90938, 90940, 90945, 90948, 90956, 90957, 90961, 90962, 90963, 90964, 90966   }; // Spell IDs for other classes to unlearn
            }

            // Loop through and unlearn the appropriate spells
            for (uint32 spellId : spellToUnlearn)
            {
                if (player->HasSpell(spellId))
                {
                    player->RemoveSpell(spellId, false, false);
                    // TC_LOG_DEBUG("spells", "Spell: Player {} has unlearned spell {} due to removal of Symbiosis", player->GetGUID().ToString(), spellId);
                }
            }
        }
    }

    void Register() override
    {
        // Register the handler for removing the aura
        OnEffectRemove += AuraEffectRemoveFn(AuraScript_UnlearnSpellOnAuraRemove::OnRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectRemove += AuraEffectRemoveFn(AuraScript_UnlearnSpellOnAuraRemove::OnRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

class spell_symbiosis : public SpellScript
{
    PrepareSpellScript(spell_symbiosis);


    SpellCastResult CheckCast()
    {
        if (Unit* target = GetExplTargetUnit())
        {
            Player* caster = GetCaster()->ToPlayer();
            if (target->GetClass() == CLASS_DRUID)
                TC_LOG_DEBUG("spells", "Symbiosis cast failed.  Target is a Druid.", caster->GetName().c_str());
                return SPELL_FAILED_BAD_TARGETS;

            if (target->GetLevel() < 58)
                TC_LOG_DEBUG("spells", "Symbiosis cast failed.  Target is low level.", caster->GetName().c_str());
                return SPELL_FAILED_BAD_TARGETS;

            if (target->HasAura(90900))
                TC_LOG_DEBUG("spells", "Symbiosis cast failed.  Target has symbiosis.", caster->GetName().c_str());
                return SPELL_FAILED_BAD_TARGETS;
        }
        else
            return SPELL_FAILED_NO_VALID_TARGETS;

        return SPELL_CAST_OK;
    }



    void HandleOnHit(SpellEffIndex /*effIndex*/)
    {
        Unit* target = GetHitUnit();
        Player* caster = GetCaster()->ToPlayer();

        if (!target || !caster)
            return;

        if (Player* playerTarget = target->ToPlayer())
        {
            uint32 spellToCast = 0;
            uint32 druidspec = GetSpec(caster);
            uint32 playerSpec = GetSpec(playerTarget);

            // Set the spell to cast based on the target's specialization and the caster's (Druid's) specialization


            switch (playerSpec)
            {
            case CLASS_SPEC_WARRIOR_ARMS:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90500; // Feral Druid - Arms Warrior
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90501; // Balance Druid - Arms Warrior
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90502; // Restoration Druid - Arms Warrior
                    break;
                }
                break;
            case CLASS_SPEC_WARRIOR_FURY:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90503; // Feral Druid - Fury Warrior
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90504; // Balance Druid - Fury Warrior
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90505; // Restoration Druid - Fury Warrior
                    break;
                }
                break;
            case CLASS_SPEC_WARRIOR_PROTECTION:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90506; // Feral Druid - Protection Warrior
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90507; // Balance Druid - Protection Warrior
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90508; // Restoration Druid - Protection Warrior
                    break;
                }
                break;
            case CLASS_SPEC_PALADIN_HOLY:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90509; // Feral Druid - Holy Paladin
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90510; // Balance Druid - Holy Paladin
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90511; // Restoration Druid - Holy Paladin
                    break;
                }
                break;
            case CLASS_SPEC_PALADIN_PROTECTION:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90512; // Feral Druid - Protection Paladin
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90513; // Balance Druid - Protection Paladin
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90514; // Restoration Druid - Protection Paladin
                    break;
                }
                break;
            case CLASS_SPEC_PALADIN_RETRIBUTION:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90515; // Feral Druid - Retribution Paladin
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90516; // Balance Druid - Retribution Paladin
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90517; // Restoration Druid - Retribution Paladin
                    break;
                }
                break;
            case CLASS_SPEC_HUNTER_BEAST_MASTERY:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90518; // Feral Druid - Beast Mastery Hunter
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90519; // Balance Druid - Beast Mastery Hunter
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90520; // Restoration Druid - Beast Mastery Hunter
                    break;
                }
                break;
            case CLASS_SPEC_HUNTER_MARKSMANSHIP:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90521; // Feral Druid - Marksmanship Hunter
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90522; // Balance Druid - Marksmanship Hunter
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90523; // Restoration Druid - Marksmanship Hunter
                    break;
                }
                break;
            case CLASS_SPEC_HUNTER_SURVIVAL:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90524; // Feral Druid - Survival Hunter
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90525; // Balance Druid - Survival Hunter
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90526; // Restoration Druid - Survival Hunter
                    break;
                }
                break;
            case CLASS_SPEC_HUNTER_RANGER:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90527; // Feral Druid - Ranger Hunter
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90528; // Balance Druid - Ranger Hunter
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90529; // Restoration Druid - Ranger Hunter
                    break;
                }
                break;
            case CLASS_SPEC_ROGUE_ASSASSINATION:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90530; // Feral Druid - Assassination Rogue
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90531; // Balance Druid - Assassination Rogue
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90532; // Restoration Druid - Assassination Rogue
                    break;
                }
                break;
            case CLASS_SPEC_ROGUE_COMBAT:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90533; // Feral Druid - Combat Rogue
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90534; // Balance Druid - Combat Rogue
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90535; // Restoration Druid - Combat Rogue
                    break;
                }
                break;
            case CLASS_SPEC_ROGUE_SUBLETY:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90536; // Feral Druid - Subtlety Rogue
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90537; // Balance Druid - Subtlety Rogue
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90538; // Restoration Druid - Subtlety Rogue
                    break;
                }
                break;
            case CLASS_SPEC_PRIEST_DISCIPLINE:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90539; // Feral Druid - Discipline Priest
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90540; // Balance Druid - Discipline Priest
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90541; // Restoration Druid - Discipline Priest
                    break;
                }
                break;
            case CLASS_SPEC_PRIEST_HOLY:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90542; // Feral Druid - Holy Priest
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90543; // Balance Druid - Holy Priest
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90544; // Restoration Druid - Holy Priest
                    break;
                }
                break;
            case CLASS_SPEC_PRIEST_SHADOW:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90545; // Feral Druid - Shadow Priest
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90546; // Balance Druid - Shadow Priest
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90547; // Restoration Druid - Shadow Priest
                    break;
                }
                break;
            case CLASS_SPEC_SHAMAN_ELEMENTAL:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90548; // Feral Druid - Elemental Shaman
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90549; // Balance Druid - Elemental Shaman
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90550; // Restoration Druid - Elemental Shaman
                    break;
                }
                break;
            case CLASS_SPEC_SHAMAN_ENHANCEMENT:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90551; // Feral Druid - Enhancement Shaman
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90552; // Balance Druid - Enhancement Shaman
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90553; // Restoration Druid - Enhancement Shaman
                    break;
                }
                break;
            case CLASS_SPEC_SHAMAN_RESTORATION:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90554; // Feral Druid - Restoration Shaman
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90555; // Balance Druid - Restoration Shaman
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90556; // Restoration Druid - Restoration Shaman
                    break;
                }
                break;
            case CLASS_SPEC_MAGE_ARCANE:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90557; // Feral Druid - Arcane Mage
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90558; // Balance Druid - Arcane Mage
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90559; // Restoration Druid - Arcane Mage
                    break;
                }
                break;
            case CLASS_SPEC_MAGE_FIRE:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90560; // Feral Druid - Fire Mage
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90561; // Balance Druid - Fire Mage
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90562; // Restoration Druid - Fire Mage
                    break;
                }
                break;
            case CLASS_SPEC_MAGE_FROST:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90563; // Feral Druid - Frost Mage
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90564; // Balance Druid - Frost Mage
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90565; // Restoration Druid - Frost Mage
                    break;
                }
                break;
            case CLASS_SPEC_WARLOCK_AFFLICTION:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90566; // Feral Druid - Affliction Warlock
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90567; // Balance Druid - Affliction Warlock
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90568; // Restoration Druid - Affliction Warlock
                    break;
                }
                break;
            case CLASS_SPEC_WARLOCK_SOUL_REAPING:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90566; // Feral Druid - Affliction Warlock
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90567; // Balance Druid - Affliction Warlock
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90568; // Restoration Druid - Affliction Warlock
                    break;
                }
                break;
            case CLASS_SPEC_WARLOCK_DEMONOLOGY:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90569; // Feral Druid - Demonology Warlock
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90570; // Balance Druid - Demonology Warlock
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90571; // Restoration Druid - Demonology Warlock
                    break;
                }
                break;
            case CLASS_SPEC_WARLOCK_DESTRUCTION:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90572; // Feral Druid - Destruction Warlock
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90573; // Balance Druid - Destruction Warlock
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90574; // Restoration Druid - Destruction Warlock
                    break;
                }
                break;
         
            case CLASS_SPEC_NECROMANCER_PUPPET:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90575; // Feral Druid - Puppet Necromancer
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90576; // Balance Druid - Puppet Necromancer
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90577; // Restoration Druid - Puppet Necromancer
                    break;
                }
                break;
            case CLASS_SPEC_NECROMANCER_DESOLATION:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90578; // Feral Druid - Desolation Necromancer
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90579; // Balance Druid - Desolation Necromancer
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90580; // Restoration Druid - Desolation Necromancer
                    break;
                }
                break;
            case CLASS_SPEC_NECROMANCER_PAIN:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90581; // Feral Druid - Pain Necromancer
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90582; // Balance Druid - Pain Necromancer
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90583; // Restoration Druid - Pain Necromancer
                    break;
                }
                break;
            case CLASS_SPEC_MONK_FELLOWSHIP:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90584; // Feral Druid - Fellowship Monk
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90585; // Balance Druid - Fellowship Monk
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90586; // Restoration Druid - Fellowship Monk
                    break;
                }
                break;
            case CLASS_SPEC_MONK_DEVOTION:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90587; // Feral Druid - Devotion Monk
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90588; // Balance Druid - Devotion Monk
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90589; // Restoration Druid - Devotion Monk
                    break;
                }
                break;
            case CLASS_SPEC_MONK_COMBAT:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90590; // Feral Druid - Combat Monk
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90591; // Balance Druid - Combat Monk
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90592; // Restoration Druid - Combat Monk
                    break;
                }
                break;
            case CLASS_SPEC_WITCH_SOUL_BINDING:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90593; // Feral Druid - Soul Binding Witch
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90594; // Balance Druid - Soul Binding Witch
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90595; // Restoration Druid - Soul Binding Witch
                    break;
                }
                break;
            case CLASS_SPEC_WITCH_PREDATION:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90596; // Feral Druid - Predation Witch
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90597; // Balance Druid - Predation Witch
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90598; // Restoration Druid - Predation Witch
                    break;
                }
                break;
            case CLASS_SPEC_WITCH_VOODOO:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90599; // Feral Druid - Voodoo Witch
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90600; // Balance Druid - Voodoo Witch
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90601; // Restoration Druid - Voodoo Witch
                    break;
                }
                break;
            case CLASS_SPEC_WARDEN_NATURE:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90602; // Feral Druid - Nature Warden
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90603; // Balance Druid - Nature Warden
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90604; // Restoration Druid - Nature Warden
                    break;
                }
                break;
            case CLASS_SPEC_WARDEN_GUARDIAN:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90605; // Feral Druid - Guardian Warden
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90606; // Balance Druid - Guardian Warden
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90607; // Restoration Druid - Guardian Warden
                    break;
                }
                break;
            case CLASS_SPEC_WARDEN_WATCHER:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90608; // Feral Druid - Watcher Warden
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90609; // Balance Druid - Watcher Warden
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90610; // Restoration Druid - Watcher Warden
                    break;
                }
                break;
            case CLASS_SPEC_BARD_UNITY:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90611; // Feral Druid - Unity Bard
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90612; // Balance Druid - Unity Bard
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90613; // Restoration Druid - Unity Bard
                    break;
                }
                break;
            case CLASS_SPEC_BARD_SUBTERFUGE:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90614; // Feral Druid - Subterfuge Bard
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90615; // Balance Druid - Subterfuge Bard
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90616; // Restoration Druid - Subterfuge Bard
                    break;
                }
                break;
            case CLASS_SPEC_BARD_JESTER:
                switch (druidspec)
                {
                case CLASS_SPEC_DRUID_FERAL:
                    spellToCast = 90617; // Feral Druid - Jester Bard
                    break;
                case CLASS_SPEC_DRUID_BALANCE:
                    spellToCast = 90618; // Balance Druid - Jester Bard
                    break;
                case CLASS_SPEC_DRUID_RESTORATION:
                    spellToCast = 90619; // Restoration Druid - Jester Bard
                    break;
                }
                break;
            case SPEC_NONE:
                switch (playerTarget->GetClass())
                {
                case CLASS_WARRIOR:
                    spellToCast = 90912; // Replace with the actual spell ID for Warriors
                    break;
                case CLASS_PALADIN:
                    spellToCast = 90913; // Replace with the actual spell ID for Paladins
                    break;
                case CLASS_HUNTER:
                    spellToCast = 90910; // Replace with the actual spell ID for Hunters
                    break;
                case CLASS_ROGUE:
                    spellToCast = 90905; // Replace with the actual spell ID for Rogues
                    break;
                case CLASS_PRIEST:
                    spellToCast = 90901; // Replace with the actual spell ID for Priests
                    break;
                case CLASS_SHAMAN:
                    spellToCast = 90909; // Replace with the actual spell ID for Shamans
                    break;
                case CLASS_MAGE:
                    spellToCast = 90903; // Replace with the actual spell ID for Mages
                    break;
                case CLASS_WARLOCK:
                    spellToCast = 90902; // Replace with the actual spell ID for Warlocks
                    break;
                case CLASS_NECROMANCER:
                    spellToCast = 90904; // Replace with the actual spell ID for Druids
                    break;
                case CLASS_WARDEN:
                    spellToCast = 90911; // Replace with the actual spell ID for Druids
                    break;
                case CLASS_BARD:
                    spellToCast = 90906; // Replace with the actual spell ID for Druids
                    break;
                case CLASS_MONK:
                    spellToCast = 90907; // Replace with the actual spell ID for Druids
                    break;
                case CLASS_WITCH:
                    spellToCast = 90908; // Replace with the actual spell ID for Druids
                    break;
                }
                break;
            default:
                break;
          
            }   

        // If a spell is selected, cast it on the target
            if (spellToCast)
            {
            caster->CastSpell(playerTarget, spellToCast, true);
            // TC_LOG_DEBUG("spells", "Symbiosis: Cast spell %u on player %s", spellToCast, playerTarget->GetName().c_str());
            }
        }
    }
    
    void Register() override
    {
        // Register the custom spell handler
        OnCheckCast += SpellCheckCastFn(spell_symbiosis::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_symbiosis::HandleOnHit, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};
void AddSC_custom_aura_scripts()
{
    // Register the AuraScript with spell 90900 (the aura)
    RegisterSpellScript(AuraScript_UnlearnSpellOnAuraRemove);
    RegisterSpellScript(spell_symbiosis);
}



//old code archive
/*/*  void HandleOnHit(SpellEffIndex )
    {
        Unit* target = GetHitUnit();
        Player* caster = GetCaster()->ToPlayer();

        if (!target || !caster)
            return;

        if (Player* playerTarget = target->ToPlayer())
        {

        
            uint32 spellToCast = 0;

            // Check target's class and set the spell to cast
            switch (playerTarget->GetClass())
            {
            case CLASS_WARRIOR:
                spellToCast = 90912; // Replace with the actual spell ID for Warriors
                break;
            case CLASS_PALADIN:
                spellToCast = 90913; // Replace with the actual spell ID for Paladins
                break;
            case CLASS_HUNTER:
                spellToCast = 90910; // Replace with the actual spell ID for Hunters
                break;
            case CLASS_ROGUE:
                spellToCast = 90905; // Replace with the actual spell ID for Rogues
                break;
            case CLASS_PRIEST:
                spellToCast = 90901; // Replace with the actual spell ID for Priests
                break;
            case CLASS_SHAMAN:
                spellToCast = 90909; // Replace with the actual spell ID for Shamans
                break;
            case CLASS_MAGE:
                spellToCast = 90903; // Replace with the actual spell ID for Mages
                break;
            case CLASS_WARLOCK:
                spellToCast = 90902; // Replace with the actual spell ID for Warlocks
                break;
            case CLASS_NECROMANCER:
                spellToCast = 90904; // Replace with the actual spell ID for Druids
                break;
            case CLASS_WARDEN:
                spellToCast = 90911; // Replace with the actual spell ID for Druids
                break;
            case CLASS_BARD:
                spellToCast = 90906; // Replace with the actual spell ID for Druids
                break;
            case CLASS_MONK:
                spellToCast = 90907; // Replace with the actual spell ID for Druids
                break;
            case CLASS_WITCH:
                spellToCast = 90908; // Replace with the actual spell ID for Druids
                break;
            default:
                break;
            }

            // If a spell is selected, cast it on the target
            if (spellToCast)
            {
                caster->CastSpell(playerTarget, spellToCast, true);
               // TC_LOG_DEBUG("spells", "Symbiosis: Cast spell %u on player %s", spellToCast, playerTarget->GetName().c_str());
            }
        }
    }*/
