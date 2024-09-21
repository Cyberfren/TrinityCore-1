/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
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

#include "SpellMgr.h"
#include "BattlefieldMgr.h"
#include "BattlegroundMgr.h"
#include "Chat.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"

bool IsPrimaryProfessionSkill(uint32 skill)
{
    SkillLineEntry const* pSkill = sSkillLineStore.LookupEntry(skill);
    if (!pSkill)
        return false;

    if (pSkill->CategoryID != SKILL_CATEGORY_PROFESSION)
        return false;

    return true;
}

bool IsPartOfSkillLine(uint32 skillId, uint32 spellId)
{
    SkillLineAbilityMapBounds skillBounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (SkillLineAbilityMap::const_iterator itr = skillBounds.first; itr != skillBounds.second; ++itr)
        if (itr->second->SkillLine == skillId)
            return true;

    return false;
}

SpellMgr::SpellMgr() { }

SpellMgr::~SpellMgr()
{
    UnloadSpellInfoStore();
}

SpellMgr* SpellMgr::instance()
{
    static SpellMgr instance;
    return &instance;
}

/// Some checks for spells, to prevent adding deprecated/broken spells for trainers, spell book, etc
bool SpellMgr::IsSpellValid(SpellInfo const* spellInfo, Player* player, bool msg)
{
    // not exist
    if (!spellInfo)
        return false;

    bool needCheckReagents = false;

    // check effects
    for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
    {
        switch (spellEffectInfo.Effect)
        {
            // craft spell for crafting non-existed item (break client recipes list show)
            case SPELL_EFFECT_CREATE_ITEM:
            case SPELL_EFFECT_CREATE_ITEM_2:
            {
                if (spellEffectInfo.ItemType == 0)
                {
                    // skip auto-loot crafting spells, it does not need explicit item info (but has special fake items sometimes).
                    if (!spellInfo->IsLootCrafting())
                    {
                        if (msg)
                        {
                            if (player)
                                ChatHandler(player->GetSession()).PSendSysMessage("The craft spell %u does not have a create item entry.", spellInfo->Id);
                            else
                                TC_LOG_ERROR("sql.sql", "The craft spell {} does not have a create item entry.", spellInfo->Id);
                        }
                        return false;
                    }

                }
                // also possible IsLootCrafting case but fake items must exist anyway
                else if (!sObjectMgr->GetItemTemplate(spellEffectInfo.ItemType))
                {
                    if (msg)
                    {
                        if (player)
                            ChatHandler(player->GetSession()).PSendSysMessage("Craft spell %u has created a non-existing item in DB (Entry: %u) and then...", spellInfo->Id, spellEffectInfo.ItemType);
                        else
                            TC_LOG_ERROR("sql.sql", "Craft spell {} has created a non-existing item in DB (Entry: {}) and then...", spellInfo->Id, spellEffectInfo.ItemType);
                    }
                    return false;
                }

                needCheckReagents = true;
                break;
            }
            case SPELL_EFFECT_LEARN_SPELL:
            {
                SpellInfo const* spellInfo2 = sSpellMgr->GetSpellInfo(spellEffectInfo.TriggerSpell);
                if (!IsSpellValid(spellInfo2, player, msg))
                {
                    if (msg)
                    {
                        if (player)
                            ChatHandler(player->GetSession()).PSendSysMessage("Spell %u learn to broken spell %u, and then...", spellInfo->Id, spellEffectInfo.TriggerSpell);
                        else
                            TC_LOG_ERROR("sql.sql", "Spell {} learn to invalid spell {}, and then...", spellInfo->Id, spellEffectInfo.TriggerSpell);
                    }
                    return false;
                }
                break;
            }
            default:
                break;
        }
    }

    if (needCheckReagents)
    {
        for (uint8 j = 0; j < MAX_SPELL_REAGENTS; ++j)
        {
            if (spellInfo->Reagent[j] > 0 && !sObjectMgr->GetItemTemplate(spellInfo->Reagent[j]))
            {
                if (msg)
                {
                    if (player)
                        ChatHandler(player->GetSession()).PSendSysMessage("Craft spell %u refers a non-existing reagent in DB item (Entry: %u) and then...", spellInfo->Id, spellInfo->Reagent[j]);
                    else
                        TC_LOG_ERROR("sql.sql", "Craft spell {} refers to a non-existing reagent in DB, item (Entry: {}) and then...", spellInfo->Id, spellInfo->Reagent[j]);
                }
                return false;
            }
        }
    }

    return true;
}

uint32 SpellMgr::GetSpellDifficultyId(uint32 spellId) const
{
    SpellDifficultySearcherMap::const_iterator i = mSpellDifficultySearcherMap.find(spellId);
    return i == mSpellDifficultySearcherMap.end() ? 0 : i->second;
}

void SpellMgr::SetSpellDifficultyId(uint32 spellId, uint32 id)
{
    if (uint32 i = GetSpellDifficultyId(spellId))
        TC_LOG_ERROR("spells", "SpellMgr::SetSpellDifficultyId: The spell {} already has spellDifficultyId {}. Will override with spellDifficultyId {}.", spellId, i, id);
    mSpellDifficultySearcherMap[spellId] = id;
}

uint32 SpellMgr::GetSpellIdForDifficulty(uint32 spellId, WorldObject const* caster) const
{
    if (!GetSpellInfo(spellId))
        return spellId;

    if (!caster || !caster->GetMap() || (!caster->GetMap()->IsDungeon() && !caster->GetMap()->IsBattleground()))
        return spellId;

    uint32 mode = uint32(caster->GetMap()->GetSpawnMode());
    if (mode >= MAX_DIFFICULTY)
    {
        TC_LOG_ERROR("spells", "SpellMgr::GetSpellIdForDifficulty: Incorrect difficulty for spell {}.", spellId);
        return spellId; //return source spell
    }

    uint32 difficultyId = GetSpellDifficultyId(spellId);
    if (!difficultyId)
        return spellId; //return source spell, it has only REGULAR_DIFFICULTY

    SpellDifficultyEntry const* difficultyEntry = sSpellDifficultyStore.LookupEntry(difficultyId);
    if (!difficultyEntry)
    {
        TC_LOG_ERROR("spells", "SpellMgr::GetSpellIdForDifficulty: SpellDifficultyEntry was not found for spell {}. This should never happen.", spellId);
        return spellId; //return source spell
    }

    if (difficultyEntry->DifficultySpellID[mode] <= 0 && mode > DUNGEON_DIFFICULTY_HEROIC)
    {
        TC_LOG_DEBUG("spells", "SpellMgr::GetSpellIdForDifficulty: spell {} mode {} spell is NULL, using mode {}", spellId, mode, mode - 2);
        mode -= 2;
    }

    if (difficultyEntry->DifficultySpellID[mode] <= 0)
    {
        TC_LOG_ERROR("sql.sql", "SpellMgr::GetSpellIdForDifficulty: spell {} mode {} spell is 0. Check spelldifficulty_dbc!", spellId, mode);
        return spellId;
    }

    TC_LOG_DEBUG("spells", "SpellMgr::GetSpellIdForDifficulty: spellid for spell {} in mode {} is {}", spellId, mode, difficultyEntry->DifficultySpellID[mode]);
    return uint32(difficultyEntry->DifficultySpellID[mode]);
}

SpellInfo const* SpellMgr::GetSpellForDifficultyFromSpell(SpellInfo const* spell, WorldObject const* caster) const
{
    if (!spell)
        return nullptr;

    uint32 newSpellId = GetSpellIdForDifficulty(spell->Id, caster);
    SpellInfo const* newSpell = GetSpellInfo(newSpellId);
    if (!newSpell)
    {
        TC_LOG_DEBUG("spells", "SpellMgr::GetSpellForDifficultyFromSpell: spell {} not found. Check spelldifficulty_dbc!", newSpellId);
        return spell;
    }

    TC_LOG_DEBUG("spells", "SpellMgr::GetSpellForDifficultyFromSpell: Spell id for instance mode is {} (original {})", newSpell->Id, spell->Id);
    return newSpell;
}

SpellChainNode const* SpellMgr::GetSpellChainNode(uint32 spell_id) const
{
    SpellChainMap::const_iterator itr = mSpellChains.find(spell_id);
    if (itr == mSpellChains.end())
        return nullptr;

    return &itr->second;
}

uint32 SpellMgr::GetFirstSpellInChain(uint32 spell_id) const
{
    if (SpellChainNode const* node = GetSpellChainNode(spell_id))
        return node->first->Id;

    return spell_id;
}

uint32 SpellMgr::GetLastSpellInChain(uint32 spell_id) const
{
    if (SpellChainNode const* node = GetSpellChainNode(spell_id))
        return node->last->Id;

    return spell_id;
}

uint32 SpellMgr::GetNextSpellInChain(uint32 spell_id) const
{
    if (SpellChainNode const* node = GetSpellChainNode(spell_id))
        if (node->next)
            return node->next->Id;

    return 0;
}

uint32 SpellMgr::GetPrevSpellInChain(uint32 spell_id) const
{
    if (SpellChainNode const* node = GetSpellChainNode(spell_id))
        if (node->prev)
            return node->prev->Id;

    return 0;
}

uint8 SpellMgr::GetSpellRank(uint32 spell_id) const
{
    if (SpellChainNode const* node = GetSpellChainNode(spell_id))
        return node->rank;

    return 0;
}

uint32 SpellMgr::GetSpellWithRank(uint32 spell_id, uint32 rank, bool strict) const
{
    if (SpellChainNode const* node = GetSpellChainNode(spell_id))
    {
        if (rank != node->rank)
            return GetSpellWithRank(node->rank < rank ? node->next->Id : node->prev->Id, rank, strict);
    }
    else if (strict && rank > 1)
        return 0;
    return spell_id;
}

Trinity::IteratorPair<SpellRequiredMap::const_iterator> SpellMgr::GetSpellsRequiredForSpellBounds(uint32 spell_id) const
{
    return Trinity::Containers::MapEqualRange(mSpellReq, spell_id);
}

SpellsRequiringSpellMapBounds SpellMgr::GetSpellsRequiringSpellBounds(uint32 spell_id) const
{
    return mSpellsReqSpell.equal_range(spell_id);
}

bool SpellMgr::IsSpellRequiringSpell(uint32 spellid, uint32 req_spellid) const
{
    SpellsRequiringSpellMapBounds spellsRequiringSpell = GetSpellsRequiringSpellBounds(req_spellid);
    for (SpellsRequiringSpellMap::const_iterator itr = spellsRequiringSpell.first; itr != spellsRequiringSpell.second; ++itr)
    {
        if (itr->second == spellid)
            return true;
    }
    return false;
}

SpellLearnSkillNode const* SpellMgr::GetSpellLearnSkill(uint32 spell_id) const
{
    SpellLearnSkillMap::const_iterator itr = mSpellLearnSkills.find(spell_id);
    if (itr != mSpellLearnSkills.end())
        return &itr->second;
    else
        return nullptr;
}

SpellLearnSpellMapBounds SpellMgr::GetSpellLearnSpellMapBounds(uint32 spell_id) const
{
    return mSpellLearnSpells.equal_range(spell_id);
}

bool SpellMgr::IsSpellLearnSpell(uint32 spell_id) const
{
    return mSpellLearnSpells.find(spell_id) != mSpellLearnSpells.end();
}

bool SpellMgr::IsSpellLearnToSpell(uint32 spell_id1, uint32 spell_id2) const
{
    SpellLearnSpellMapBounds bounds = GetSpellLearnSpellMapBounds(spell_id1);
    for (SpellLearnSpellMap::const_iterator i = bounds.first; i != bounds.second; ++i)
        if (i->second.spell == spell_id2)
            return true;
    return false;
}

SpellTargetPosition const* SpellMgr::GetSpellTargetPosition(uint32 spell_id, SpellEffIndex effIndex) const
{
    SpellTargetPositionMap::const_iterator itr = mSpellTargetPositions.find(std::make_pair(spell_id, effIndex));
    if (itr != mSpellTargetPositions.end())
        return &itr->second;
    return nullptr;
}

SpellSpellGroupMapBounds SpellMgr::GetSpellSpellGroupMapBounds(uint32 spell_id) const
{
    spell_id = GetFirstSpellInChain(spell_id);
    return mSpellSpellGroup.equal_range(spell_id);
}

bool SpellMgr::IsSpellMemberOfSpellGroup(uint32 spellid, SpellGroup groupid) const
{
    SpellSpellGroupMapBounds spellGroup = GetSpellSpellGroupMapBounds(spellid);
    for (SpellSpellGroupMap::const_iterator itr = spellGroup.first; itr != spellGroup.second; ++itr)
    {
        if (itr->second == groupid)
            return true;
    }
    return false;
}

SpellGroupSpellMapBounds SpellMgr::GetSpellGroupSpellMapBounds(SpellGroup group_id) const
{
    return mSpellGroupSpell.equal_range(group_id);
}

void SpellMgr::GetSetOfSpellsInSpellGroup(SpellGroup group_id, std::set<uint32>& foundSpells) const
{
    std::set<SpellGroup> usedGroups;
    GetSetOfSpellsInSpellGroup(group_id, foundSpells, usedGroups);
}

void SpellMgr::GetSetOfSpellsInSpellGroup(SpellGroup group_id, std::set<uint32>& foundSpells, std::set<SpellGroup>& usedGroups) const
{
    if (usedGroups.find(group_id) != usedGroups.end())
        return;
    usedGroups.insert(group_id);

    SpellGroupSpellMapBounds groupSpell = GetSpellGroupSpellMapBounds(group_id);
    for (SpellGroupSpellMap::const_iterator itr = groupSpell.first; itr != groupSpell.second; ++itr)
    {
        if (itr->second < 0)
        {
            SpellGroup currGroup = (SpellGroup)abs(itr->second);
            GetSetOfSpellsInSpellGroup(currGroup, foundSpells, usedGroups);
        }
        else
        {
            foundSpells.insert(itr->second);
        }
    }
}

bool SpellMgr::AddSameEffectStackRuleSpellGroups(SpellInfo const* spellInfo, uint32 auraType, int32 amount, std::map<SpellGroup, int32>& groups) const
{
    uint32 spellId = spellInfo->GetFirstRankSpell()->Id;
    auto spellGroupBounds = GetSpellSpellGroupMapBounds(spellId);
    // Find group with SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT if it belongs to one
    for (auto itr = spellGroupBounds.first; itr != spellGroupBounds.second; ++itr)
    {
        SpellGroup group = itr->second;
        auto found = mSpellSameEffectStack.find(group);
        if (found != mSpellSameEffectStack.end())
        {
            // check auraTypes
            if (!found->second.count(auraType))
                continue;

            // Put the highest amount in the map
            auto groupItr = groups.find(group);
            if (groupItr == groups.end())
                groups.emplace(group, amount);
            else
            {
                int32 curr_amount = groups[group];
                // Take absolute value because this also counts for the highest negative aura
                if (std::abs(curr_amount) < std::abs(amount))
                    groupItr->second = amount;
            }
            // return because a spell should be in only one SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT group per auraType
            return true;
        }
    }
    // Not in a SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT group, so return false
    return false;
}

SpellGroupStackRule SpellMgr::CheckSpellGroupStackRules(SpellInfo const* spellInfo1, SpellInfo const* spellInfo2) const
{
    ASSERT(spellInfo1);
    ASSERT(spellInfo2);

    uint32 spellid_1 = spellInfo1->GetFirstRankSpell()->Id;
    uint32 spellid_2 = spellInfo2->GetFirstRankSpell()->Id;

    // find SpellGroups which are common for both spells
    SpellSpellGroupMapBounds spellGroup1 = GetSpellSpellGroupMapBounds(spellid_1);
    std::set<SpellGroup> groups;
    for (SpellSpellGroupMap::const_iterator itr = spellGroup1.first; itr != spellGroup1.second; ++itr)
    {
        if (IsSpellMemberOfSpellGroup(spellid_2, itr->second))
        {
            bool add = true;
            SpellGroupSpellMapBounds groupSpell = GetSpellGroupSpellMapBounds(itr->second);
            for (SpellGroupSpellMap::const_iterator itr2 = groupSpell.first; itr2 != groupSpell.second; ++itr2)
            {
                if (itr2->second < 0)
                {
                    SpellGroup currGroup = (SpellGroup)abs(itr2->second);
                    if (IsSpellMemberOfSpellGroup(spellid_1, currGroup) && IsSpellMemberOfSpellGroup(spellid_2, currGroup))
                    {
                        add = false;
                        break;
                    }
                }
            }
            if (add)
                groups.insert(itr->second);
        }
    }

    SpellGroupStackRule rule = SPELL_GROUP_STACK_RULE_DEFAULT;

    for (std::set<SpellGroup>::iterator itr = groups.begin(); itr!= groups.end(); ++itr)
    {
        SpellGroupStackMap::const_iterator found = mSpellGroupStack.find(*itr);
        if (found != mSpellGroupStack.end())
            rule = found->second;
        if (rule)
            break;
    }
    return rule;
}

SpellGroupStackRule SpellMgr::GetSpellGroupStackRule(SpellGroup group) const
{
    SpellGroupStackMap::const_iterator itr = mSpellGroupStack.find(group);
    if (itr != mSpellGroupStack.end())
        return itr->second;

    return SPELL_GROUP_STACK_RULE_DEFAULT;
}

SpellProcEntry const* SpellMgr::GetSpellProcEntry(uint32 spellId) const
{
    SpellProcMap::const_iterator itr = mSpellProcMap.find(spellId);
    if (itr != mSpellProcMap.end())
        return &itr->second;
    return nullptr;
}

bool SpellMgr::CanSpellTriggerProcOnEvent(SpellProcEntry const& procEntry, ProcEventInfo& eventInfo)
{
    // proc type doesn't match
    if (!(eventInfo.GetTypeMask() & procEntry.ProcFlags))
        return false;

    // check XP or honor target requirement
    if (procEntry.AttributesMask & PROC_ATTR_REQ_EXP_OR_HONOR)
        if (Player* actor = eventInfo.GetActor()->ToPlayer())
            if (eventInfo.GetActionTarget() && !actor->isHonorOrXPTarget(eventInfo.GetActionTarget()))
                return false;

    // check mana requirement
    if (procEntry.AttributesMask & PROC_ATTR_REQ_MANA_COST)
        if (SpellInfo const* eventSpellInfo = eventInfo.GetSpellInfo())
            if (!eventSpellInfo->ManaCost && !eventSpellInfo->ManaCostPercentage)
                return false;

    // always trigger for these types
    if (eventInfo.GetTypeMask() & (PROC_FLAG_KILLED | PROC_FLAG_KILL | PROC_FLAG_DEATH))
        return true;

    // do triggered cast checks
    // Do not consider autoattacks as triggered spells
    if (!(procEntry.AttributesMask & PROC_ATTR_TRIGGERED_CAN_PROC) && !(eventInfo.GetTypeMask() & AUTO_ATTACK_PROC_FLAG_MASK))
    {
        if (Spell const* spell = eventInfo.GetProcSpell())
        {
            if (spell->IsTriggered())
            {
                SpellInfo const* spellInfo = spell->GetSpellInfo();
                if (!spellInfo->HasAttribute(SPELL_ATTR3_TRIGGERED_CAN_TRIGGER_PROC_2) &&
                    !spellInfo->HasAttribute(SPELL_ATTR2_TRIGGERED_CAN_TRIGGER_PROC))
                    return false;
            }
        }
    }

    // check school mask (if set) for other trigger types
    if (procEntry.SchoolMask && !(eventInfo.GetSchoolMask() & procEntry.SchoolMask))
        return false;

    // check spell family name/flags (if set) for spells
    if (eventInfo.GetTypeMask() & SPELL_PROC_FLAG_MASK)
    {
        if (SpellInfo const* eventSpellInfo = eventInfo.GetSpellInfo())
            if (!eventSpellInfo->IsAffected(procEntry.SpellFamilyName, procEntry.SpellFamilyMask))
                return false;

        // check spell type mask (if set)
        if (procEntry.SpellTypeMask && !(eventInfo.GetSpellTypeMask() & procEntry.SpellTypeMask))
            return false;
    }

    // check spell phase mask
    if (eventInfo.GetTypeMask() & REQ_SPELL_PHASE_PROC_FLAG_MASK)
    {
        if (!(eventInfo.GetSpellPhaseMask() & procEntry.SpellPhaseMask))
            return false;
    }

    // check hit mask (on taken hit or on done hit, but not on spell cast phase)
    if ((eventInfo.GetTypeMask() & TAKEN_HIT_PROC_FLAG_MASK) || ((eventInfo.GetTypeMask() & DONE_HIT_PROC_FLAG_MASK) && !(eventInfo.GetSpellPhaseMask() & PROC_SPELL_PHASE_CAST)))
    {
        uint32 hitMask = procEntry.HitMask;
        // get default values if hit mask not set
        if (!hitMask)
        {
            // for taken procs allow normal + critical hits by default
            if (eventInfo.GetTypeMask() & TAKEN_HIT_PROC_FLAG_MASK)
                hitMask |= PROC_HIT_NORMAL | PROC_HIT_CRITICAL;
            // for done procs allow normal + critical + absorbs by default
            else
                hitMask |= PROC_HIT_NORMAL | PROC_HIT_CRITICAL | PROC_HIT_ABSORB;
        }
        if (!(eventInfo.GetHitMask() & hitMask))
            return false;
    }

    return true;
}

SpellBonusEntry const* SpellMgr::GetSpellBonusData(uint32 spellId) const
{
    // Lookup data
    SpellBonusMap::const_iterator itr = mSpellBonusMap.find(spellId);
    if (itr != mSpellBonusMap.end())
        return &itr->second;
    // Not found, try lookup for 1 spell rank if exist
    if (uint32 rank_1 = GetFirstSpellInChain(spellId))
    {
        SpellBonusMap::const_iterator itr2 = mSpellBonusMap.find(rank_1);
        if (itr2 != mSpellBonusMap.end())
            return &itr2->second;
    }
    return nullptr;
}

SpellThreatEntry const* SpellMgr::GetSpellThreatEntry(uint32 spellID) const
{
    SpellThreatMap::const_iterator itr = mSpellThreatMap.find(spellID);
    if (itr != mSpellThreatMap.end())
        return &itr->second;
    else
    {
        uint32 firstSpell = GetFirstSpellInChain(spellID);
        itr = mSpellThreatMap.find(firstSpell);
        if (itr != mSpellThreatMap.end())
            return &itr->second;
    }
    return nullptr;
}

SkillLineAbilityMapBounds SpellMgr::GetSkillLineAbilityMapBounds(uint32 spell_id) const
{
    return mSkillLineAbilityMap.equal_range(spell_id);
}

PetAura const* SpellMgr::GetPetAura(uint32 spell_id, uint8 eff) const
{
    SpellPetAuraMap::const_iterator itr = mSpellPetAuraMap.find((spell_id<<8) + eff);
    if (itr != mSpellPetAuraMap.end())
        return &itr->second;
    else
        return nullptr;
}

SpellEnchantProcEntry const* SpellMgr::GetSpellEnchantProcEvent(uint32 enchId) const
{
    SpellEnchantProcEventMap::const_iterator itr = mSpellEnchantProcEventMap.find(enchId);
    if (itr != mSpellEnchantProcEventMap.end())
        return &itr->second;
    return nullptr;
}

bool SpellMgr::IsArenaAllowedEnchancment(uint32 ench_id) const
{
    return mEnchantCustomAttr[ench_id];
}

std::vector<int32> const* SpellMgr::GetSpellLinked(int32 spell_id) const
{
    return Trinity::Containers::MapGetValuePtr(mSpellLinkedMap, spell_id);
}

PetLevelupSpellSet const* SpellMgr::GetPetLevelupSpellList(uint32 petFamily) const
{
    PetLevelupSpellMap::const_iterator itr = mPetLevelupSpellMap.find(petFamily);
    if (itr != mPetLevelupSpellMap.end())
        return &itr->second;
    else
        return nullptr;
}

PetDefaultSpellsEntry const* SpellMgr::GetPetDefaultSpellsEntry(int32 id) const
{
    PetDefaultSpellsMap::const_iterator itr = mPetDefaultSpellsMap.find(id);
    if (itr != mPetDefaultSpellsMap.end())
        return &itr->second;
    return nullptr;
}

SpellAreaMapBounds SpellMgr::GetSpellAreaMapBounds(uint32 spell_id) const
{
    return mSpellAreaMap.equal_range(spell_id);
}

SpellAreaForQuestMapBounds SpellMgr::GetSpellAreaForQuestMapBounds(uint32 quest_id) const
{
    return mSpellAreaForQuestMap.equal_range(quest_id);
}

SpellAreaForQuestMapBounds SpellMgr::GetSpellAreaForQuestEndMapBounds(uint32 quest_id) const
{
    return mSpellAreaForQuestEndMap.equal_range(quest_id);
}

SpellAreaForAuraMapBounds SpellMgr::GetSpellAreaForAuraMapBounds(uint32 spell_id) const
{
    return mSpellAreaForAuraMap.equal_range(spell_id);
}

SpellAreaForAreaMapBounds SpellMgr::GetSpellAreaForAreaMapBounds(uint32 area_id) const
{
    return mSpellAreaForAreaMap.equal_range(area_id);
}

bool SpellArea::IsFitToRequirements(Player const* player, uint32 newZone, uint32 newArea) const
{
    if (gender != GENDER_NONE)                   // is not expected gender
        if (!player || gender != player->GetNativeGender())
            return false;

    if (raceMask)                                // is not expected race
        if (!player || !(raceMask & player->GetRaceMask()))
            return false;

    if (areaId)                                  // is not in expected zone
        if (newZone != areaId && newArea != areaId)
            return false;

    if (questStart)                              // is not in expected required quest state
        if (!player || (((1 << player->GetQuestStatus(questStart)) & questStartStatus) == 0))
            return false;

    if (questEnd)                                // is not in expected forbidden quest state
        if (!player || (((1 << player->GetQuestStatus(questEnd)) & questEndStatus) == 0))
            return false;

    if (auraSpell)                               // does not have expected aura
        if (!player || (auraSpell > 0 && !player->HasAura(auraSpell)) || (auraSpell < 0 && player->HasAura(-auraSpell)))
            return false;

    if (player)
    {
        if (Battleground* bg = player->GetBattleground())
            return bg->IsSpellAllowed(spellId, player);
    }

    // Extra conditions
    switch (spellId)
    {
        case 58600: // No fly Zone - Dalaran
        {
            if (!player)
                return false;

            AreaTableEntry const* pArea = sAreaTableStore.LookupEntry(player->GetAreaId());
            if (!(pArea && pArea->Flags & AREA_FLAG_NO_FLY_ZONE))
                return false;
            if (!player->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) && !player->HasAuraType(SPELL_AURA_FLY))
                return false;
            break;
        }
        case 58730: // No fly Zone - Wintergrasp
        {
            if (!player)
                return false;

            Battlefield* Bf = sBattlefieldMgr->GetBattlefieldToZoneId(player->GetZoneId());
            if (!Bf || Bf->CanFlyIn() || (!player->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) && !player->HasAuraType(SPELL_AURA_FLY)))
                return false;
            break;
        }
        case 56618: // Horde Controls Factory Phase Shift
        case 56617: // Alliance Controls Factory Phase Shift
        {
            if (!player)
                return false;

            Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(player->GetZoneId());

            if (!bf || bf->GetTypeId() != BATTLEFIELD_WG)
                return false;

            // team that controls the workshop in the specified area
            uint32 team = bf->GetData(newArea);

            if (team == TEAM_HORDE)
                return spellId == 56618;
            else if (team == TEAM_ALLIANCE)
                return spellId == 56617;
            break;
        }
        case 57940: // Essence of Wintergrasp - Northrend
        case 58045: // Essence of Wintergrasp - Wintergrasp
        {
            if (!player)
                return false;

            if (Battlefield* battlefieldWG = sBattlefieldMgr->GetBattlefieldByBattleId(BATTLEFIELD_BATTLEID_WG))
                return battlefieldWG->IsEnabled() && (player->GetTeamId() == battlefieldWG->GetDefenderTeam()) && !battlefieldWG->IsWarTime();
            break;
        }
        case 74411: // Battleground - Dampening
        {
            if (!player)
                return false;

            if (Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(player->GetZoneId()))
                return bf->IsWarTime();
            break;
        }

    }

    return true;
}

void SpellMgr::UnloadSpellInfoChains()
{
    for (SpellChainMap::iterator itr = mSpellChains.begin(); itr != mSpellChains.end(); ++itr)
        mSpellInfoMap[itr->first]->ChainEntry = nullptr;

    mSpellChains.clear();
}

void SpellMgr::LoadSpellTalentRanks()
{
    // cleanup core data before reload - remove reference to ChainNode from SpellInfo
    UnloadSpellInfoChains();

    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
        if (!talentInfo)
            continue;

        SpellInfo const* lastSpell = nullptr;
        for (uint8 rank = MAX_TALENT_RANK - 1; rank > 0; --rank)
        {
            if (talentInfo->SpellRank[rank])
            {
                lastSpell = GetSpellInfo(talentInfo->SpellRank[rank]);
                break;
            }
        }

        if (!lastSpell)
            continue;

        SpellInfo const* firstSpell = GetSpellInfo(talentInfo->SpellRank[0]);
        if (!firstSpell)
        {
            TC_LOG_ERROR("spells", "SpellMgr::LoadSpellTalentRanks: First Rank Spell {} for TalentEntry {} does not exist.", talentInfo->SpellRank[0], i);
            continue;
        }

        SpellInfo const* prevSpell = nullptr;
        for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
        {
            uint32 spellId = talentInfo->SpellRank[rank];
            if (!spellId)
                break;

            SpellInfo const* currentSpell = GetSpellInfo(spellId);
            if (!currentSpell)
            {
                TC_LOG_ERROR("spells", "SpellMgr::LoadSpellTalentRanks: Spell {} (Rank: {}) for TalentEntry {} does not exist.", spellId, rank + 1, i);
                break;
            }

            SpellChainNode node;
            node.first = firstSpell;
            node.last  = lastSpell;
            node.rank  = rank + 1;

            node.prev = prevSpell;
            node.next = node.rank < MAX_TALENT_RANK ? GetSpellInfo(talentInfo->SpellRank[node.rank]) : nullptr;

            mSpellChains[spellId] = node;
            mSpellInfoMap[spellId]->ChainEntry = &mSpellChains[spellId];

            prevSpell = currentSpell;
        }
    }
}

void SpellMgr::LoadSpellRanks()
{
    // cleanup data and load spell ranks for talents from dbc
    LoadSpellTalentRanks();

    uint32 oldMSTime = getMSTime();

    //                                                     0             1       2
    QueryResult result = WorldDatabase.Query("SELECT first_spell_id, spell_id, `rank` from spell_ranks ORDER BY first_spell_id, `rank`");

    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell rank records. DB table `spell_ranks` is empty.");
        return;
    }

    uint32 count = 0;
    bool finished = false;

    do
    {
                        // spellid, rank
        std::list < std::pair < int32, int32 > > rankChain;
        int32 currentSpell = -1;
        int32 lastSpell = -1;

        // fill one chain
        while (currentSpell == lastSpell && !finished)
        {
            Field* fields = result->Fetch();

            currentSpell = fields[0].GetUInt32();
            if (lastSpell == -1)
                lastSpell = currentSpell;
            uint32 spell_id = fields[1].GetUInt32();
            uint32 rank = fields[2].GetUInt8();

            // don't drop the row if we're moving to the next rank
            if (currentSpell == lastSpell)
            {
                rankChain.push_back(std::make_pair(spell_id, rank));
                if (!result->NextRow())
                    finished = true;
            }
            else
                break;
        }
        // check if chain is made with valid first spell
        SpellInfo const* first = GetSpellInfo(lastSpell);
        if (!first)
        {
            TC_LOG_ERROR("sql.sql", "The spell rank identifier(first_spell_id) {} listed in `spell_ranks` does not exist!", lastSpell);
            continue;
        }
        // check if chain is long enough
        if (rankChain.size() < 2)
        {
            TC_LOG_ERROR("sql.sql", "There is only 1 spell rank for identifier(first_spell_id) {} in `spell_ranks`, entry is not needed!", lastSpell);
            continue;
        }
        int32 curRank = 0;
        bool valid = true;
        // check spells in chain
        for (std::list<std::pair<int32, int32> >::iterator itr = rankChain.begin(); itr!= rankChain.end(); ++itr)
        {
            SpellInfo const* spell = GetSpellInfo(itr->first);
            if (!spell)
            {
                TC_LOG_ERROR("sql.sql", "The spell {} (rank {}) listed in `spell_ranks` for chain {} does not exist!", itr->first, itr->second, lastSpell);
                valid = false;
                break;
            }
            ++curRank;
            if (itr->second != curRank)
            {
                TC_LOG_ERROR("sql.sql", "The spell {} (rank {}) listed in `spell_ranks` for chain {} does not have a proper rank value (should be {})!", itr->first, itr->second, lastSpell, curRank);
                valid = false;
                break;
            }
        }
        if (!valid)
            continue;
        int32 prevRank = 0;
        // insert the chain
        std::list<std::pair<int32, int32> >::iterator itr = rankChain.begin();
        do
        {
            ++count;
            int32 addedSpell = itr->first;

            if (mSpellInfoMap[addedSpell]->ChainEntry)
                TC_LOG_ERROR("sql.sql", "The spell {} (rank: {}, first: {}) listed in `spell_ranks` already has ChainEntry from dbc.", addedSpell, itr->second, lastSpell);

            mSpellChains[addedSpell].first = GetSpellInfo(lastSpell);
            mSpellChains[addedSpell].last = GetSpellInfo(rankChain.back().first);
            mSpellChains[addedSpell].rank = itr->second;
            mSpellChains[addedSpell].prev = GetSpellInfo(prevRank);
            mSpellInfoMap[addedSpell]->ChainEntry = &mSpellChains[addedSpell];
            prevRank = addedSpell;
            ++itr;

            if (itr == rankChain.end())
            {
                mSpellChains[addedSpell].next = nullptr;
                break;
            }
            else
                mSpellChains[addedSpell].next = GetSpellInfo(itr->first);
        }
        while (true);
    }
    while (!finished);

    TC_LOG_INFO("server.loading", ">> Loaded {} spell rank records in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellRequired()
{
    uint32 oldMSTime = getMSTime();

    mSpellsReqSpell.clear();                                   // need for reload case
    mSpellReq.clear();                                         // need for reload case

    //                                                   0        1
    QueryResult result = WorldDatabase.Query("SELECT spell_id, req_spell from spell_required");

    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell required records. DB table `spell_required` is empty.");

        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 spell_id = fields[0].GetUInt32();
        uint32 spell_req = fields[1].GetUInt32();

        // check if chain is made with valid first spell
        SpellInfo const* spell = GetSpellInfo(spell_id);
        if (!spell)
        {
            TC_LOG_ERROR("sql.sql", "spell_id {} in `spell_required` table could not be found in dbc, skipped.", spell_id);
            continue;
        }

        SpellInfo const* reqSpell = GetSpellInfo(spell_req);
        if (!reqSpell)
        {
            TC_LOG_ERROR("sql.sql", "req_spell {} in `spell_required` table could not be found in dbc, skipped.", spell_req);
            continue;
        }

        if (spell->IsRankOf(reqSpell))
        {
            TC_LOG_ERROR("sql.sql", "req_spell {} and spell_id {} in `spell_required` table are ranks of the same spell, entry not needed, skipped.", spell_req, spell_id);
            continue;
        }

        if (IsSpellRequiringSpell(spell_id, spell_req))
        {
            TC_LOG_ERROR("sql.sql", "Duplicate entry of req_spell {} and spell_id {} in `spell_required`, skipped.", spell_req, spell_id);
            continue;
        }

        mSpellReq.insert (std::pair<uint32, uint32>(spell_id, spell_req));
        mSpellsReqSpell.insert (std::pair<uint32, uint32>(spell_req, spell_id));
        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} spell required records in {} ms", count, GetMSTimeDiffToNow(oldMSTime));

}

void SpellMgr::LoadSpellLearnSkills()
{
    uint32 oldMSTime = getMSTime();

    mSpellLearnSkills.clear();                              // need for reload case

    // search auto-learned skills and add its to map also for use in unlearn spells/talents
    uint32 dbc_count = 0;
    for (SpellInfo const* entry : mSpellInfoMap)
    {
        if (!entry)
            continue;

        for (SpellEffectInfo const& spellEffectInfo : entry->GetEffects())
        {
            SpellLearnSkillNode dbc_node;
            switch (spellEffectInfo.Effect)
            {
                case SPELL_EFFECT_SKILL:
                    dbc_node.skill = spellEffectInfo.MiscValue;
                    dbc_node.step = spellEffectInfo.CalcValue();
                    if (dbc_node.skill != SKILL_RIDING)
                        dbc_node.value = 1;
                    else
                        dbc_node.value = dbc_node.step * 75;
                    dbc_node.maxvalue = dbc_node.step * 75;
                    break;
                case SPELL_EFFECT_DUAL_WIELD:
                    dbc_node.skill = SKILL_DUAL_WIELD;
                    dbc_node.step = 1;
                    dbc_node.value = 1;
                    dbc_node.maxvalue = 1;
                    break;
                default:
                    continue;
            }

            mSpellLearnSkills[entry->Id] = dbc_node;
            ++dbc_count;
            break;
        }
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} Spell Learn Skills from DBC in {} ms", dbc_count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellLearnSpells()
{
    uint32 oldMSTime = getMSTime();

    mSpellLearnSpells.clear();                              // need for reload case

    //                                                  0      1        2
    QueryResult result = WorldDatabase.Query("SELECT entry, SpellID, Active FROM spell_learn_spell");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell learn spells. DB table `spell_learn_spell` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 spell_id = fields[0].GetUInt32();

        SpellLearnSpellNode node;
        node.spell       = fields[1].GetUInt32();
        node.active      = fields[2].GetBool();
        node.autoLearned = false;

        if (!GetSpellInfo(spell_id))
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_learn_spell` does not exist.", spell_id);
            continue;
        }

        if (!GetSpellInfo(node.spell))
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_learn_spell` learning non-existing spell {}.", spell_id, node.spell);
            continue;
        }

        if (GetTalentSpellCost(node.spell))
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_learn_spell` attempts learning talent spell {}, skipped.", spell_id, node.spell);
            continue;
        }

        mSpellLearnSpells.insert(SpellLearnSpellMap::value_type(spell_id, node));

        ++count;
    } while (result->NextRow());

    // search auto-learned spells and add its to map also for use in unlearn spells/talents
    uint32 dbc_count = 0;
    for (uint32 spell = 0; spell < GetSpellInfoStoreSize(); ++spell)
    {
        SpellInfo const* entry = GetSpellInfo(spell);

        if (!entry)
            continue;

        for (SpellEffectInfo const& spellEffectInfo : entry->GetEffects())
        {
            if (spellEffectInfo.IsEffect(SPELL_EFFECT_LEARN_SPELL))
            {
                SpellLearnSpellNode dbc_node;
                dbc_node.spell = spellEffectInfo.TriggerSpell;
                dbc_node.active = true;                     // all dbc based learned spells is active (show in spell book or hide by client itself)

                // ignore learning not existed spells (broken/outdated/or generic learnig spell 483
                if (!GetSpellInfo(dbc_node.spell))
                    continue;

                // talent or passive spells or skill-step spells auto-cast and not need dependent learning,
                // pet teaching spells must not be dependent learning (cast)
                // other required explicit dependent learning
                dbc_node.autoLearned = spellEffectInfo.TargetA.GetTarget() == TARGET_UNIT_PET || GetTalentSpellCost(spell) > 0 || entry->IsPassive() || entry->HasEffect(SPELL_EFFECT_SKILL_STEP);

                SpellLearnSpellMapBounds db_node_bounds = GetSpellLearnSpellMapBounds(spell);

                bool found = false;
                for (SpellLearnSpellMap::const_iterator itr = db_node_bounds.first; itr != db_node_bounds.second; ++itr)
                {
                    if (itr->second.spell == dbc_node.spell)
                    {
                        TC_LOG_ERROR("sql.sql", "The spell {} is an auto-learn spell {} in spell.dbc and the record in `spell_learn_spell` is redundant. Please update your DB.",
                            spell, dbc_node.spell);
                        found = true;
                        break;
                    }
                }

                if (!found)                                  // add new spell-spell pair if not found
                {
                    mSpellLearnSpells.insert(SpellLearnSpellMap::value_type(spell, dbc_node));
                    ++dbc_count;
                }
            }
        }
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} spell learn spells + {} found in DBC in {} ms", count, dbc_count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellTargetPositions()
{
    uint32 oldMSTime = getMSTime();

    mSpellTargetPositions.clear();                                // need for reload case

    //                                                0      1          2        3         4           5            6
    QueryResult result = WorldDatabase.Query("SELECT ID, EffectIndex, MapID, PositionX, PositionY, PositionZ, Orientation FROM spell_target_position");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell target coordinates. DB table `spell_target_position` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 Spell_ID = fields[0].GetUInt32();
        SpellEffIndex effIndex = SpellEffIndex(fields[1].GetUInt8());

        SpellTargetPosition st;

        st.target_mapId       = fields[2].GetUInt16();
        st.target_X           = fields[3].GetFloat();
        st.target_Y           = fields[4].GetFloat();
        st.target_Z           = fields[5].GetFloat();
        st.target_Orientation = fields[6].GetFloat();

        MapEntry const* mapEntry = sMapStore.LookupEntry(st.target_mapId);
        if (!mapEntry)
        {
            TC_LOG_ERROR("sql.sql", "Spell (Id: {}, effIndex: {}) target map (ID: {}) does not exist in `Map.dbc`.", Spell_ID, effIndex, st.target_mapId);
            continue;
        }

        if (st.target_X==0 && st.target_Y==0 && st.target_Z==0)
        {
            TC_LOG_ERROR("sql.sql", "Spell (Id: {}, effIndex: {}) target coordinates not provided.", Spell_ID, effIndex);
            continue;
        }

        SpellInfo const* spellInfo = GetSpellInfo(Spell_ID);
        if (!spellInfo)
        {
            TC_LOG_ERROR("sql.sql", "Spell (Id: {}) listed in `spell_target_position` does not exist.", Spell_ID);
            continue;
        }

        if (spellInfo->GetEffect(effIndex).TargetA.GetTarget() == TARGET_DEST_DB || spellInfo->GetEffect(effIndex).TargetB.GetTarget() == TARGET_DEST_DB)
        {
            std::pair<uint32, SpellEffIndex> key = std::make_pair(Spell_ID, effIndex);
            mSpellTargetPositions[key] = st;
            ++count;
        }
        else
        {
            TC_LOG_ERROR("sql.sql", "Spell (Id: {}, effIndex: {}) listed in `spell_target_position` does not have a target TARGET_DEST_DB (17).", Spell_ID, effIndex);
            continue;
        }

    } while (result->NextRow());

    /*
    // Check all spells
    for (uint32 i = 1; i < GetSpellInfoStoreSize; ++i)
    {
        SpellInfo const* spellInfo = GetSpellInfo(i);
        if (!spellInfo)
            continue;

        bool found = false;
        for (int j = 0; j < MAX_SPELL_EFFECTS; ++j)
        {
            switch (spellInfo->Effects[j].TargetA)
            {
                case TARGET_DEST_DB:
                    found = true;
                    break;
            }
            if (found)
                break;
            switch (spellInfo->Effects[j].TargetB)
            {
                case TARGET_DEST_DB:
                    found = true;
                    break;
            }
            if (found)
                break;
        }
        if (found)
        {
            if (!sSpellMgr->GetSpellTargetPosition(i))
                TC_LOG_DEBUG("spells", "Spell (ID: {}) does not have a record in `spell_target_position`.", i);
        }
    }*/

    TC_LOG_INFO("server.loading", ">> Loaded {} spell teleport coordinates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellGroups()
{
    uint32 oldMSTime = getMSTime();

    mSpellSpellGroup.clear();                                  // need for reload case
    mSpellGroupSpell.clear();

    //                                                0     1
    QueryResult result = WorldDatabase.Query("SELECT id, spell_id FROM spell_group");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell group definitions. DB table `spell_group` is empty.");
        return;
    }

    std::set<uint32> groups;
    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 group_id = fields[0].GetUInt32();
        if (group_id <= SPELL_GROUP_DB_RANGE_MIN && group_id >= SPELL_GROUP_CORE_RANGE_MAX)
        {
            TC_LOG_ERROR("sql.sql", "SpellGroup id {} listed in `spell_group` is in core range, but is not defined in core!", group_id);
            continue;
        }
        int32 spell_id = fields[1].GetInt32();

        groups.insert(group_id);
        mSpellGroupSpell.emplace(SpellGroup(group_id), spell_id);

    } while (result->NextRow());

    for (auto itr = mSpellGroupSpell.begin(); itr!= mSpellGroupSpell.end();)
    {
        if (itr->second < 0)
        {
            if (groups.find(abs(itr->second)) == groups.end())
            {
                TC_LOG_ERROR("sql.sql", "SpellGroup id {} listed in `spell_group` does not exist", abs(itr->second));
                itr = mSpellGroupSpell.erase(itr);
            }
            else
                ++itr;
        }
        else
        {
            SpellInfo const* spellInfo = GetSpellInfo(itr->second);
            if (!spellInfo)
            {
                TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_group` does not exist", itr->second);
                itr = mSpellGroupSpell.erase(itr);
            }
            else if (spellInfo->GetRank() > 1)
            {
                TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_group` is not the first rank of the spell.", itr->second);
                itr = mSpellGroupSpell.erase(itr);
            }
            else
                ++itr;
        }
    }

    for (auto groupItr = groups.begin(); groupItr != groups.end(); ++groupItr)
    {
        std::set<uint32> spells;
        GetSetOfSpellsInSpellGroup(SpellGroup(*groupItr), spells);

        for (auto spellItr = spells.begin(); spellItr != spells.end(); ++spellItr)
        {
            ++count;
            mSpellSpellGroup.emplace(*spellItr, SpellGroup(*groupItr));
        }
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} spell group definitions in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellGroupStackRules()
{
    uint32 oldMSTime = getMSTime();

    mSpellGroupStack.clear();                                  // need for reload case
    mSpellSameEffectStack.clear();

    std::vector<uint32> sameEffectGroups;

    //                                                       0         1
    QueryResult result = WorldDatabase.Query("SELECT group_id, stack_rule FROM spell_group_stack_rules");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell group stack rules. DB table `spell_group_stack_rules` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 group_id = fields[0].GetUInt32();
        uint8 stack_rule = fields[1].GetInt8();
        if (stack_rule >= SPELL_GROUP_STACK_RULE_MAX)
        {
            TC_LOG_ERROR("sql.sql", "SpellGroupStackRule {} listed in `spell_group_stack_rules` does not exist.", stack_rule);
            continue;
        }

        auto bounds = GetSpellGroupSpellMapBounds((SpellGroup)group_id);
        if (bounds.first == bounds.second)
        {
            TC_LOG_ERROR("sql.sql", "SpellGroup id {} listed in `spell_group_stack_rules` does not exist.", group_id);
            continue;
        }

        mSpellGroupStack.emplace(SpellGroup(group_id), SpellGroupStackRule(stack_rule));

        // different container for same effect stack rules, need to check effect types
        if (stack_rule == SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT)
            sameEffectGroups.push_back(group_id);

        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} spell group stack rules in {} ms", count, GetMSTimeDiffToNow(oldMSTime));

    count = 0;
    oldMSTime = getMSTime();
    TC_LOG_INFO("server.loading", ">> Parsing SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT stack rules...");

    for (uint32 group_id : sameEffectGroups)
    {
        std::set<uint32> spellIds;
        GetSetOfSpellsInSpellGroup(SpellGroup(group_id), spellIds);

        std::unordered_set<uint32> auraTypes;

        // we have to 'guess' what effect this group corresponds to
        {
            std::unordered_multiset<uint32 /*auraName*/> frequencyContainer;

            // only waylay for the moment (shared group)
            std::vector<std::vector<uint32 /*auraName*/>> const SubGroups =
            {
                { SPELL_AURA_MOD_MELEE_HASTE, SPELL_AURA_MOD_MELEE_RANGED_HASTE, SPELL_AURA_MOD_RANGED_HASTE }
            };

            for (uint32 spellId : spellIds)
            {
                SpellInfo const* spellInfo = AssertSpellInfo(spellId);
                for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
                {
                    if (!spellEffectInfo.IsAura())
                        continue;

                    uint32 auraName = spellEffectInfo.ApplyAuraName;
                    for (std::vector<uint32> const& subGroup : SubGroups)
                    {
                        if (std::find(subGroup.begin(), subGroup.end(), auraName) != subGroup.end())
                        {
                            // count as first aura
                            auraName = subGroup.front();
                            break;
                        }
                    }

                    frequencyContainer.insert(auraName);
                }
            }

            uint32 auraType = 0;
            size_t auraTypeCount = 0;
            for (uint32 auraName : frequencyContainer)
            {
                size_t currentCount = frequencyContainer.count(auraName);
                if (currentCount > auraTypeCount)
                {
                    auraType = auraName;
                    auraTypeCount = currentCount;
                }
            }

            for (std::vector<uint32> const& subGroup : SubGroups)
            {
                if (auraType == subGroup.front())
                {
                    auraTypes.insert(subGroup.begin(), subGroup.end());
                    break;
                }
            }

            if (auraTypes.empty())
                auraTypes.insert(auraType);
        }

        // re-check spells against guessed group
        for (uint32 spellId : spellIds)
        {
            SpellInfo const* spellInfo = AssertSpellInfo(spellId);

            bool found = false;
            while (spellInfo)
            {
                for (uint32 auraType : auraTypes)
                {
                    if (spellInfo->HasAura(AuraType(auraType)))
                    {
                        found = true;
                        break;
                    }
                }

                if (found)
                    break;

                spellInfo = spellInfo->GetNextRankSpell();
            }

            // not found either, log error
            if (!found)
                TC_LOG_ERROR("sql.sql", "SpellId {} listed in `spell_group` with stack rule 3 does not share aura assigned for group {}", spellId, group_id);
        }

        mSpellSameEffectStack[SpellGroup(group_id)] = auraTypes;
        ++count;
    }

    TC_LOG_INFO("server.loading", ">> Parsed {} SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT stack rules in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellProcs()
{
    uint32 oldMSTime = getMSTime();

    mSpellProcMap.clear();                             // need for reload case

    //                                                     0           1                2                 3                 4                 5
    QueryResult result = WorldDatabase.Query("SELECT SpellId, SchoolMask, SpellFamilyName, SpellFamilyMask0, SpellFamilyMask1, SpellFamilyMask2, "
    //           6              7               8        9               10                  11              12      13        14       15
        "ProcFlags, SpellTypeMask, SpellPhaseMask, HitMask, AttributesMask, DisableEffectsMask, ProcsPerMinute, Chance, Cooldown, Charges FROM spell_proc");

    uint32 count = 0;
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();

            int32 spellId = fields[0].GetInt32();

            bool allRanks = false;
            if (spellId < 0)
            {
                allRanks = true;
                spellId = -spellId;
            }

            SpellInfo const* spellInfo = GetSpellInfo(spellId);
            if (!spellInfo)
            {
                TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_proc` does not exist", spellId);
                continue;
            }

            if (allRanks)
            {
                if (!spellInfo->IsRanked())
                    TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_proc` with all ranks, but spell has no ranks.", spellId);

                if (spellInfo->GetFirstRankSpell()->Id != uint32(spellId))
                {
                    TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_proc` is not the first rank of the spell.", spellId);
                    continue;
                }
            }

            SpellProcEntry baseProcEntry;

            baseProcEntry.SchoolMask = fields[1].GetInt8();
            baseProcEntry.SpellFamilyName = fields[2].GetUInt16();
            baseProcEntry.SpellFamilyMask[0] = fields[3].GetUInt32();
            baseProcEntry.SpellFamilyMask[1] = fields[4].GetUInt32();
            baseProcEntry.SpellFamilyMask[2] = fields[5].GetUInt32();
            baseProcEntry.ProcFlags = fields[6].GetUInt32();
            baseProcEntry.SpellTypeMask = fields[7].GetUInt32();
            baseProcEntry.SpellPhaseMask = fields[8].GetUInt32();
            baseProcEntry.HitMask = fields[9].GetUInt32();
            baseProcEntry.AttributesMask = fields[10].GetUInt32();
            baseProcEntry.DisableEffectsMask = fields[11].GetUInt32();
            baseProcEntry.ProcsPerMinute = fields[12].GetFloat();
            baseProcEntry.Chance = fields[13].GetFloat();
            baseProcEntry.Cooldown = Milliseconds(fields[14].GetUInt32());
            baseProcEntry.Charges = fields[15].GetUInt8();

            while (spellInfo)
            {
                if (mSpellProcMap.find(spellInfo->Id) != mSpellProcMap.end())
                {
                    TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_proc` already has its first rank in the table.", spellInfo->Id);
                    break;
                }

                SpellProcEntry procEntry = SpellProcEntry(baseProcEntry);

                // take defaults from dbcs
                if (!procEntry.ProcFlags)
                    procEntry.ProcFlags = spellInfo->ProcFlags;
                if (!procEntry.Charges)
                    procEntry.Charges = spellInfo->ProcCharges;
                if (!procEntry.Chance && !procEntry.ProcsPerMinute)
                    procEntry.Chance = float(spellInfo->ProcChance);

                // validate data
                if (procEntry.SchoolMask & ~SPELL_SCHOOL_MASK_ALL)
                    TC_LOG_ERROR("sql.sql", "`spell_proc` table entry for spellId {} has wrong `SchoolMask` set: {}", spellInfo->Id, procEntry.SchoolMask);
                if (procEntry.SpellFamilyName && (procEntry.SpellFamilyName < SPELLFAMILY_MAGE || procEntry.SpellFamilyName > SPELLFAMILY_PET || procEntry.SpellFamilyName == 14 || procEntry.SpellFamilyName == 16))
                    TC_LOG_ERROR("sql.sql", "`spell_proc` table entry for spellId {} has wrong `SpellFamilyName` set: {}", spellInfo->Id, procEntry.SpellFamilyName);
                if (procEntry.Chance < 0)
                {
                    TC_LOG_ERROR("sql.sql", "`spell_proc` table entry for spellId {} has negative value in the `Chance` field", spellInfo->Id);
                    procEntry.Chance = 0;
                }
                if (procEntry.ProcsPerMinute < 0)
                {
                    TC_LOG_ERROR("sql.sql", "`spell_proc` table entry for spellId {} has negative value in the `ProcsPerMinute` field", spellInfo->Id);
                    procEntry.ProcsPerMinute = 0;
                }
                if (!procEntry.ProcFlags)
                    TC_LOG_ERROR("sql.sql", "The `spell_proc` table entry for spellId {} doesn't have any `ProcFlags` value defined, proc will not be triggered.", spellInfo->Id);
                if (procEntry.SpellTypeMask & ~PROC_SPELL_TYPE_MASK_ALL)
                    TC_LOG_ERROR("sql.sql", "`spell_proc` table entry for spellId {} has wrong `SpellTypeMask` set: {}", spellInfo->Id, procEntry.SpellTypeMask);
                if (procEntry.SpellTypeMask && !(procEntry.ProcFlags & SPELL_PROC_FLAG_MASK))
                    TC_LOG_ERROR("sql.sql", "The `spell_proc` table entry for spellId {} has `SpellTypeMask` value defined, but it will not be used for the defined `ProcFlags` value.", spellInfo->Id);
                if (!procEntry.SpellPhaseMask && procEntry.ProcFlags & REQ_SPELL_PHASE_PROC_FLAG_MASK)
                    TC_LOG_ERROR("sql.sql", "The `spell_proc` table entry for spellId {} doesn't have any `SpellPhaseMask` value defined, but it is required for the defined `ProcFlags` value. Proc will not be triggered.", spellInfo->Id);
                if (procEntry.SpellPhaseMask & ~PROC_SPELL_PHASE_MASK_ALL)
                    TC_LOG_ERROR("sql.sql", "The `spell_proc` table entry for spellId {} has wrong `SpellPhaseMask` set: {}", spellInfo->Id, procEntry.SpellPhaseMask);
                if (procEntry.SpellPhaseMask && !(procEntry.ProcFlags & REQ_SPELL_PHASE_PROC_FLAG_MASK))
                    TC_LOG_ERROR("sql.sql", "The `spell_proc` table entry for spellId {} has a `SpellPhaseMask` value defined, but it will not be used for the defined `ProcFlags` value.", spellInfo->Id);
                if (procEntry.HitMask & ~PROC_HIT_MASK_ALL)
                    TC_LOG_ERROR("sql.sql", "The `spell_proc` table entry for spellId {} has wrong `HitMask` set: {}", spellInfo->Id, procEntry.HitMask);
                if (procEntry.HitMask && !(procEntry.ProcFlags & TAKEN_HIT_PROC_FLAG_MASK || (procEntry.ProcFlags & DONE_HIT_PROC_FLAG_MASK && (!procEntry.SpellPhaseMask || procEntry.SpellPhaseMask & (PROC_SPELL_PHASE_HIT | PROC_SPELL_PHASE_FINISH)))))
                    TC_LOG_ERROR("sql.sql", "The `spell_proc` table entry for spellId {} has `HitMask` value defined, but it will not be used for defined `ProcFlags` and `SpellPhaseMask` values.", spellInfo->Id);
                for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
                    if ((procEntry.DisableEffectsMask & (1u << spellEffectInfo.EffectIndex)) && !spellEffectInfo.IsAura())
                        TC_LOG_ERROR("sql.sql", "The `spell_proc` table entry for spellId {} has DisableEffectsMask with effect {}, but effect {} is not an aura effect", spellInfo->Id, static_cast<uint32>(spellEffectInfo.EffectIndex), static_cast<uint32>(spellEffectInfo.EffectIndex));
                if (procEntry.AttributesMask & PROC_ATTR_REQ_SPELLMOD)
                {
                    bool found = false;
                    for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
                    {
                        if (!spellEffectInfo.IsAura())
                            continue;

                        if (spellEffectInfo.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER || spellEffectInfo.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER)
                        {
                            found = true;
                            break;
                        }
                    }

                    if (!found)
                        TC_LOG_ERROR("sql.sql", "The `spell_proc` table entry for spellId {} has Attribute PROC_ATTR_REQ_SPELLMOD, but spell has no spell mods. Proc will not be triggered", spellInfo->Id);
                }

                mSpellProcMap[spellInfo->Id] = procEntry;

                if (allRanks)
                    spellInfo = spellInfo->GetNextRankSpell();
                else
                    break;
            }
            ++count;
        } while (result->NextRow());
    }
    else
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell proc conditions and data. DB table `spell_proc` is empty.");

    TC_LOG_INFO("server.loading", ">> Loaded {} spell proc conditions and data in {} ms", count, GetMSTimeDiffToNow(oldMSTime));

    // Define can trigger auras
    bool isTriggerAura[TOTAL_AURAS];
    // Triggered always, even from triggered spells
    bool isAlwaysTriggeredAura[TOTAL_AURAS];
    // SpellTypeMask to add to the proc
    uint32 spellTypeMask[TOTAL_AURAS];

    // List of auras that CAN trigger but may not exist in spell_proc
    // in most cases needed to drop charges

    // some aura types need additional checks (eg SPELL_AURA_MECHANIC_IMMUNITY needs mechanic check)
    // see AuraEffect::CheckEffectProc
    for (uint16 i = 0; i < TOTAL_AURAS; ++i)
    {
        isTriggerAura[i] = false;
        isAlwaysTriggeredAura[i] = false;
        spellTypeMask[i] = PROC_SPELL_TYPE_MASK_ALL;
    }

    isTriggerAura[SPELL_AURA_DUMMY] = true;                                 // Most dummy auras should require scripting, but there are some exceptions (ie 12311)
    isTriggerAura[SPELL_AURA_MOD_CONFUSE] = true;                           // "Any direct damaging attack will revive targets"
    isTriggerAura[SPELL_AURA_MOD_THREAT] = true;                            // Only one spell: 28762 part of Mage T3 8p bonus
    isTriggerAura[SPELL_AURA_MOD_STUN] = true;                              // Aura does not have charges but needs to be removed on trigger
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_DONE] = true;
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_TAKEN] = true;
    isTriggerAura[SPELL_AURA_MOD_RESISTANCE] = true;
    isTriggerAura[SPELL_AURA_MOD_STEALTH] = true;
    isTriggerAura[SPELL_AURA_MOD_FEAR] = true;                              // Aura does not have charges but needs to be removed on trigger
    isTriggerAura[SPELL_AURA_MOD_ROOT] = true;
    isTriggerAura[SPELL_AURA_TRANSFORM] = true;
    isTriggerAura[SPELL_AURA_REFLECT_SPELLS] = true;
    isTriggerAura[SPELL_AURA_DAMAGE_IMMUNITY] = true;
    isTriggerAura[SPELL_AURA_PROC_TRIGGER_SPELL] = true;
    isTriggerAura[SPELL_AURA_PROC_TRIGGER_DAMAGE] = true;
    isTriggerAura[SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK] = true;
    isTriggerAura[SPELL_AURA_MOD_POWER_COST_SCHOOL_PCT] = true;
    isTriggerAura[SPELL_AURA_MOD_POWER_COST_SCHOOL] = true;
    isTriggerAura[SPELL_AURA_REFLECT_SPELLS_SCHOOL] = true;
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN] = true;
    isTriggerAura[SPELL_AURA_MOD_ATTACK_POWER] = true;
    isTriggerAura[SPELL_AURA_ADD_CASTER_HIT_TRIGGER] = true;
    isTriggerAura[SPELL_AURA_OVERRIDE_CLASS_SCRIPTS] = true;
    isTriggerAura[SPELL_AURA_MOD_MELEE_HASTE] = true;
    isTriggerAura[SPELL_AURA_MOD_ATTACKER_MELEE_HIT_CHANCE] = true;
    isTriggerAura[SPELL_AURA_RAID_PROC_FROM_CHARGE] = true;
    isTriggerAura[SPELL_AURA_RAID_PROC_FROM_CHARGE_WITH_VALUE] = true;
    isTriggerAura[SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE] = true;
    isTriggerAura[SPELL_AURA_MOD_SPELL_CRIT_CHANCE] = true;
    isTriggerAura[SPELL_AURA_ADD_FLAT_MODIFIER] = true;
    isTriggerAura[SPELL_AURA_ADD_PCT_MODIFIER] = true;
    isTriggerAura[SPELL_AURA_ABILITY_IGNORE_AURASTATE] = true;
    isTriggerAura[SPELL_AURA_MOD_INVISIBILITY] = true;
    isTriggerAura[SPELL_AURA_FORCE_REACTION] = true;
    isTriggerAura[SPELL_AURA_MOD_TAUNT] = true;
    isTriggerAura[SPELL_AURA_MOD_DETAUNT] = true;
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_PERCENT_DONE] = true;
    isTriggerAura[SPELL_AURA_MOD_ATTACK_POWER_PCT] = true;
    isTriggerAura[SPELL_AURA_MOD_HIT_CHANCE] = true;
    isTriggerAura[SPELL_AURA_MOD_WEAPON_CRIT_PERCENT] = true;
    isTriggerAura[SPELL_AURA_MOD_BLOCK_PERCENT] = true;

    isAlwaysTriggeredAura[SPELL_AURA_OVERRIDE_CLASS_SCRIPTS] = true;
    isAlwaysTriggeredAura[SPELL_AURA_MOD_STEALTH] = true;
    isAlwaysTriggeredAura[SPELL_AURA_MOD_CONFUSE] = true;
    isAlwaysTriggeredAura[SPELL_AURA_MOD_FEAR] = true;
    isAlwaysTriggeredAura[SPELL_AURA_MOD_ROOT] = true;
    isAlwaysTriggeredAura[SPELL_AURA_MOD_STUN] = true;
    isAlwaysTriggeredAura[SPELL_AURA_TRANSFORM] = true;
    isAlwaysTriggeredAura[SPELL_AURA_MOD_INVISIBILITY] = true;

    spellTypeMask[SPELL_AURA_MOD_STEALTH] = PROC_SPELL_TYPE_DAMAGE | PROC_SPELL_TYPE_NO_DMG_HEAL;
    spellTypeMask[SPELL_AURA_MOD_CONFUSE] = PROC_SPELL_TYPE_DAMAGE;
    spellTypeMask[SPELL_AURA_MOD_FEAR] = PROC_SPELL_TYPE_DAMAGE;
    spellTypeMask[SPELL_AURA_MOD_ROOT] = PROC_SPELL_TYPE_DAMAGE;
    spellTypeMask[SPELL_AURA_MOD_STUN] = PROC_SPELL_TYPE_DAMAGE;
    spellTypeMask[SPELL_AURA_TRANSFORM] = PROC_SPELL_TYPE_DAMAGE;
    spellTypeMask[SPELL_AURA_MOD_INVISIBILITY] = PROC_SPELL_TYPE_DAMAGE;

    // This generates default procs to retain compatibility with previous proc system
    TC_LOG_INFO("server.loading", "Generating spell proc data from SpellMap...");
    count = 0;
    oldMSTime = getMSTime();

    for (SpellInfo const* spellInfo : mSpellInfoMap)
    {
        if (!spellInfo)
            continue;

        // Data already present in DB, overwrites default proc
        if (mSpellProcMap.find(spellInfo->Id) != mSpellProcMap.end())
            continue;

        // Nothing to do if no flags set
        if (!spellInfo->ProcFlags)
            continue;

        bool addTriggerFlag = false;
        uint32 procSpellTypeMask = PROC_SPELL_TYPE_NONE;
        uint32 nonProcMask = 0;
        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
        {
            if (!spellEffectInfo.IsEffect())
                continue;

            uint32 auraName = spellEffectInfo.ApplyAuraName;
            if (!auraName)
                continue;

            if (!isTriggerAura[auraName])
            {
                // explicitly disable non proccing auras to avoid losing charges on self proc
                nonProcMask |= 1 << spellEffectInfo.EffectIndex;
                continue;
            }

            procSpellTypeMask |= spellTypeMask[auraName];
            if (isAlwaysTriggeredAura[auraName])
                addTriggerFlag = true;

            // many proc auras with taken procFlag mask don't have attribute "can proc with triggered"
            // they should proc nevertheless (example mage armor spells with judgement)
            if (!addTriggerFlag && (spellInfo->ProcFlags & TAKEN_HIT_PROC_FLAG_MASK) != 0)
            {
                switch (auraName)
                {
                    case SPELL_AURA_PROC_TRIGGER_SPELL:
                    case SPELL_AURA_PROC_TRIGGER_DAMAGE:
                        addTriggerFlag = true;
                        break;
                    default:
                        break;
                }
            }
        }

        if (!procSpellTypeMask)
        {
            for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
            {
                if (spellEffectInfo.IsAura())
                {
                    TC_LOG_ERROR("sql.sql", "Spell Id {} has DBC ProcFlags {}, but it's of non-proc aura type, it probably needs an entry in `spell_proc` table to be handled correctly.", spellInfo->Id, spellInfo->ProcFlags);
                    break;
                }
            }

            continue;
        }

        SpellProcEntry procEntry;
        procEntry.SchoolMask      = 0;
        procEntry.ProcFlags = spellInfo->ProcFlags;
        procEntry.SpellFamilyName = 0;
        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
            if (spellEffectInfo.IsEffect() && isTriggerAura[spellEffectInfo.ApplyAuraName])
                procEntry.SpellFamilyMask |= spellEffectInfo.SpellClassMask;

        if (procEntry.SpellFamilyMask)
            procEntry.SpellFamilyName = spellInfo->SpellFamilyName;

        procEntry.SpellTypeMask   = procSpellTypeMask;
        procEntry.SpellPhaseMask  = PROC_SPELL_PHASE_HIT;
        procEntry.HitMask         = PROC_HIT_NONE; // uses default proc @see SpellMgr::CanSpellTriggerProcOnEvent

        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
        {
            if (!spellEffectInfo.IsAura())
                continue;

            switch (spellEffectInfo.ApplyAuraName)
            {
                // Reflect auras should only proc off reflects
                case SPELL_AURA_REFLECT_SPELLS:
                case SPELL_AURA_REFLECT_SPELLS_SCHOOL:
                    procEntry.HitMask = PROC_HIT_REFLECT;
                    break;
                // Only drop charge on crit
                case SPELL_AURA_MOD_WEAPON_CRIT_PERCENT:
                    procEntry.HitMask = PROC_HIT_CRITICAL;
                    break;
                // Only drop charge on block
                case SPELL_AURA_MOD_BLOCK_PERCENT:
                    procEntry.HitMask = PROC_HIT_BLOCK;
                    break;
                // proc auras with another aura reducing hit chance (eg 63767) only proc on missed attack
                case SPELL_AURA_MOD_HIT_CHANCE:
                    if (spellEffectInfo.CalcValue() <= -100)
                        procEntry.HitMask = PROC_HIT_MISS;
                    break;
                default:
                    continue;
            }
            break;
        }

        procEntry.AttributesMask  = 0;
        procEntry.DisableEffectsMask = nonProcMask;
        if (spellInfo->ProcFlags & PROC_FLAG_KILL)
            procEntry.AttributesMask |= PROC_ATTR_REQ_EXP_OR_HONOR;
        if (addTriggerFlag)
            procEntry.AttributesMask |= PROC_ATTR_TRIGGERED_CAN_PROC;

        procEntry.ProcsPerMinute  = 0;
        procEntry.Chance          = spellInfo->ProcChance;
        procEntry.Cooldown        = Milliseconds::zero();
        procEntry.Charges         = spellInfo->ProcCharges;

        mSpellProcMap[spellInfo->Id] = procEntry;
        ++count;
    }

    TC_LOG_INFO("server.loading", ">> Generated spell proc data for {} spells in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellBonuses()
{
    uint32 oldMSTime = getMSTime();

    mSpellBonusMap.clear();                             // need for reload case

    //                                                0      1             2          3         4
    QueryResult result = WorldDatabase.Query("SELECT entry, direct_bonus, dot_bonus, ap_bonus, ap_dot_bonus FROM spell_bonus_data");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell bonus data. DB table `spell_bonus_data` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 entry = fields[0].GetUInt32();

        SpellInfo const* spell = GetSpellInfo(entry);
        if (!spell)
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_bonus_data` does not exist.", entry);
            continue;
        }

        SpellBonusEntry& sbe = mSpellBonusMap[entry];
        sbe.direct_damage = fields[1].GetFloat();
        sbe.dot_damage    = fields[2].GetFloat();
        sbe.ap_bonus      = fields[3].GetFloat();
        sbe.ap_dot_bonus   = fields[4].GetFloat();

        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} extra spell bonus data in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellThreats()
{
    uint32 oldMSTime = getMSTime();

    mSpellThreatMap.clear();                                // need for reload case

    //                                                0      1        2       3
    QueryResult result = WorldDatabase.Query("SELECT entry, flatMod, pctMod, apPctMod FROM spell_threat");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 aggro generating spells. DB table `spell_threat` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 entry = fields[0].GetUInt32();

        if (!GetSpellInfo(entry))
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_threat` does not exist.", entry);
            continue;
        }

        SpellThreatEntry ste;
        ste.flatMod  = fields[1].GetInt32();
        ste.pctMod   = fields[2].GetFloat();
        ste.apPctMod = fields[3].GetFloat();

        mSpellThreatMap[entry] = ste;
        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} SpellThreatEntries in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSkillLineAbilityMap()
{
    uint32 oldMSTime = getMSTime();

    mSkillLineAbilityMap.clear();

    uint32 count = 0;

    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        SkillLineAbilityEntry const* SkillInfo = sSkillLineAbilityStore.LookupEntry(i);
        if (!SkillInfo)
            continue;

        mSkillLineAbilityMap.insert(SkillLineAbilityMap::value_type(SkillInfo->Spell, SkillInfo));
        ++count;
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} SkillLineAbility MultiMap Data in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellPetAuras()
{
    uint32 oldMSTime = getMSTime();

    mSpellPetAuraMap.clear();                                  // need for reload case

    //                                                  0       1       2    3
    QueryResult result = WorldDatabase.Query("SELECT spell, effectId, pet, aura FROM spell_pet_auras");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell pet auras. DB table `spell_pet_auras` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 spell = fields[0].GetUInt32();
        SpellEffIndex eff = SpellEffIndex(fields[1].GetUInt8());
        uint32 pet = fields[2].GetUInt32();
        uint32 aura = fields[3].GetUInt32();

        SpellPetAuraMap::iterator itr = mSpellPetAuraMap.find((spell << 8) + eff);
        if (itr != mSpellPetAuraMap.end())
            itr->second.AddAura(pet, aura);
        else
        {
            SpellInfo const* spellInfo = GetSpellInfo(spell);
            if (!spellInfo)
            {
                TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_pet_auras` does not exist.", spell);
                continue;
            }
            if (spellInfo->GetEffect(eff).Effect != SPELL_EFFECT_DUMMY &&
               (spellInfo->GetEffect(eff).Effect != SPELL_EFFECT_APPLY_AURA ||
                spellInfo->GetEffect(eff).ApplyAuraName != SPELL_AURA_DUMMY))
            {
                TC_LOG_ERROR("spells", "The spell {} listed in `spell_pet_auras` does not have any dummy aura or dummy effect.", spell);
                continue;
            }

            SpellInfo const* spellInfo2 = GetSpellInfo(aura);
            if (!spellInfo2)
            {
                TC_LOG_ERROR("sql.sql", "The aura {} listed in `spell_pet_auras` does not exist.", aura);
                continue;
            }

            PetAura pa(pet, aura, spellInfo->GetEffect(eff).TargetA.GetTarget() == TARGET_UNIT_PET, spellInfo->GetEffect(eff).CalcValue());
            mSpellPetAuraMap[(spell<<8) + eff] = pa;
        }

        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} spell pet auras in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

// Fill custom data about enchancments
void SpellMgr::LoadEnchantCustomAttr()
{
    uint32 oldMSTime = getMSTime();

    uint32 size = sSpellItemEnchantmentStore.GetNumRows();
    mEnchantCustomAttr.resize(size, false);

    uint32 count = 0;
    for (uint32 i = 0; i < GetSpellInfoStoreSize(); ++i)
    {
        SpellInfo const* spellInfo = GetSpellInfo(i);
        if (!spellInfo)
            continue;

        /// @todo find a better check
        if (!spellInfo->HasAttribute(SPELL_ATTR2_PRESERVE_ENCHANT_IN_ARENA) || !spellInfo->HasAttribute(SPELL_ATTR0_NOT_SHAPESHIFT))
            continue;

        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
        {
            if (spellEffectInfo.Effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
            {
                uint32 enchId = spellEffectInfo.MiscValue;
                SpellItemEnchantmentEntry const* ench = sSpellItemEnchantmentStore.LookupEntry(enchId);
                if (!ench)
                    continue;
                mEnchantCustomAttr[enchId] = true;
                ++count;
                break;
            }
        }
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} custom enchant attributes in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellEnchantProcData()
{
    uint32 oldMSTime = getMSTime();

    mSpellEnchantProcEventMap.clear();                             // need for reload case

    //                                                       0       1               2        3               4
    QueryResult result = WorldDatabase.Query("SELECT EnchantID, Chance, ProcsPerMinute, HitMask, AttributesMask FROM spell_enchant_proc_data");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell enchant proc event conditions. DB table `spell_enchant_proc_data` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 enchantId = fields[0].GetUInt32();

        SpellItemEnchantmentEntry const* ench = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!ench)
        {
            TC_LOG_ERROR("sql.sql", "The enchancment {} listed in `spell_enchant_proc_data` does not exist.", enchantId);
            continue;
        }

        SpellEnchantProcEntry spe;
        spe.Chance = fields[1].GetFloat();
        spe.ProcsPerMinute = fields[2].GetFloat();
        spe.HitMask = fields[3].GetUInt32();
        spe.AttributesMask = fields[4].GetUInt32();

        mSpellEnchantProcEventMap[enchantId] = spe;

        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} enchant proc data definitions in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellLinked()
{
    uint32 oldMSTime = getMSTime();

    mSpellLinkedMap.clear();    // need for reload case

    //                                                0              1             2
    QueryResult result = WorldDatabase.Query("SELECT spell_trigger, spell_effect, type FROM spell_linked_spell");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 linked spells. DB table `spell_linked_spell` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        int32 trigger = fields[0].GetInt32();
        int32 effect = fields[1].GetInt32();
        int32 type = fields[2].GetUInt8();

        SpellInfo const* spellInfo = GetSpellInfo(abs(trigger));
        if (!spellInfo)
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_linked_spell` does not exist.", abs(trigger));
            continue;
        }

        if (effect >= 0)
            for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
            {
                if (spellEffectInfo.CalcValue() == abs(effect))
                    TC_LOG_ERROR("sql.sql", "The spell {} Effect: {} listed in `spell_linked_spell` has same bp{} like effect (possible hack).", abs(trigger), abs(effect), uint32(spellEffectInfo.EffectIndex));
            }

        spellInfo = GetSpellInfo(abs(effect));
        if (!spellInfo)
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_linked_spell` does not exist.", abs(effect));
            continue;
        }

        if (type) //we will find a better way when more types are needed
        {
            if (trigger > 0)
                trigger += SPELL_LINKED_MAX_SPELLS * type;
            else
                trigger -= SPELL_LINKED_MAX_SPELLS * type;
        }
        mSpellLinkedMap[trigger].push_back(effect);

        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} linked spells in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadPetLevelupSpellMap()
{
    uint32 oldMSTime = getMSTime();

    mPetLevelupSpellMap.clear();                                   // need for reload case

    uint32 count = 0;
    uint32 family_count = 0;

    for (uint32 i = 0; i < sCreatureFamilyStore.GetNumRows(); ++i)
    {
        CreatureFamilyEntry const* creatureFamily = sCreatureFamilyStore.LookupEntry(i);
        if (!creatureFamily)                                     // not exist
            continue;

        for (uint8 j = 0; j < 2; ++j)
        {
            if (!creatureFamily->SkillLine[j])
                continue;

            std::vector<SkillLineAbilityEntry const*> const* skillLineAbilities = GetSkillLineAbilitiesBySkill(creatureFamily->SkillLine[j]);
            if (!skillLineAbilities)
                continue;

            for (SkillLineAbilityEntry const* skillLine : *skillLineAbilities)
            {
                if (skillLine->AcquireMethod != SKILL_LINE_ABILITY_LEARNED_ON_SKILL_LEARN)
                    continue;

                SpellInfo const* spell = GetSpellInfo(skillLine->Spell);
                if (!spell) // not exist or triggered or talent
                    continue;

                if (!spell->SpellLevel)
                    continue;

                PetLevelupSpellSet& spellSet = mPetLevelupSpellMap[creatureFamily->ID];
                if (spellSet.empty())
                    ++family_count;

                spellSet.insert(PetLevelupSpellSet::value_type(spell->SpellLevel, spell->Id));
                ++count;
            }
        }
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} pet levelup and default spells for {} families in {} ms", count, family_count, GetMSTimeDiffToNow(oldMSTime));
}

bool LoadPetDefaultSpells_helper(CreatureTemplate const* cInfo, PetDefaultSpellsEntry& petDefSpells)
{
    // skip empty list;
    bool have_spell = false;
    for (uint8 j = 0; j < MAX_CREATURE_SPELL_DATA_SLOT; ++j)
    {
        if (petDefSpells.spellid[j])
        {
            have_spell = true;
            break;
        }
    }
    if (!have_spell)
        return false;

    // remove duplicates with levelupSpells if any
    if (PetLevelupSpellSet const* levelupSpells = cInfo->family ? sSpellMgr->GetPetLevelupSpellList(cInfo->family) : nullptr)
    {
        for (uint8 j = 0; j < MAX_CREATURE_SPELL_DATA_SLOT; ++j)
        {
            if (!petDefSpells.spellid[j])
                continue;

            for (PetLevelupSpellSet::const_iterator itr = levelupSpells->begin(); itr != levelupSpells->end(); ++itr)
            {
                if (itr->second == petDefSpells.spellid[j])
                {
                    petDefSpells.spellid[j] = 0;
                    break;
                }
            }
        }
    }

    // skip empty list;
    have_spell = false;
    for (uint8 j = 0; j < MAX_CREATURE_SPELL_DATA_SLOT; ++j)
    {
        if (petDefSpells.spellid[j])
        {
            have_spell = true;
            break;
        }
    }

    return have_spell;
}

void SpellMgr::LoadPetDefaultSpells()
{
    uint32 oldMSTime = getMSTime();

    mPetDefaultSpellsMap.clear();

    uint32 countCreature = 0;
    uint32 countData = 0;

    CreatureTemplateContainer const& ctc = sObjectMgr->GetCreatureTemplates();
    for (auto const& creatureTemplatePair : ctc)
    {
        if (!creatureTemplatePair.second.PetSpellDataId)
            continue;

        // for creature with PetSpellDataId get default pet spells from dbc
        CreatureSpellDataEntry const* spellDataEntry = sCreatureSpellDataStore.LookupEntry(creatureTemplatePair.second.PetSpellDataId);
        if (!spellDataEntry)
            continue;

        int32 petSpellsId = -int32(creatureTemplatePair.second.PetSpellDataId);
        PetDefaultSpellsEntry petDefSpells;
        for (uint8 j = 0; j < MAX_CREATURE_SPELL_DATA_SLOT; ++j)
            petDefSpells.spellid[j] = spellDataEntry->Spells[j];

        if (LoadPetDefaultSpells_helper(&creatureTemplatePair.second, petDefSpells))
        {
            mPetDefaultSpellsMap[petSpellsId] = petDefSpells;
            ++countData;
        }
    }

    TC_LOG_INFO("server.loading", ">> Loaded addition spells for {} pet spell data entries in {} ms", countData, GetMSTimeDiffToNow(oldMSTime));

    TC_LOG_INFO("server.loading", "Loading summonable creature templates...");
    oldMSTime = getMSTime();

    // different summon spells
    for (uint32 i = 0; i < GetSpellInfoStoreSize(); ++i)
    {
        SpellInfo const* spellEntry = GetSpellInfo(i);
        if (!spellEntry)
            continue;

        for (SpellEffectInfo const& spellEffectInfo : spellEntry->GetEffects())
        {
            if (spellEffectInfo.IsEffect(SPELL_EFFECT_SUMMON) || spellEffectInfo.IsEffect(SPELL_EFFECT_SUMMON_PET))
            {
                uint32 creature_id = spellEffectInfo.MiscValue;
                CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(creature_id);
                if (!cInfo)
                    continue;

                // already loaded
                if (cInfo->PetSpellDataId)
                    continue;

                // for creature without PetSpellDataId get default pet spells from creature_template
                int32 petSpellsId = cInfo->Entry;
                if (mPetDefaultSpellsMap.find(cInfo->Entry) != mPetDefaultSpellsMap.end())
                    continue;

                PetDefaultSpellsEntry petDefSpells;
                for (uint8 j = 0; j < MAX_CREATURE_SPELL_DATA_SLOT; ++j)
                    petDefSpells.spellid[j] = cInfo->spells[j];

                if (LoadPetDefaultSpells_helper(cInfo, petDefSpells))
                {
                    mPetDefaultSpellsMap[petSpellsId] = petDefSpells;
                    ++countCreature;
                }
            }
        }
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} summonable creature templates in {} ms", countCreature, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellAreas()
{
    uint32 oldMSTime = getMSTime();

    mSpellAreaMap.clear();                                  // need for reload case
    mSpellAreaForAreaMap.clear();
    mSpellAreaForQuestMap.clear();
    mSpellAreaForQuestEndMap.clear();
    mSpellAreaForAuraMap.clear();

    //                                                  0     1         2              3               4                 5          6          7       8         9
    QueryResult result = WorldDatabase.Query("SELECT spell, area, quest_start, quest_start_status, quest_end_status, quest_end, aura_spell, racemask, gender, autocast FROM spell_area");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell area requirements. DB table `spell_area` is empty.");

        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 spell = fields[0].GetUInt32();
        SpellArea spellArea;
        spellArea.spellId             = spell;
        spellArea.areaId              = fields[1].GetUInt32();
        spellArea.questStart          = fields[2].GetUInt32();
        spellArea.questStartStatus    = fields[3].GetUInt32();
        spellArea.questEndStatus      = fields[4].GetUInt32();
        spellArea.questEnd            = fields[5].GetUInt32();
        spellArea.auraSpell           = fields[6].GetInt32();
        spellArea.raceMask            = fields[7].GetUInt32();
        spellArea.gender              = Gender(fields[8].GetUInt8());
        spellArea.autocast            = fields[9].GetBool();

        if (SpellInfo const* spellInfo = GetSpellInfo(spell))
        {
            if (spellArea.autocast)
                const_cast<SpellInfo*>(spellInfo)->Attributes |= SPELL_ATTR0_CANT_CANCEL;
        }
        else
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` does not exist", spell);
            continue;
        }

        {
            bool ok = true;
            SpellAreaMapBounds sa_bounds = GetSpellAreaMapBounds(spellArea.spellId);
            for (SpellAreaMap::const_iterator itr = sa_bounds.first; itr != sa_bounds.second; ++itr)
            {
                if (spellArea.spellId != itr->second.spellId)
                    continue;
                if (spellArea.areaId != itr->second.areaId)
                    continue;
                if (spellArea.questStart != itr->second.questStart)
                    continue;
                if (spellArea.auraSpell != itr->second.auraSpell)
                    continue;
                if ((spellArea.raceMask & itr->second.raceMask) == 0)
                    continue;
                if (spellArea.gender != itr->second.gender)
                    continue;

                // duplicate by requirements
                ok = false;
                break;
            }

            if (!ok)
            {
                TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` is already listed with similar requirements.", spell);
                continue;
            }
        }

        if (spellArea.areaId && !sAreaTableStore.LookupEntry(spellArea.areaId))
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` has a wrong area ({}) requirement.", spell, spellArea.areaId);
            continue;
        }

        if (spellArea.questStart && !sObjectMgr->GetQuestTemplate(spellArea.questStart))
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` has a wrong start quest ({}) requirement.", spell, spellArea.questStart);
            continue;
        }

        if (spellArea.questEnd)
        {
            if (!sObjectMgr->GetQuestTemplate(spellArea.questEnd))
            {
                TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` has a wrong ending quest ({}) requirement.", spell, spellArea.questEnd);
                continue;
            }
        }

        if (spellArea.auraSpell)
        {
            SpellInfo const* spellInfo = GetSpellInfo(abs(spellArea.auraSpell));
            if (!spellInfo)
            {
                TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` has wrong aura spell ({}) requirement", spell, abs(spellArea.auraSpell));
                continue;
            }

            if (uint32(abs(spellArea.auraSpell)) == spellArea.spellId)
            {
                TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` has aura spell ({}) requirement for itself", spell, abs(spellArea.auraSpell));
                continue;
            }

            // not allow autocast chains by auraSpell field (but allow use as alternative if not present)
            if (spellArea.autocast && spellArea.auraSpell > 0)
            {
                bool chain = false;
                SpellAreaForAuraMapBounds saBound = GetSpellAreaForAuraMapBounds(spellArea.spellId);
                for (SpellAreaForAuraMap::const_iterator itr = saBound.first; itr != saBound.second; ++itr)
                {
                    if (itr->second->autocast && itr->second->auraSpell > 0)
                    {
                        chain = true;
                        break;
                    }
                }

                if (chain)
                {
                    TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` has the aura spell ({}) requirement that it autocasts itself from the aura.", spell, spellArea.auraSpell);
                    continue;
                }

                SpellAreaMapBounds saBound2 = GetSpellAreaMapBounds(spellArea.auraSpell);
                for (SpellAreaMap::const_iterator itr2 = saBound2.first; itr2 != saBound2.second; ++itr2)
                {
                    if (itr2->second.autocast && itr2->second.auraSpell > 0)
                    {
                        chain = true;
                        break;
                    }
                }

                if (chain)
                {
                    TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` has the aura spell ({}) requirement that the spell itself autocasts from the aura.", spell, spellArea.auraSpell);
                    continue;
                }
            }
        }

        if (spellArea.raceMask && (spellArea.raceMask & RACEMASK_ALL_PLAYABLE) == 0)
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` has wrong race mask ({}) requirement.", spell, spellArea.raceMask);
            continue;
        }

        if (spellArea.gender != GENDER_NONE && spellArea.gender != GENDER_FEMALE && spellArea.gender != GENDER_MALE)
        {
            TC_LOG_ERROR("sql.sql", "The spell {} listed in `spell_area` has wrong gender ({}) requirement.", spell, spellArea.gender);
            continue;
        }

        SpellArea const* sa = &mSpellAreaMap.insert(SpellAreaMap::value_type(spell, spellArea))->second;

        // for search by current zone/subzone at zone/subzone change
        if (spellArea.areaId)
            mSpellAreaForAreaMap.insert(SpellAreaForAreaMap::value_type(spellArea.areaId, sa));

        // for search at quest update checks
        if (spellArea.questStart || spellArea.questEnd)
        {
            if (spellArea.questStart == spellArea.questEnd)
                mSpellAreaForQuestMap.insert(SpellAreaForQuestMap::value_type(spellArea.questStart, sa));
            else
            {
                if (spellArea.questStart)
                    mSpellAreaForQuestMap.insert(SpellAreaForQuestMap::value_type(spellArea.questStart, sa));
                if (spellArea.questEnd)
                    mSpellAreaForQuestMap.insert(SpellAreaForQuestMap::value_type(spellArea.questEnd, sa));
            }
        }

        // for search at quest start/reward
        if (spellArea.questEnd)
            mSpellAreaForQuestEndMap.insert(SpellAreaForQuestMap::value_type(spellArea.questEnd, sa));

        // for search at aura apply
        if (spellArea.auraSpell)
            mSpellAreaForAuraMap.insert(SpellAreaForAuraMap::value_type(abs(spellArea.auraSpell), sa));

        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} spell area requirements in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellInfoStore()
{
    uint32 oldMSTime = getMSTime();

    UnloadSpellInfoStore();
    mSpellInfoMap.resize(sSpellStore.GetNumRows(), nullptr);

    for (SpellEntry const* spellEntry : sSpellStore)
        mSpellInfoMap[spellEntry->ID] = new SpellInfo(spellEntry);

    for (uint32 spellIndex = 0; spellIndex < GetSpellInfoStoreSize(); ++spellIndex)
    {
        if (!mSpellInfoMap[spellIndex])
            continue;

        for (SpellEffectInfo const& spellEffectInfo : mSpellInfoMap[spellIndex]->GetEffects())
        {
            //ASSERT(effect.EffectIndex < MAX_SPELL_EFFECTS, "MAX_SPELL_EFFECTS must be at least %u", effect.EffectIndex + 1);
            ASSERT(spellEffectInfo.Effect < TOTAL_SPELL_EFFECTS, "TOTAL_SPELL_EFFECTS must be at least %u", spellEffectInfo.Effect + 1);
            ASSERT(spellEffectInfo.ApplyAuraName < TOTAL_AURAS, "TOTAL_AURAS must be at least %u", spellEffectInfo.ApplyAuraName + 1);
            ASSERT(spellEffectInfo.TargetA.GetTarget() < TOTAL_SPELL_TARGETS, "TOTAL_SPELL_TARGETS must be at least %u", spellEffectInfo.TargetA.GetTarget() + 1);
            ASSERT(spellEffectInfo.TargetB.GetTarget() < TOTAL_SPELL_TARGETS, "TOTAL_SPELL_TARGETS must be at least %u", spellEffectInfo.TargetB.GetTarget() + 1);
        }
    }

    TC_LOG_INFO("server.loading", ">> Loaded SpellInfo store in {} ms", GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::UnloadSpellInfoStore()
{
    for (uint32 i = 0; i < GetSpellInfoStoreSize(); ++i)
        delete mSpellInfoMap[i];

    mSpellInfoMap.clear();
}

void SpellMgr::UnloadSpellInfoImplicitTargetConditionLists()
{
    for (uint32 i = 0; i < GetSpellInfoStoreSize(); ++i)
        if (mSpellInfoMap[i])
            mSpellInfoMap[i]->_UnloadImplicitTargetConditionLists();
}

void SpellMgr::LoadSpellInfoCustomAttributes()
{
    uint32 oldMSTime = getMSTime();
    uint32 oldMSTime2 = oldMSTime;

    QueryResult result = WorldDatabase.Query("SELECT entry, attributes FROM spell_custom_attr");

    if (!result)
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell custom attributes from DB. DB table `spell_custom_attr` is empty.");
    else
    {
        uint32 count = 0;
        do
        {
            Field* fields = result->Fetch();

            uint32 spellId = fields[0].GetUInt32();
            uint32 attributes = fields[1].GetUInt32();

            SpellInfo* spellInfo = _GetSpellInfo(spellId);
            if (!spellInfo)
            {
                TC_LOG_ERROR("sql.sql", "Table `spell_custom_attr` has wrong spell (entry: {}), ignored.", spellId);
                continue;
            }

            if ((attributes & SPELL_ATTR0_CU_NEGATIVE) != 0)
            {
                for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
                {
                    if (spellEffectInfo.IsEffect())
                        continue;

                    if ((attributes & (SPELL_ATTR0_CU_NEGATIVE_EFF0 << spellEffectInfo.EffectIndex)) != 0)
                    {
                        TC_LOG_ERROR("sql.sql", "Table `spell_custom_attr` has attribute SPELL_ATTR0_CU_NEGATIVE_EFF{} for spell {} with no EFFECT_{}", uint32(spellEffectInfo.EffectIndex), spellId, uint32(spellEffectInfo.EffectIndex));
                        continue;
                    }
                }
            }

            spellInfo->AttributesCu |= attributes;
            ++count;
        } while (result->NextRow());

        TC_LOG_INFO("server.loading", ">> Loaded {} spell custom attributes from DB in {} ms", count, GetMSTimeDiffToNow(oldMSTime2));
    }

    for (SpellInfo* spellInfo : mSpellInfoMap)
    {
        if (!spellInfo)
            continue;

        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
        {
            // all bleed effects and spells ignore armor
            if (spellInfo->GetEffectMechanicMask(spellEffectInfo.EffectIndex) & (1 << MECHANIC_BLEED))
                spellInfo->AttributesCu |= SPELL_ATTR0_CU_IGNORE_ARMOR;

            switch (spellEffectInfo.ApplyAuraName)
            {
                case SPELL_AURA_MOD_POSSESS:
                case SPELL_AURA_MOD_CONFUSE:
                case SPELL_AURA_MOD_CHARM:
                case SPELL_AURA_AOE_CHARM:
                case SPELL_AURA_MOD_FEAR:
                case SPELL_AURA_MOD_STUN:
                    spellInfo->AttributesCu |= SPELL_ATTR0_CU_AURA_CC;
                    break;
                default:
                    break;
            }

            switch (spellEffectInfo.ApplyAuraName)
            {
                case SPELL_AURA_CONVERT_RUNE:   // Can't be saved - aura handler relies on calculated amount and changes it
                case SPELL_AURA_OPEN_STABLE:    // No point in saving this, since the stable dialog can't be open on aura load anyway.
                // Auras that require both caster & target to be in world cannot be saved
                case SPELL_AURA_CONTROL_VEHICLE:
                case SPELL_AURA_BIND_SIGHT:
                case SPELL_AURA_MOD_POSSESS:
                case SPELL_AURA_MOD_POSSESS_PET:
                case SPELL_AURA_MOD_CHARM:
                case SPELL_AURA_AOE_CHARM:
                    spellInfo->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
                    break;
                default:
                    break;
            }

            switch (spellEffectInfo.Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_HEALTH_LEECH:
                case SPELL_EFFECT_HEAL:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_POWER_BURN:
                case SPELL_EFFECT_HEAL_MECHANICAL:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                case SPELL_EFFECT_HEAL_PCT:
                    spellInfo->AttributesCu |= SPELL_ATTR0_CU_CAN_CRIT;
                    break;
                default:
                    break;
            }

            switch (spellEffectInfo.Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                case SPELL_EFFECT_HEAL:
                    spellInfo->AttributesCu |= SPELL_ATTR0_CU_DIRECT_DAMAGE;
                    break;
                case SPELL_EFFECT_POWER_DRAIN:
                case SPELL_EFFECT_POWER_BURN:
                case SPELL_EFFECT_HEAL_MAX_HEALTH:
                case SPELL_EFFECT_HEALTH_LEECH:
                case SPELL_EFFECT_HEAL_PCT:
                case SPELL_EFFECT_ENERGIZE_PCT:
                case SPELL_EFFECT_ENERGIZE:
                case SPELL_EFFECT_HEAL_MECHANICAL:
                    spellInfo->AttributesCu |= SPELL_ATTR0_CU_NO_INITIAL_THREAT;
                    break;
                case SPELL_EFFECT_CHARGE:
                case SPELL_EFFECT_CHARGE_DEST:
                case SPELL_EFFECT_JUMP:
                case SPELL_EFFECT_JUMP_DEST:
                case SPELL_EFFECT_LEAP_BACK:
                    spellInfo->AttributesCu |= SPELL_ATTR0_CU_CHARGE;
                    break;
                case SPELL_EFFECT_PICKPOCKET:
                    spellInfo->AttributesCu |= SPELL_ATTR0_CU_PICKPOCKET;
                    break;
                case SPELL_EFFECT_ENCHANT_ITEM:
                case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
                case SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC:
                case SPELL_EFFECT_ENCHANT_HELD_ITEM:
                {
                    // only enchanting profession enchantments procs can stack
                    if (IsPartOfSkillLine(SKILL_ENCHANTING, spellInfo->Id))
                    {
                        uint32 enchantId = spellEffectInfo.MiscValue;
                        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
                        if (!enchant)
                            break;

                        for (uint8 s = 0; s < MAX_ITEM_ENCHANTMENT_EFFECTS; ++s)
                        {
                            if (enchant->Effect[s] != ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL)
                                continue;

                            SpellInfo* procInfo = _GetSpellInfo(enchant->EffectArg[s]);
                            if (!procInfo)
                                continue;

                            // if proced directly from enchantment, not via proc aura
                            // NOTE: Enchant Weapon - Blade Ward also has proc aura spell and is proced directly
                            // however its not expected to stack so this check is good
                            if (procInfo->HasAura(SPELL_AURA_PROC_TRIGGER_SPELL))
                                continue;

                            procInfo->AttributesCu |= SPELL_ATTR0_CU_ENCHANT_PROC;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // spells ignoring hit result should not be binary
        if (!spellInfo->HasAttribute(SPELL_ATTR3_IGNORE_HIT_RESULT))
        {
            bool setFlag = false;
            for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
            {
                if (spellEffectInfo.IsEffect())
                {
                    switch (spellEffectInfo.Effect)
                    {
                        case SPELL_EFFECT_SCHOOL_DAMAGE:
                        case SPELL_EFFECT_WEAPON_DAMAGE:
                        case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                        case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                        case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                        case SPELL_EFFECT_TRIGGER_SPELL:
                        case SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE:
                            break;
                        case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                        case SPELL_EFFECT_APPLY_AURA:
                        case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                        case SPELL_EFFECT_APPLY_AREA_AURA_RAID:
                        case SPELL_EFFECT_APPLY_AREA_AURA_FRIEND:
                        case SPELL_EFFECT_APPLY_AREA_AURA_ENEMY:
                        case SPELL_EFFECT_APPLY_AREA_AURA_PET:
                        case SPELL_EFFECT_APPLY_AREA_AURA_OWNER:
                        {
                            if (spellEffectInfo.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE ||
                                spellEffectInfo.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE_PERCENT ||
                                spellEffectInfo.ApplyAuraName == SPELL_AURA_DUMMY ||
                                spellEffectInfo.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH ||
                                spellEffectInfo.ApplyAuraName == SPELL_AURA_PERIODIC_HEALTH_FUNNEL ||
                                spellEffectInfo.ApplyAuraName == SPELL_AURA_PERIODIC_DUMMY)
                                break;
                            [[fallthrough]];
                        }
                        default:
                        {
                            // No value and not interrupt cast or crowd control without SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY flag
                            if (!spellEffectInfo.CalcValue() && !((spellEffectInfo.Effect == SPELL_EFFECT_INTERRUPT_CAST || spellInfo->HasAttribute(SPELL_ATTR0_CU_AURA_CC)) && !spellInfo->HasAttribute(SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY)))
                                break;

                            // Sindragosa Frost Breath
                            if (spellInfo->Id == 69649 || spellInfo->Id == 71056 || spellInfo->Id == 71057 || spellInfo->Id == 71058 || spellInfo->Id == 73061 || spellInfo->Id == 73062 || spellInfo->Id == 73063 || spellInfo->Id == 73064)
                                break;

                            // Frostbolt
                            if (spellInfo->SpellFamilyName == SPELLFAMILY_MAGE && (spellInfo->SpellFamilyFlags[0] & 0x20))
                                break;

                            // Frost Fever
                            if (spellInfo->Id == 55095)
                                break;

                            // Haunt
                            if (spellInfo->SpellFamilyName == SPELLFAMILY_WARLOCK && (spellInfo->SpellFamilyFlags[1] & 0x40000))
                                break;

                            setFlag = true;
                            break;
                        }
                    }

                    if (setFlag)
                    {
                        spellInfo->AttributesCu |= SPELL_ATTR0_CU_BINARY_SPELL;
                        break;
                    }
                }
            }
        }

        // Remove normal school mask to properly calculate damage
        if ((spellInfo->SchoolMask & SPELL_SCHOOL_MASK_NORMAL) && (spellInfo->SchoolMask & SPELL_SCHOOL_MASK_MAGIC))
        {
            spellInfo->SchoolMask &= ~SPELL_SCHOOL_MASK_NORMAL;
            spellInfo->AttributesCu |= SPELL_ATTR0_CU_SCHOOLMASK_NORMAL_WITH_MAGIC;
        }

        spellInfo->_InitializeSpellPositivity();

        if (spellInfo->SpellVisual[0] == 3879)
            spellInfo->AttributesCu |= SPELL_ATTR0_CU_CONE_BACK;

        switch (spellInfo->SpellFamilyName)
        {
            case SPELLFAMILY_WARRIOR:
                // Shout / Piercing Howl
                if (spellInfo->SpellFamilyFlags[0] & 0x20000/* || spellInfo->SpellFamilyFlags[1] & 0x20*/)
                    spellInfo->AttributesCu |= SPELL_ATTR0_CU_AURA_CC;
                break;
            case SPELLFAMILY_DRUID:
                // Roar
                if (spellInfo->SpellFamilyFlags[0] & 0x8)
                    spellInfo->AttributesCu |= SPELL_ATTR0_CU_AURA_CC;
                break;
            case SPELLFAMILY_GENERIC:
                // Stoneclaw Totem effect
                if (spellInfo->Id == 5729)
                    spellInfo->AttributesCu |= SPELL_ATTR0_CU_AURA_CC;
                break;
            default:
                break;
        }

        spellInfo->_InitializeExplicitTargetMask();

        if (spellInfo->Speed > 0.0f)
            if (SpellVisualEntry const* spellVisual = sSpellVisualStore.LookupEntry(spellInfo->SpellVisual[0]))
                if (spellVisual->HasMissile)
                    if (spellVisual->MissileModel == -4 || spellVisual->MissileModel == -5)
                        spellInfo->AttributesCu |= SPELL_ATTR0_CU_NEEDS_AMMO_DATA;

    }

    // addition for binary spells, omit spells triggering other spells
    for (SpellInfo* spellInfo : mSpellInfoMap)
    {
        if (!spellInfo)
            continue;

        if (spellInfo->HasAttribute(SPELL_ATTR0_CU_BINARY_SPELL))
            continue;

        bool allNonBinary = true;
        bool overrideAttr = false;
        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
        {
            if (spellEffectInfo.IsAura() && spellEffectInfo.TriggerSpell)
            {
                switch (spellEffectInfo.ApplyAuraName)
                {
                    case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                    case SPELL_AURA_PERIODIC_TRIGGER_SPELL_FROM_CLIENT:
                    case SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE:
                        if (SpellInfo const* triggerSpell = sSpellMgr->GetSpellInfo(spellEffectInfo.TriggerSpell))
                        {
                            overrideAttr = true;
                            if (triggerSpell->HasAttribute(SPELL_ATTR0_CU_BINARY_SPELL))
                                allNonBinary = false;
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        if (overrideAttr && allNonBinary)
            spellInfo->AttributesCu &= ~SPELL_ATTR0_CU_BINARY_SPELL;
    }

    // remove attribute from spells that can't crit
    for (SpellInfo* spellInfo : mSpellInfoMap)
    {
        if (!spellInfo)
            continue;

        if (spellInfo->HasAttribute(SPELL_ATTR2_CANT_CRIT))
            spellInfo->AttributesCu &= ~SPELL_ATTR0_CU_CAN_CRIT;
    }

    // add custom attribute to liquid auras
    for (LiquidTypeEntry const* liquid : sLiquidTypeStore)
    {
        if (uint32 spellId = liquid->SpellID)
            if (SpellInfo* spellInfo = _GetSpellInfo(spellId))
                spellInfo->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
    }

    TC_LOG_INFO("server.loading", ">> Loaded SpellInfo custom attributes in {} ms", GetMSTimeDiffToNow(oldMSTime));
}

inline void ApplySpellFix(std::initializer_list<uint32> spellIds, void(*fix)(SpellInfo*))
{
    for (uint32 spellId : spellIds)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
        {
            TC_LOG_ERROR("server.loading", "Spell info correction specified for non-existing spell {}", spellId);
            continue;
        }

        fix(const_cast<SpellInfo*>(spellInfo));
    }
}

void SpellMgr::LoadSpellInfoCorrections()
{
    uint32 oldMSTime = getMSTime();

    // Some spells have no amplitude set
    {
        ApplySpellFix({
            6727,  // Poison Mushroom
            7288,  // Immolate Cumulative (TEST) (Rank 1)
            7291,  // Food (TEST)
            7331,  // Healing Aura (TEST) (Rank 1)
            /*
            30400, // Nether Beam - Perseverance
                Blizzlike to have it disabled? DBC says:
                "This is currently turned off to increase performance. Enable this to make it fire more frequently."
            */
            34589, // Dangerous Water
            52562, // Arthas Zombie Catcher
            57550, // Tirion Aggro
            65755
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_GetEffect(EFFECT_0).Amplitude = 1 * IN_MILLISECONDS;
        });

        ApplySpellFix({
            24707, // Food
            26263, // Dim Sum
            29055, // Refreshing Red Apple
            37504  // Karazhan - Chess NPC AI, action timer
        }, [](SpellInfo* spellInfo)
        {
            // first effect has correct amplitude
            spellInfo->_GetEffect(EFFECT_1).Amplitude = spellInfo->GetEffect(EFFECT_0).Amplitude;
        });

        // Vomit
        ApplySpellFix({ 43327 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_GetEffect(EFFECT_1).Amplitude = 1 * IN_MILLISECONDS;
        });

        // Strider Presence
        ApplySpellFix({ 4312 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_GetEffect(EFFECT_0).Amplitude = 1 * IN_MILLISECONDS;
            spellInfo->_GetEffect(EFFECT_1).Amplitude = 1 * IN_MILLISECONDS;
        });

        // Food
        ApplySpellFix({ 64345 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_GetEffect(EFFECT_0).Amplitude = 1 * IN_MILLISECONDS;
            spellInfo->_GetEffect(EFFECT_2).Amplitude = 1 * IN_MILLISECONDS;
        });
    }

    // specific code for cases with no trigger spell provided in field
    {
        // Brood Affliction: Bronze
        ApplySpellFix({ 23170 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_GetEffect(EFFECT_0).TriggerSpell = 23171;
        });

        // Feed Captured Animal
        ApplySpellFix({ 29917 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_GetEffect(EFFECT_0).TriggerSpell = 29916;
        });

        // Remote Toy
        ApplySpellFix({ 37027 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_GetEffect(EFFECT_0).TriggerSpell = 37029;
        });

        // Eye of Grillok
        ApplySpellFix({ 38495 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_GetEffect(EFFECT_0).TriggerSpell = 38530;
        });

        // Tear of Azzinoth Summon Channel - it's not really supposed to do anything, and this only prevents the console spam
        ApplySpellFix({ 39857 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_GetEffect(EFFECT_0).TriggerSpell = 39856;
        });

        // Personalized Weather
        ApplySpellFix({ 46736 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_GetEffect(EFFECT_1).TriggerSpell = 46737;
        });
    }

    // this one is here because we have no SP bonus for dmgclass none spell
    // but this one should since it's DBC data
    ApplySpellFix({
        52042, // Healing Stream Totem
    }, [](SpellInfo* spellInfo)
    {
        // We need more spells to find a general way (if there is any)
        spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
    });

    // Spell Reflection
    ApplySpellFix({ 57643 }, [](SpellInfo* spellInfo)
    {
        spellInfo->EquippedItemClass = -1;
    });

    ApplySpellFix({
        63026, // Force Cast (HACK: Target shouldn't be changed)
        63137  // Force Cast (HACK: Target shouldn't be changed; summon position should be untied from spell destination)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_DEST_DB);
    });

    // Immolate
    ApplySpellFix({
        348,
        707,
        1094,
        2941,
        11665,
        11667,
        11668,
        25309,
        27215,
        47810,
        47811
        }, [](SpellInfo* spellInfo)
    {
        // copy SP scaling data from direct damage to DoT
        spellInfo->_GetEffect(EFFECT_0).BonusMultiplier = spellInfo->GetEffect(EFFECT_1).BonusMultiplier;
    });

    // Detect Undead
    ApplySpellFix({ 11389 }, [](SpellInfo* spellInfo)
    {
        spellInfo->PowerType = POWER_MANA;
        spellInfo->ManaCost = 0;
        spellInfo->ManaPerSecond = 0;
    });

    // Drink! (Brewfest)
    ApplySpellFix({ 42436 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
    });

    // Warsong Gulch Anti-Stall Debuffs
    ApplySpellFix({
        46392, // Focused Assault
        46393, // Brutal Assault
    }, [](SpellInfo* spellInfo)
    {
        // due to discrepancies between ranks
        spellInfo->Attributes |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
    });

    // Summon Skeletons
    ApplySpellFix({ 52611, 52612 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).MiscValueB = 64;
    });

    // Battlegear of Eternal Justice
    ApplySpellFix({
        26135, // Battlegear of Eternal Justice
        37557  // Mark of Light
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->SpellFamilyFlags = flag96();
    });

    ApplySpellFix({
        40244, // Simon Game Visual
        40245, // Simon Game Visual
        40246, // Simon Game Visual
        40247, // Simon Game Visual
        42835  // Spout, remove damage effect, only anim is needed
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).Effect = SPELL_EFFECT_NONE;
    });

    ApplySpellFix({
        63665, // Charge (Argent Tournament emote on riders)
        31298, // Sleep (needs target selection script)
        51904, // Summon Ghouls On Scarlet Crusade (this should use conditions table, script for this spell needs to be fixed)
        2895,  // Wrath of Air Totem rank 1 (Aura)
        68933, // Wrath of Air Totem rank 2 (Aura)
        29200, // Purify Helboar Meat
        10872, // Abolish Disease Effect
        3137   // Abolish Poison Effect
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        spellInfo->_GetEffect(EFFECT_0).TargetB = SpellImplicitTargetInfo();
    });

    ApplySpellFix({
        56690, // Thrust Spear
        60586, // Mighty Spear Thrust
        60776, // Claw Swipe
        60881, // Fatal Strike
        60864  // Jaws of Death
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx4 |= SPELL_ATTR4_FIXED_DAMAGE;
    });

    // Missile Barrage
    ApplySpellFix({ 44401 }, [](SpellInfo* spellInfo)
    {
        // should be consumed before Clearcasting
        spellInfo->Priority = 100;
    });

    // Howl of Azgalor
    ApplySpellFix({ 31344 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yards instead of 50000?!
    });

    ApplySpellFix({
        42818, // Headless Horseman - Wisp Flight Port
        42821, // Headless Horseman - Wisp Flight Missile
        17678  // Despawn Spectral Combatants
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(6); // 100 yards
    });

    // They Must Burn Bomb Aura (self)
    ApplySpellFix({ 36350 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TriggerSpell = 36325; // They Must Burn Bomb Drop (DND)
    });

    ApplySpellFix({
        61407, // Energize Cores
        62136, // Energize Cores
        54069, // Energize Cores
        56251  // Energize Cores
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENTRY);
    });

    ApplySpellFix({
        50785, // Energize Cores
        59372  // Energize Cores
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENEMY);
    });

    // Mana Shield (rank 2)
    ApplySpellFix({ 8494 }, [](SpellInfo* spellInfo)
    {
        // because of bug in dbc
        spellInfo->ProcChance = 0;
    });

    // Maelstrom Weapon
    ApplySpellFix({
        51528, // (Rank 1)
        51529, // (Rank 2)
        51530, // (Rank 3)
        51531, // (Rank 4)
        51532  // (Rank 5)
    }, [](SpellInfo* spellInfo)
    {
        // due to discrepancies between ranks
        spellInfo->EquippedItemSubClassMask = 0x0000FC33;
        spellInfo->AttributesEx3 |= SPELL_ATTR3_CAN_PROC_WITH_TRIGGERED;
    });

    ApplySpellFix({
        20335, // Heart of the Crusader
        20336,
        20337,
        53228, // Rapid Killing (Rank 1)
        53232, // Rapid Killing (Rank 2)
        63320  // Glyph of Life Tap
    }, [](SpellInfo* spellInfo)
    {
        // Entries were not updated after spell effect change, we have to do that manually :/
        spellInfo->AttributesEx3 |= SPELL_ATTR3_CAN_PROC_WITH_TRIGGERED;
    });

    ApplySpellFix({
        51627, // Turn the Tables (Rank 1)
        51628, // Turn the Tables (Rank 2)
        51629  // Turn the Tables (Rank 3)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
    });

    ApplySpellFix({
        52910, // Turn the Tables
        52914, // Turn the Tables
        52915  // Turn the Tables
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
    });

    // Magic Absorption
    ApplySpellFix({
        29441, // (Rank 1)
        29444  // (Rank 2)
    }, [](SpellInfo* spellInfo)
    {
        // Caused off by 1 calculation (ie 79 resistance at level 80)
        spellInfo->SpellLevel = 0;
    });

    // Execute
    ApplySpellFix({
        5308,  // (Rank 1)
        20658, // (Rank 2)
        20660, // (Rank 3)
        20661, // (Rank 4)
        20662, // (Rank 5)
        25234, // (Rank 6)
        25236, // (Rank 7)
        47470, // (Rank 8)
        47471  // (Rank 9)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_CANT_TRIGGER_PROC;
    });

    // Improved Spell Reflection - aoe aura
    ApplySpellFix({ 59725 }, [](SpellInfo* spellInfo)
    {
        // Target entry seems to be wrong for this spell :/
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER_AREA_PARTY);
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_20_YARDS);
    });

    ApplySpellFix({
        44978, // Wild Magic
        45001, // Wild Magic
        45002, // Wild Magic
        45004, // Wild Magic
        45006, // Wild Magic
        45010, // Wild Magic
        31347, // Doom
        41635, // Prayer of Mending
        44869, // Spectral Blast
        45027, // Revitalize
        45976, // Muru Portal Channel
        39365, // Thundering Storm
        41071, // Raise Dead (HACK)
        52124, // Sky Darkener Assault
        42442, // Vengeance Landing Cannonfire
        45863, // Cosmetic - Incinerate to Random Target
        25425, // Shoot
        45761, // Shoot
        42611, // Shoot
        61588, // Blazing Harpoon
        52479, // Gift of the Harvester
        48246, // Ball of Flame
        36327, // Shoot Arcane Explosion Arrow
        55479, // Force Obedience
        28560, // Summon Blizzard (Sapphiron)
        53096, // Quetz'lun's Judgment
        70743, // AoD Special
        70614, // AoD Special - Vegard
        4020,  // Safirdrang's Chill
        52438, // Summon Skittering Swarmer (Force Cast)
        52449, // Summon Skittering Infector (Force Cast)
        53609, // Summon Anub'ar Assassin (Force Cast)
        53457, // Summon Impale Trigger (AoE)
        45907, // Torch Target Picker
        52953, // Torch
        58121, // Torch
        43109, // Throw Torch
        58552, // Return to Orgrimmar
        58533, // Return to Stormwind
        21855, // Challenge Flag
        38762, // Force of Neltharaku
        51122, // Fierce Lightning Stike
        71848, // Toxic Wasteling Find Target
        36146, // Chains of Naberius
        33711, // Murmur's Touch
        38794  // Murmur's Touch
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 1;
    });

    ApplySpellFix({
        36384, // Skartax Purple Beam
        47731  // Critter
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 2;
    });

    ApplySpellFix({
        41376, // Spite
        39992, // Needle Spine
        29576, // Multi-Shot
        40816, // Saber Lash
        37790, // Spread Shot
        46771, // Flame Sear
        45248, // Shadow Blades
        41303, // Soul Drain
        54172, // Divine Storm (heal)
        29213, // Curse of the Plaguebringer - Noth
        28542, // Life Drain - Sapphiron
        66588, // Flaming Spear
        54171  // Divine Storm
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 3;
    });

    ApplySpellFix({
        38310, // Multi-Shot
        53385  // Divine Storm (Damage)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 4;
    });

    ApplySpellFix({
        42005, // Bloodboil
        38296, // Spitfire Totem
        37676, // Insidious Whisper
        46008, // Negative Energy
        45641, // Fire Bloom
        55665, // Life Drain - Sapphiron (H)
        28796, // Poison Bolt Volly - Faerlina
        37135  // Domination
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 5;
    });

    ApplySpellFix({
        40827, // Sinful Beam
        40859, // Sinister Beam
        40860, // Vile Beam
        40861, // Wicked Beam
        54098, // Poison Bolt Volly - Faerlina (H)
        54835  // Curse of the Plaguebringer - Noth (H)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 10;
    });

    ApplySpellFix({
        50312  // Unholy Frenzy
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 15;
    });

    ApplySpellFix({
        47977, // Magic Broom
        48025, // Headless Horseman's Mount
        54729, // Winged Steed of the Ebon Blade
        58983, // Big Blizzard Bear
        65917, // Magic Rooster
        71342, // Big Love Rocket
        72286, // Invincible
        74856, // Blazing Hippogryph
        75614, // Celestial Steed
        75973  // X-53 Touring Rocket
    }, [](SpellInfo* spellInfo)
    {
        // First two effects apply auras, which shouldn't be there
        // due to NO_TARGET applying aura on current caster (core bug)
        // Just wipe effect data, to mimic blizz-behavior
        spellInfo->_GetEffect(EFFECT_0).Effect = SPELL_EFFECT_NONE;
        spellInfo->_GetEffect(EFFECT_1).Effect = SPELL_EFFECT_NONE;
    });

    // Lock and Load (Rank 1)
    ApplySpellFix({ 56342 }, [](SpellInfo* spellInfo)
    {
        // @workaround: Delete dummy effect from rank 1
        // effect apply aura has NO_TARGET but core still applies it to caster (same as above)
        spellInfo->_GetEffect(EFFECT_2).Effect = SPELL_EFFECT_NONE;
    });

    // Roar of Sacrifice
    ApplySpellFix({ 53480 }, [](SpellInfo* spellInfo)
    {
        // missing spell effect 2 data, taken from 4.3.4
        spellInfo->_GetEffect(EFFECT_1).Effect = SPELL_EFFECT_APPLY_AURA;
        spellInfo->_GetEffect(EFFECT_1).ApplyAuraName = SPELL_AURA_DUMMY;
        spellInfo->_GetEffect(EFFECT_1).MiscValue = 127;
        spellInfo->_GetEffect(EFFECT_1).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
    });

    // Fingers of Frost
    ApplySpellFix({ 44544 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).SpellClassMask = flag96(685904631, 1151048, 0);
    });

    // Magic Suppression - DK
    ApplySpellFix({ 49224, 49610, 49611 }, [](SpellInfo* spellInfo)
    {
        spellInfo->ProcCharges = 0;
    });

    // Death and Decay
    ApplySpellFix({ 52212 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx6 |= SPELL_ATTR6_CAN_TARGET_INVISIBLE;
    });

    // Oscillation Field
    ApplySpellFix({ 37408 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
    });

    // Everlasting Affliction
    ApplySpellFix({ 47201, 47202, 47203, 47204, 47205 }, [](SpellInfo* spellInfo)
    {
        // add corruption to affected spells
        spellInfo->_GetEffect(EFFECT_1).SpellClassMask[0] |= 2;
    });

    // Renewed Hope
    ApplySpellFix({
        57470, // (Rank 1)
        57472  // (Rank 2)
    }, [](SpellInfo* spellInfo)
    {
        // should also affect Flash Heal
        spellInfo->_GetEffect(EFFECT_0).SpellClassMask[0] |= 0x800;
    });

    // Crafty's Ultra-Advanced Proto-Typical Shortening Blaster
    ApplySpellFix({ 51912 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).Amplitude = 3000;
    });

    // Desecration Arm - 36 instead of 37 - typo? :/
    ApplySpellFix({ 29809 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_7_YARDS);
    });

    // In sniff caster hits multiple targets
    ApplySpellFix({
        73725, // [DND] Test Cheer
        73835, // [DND] Test Salute
        73836  // [DND] Test Roar
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50_YARDS); // 50yd
    });

    // In sniff caster hits multiple targets
    ApplySpellFix({
        73837, // [DND] Test Dance
        73886  // [DND] Test Stop Dance
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_150_YARDS); // 150yd
    });

    // Master Shapeshifter: missing stance data for forms other than bear - bear version has correct data
    // To prevent aura staying on target after talent unlearned
    ApplySpellFix({ 48420 }, [](SpellInfo* spellInfo)
    {
        spellInfo->Stances = UI64LIT(1) << (FORM_CAT - 1);
    });

    ApplySpellFix({ 48421 }, [](SpellInfo* spellInfo)
    {
        spellInfo->Stances = UI64LIT(1) << (FORM_MOONKIN - 1);
    });

    ApplySpellFix({ 48422 }, [](SpellInfo* spellInfo)
    {
        spellInfo->Stances = UI64LIT(1) << (FORM_TREE - 1);
    });

    // Improved Shadowform (Rank 1)
    ApplySpellFix({ 47569 }, [](SpellInfo* spellInfo)
    {
        // with this spell atrribute aura can be stacked several times
        spellInfo->Attributes &= ~SPELL_ATTR0_NOT_SHAPESHIFT;
    });

    // Hymn of Hope
    ApplySpellFix({ 64904 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_1).ApplyAuraName = SPELL_AURA_MOD_INCREASE_ENERGY_PERCENT;
    });

    // Improved Stings (Rank 2)
    ApplySpellFix({ 19465 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_2).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
    });

    // Nether Portal - Perseverence
    ApplySpellFix({ 30421 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_2).BasePoints += 30000;
    });

    // Natural shapeshifter
    ApplySpellFix({ 16834, 16835 }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(21);
    });

    // Ebon Plague
    ApplySpellFix({ 65142 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 &= ~SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
    });

    // Ebon Plague
    ApplySpellFix({ 51735, 51734, 51726 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        spellInfo->SpellFamilyFlags[2] = 0x10;
        spellInfo->_GetEffect(EFFECT_1).ApplyAuraName = SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN;
    });

    // Parasitic Shadowfiend Passive
    ApplySpellFix({ 41913 }, [](SpellInfo* spellInfo)
    {
        // proc debuff, and summon infinite fiends
        spellInfo->_GetEffect(EFFECT_0).ApplyAuraName = SPELL_AURA_DUMMY;
    });

    ApplySpellFix({
        27892, // To Anchor 1
        27928, // To Anchor 1
        27935, // To Anchor 1
        27915, // Anchor to Skulls
        27931, // Anchor to Skulls
        27937, // Anchor to Skulls
        16177, // Ancestral Fortitude (Rank 1)
        16236, // Ancestral Fortitude (Rank 2)
        16237, // Ancestral Fortitude (Rank 3)
        47930, // Grace
        45145, // Snake Trap Effect (Rank 1)
        13812, // Explosive Trap Effect (Rank 1)
        14314, // Explosive Trap Effect (Rank 2)
        14315, // Explosive Trap Effect (Rank 3)
        27026, // Explosive Trap Effect (Rank 4)
        49064, // Explosive Trap Effect (Rank 5)
        49065, // Explosive Trap Effect (Rank 6)
        43446, // Explosive Trap Effect (Hexlord Malacrass)
        50661, // Weakened Resolve
        68979, // Unleashed Souls
        48714, // Compelled
        7853   // The Art of Being a Water Terror: Force Cast on Player
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13);
    });

    // Wrath of the Plaguebringer
    ApplySpellFix({ 29214, 54836 }, [](SpellInfo* spellInfo)
    {
        // target allys instead of enemies, target A is src_caster, spells with effect like that have ally target
        // this is the only known exception, probably just wrong data
        spellInfo->_GetEffect(EFFECT_0).TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ALLY);
        spellInfo->_GetEffect(EFFECT_1).TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ALLY);
    });

    // Wind Shear
    ApplySpellFix({ 57994 }, [](SpellInfo* spellInfo)
    {
        // improper data for EFFECT_1 in 3.3.5 DBC, but is correct in 4.x
        spellInfo->_GetEffect(EFFECT_1).Effect = SPELL_EFFECT_MODIFY_THREAT_PERCENT;
        spellInfo->_GetEffect(EFFECT_1).BasePoints = -6; // -5%
    });

    ApplySpellFix({
        50526, // Wandering Plague
        15290  // Vampiric Embrace
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
    });

    // Vampiric Touch (dispel effect)
    ApplySpellFix({ 64085 }, [](SpellInfo* spellInfo)
    {
        // copy from similar effect of Unstable Affliction (31117)
        spellInfo->AttributesEx4 |= SPELL_ATTR4_FIXED_DAMAGE;
        spellInfo->AttributesEx6 |= SPELL_ATTR6_LIMIT_PCT_DAMAGE_MODS;
    });

    // Improved Devouring Plague
    ApplySpellFix({ 63675 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
    });

    // Deep Wounds
    ApplySpellFix({ 12721 }, [](SpellInfo* spellInfo)
    {
        // shouldnt ignore resillience or damage taken auras because its damage is not based off a spell.
        spellInfo->AttributesEx4 &= ~SPELL_ATTR4_FIXED_DAMAGE;
    });

    // Tremor Totem (instant pulse)
    ApplySpellFix({ 8145 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        spellInfo->AttributesEx5 |= SPELL_ATTR5_START_PERIODIC_AT_APPLY;
    });

    // Earthbind Totem (instant pulse)
    ApplySpellFix({ 6474 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx5 |= SPELL_ATTR5_START_PERIODIC_AT_APPLY;
    });

    // Flametongue Totem (Aura)
    ApplySpellFix({
        52109, // rank 1
        52110, // rank 2
        52111, // rank 3
        52112, // rank 4
        52113, // rank 5
        58651, // rank 6
        58654, // rank 7
        58655  // rank 8
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        spellInfo->_GetEffect(EFFECT_1).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        spellInfo->_GetEffect(EFFECT_0).TargetB = SpellImplicitTargetInfo();
        spellInfo->_GetEffect(EFFECT_1).TargetB = SpellImplicitTargetInfo();
    });

    // Marked for Death
    ApplySpellFix({
        53241, // (Rank 1)
        53243, // (Rank 2)
        53244, // (Rank 3)
        53245, // (Rank 4)
        53246  // (Rank 5)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).SpellClassMask = flag96(0x00067801, 0x10820001, 0x00000801);
    });

    ApplySpellFix({
        70728, // Exploit Weakness (needs target selection script)
        70840  // Devious Minds (needs target selection script)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        spellInfo->_GetEffect(EFFECT_0).TargetB = SpellImplicitTargetInfo(TARGET_UNIT_PET);
    });

    // Culling The Herd (needs target selection script)
    ApplySpellFix({ 70893 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        spellInfo->_GetEffect(EFFECT_0).TargetB = SpellImplicitTargetInfo(TARGET_UNIT_MASTER);
    });

    // Sigil of the Frozen Conscience
    ApplySpellFix({ 54800 }, [](SpellInfo* spellInfo)
    {
        // change class mask to custom extended flags of Icy Touch
        // this is done because another spell also uses the same SpellFamilyFlags as Icy Touch
        // SpellFamilyFlags[0] & 0x00000040 in SPELLFAMILY_NECROMANCER is currently unused (3.3.5a)
        // this needs research on modifier applying rules, does not seem to be in Attributes fields
        spellInfo->_GetEffect(EFFECT_0).SpellClassMask = flag96(0x00000040, 0x00000000, 0x00000000);
    });

    // Idol of the Flourishing Life
    ApplySpellFix({ 64949 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).SpellClassMask = flag96(0x00000000, 0x02000000, 0x00000000);
        spellInfo->_GetEffect(EFFECT_0).ApplyAuraName = SPELL_AURA_ADD_FLAT_MODIFIER;
    });

    ApplySpellFix({
        34231, // Libram of the Lightbringer
        60792, // Libram of Tolerance
        64956  // Libram of the Resolute
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).SpellClassMask = flag96(0x80000000, 0x00000000, 0x00000000);
        spellInfo->_GetEffect(EFFECT_0).ApplyAuraName = SPELL_AURA_ADD_FLAT_MODIFIER;
    });

    ApplySpellFix({
        28851, // Libram of Light
        28853, // Libram of Divinity
        32403  // Blessed Book of Nagrand
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).SpellClassMask = flag96(0x40000000, 0x00000000, 0x00000000);
        spellInfo->_GetEffect(EFFECT_0).ApplyAuraName = SPELL_AURA_ADD_FLAT_MODIFIER;
    });

    // Ride Carpet
    ApplySpellFix({ 45602 }, [](SpellInfo* spellInfo)
    {
        // force seat 0, vehicle doesn't have the required seat flags for "no seat specified (-1)"
        spellInfo->_GetEffect(EFFECT_0).BasePoints = 0;
    });

    ApplySpellFix({
        64745, // Item - Death Knight T8 Tank 4P Bonus
        64936  // Item - Warrior T8 Protection 4P Bonus
    }, [](SpellInfo* spellInfo)
    {
        // 100% chance of procc'ing, not -10% (chance calculated in PrepareTriggersExecutedOnHit)
        spellInfo->_GetEffect(EFFECT_0).BasePoints = 100;
    });

    // Entangling Roots -- Nature's Grasp Proc
    ApplySpellFix({
        19970, // (Rank 6)
        19971, // (Rank 5)
        19972, // (Rank 4)
        19973, // (Rank 3)
        19974, // (Rank 2)
        19975, // (Rank 1)
        27010, // (Rank 7)
        53313  // (Rank 8)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->CastTimeEntry = sSpellCastTimesStore.LookupEntry(1);
    });

    // Easter Lay Noblegarden Egg Aura
    ApplySpellFix({ 61719 }, [](SpellInfo* spellInfo)
    {
        // Interrupt flags copied from aura which this aura is linked with
        spellInfo->AuraInterruptFlags = AURA_INTERRUPT_FLAG_HITBYSPELL | AURA_INTERRUPT_FLAG_TAKE_DAMAGE;
    });

    // Death Knight T10 Tank 2P Bonus
    ApplySpellFix({ 70650 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).ApplyAuraName = SPELL_AURA_ADD_PCT_MODIFIER;
    });

    ApplySpellFix({
        6789,  // Warlock - Death Coil (Rank 1)
        17925, // Warlock - Death Coil (Rank 2)
        17926, // Warlock - Death Coil (Rank 3)
        27223, // Warlock - Death Coil (Rank 4)
        47859, // Warlock - Death Coil (Rank 5)
        47860, // Warlock - Death Coil (Rank 6)
        71838, // Drain Life - Bryntroll Normal
        71839  // Drain Life - Bryntroll Heroic
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
    });

    ApplySpellFix({
        51597, // Summon Scourged Captive
        56606, // Ride Jokkum
        61791  // Ride Vehicle (Yogg-Saron)
    }, [](SpellInfo* spellInfo)
    {
        /// @todo: remove this when basepoints of all Ride Vehicle auras are calculated correctly
        spellInfo->_GetEffect(EFFECT_0).BasePoints = 1;
    });

    // Summon Scourged Captive
    ApplySpellFix({ 51597 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).DieSides = 0;
    });

    // Black Magic
    ApplySpellFix({ 59630 }, [](SpellInfo* spellInfo)
    {
        spellInfo->Attributes |= SPELL_ATTR0_PASSIVE;
    });

    ApplySpellFix({
        17364, // Stormstrike
        48278, // Paralyze
        53651  // Light's Beacon
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
    });

    ApplySpellFix({
        51798, // Brewfest - Relay Race - Intro - Quest Complete
        47134  // Quest Complete
    }, [](SpellInfo* spellInfo)
    {
        //! HACK: This spell break quest complete for alliance and on retail not used
        spellInfo->_GetEffect(EFFECT_0).Effect = SPELL_EFFECT_NONE;
    });

    ApplySpellFix({
        47476, // Deathknight - Strangulate
        15487, // Priest - Silence
        5211,  // Druid - Bash  - R1
        6798,  // Druid - Bash  - R2
        8983   // Druid - Bash  - R3
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx7 |= SPELL_ATTR7_INTERRUPT_ONLY_NONPLAYER;
    });

    // Guardian Spirit
    ApplySpellFix({ 47788 }, [](SpellInfo* spellInfo)
    {
        spellInfo->ExcludeTargetAuraSpell = 72232; // Weakened Spirit
    });

    ApplySpellFix({
        15538, // Gout of Flame
        42490, // Energized!
        42492, // Cast Energized
        43115  // Plague Vial
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx |= SPELL_ATTR1_NO_THREAT;
    });

    ApplySpellFix({
        46842, // Flame Ring
        46836  // Flame Patch
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo();
    });

    // Test Ribbon Pole Channel
    ApplySpellFix({ 29726 }, [](SpellInfo* spellInfo)
    {
        spellInfo->InterruptFlags &= ~AURA_INTERRUPT_FLAG_CAST;
    });

    ApplySpellFix({
        42767, // Sic'em
        43092  // Stop the Ascension!: Halfdan's Soul Destruction
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_NEARBY_ENTRY);
    });

    // Polymorph (Six Demon Bag)
    ApplySpellFix({ 14621 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(4); // Medium Range
    });

    // Concussive Barrage
    ApplySpellFix({ 35101 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(155); // Hunter Range (Long)
    });

    ApplySpellFix({
        44327, // Trained Rock Falcon/Hawk Hunting
        44408  // Trained Rock Falcon/Hawk Hunting
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->Speed = 0.f;
    });

    ApplySpellFix({
        51675,  // Rogue - Unfair Advantage (Rank 1)
        51677   // Rogue - Unfair Advantage (Rank 2)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(2); // 5 yards
    });

    ApplySpellFix({
        55741, // Desecration (Rank 1)
        68766, // Desecration (Rank 2)
        57842  // Killing Spree (Off hand damage)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(2); // Melee Range
    });

    // Safeguard
    ApplySpellFix({
        46946, // (Rank 1)
        46947  // (Rank 2)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(34); // Twenty-Five yards
    });

    // Summon Corpse Scarabs
    ApplySpellFix({ 28864, 29105 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS);
    });

    ApplySpellFix({
        37851, // Tag Greater Felfire Diemetradon
        37918  // Arcano-pince
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->RecoveryTime = 3000;
    });

    // Jormungar Strike
    ApplySpellFix({ 56513 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RecoveryTime = 2000;
    });

    ApplySpellFix({
        54997, // Cast Net (tooltip says 10s but sniffs say 6s)
        56524  // Acid Breath
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->RecoveryTime = 6000;
    });

    ApplySpellFix({
        47911, // EMP
        48620, // Wing Buffet
        51752  // Stampy's Stompy-Stomp
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->RecoveryTime = 10000;
    });

    ApplySpellFix({
        37727, // Touch of Darkness
        54996  // Ice Slick (tooltip says 20s but sniffs say 12s)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->RecoveryTime = 12000;
    });

    // Signal Helmet to Attack
    ApplySpellFix({ 51748 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RecoveryTime = 15000;
    });

    ApplySpellFix({
        51756, // Charge
        37919, //Arcano-dismantle
        37917  //Arcano-Cloak
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->RecoveryTime = 20000;
    });

    // Summon Frigid Bones
    ApplySpellFix({ 53525 }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(4); // 2 minutes
    });

    // Dark Conclave Ritualist Channel
    ApplySpellFix({ 38469 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(6);  // 100yd
    });

    //
    // VIOLET HOLD SPELLS
    //
    // Water Globule (Ichoron)
    ApplySpellFix({ 54258, 54264, 54265, 54266, 54267 }, [](SpellInfo* spellInfo)
    {
        // in 3.3.5 there is only one radius in dbc which is 0 yards in this
        // use max radius from 4.3.4
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_25_YARDS);
    });
    // ENDOF VIOLET HOLD

    //
    // ULDUAR SPELLS
    //
    // Pursued (Flame Leviathan)
    ApplySpellFix({ 62374 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS);   // 50000yd
    });

    // Focused Eyebeam Summon Trigger (Kologarn)
    ApplySpellFix({ 63342 }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 1;
    });

    ApplySpellFix({
        62716, // Growth of Nature (Freya)
        65584, // Growth of Nature (Freya)
        64381  // Strength of the Pack (Auriaya)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
    });

    ApplySpellFix({
        63018, // Searing Light (XT-002)
        65121, // Searing Light (25m) (XT-002)
        63024, // Gravity Bomb (XT-002)
        64234  // Gravity Bomb (25m) (XT-002)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 1;
    });

    ApplySpellFix({
        64386, // Terrifying Screech (Auriaya)
        64389, // Sentinel Blast (Auriaya)
        64678  // Sentinel Blast (Auriaya)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(28); // 5 seconds, wrong DBC data?
    });

    // Summon Swarming Guardian (Auriaya)
    ApplySpellFix({ 64397 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(137); // 8y, Based in BFA effect radius
    });

    // Potent Pheromones (Freya)
    ApplySpellFix({ 64321 }, [](SpellInfo* spellInfo)
    {
        // spell should dispel area aura, but doesn't have the attribute
        // may be db data bug, or blizz may keep reapplying area auras every update with checking immunity
        // that will be clear if we get more spells with problem like this
        spellInfo->AttributesEx |= SPELL_ATTR1_DISPEL_AURAS_ON_IMMUNITY;
    });

    // Blizzard (Thorim)
    ApplySpellFix({ 62576, 62602 }, [](SpellInfo* spellInfo)
    {
        // DBC data is wrong for EFFECT_0, it's a different dynobject target than EFFECT_1
        // Both effects should be shared by the same DynObject
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_DEST_CASTER_LEFT);
    });

    // Spinning Up (Mimiron)
    ApplySpellFix({ 63414 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetB = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        spellInfo->ChannelInterruptFlags = 0;
    });

    // Rocket Strike (Mimiron)
    ApplySpellFix({ 63036 }, [](SpellInfo* spellInfo)
    {
        spellInfo->Speed = 0;
    });

    // Magnetic Field (Mimiron)
    ApplySpellFix({ 64668 }, [](SpellInfo* spellInfo)
    {
        spellInfo->Mechanic = MECHANIC_NONE;
    });

    // Empowering Shadows (Yogg-Saron)
    ApplySpellFix({ 64468, 64486 }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 3;  // same for both modes?
    });

    // Cosmic Smash (Algalon the Observer)
    ApplySpellFix({ 62301 }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 1;
    });

    // Cosmic Smash (Algalon the Observer)
    ApplySpellFix({ 64598 }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 3;
    });

    // Cosmic Smash (Algalon the Observer)
    ApplySpellFix({ 62293 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetB = SpellImplicitTargetInfo(TARGET_DEST_CASTER);
    });

    // Cosmic Smash (Algalon the Observer)
    ApplySpellFix({ 62311, 64596 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(6);  // 100yd
    });

    ApplySpellFix({
        64014, // Expedition Base Camp Teleport
        64024, // Conservatory Teleport
        64025, // Halls of Invention Teleport
        64028, // Colossal Forge Teleport
        64029, // Shattered Walkway Teleport
        64030, // Antechamber Teleport
        64031, // Scrapyard Teleport
        64032, // Formation Grounds Teleport
        65042  // Prison of Yogg-Saron Teleport
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_DEST_DB);
    });
    // ENDOF ULDUAR SPELLS

    //
    // TRIAL OF THE CRUSADER SPELLS
    //
    // Infernal Eruption
    ApplySpellFix({ 66258, 67901 }, [](SpellInfo* spellInfo)
    {
        // increase duration from 15 to 18 seconds because caster is already
        // unsummoned when spell missile hits the ground so nothing happen in result
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(85);
    });
    // ENDOF TRIAL OF THE CRUSADER SPELLS

    //
    // HALLS OF REFLECTION SPELLS
    //
    ApplySpellFix({
        72435, // Defiling Horror
        72452  // Defiling Horror
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_60_YARDS); // 60yd
    });

    // Achievement Check
    ApplySpellFix({ 72830 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
    });

    // Start Halls of Reflection Quest AE
    ApplySpellFix({ 72900 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
    });
    // ENDOF HALLS OF REFLECTION SPELLS

    //
    // ICECROWN CITADEL SPELLS
    //
    ApplySpellFix({
        // THESE SPELLS ARE WORKING CORRECTLY EVEN WITHOUT THIS HACK
        // THE ONLY REASON ITS HERE IS THAT CURRENT GRID SYSTEM
        // DOES NOT ALLOW FAR OBJECT SELECTION (dist > 333)
        70781, // Light's Hammer Teleport
        70856, // Oratory of the Damned Teleport
        70857, // Rampart of Skulls Teleport
        70858, // Deathbringer's Rise Teleport
        70859, // Upper Spire Teleport
        70860, // Frozen Throne Teleport
        70861  // Sindragosa's Lair Teleport
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_DEST_DB);
    });

    // Bone Slice (Lord Marrowgar)
    ApplySpellFix({ 69055, 70814 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_5_YARDS); // 5yd
    });

    ApplySpellFix({
        69075, // Bone Storm (Lord Marrowgar)
        70834, // Bone Storm (Lord Marrowgar)
        70835, // Bone Storm (Lord Marrowgar)
        70836, // Bone Storm (Lord Marrowgar)
        71160, // Plague Stench (Stinky)
        71161, // Plague Stench (Stinky)
        71123  // Decimate (Stinky & Precious)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yd
    });

    // Coldflame (Lord Marrowgar)
    ApplySpellFix({ 69146, 70823, 70824, 70825 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx4 &= ~SPELL_ATTR4_IGNORE_RESISTANCES;
    });

    // Shadow's Fate
    ApplySpellFix({ 71169 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
    });

    // Lock Players and Tap Chest
    ApplySpellFix({ 72347 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 &= ~SPELL_ATTR3_NO_INITIAL_AGGRO;
    });

    // Award Reputation - Boss Kill
    ApplySpellFix({ 73843, 73844, 73845, 73846 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
    });

    ApplySpellFix({
        72378, // Blood Nova (Deathbringer Saurfang)
        73058, // Blood Nova (Deathbringer Saurfang)
        72769  // Scent of Blood (Deathbringer Saurfang)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS);
    });

    // Scent of Blood (Deathbringer Saurfang)
    ApplySpellFix({ 72771 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS);
    });

    // Resistant Skin (Deathbringer Saurfang adds)
    ApplySpellFix({ 72723 }, [](SpellInfo* spellInfo)
    {
        // this spell initially granted Shadow damage immunity, however it was removed but the data was left in client
        spellInfo->_GetEffect(EFFECT_2).Effect = SPELL_EFFECT_NONE;
    });

    // Coldflame Jets (Traps after Saurfang)
    ApplySpellFix({ 70460 }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(1); // 10 seconds
    });

    ApplySpellFix({
        71412, // Green Ooze Summon (Professor Putricide)
        71415  // Orange Ooze Summon (Professor Putricide)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
    });

    // Ooze flood
    ApplySpellFix({ 69783, 69797, 69799, 69802 }, [](SpellInfo* spellInfo)
    {
        // Those spells are cast on creatures with same entry as caster while they have TARGET_UNIT_NEARBY_ENTRY.
        spellInfo->AttributesEx |= SPELL_ATTR1_CANT_TARGET_SELF;
    });

    // Awaken Plagued Zombies
    ApplySpellFix({ 71159 }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(21);
    });

    // Volatile Ooze Beam Protection (Professor Putricide)
    ApplySpellFix({ 70530 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).Effect = SPELL_EFFECT_APPLY_AURA; // for an unknown reason this was SPELL_EFFECT_APPLY_AREA_AURA_RAID
    });

    // Mutated Strength (Professor Putricide)
    ApplySpellFix({ 71604, 72673, 72674, 72675 }, [](SpellInfo* spellInfo)
    {
        // THIS IS HERE BECAUSE COOLDOWN ON CREATURE PROCS WERE NOT IMPLEMENTED WHEN THE SCRIPT WAS WRITTEN
        spellInfo->_GetEffect(EFFECT_1).Effect = SPELL_EFFECT_NONE;
    });

    // Mutated Plague (Professor Putricide)
    ApplySpellFix({ 72454, 72464, 72506, 72507 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
    });

    // Unbound Plague (Professor Putricide) (needs target selection script)
    ApplySpellFix({ 70911, 72854, 72855, 72856 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetB = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
    });

    ApplySpellFix({
        71518, // Unholy Infusion Quest Credit (Professor Putricide)
        72934, // Blood Infusion Quest Credit (Blood-Queen Lana'thel)
        72289  // Frost Infusion Quest Credit (Sindragosa)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // another missing radius
    });

    // Empowered Flare (Blood Prince Council)
    ApplySpellFix({ 71708, 72785, 72786, 72787 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
    });

    // Swarming Shadows
    ApplySpellFix({ 71266, 72890 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AreaGroupId = 0; // originally, these require area 4522, which is... outside of Icecrown Citadel
    });

    // Corruption
    ApplySpellFix({ 70602 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
    });

    // Column of Frost (visual marker)
    ApplySpellFix({ 70715 }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(32); // 6 seconds (missing)
    });

    // Mana Void (periodic aura)
    ApplySpellFix({ 71085 }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(9); // 30 seconds (missing)
    });

    // Frostbolt Volley (only heroic)
    ApplySpellFix({ 72015, 72016 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_2).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_40_YARDS);
    });

    // Summon Suppressor (needs target selection script)
    ApplySpellFix({ 70936 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
        spellInfo->_GetEffect(EFFECT_0).TargetB = SpellImplicitTargetInfo();
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(157); // 90yd
    });

    ApplySpellFix({
        72706, // Achievement Check (Valithria Dreamwalker)
        71357  // Order Whelp
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS);   // 200yd
    });

    // Sindragosa's Fury
    ApplySpellFix({ 70598 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
    });

    // Frost Bomb
    ApplySpellFix({ 69846 }, [](SpellInfo* spellInfo)
    {
        spellInfo->Speed = 0.0f;    // This spell's summon happens instantly
    });

    // Chilled to the Bone
    ApplySpellFix({ 70106 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
        spellInfo->AttributesEx6 |= SPELL_ATTR6_LIMIT_PCT_DAMAGE_MODS;
    });

    // Ice Lock
    ApplySpellFix({ 71614 }, [](SpellInfo* spellInfo)
    {
        spellInfo->Mechanic = MECHANIC_STUN;
    });

    // Defile
    ApplySpellFix({ 72762 }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(559); // 53 seconds
    });

    // Defile
    ApplySpellFix({ 72743 }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(22); // 45 seconds
    });

    // Defile
    ApplySpellFix({ 72754, 73708, 73709, 73710 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
    });

    // Val'kyr Target Search
    ApplySpellFix({ 69030 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
        spellInfo->Attributes |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
    });

    // Raging Spirit Visual
    ApplySpellFix({ 69198 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13); // 50000yd
    });

    // Harvest Souls
    ApplySpellFix({ 73654, 74295, 74296, 74297 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
        spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
        spellInfo->_GetEffect(EFFECT_2).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
    });

    // Harvest Soul
    ApplySpellFix({ 73655 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
    });

    // Summon Shadow Trap
    ApplySpellFix({ 73540 }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(3); // 60 seconds
    });

    // Shadow Trap (visual)
    ApplySpellFix({ 73530 }, [](SpellInfo* spellInfo)
    {
        spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(27); // 3 seconds
    });

    // Shadow Trap
    ApplySpellFix({ 73529 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS); // 10yd
    });

    // Shadow Trap (searcher)
    ApplySpellFix({ 74282 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_5_YARDS); // 5yd
    });

    // Restore Soul
    ApplySpellFix({ 72595, 73650 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
    });

    // Destroy Soul
    ApplySpellFix({ 74086 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
    });

    // Summon Spirit Bomb
    ApplySpellFix({ 74302, 74342 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
        spellInfo->MaxAffectedTargets = 1;
    });

    // Summon Spirit Bomb
    ApplySpellFix({ 74341, 74343 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
        spellInfo->MaxAffectedTargets = 3;
    });

    // Summon Spirit Bomb
    ApplySpellFix({ 73579 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_25_YARDS); // 25yd
    });

    // Fury of Frostmourne
    ApplySpellFix({ 72350 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
    });

    ApplySpellFix(
    {
        75127, // Kill Frostmourne Players
        72351, // Fury of Frostmourne
        72431, // Jump (removes Fury of Frostmourne debuff)
        72429, // Mass Resurrection
        73159, // Play Movie
        73582  // Trigger Vile Spirit (Inside, Heroic)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
    });

    // Raise Dead
    ApplySpellFix({ 72376 }, [](SpellInfo* spellInfo)
    {
        spellInfo->MaxAffectedTargets = 3;
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
    });

    // Jump
    ApplySpellFix({ 71809 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(5); // 40yd
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS); // 10yd
        spellInfo->_GetEffect(EFFECT_0).MiscValue = 190;
    });

    // Broken Frostmourne
    ApplySpellFix({ 72405 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_20_YARDS); // 20yd
        spellInfo->AttributesEx |= SPELL_ATTR1_NO_THREAT;
    });
    // ENDOF ICECROWN CITADEL SPELLS

    //
    // RUBY SANCTUM SPELLS
    //
    // Soul Consumption
    ApplySpellFix({ 74799 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_12_YARDS);
    });

    // Twilight Cutter
    ApplySpellFix({ 74769, 77844, 77845, 77846 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yd
    });

    // Twilight Mending
    ApplySpellFix({ 75509 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx6 |= SPELL_ATTR6_CAN_TARGET_INVISIBLE;
        spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
    });

    // Combustion and Consumption Heroic versions lacks radius data
    ApplySpellFix({ 75875 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).Mechanic = MECHANIC_NONE;
        spellInfo->_GetEffect(EFFECT_1).Mechanic = MECHANIC_SNARE;
        spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_6_YARDS);
    });

    ApplySpellFix({ 75884 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_0).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_6_YARDS);
        spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_6_YARDS);
    });

    ApplySpellFix({ 75883, 75876 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_6_YARDS);
    });
    // ENDOF RUBY SANCTUM SPELLS

    //
    // EYE OF ETERNITY SPELLS
    //
    ApplySpellFix({
        // All spells below work even without these changes. The LOS attribute is due to problem
        // from collision between maps & gos with active destroyed state.
        57473, // Arcane Storm bonus explicit visual spell
        57431, // Summon Static Field
        56091, // Flame Spike (Wyrmrest Skytalon)
        56092, // Engulf in Flames (Wyrmrest Skytalon)
        57090, // Revivify (Wyrmrest Skytalon)
        57143  // Life Burst (Wyrmrest Skytalon)
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
    });

    // Arcane Barrage (cast by players and NONMELEEDAMAGELOG with caster Scion of Eternity (original caster)).
    ApplySpellFix({ 63934 }, [](SpellInfo* spellInfo)
    {
        // This would never crit on retail and it has attribute for SPELL_ATTR3_NO_DONE_BONUS because is handled from player,
        // until someone figures how to make scions not critting without hack and without making them main casters this should stay here.
        spellInfo->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
    });
    // ENDOF EYE OF ETERNITY SPELLS

    //
    // OCULUS SPELLS
    //
    ApplySpellFix({
        // The spells below are here because their effect 1 is giving warning due to
        // triggered spell not found in any dbc and is missing from encounter source* of data.
        // Even judged as clientside these spells can't be guessed for* now.
        49462, // Call Ruby Drake
        49461, // Call Amber Drake
        49345  // Call Emerald Drake
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_1).Effect = SPELL_EFFECT_NONE;
    });
    // ENDOF OCULUS SPELLS

    // Introspection
    ApplySpellFix({ 40055, 40165, 40166, 40167 }, [](SpellInfo* spellInfo)
    {
        spellInfo->Attributes |= SPELL_ATTR0_NEGATIVE_1;
    });

    // Chains of Ice
    ApplySpellFix({ 45524 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_2).TargetA = SpellImplicitTargetInfo();
    });

    // Minor Fortitude
    ApplySpellFix({ 2378 }, [](SpellInfo* spellInfo)
    {
        spellInfo->ManaCost = 0;
        spellInfo->ManaPerSecond = 0;
    });

    // Threatening Gaze
    ApplySpellFix({ 24314 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AuraInterruptFlags |= AURA_INTERRUPT_FLAG_CAST | AURA_INTERRUPT_FLAG_MOVE | AURA_INTERRUPT_FLAG_JUMP;
    });

    //
    // ISLE OF CONQUEST SPELLS
    //
    // Teleport
    ApplySpellFix({ 66551 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13); // 50000yd
    });
    // ENDOF ISLE OF CONQUEST SPELLS

    // Aura of Fear
    ApplySpellFix({ 40453 }, [](SpellInfo* spellInfo)
    {
        // Bad DBC data? Copying 25820 here due to spell description
        // either is a periodic with chance on tick, or a proc

        spellInfo->_GetEffect(EFFECT_0).ApplyAuraName = SPELL_AURA_PROC_TRIGGER_SPELL;
        spellInfo->_GetEffect(EFFECT_0).Amplitude = 0;
        spellInfo->ProcChance = 10;
    });

    // Survey Sinkholes
    ApplySpellFix({ 45853 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(5); // 40 yards
    });

    ApplySpellFix({
        41485, // Deadly Poison - Black Temple
        41487  // Envenom - Black Temple
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx6 |= SPELL_ATTR6_CAN_TARGET_INVISIBLE;
    });

    ApplySpellFix({
        // Proc attribute correction
        // Remove procflags from test/debug/deprecated spells to avoid DB Errors
        2479,  // Honorless Target
        3232,  // Gouge Stun Test
        3409,  // Crippling Poison
        4312,  // Strider Presence
        5707,  // Lifestone Regeneration
        5760,  // Mind-numbing Poison
        6727,  // Poison Mushroom
        6940,  // Hand of Sacrifice (handled remove in split hook)
        6984,  // Frost Shot (Rank 2)
        7164,  // Defensive Stance
        7288,  // Immolate Cumulative (TEST) (Rank 1)
        7291,  // Food (TEST)
        7331,  // Healing Aura (TEST) (Rank 1)
        7366,  // Berserker Stance
        7824,  // Blacksmithing Skill +10
        12551, // Frost Shot
        13218, // Wound Poison (Rank 1)
        13222, // Wound Poison II (Rank 2)
        13223, // Wound Poison III (Rank 3)
        13224, // Wound Poison IV (Rank 4)
        14795, // Venomhide Poison
        16610, // Razorhide
        18099, // Chill Nova
        18499, // Berserker Rage (extra rage implemented in Unit::RewardRage)
        18802, // Frost Shot
        20000, // Alexander's Test Periodic Aura
        21163, // Polished Armor (Rank 1)
        22818, // Mol'dar's Moxie
        22820, // Slip'kik's Savvy
        23333, // Warsong Flag
        23335, // Silverwing Flag
        25160, // Sand Storm
        27189, // Wound Poison V (Rank 5)
        28313, // Aura of Fear
        28726, // Nightmare Seed
        28754, // Fury of the Ashbringer
        30802, // Unleashed Rage (Rank 1)
        31481, // Lung Burst
        32430, // Battle Standard
        32431, // Battle Standard
        32447, // Travel Form
        33370, // Spell Haste
        33807, // Abacus of Violent Odds
        33891, // Tree of Life (Shapeshift)
        34132, // Gladiator's Totem of the Third Wind
        34135, // Libram of Justice
        34666, // Tamed Pet Passive 08 (DND)
        34667, // Tamed Pet Passive 09 (DND)
        34775, // Dragonspine Flurry
        34889, // Fire Breath (Rank 1)
        34976, // Netherstorm Flag
        35131, // Bladestorm
        35244, // Choking Vines
        35323, // Fire Breath (Rank 2)
        35336, // Energizing Spores
        36148, // Chill Nova
        36613, // Aspect of the Spirit Hunter
        36786, // Soul Chill
        37174, // Perceived Weakness
        37482, // Exploited Weakness
        37526, // Battle Rush
        37588, // Dive
        37985, // Fire Breath
        38317, // Forgotten Knowledge
        38843, // Soul Chill
        39015, // Atrophic Blow
        40396, // Fel Infusion
        40603, // Taunt Gurtogg
        40803, // Ron's Test Buff
        40879, // Prismatic Shield (no longer used since patch 2.2/adaptive prismatic shield)
        41341, // Balance of Power (implemented by hooking absorb)
        41435, // The Twin Blades of Azzinoth
        42369, // Merciless Libram of Justice
        42371, // Merciless Gladiator's Totem of the Third Wind
        42636, // Birmingham Tools Test 3
        43727, // Vengeful Libram of Justice
        43729, // Vengeful Gladiator's Totem of the Third Wind
        43817, // Focused Assault
        44305, // You're a ...! (Effects2)
        44586, // Prayer of Mending (unknown, unused aura type)
        45384, // Birmingham Tools Test 4
        45433, // Birmingham Tools Test 5
        46093, // Brutal Libram of Justice
        46099, // Brutal Gladiator's Totem of the Third Wind
        46705, // Honorless Target
        49145, // Spell Deflection (Rank 1) (implemented by hooking absorb)
        49883, // Flames
        50365, // Improved Blood Presence (Rank 1)
        50371, // Improved Blood Presence (Rank 2)
        50462, // Anti-Magic Zone (implemented by hooking absorb)

        50498, // Savage Rend (Rank 1) - proc from Savage Rend moved from attack itself to autolearn aura 50871
        53578, // Savage Rend (Rank 2)
        53579, // Savage Rend (Rank 3)
        53580, // Savage Rend (Rank 4)
        53581, // Savage Rend (Rank 5)
        53582, // Savage Rend (Rank 6)

        50655, // Frost Cut
        50995, // Empowered Blood Presence (Rank 1)
        51809, // First Aid
        53032, // Flurry of Claws
        55482, // Fire Breath (Rank 3)
        55483, // Fire Breath (Rank 4)
        55484, // Fire Breath (Rank 5)
        55485, // Fire Breath (Rank 6)
        57974, // Wound Poison VI (Rank 6)
        57975, // Wound Poison VII (Rank 7)
        60062, // Essence of Life
        60302, // Meteorite Whetstone
        60437, // Grim Toll
        60492, // Embrace of the Spider
        62142, // Improved Chains of Ice (Rank 3)
        63024, // Gravity Bomb
        64205, // Divine Sacrifice (handled remove in split hook)
        64772, // Comet's Trail
        65004, // Alacrity of the Elements
        65019, // Mjolnir Runestone
        65024, // Implosion

        66334, // Mistress' Kiss - currently not used in script, need implement?
        67905, // Mistress' Kiss
        67906, // Mistress' Kiss
        67907, // Mistress' Kiss

        71003, // Vegard's Touch

        72151, // Frenzied Bloodthirst - currently not used in script, need implement?
        72648, // Frenzied Bloodthirst
        72649, // Frenzied Bloodthirst
        72650, // Frenzied Bloodthirst

        72559, // Birmingham Tools Test 3
        72560, // Birmingham Tools Test 3
        72561, // Birmingham Tools Test 5
        72980  // Shadow Resonance
    }, [](SpellInfo* spellInfo)
    {
        spellInfo->ProcFlags = 0;
    });

    // Feral Charge - Cat
    ApplySpellFix({ 49376 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_1).RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_3_YARDS); // 3yd
    });

    // Baron Rivendare (Stratholme) - Unholy Aura
    ApplySpellFix({ 17466, 17467 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
    });

    // Spore - Spore Visual
    ApplySpellFix({ 42525 }, [](SpellInfo* spellInfo)
    {
        spellInfo->AttributesEx3 |= SPELL_ATTR3_DEATH_PERSISTENT;
        spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_DEAD;
    });

    // Death's Embrace
    ApplySpellFix({ 47198, 47199, 47200 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_1).SpellClassMask[0] |= 0x00004000; // Drain soul
    });

    // Soul Sickness (Forge of Souls)
    ApplySpellFix({ 69131 }, [](SpellInfo* spellInfo)
    {
        spellInfo->_GetEffect(EFFECT_1).ApplyAuraName = SPELL_AURA_MOD_DECREASE_SPEED;
    });

    // Headless Horseman Climax - Return Head (Hallow End)
    // Headless Horseman Climax - Body Regen (confuse only - removed on death)
    // Headless Horseman Climax - Head Is Dead
    ApplySpellFix({ 42401, 43105, 42428 }, [](SpellInfo* spellInfo)
    {
        spellInfo->Attributes |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
    });

    // Sacred Cleansing
    ApplySpellFix({ 53659 }, [](SpellInfo* spellInfo)
    {
        spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(5); // 40yd
    });


    ///ADDITIONAL
    ApplySpellFix({
        467,    // Thorns (Rank 1)
        782,    // Thorns (Rank 2)
        1075,   // Thorns (Rank 3)
        8914,   // Thorns (Rank 4)
        9756,   // Thorns (Rank 5)
        9910,   // Thorns (Rank 6)
        26992,  // Thorns (Rank 7)
        53307,  // Thorns (Rank 8)
        53352,  // Explosive Shot (trigger)
        50783,  // Slam (Triggered spell)
        20647   // Execute (Triggered spell)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    // Scarlet Raven Priest Image
    ApplySpellFix({ 48763, 48761 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags &= ~AURA_INTERRUPT_FLAG_SPELL_ATTACK;
        });

    // Has Brewfest Mug
    ApplySpellFix({ 42533 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(347); // 15 min
        });

    // Elixir of Minor Fortitude
    ApplySpellFix({ 2378 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ManaCost = 0;
            spellInfo->ManaPerSecond = 0;
        });

    // Elixir of Detect Undead
    ApplySpellFix({ 11389 }, [](SpellInfo* spellInfo)
        {
            spellInfo->PowerType = POWER_MANA;
            spellInfo->ManaCost = 0;
            spellInfo->ManaPerSecond = 0;
        });

    // Evergrove Druid Transform Crow
    ApplySpellFix({ 38776 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(4); // 120 seconds
        });

    ApplySpellFix({
        63026, // Force Cast (HACK: Target shouldn't be changed)
        63137  // Force Cast (HACK: Target shouldn't be changed; summon position should be untied from spell destination)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DB);
        });

    ApplySpellFix({
        53096,  // Quetz'lun's Judgment
        70743,  // AoD Special
        70614   // AoD Special - Vegard
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
        });

    // Summon Skeletons
    ApplySpellFix({ 52611, 52612 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValueB = 64;
        });



    ApplySpellFix({
        63665,  // Charge (Argent Tournament emote on riders)
        2895,   // Wrath of Air Totem rank 1 (Aura)
        68933,  // Wrath of Air Totem rank 2 (Aura)
        29200   // Purify Helboar Meat
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(0);
        });

    // Howl of Azgalor
    ApplySpellFix({ 31344 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yards instead of 50000?!
        });

    ApplySpellFix({
        42818,  // Headless Horseman - Wisp Flight Port
        42821   // Headless Horseman - Wisp Flight Missile
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(6); // 100 yards
        });

    // Spirit of Kirith
    ApplySpellFix({ 10853 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(3); // 1min
        });

    // Headless Horseman - Start Fire
    ApplySpellFix({ 42132 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(6); // 100 yards
        });

    //They Must Burn Bomb Aura (self)
    ApplySpellFix({ 36350 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 36325; // They Must Burn Bomb Drop (DND)
        });

    // Mana Shield (rank 2)
    ApplySpellFix({ 8494 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcChance = 0; // because of bug in dbc
        });

    ApplySpellFix({
        63320,  // Glyph of Life Tap
        20335,  // Heart of the Crusader
        20336,  // Heart of the Crusader
        20337,  // Heart of the Crusader
        53228,  // Rapid Killing (Rank 1)
        53232,  // Rapid Killing (Rank 2)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_CAN_PROC_WITH_TRIGGERED; // Entries were not updated after spell effect change, we have to do that manually
        });

    ApplySpellFix({
        31347,  // Doom
        41635,  // Prayer of Mending
        39365,  // Thundering Storm
        52124,  // Sky Darkener Assault
        42442,  // Vengeance Landing Cannonfire
        45863,  // Cosmetic - Incinerate to Random Target
        25425,  // Shoot
        45761,  // Shoot
        42611,  // Shoot
        61588,  // Blazing Harpoon
        36327   // Shoot Arcane Explosion Arrow
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
        });

    // Skartax Purple Beam
    ApplySpellFix({ 36384 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 2;
        });

    ApplySpellFix({
        37790,  // Spread Shot
        54172,  // Divine Storm (heal)
        66588  // Flaming Spear
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 3;
        });

    // Divine Storm
    ApplySpellFix({ 54171 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 3;
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Divine Storm (Damage)
    ApplySpellFix({ 53385 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 4;
        });

    ApplySpellFix({
        20424,  // Seal of Command
        42463,  // Seal of Vengeance
        53739   // Seal of Corruption
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
        });

    // Spitfire Totem
    ApplySpellFix({ 38296 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 5;
        });

    ApplySpellFix({
        40827,  // Sinful Beam
        40859,  // Sinister Beam
        40860,  // Vile Beam
        40861   // Wicked Beam
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 10;
        });

    // Unholy Frenzy
    ApplySpellFix({ 50312 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 15;
        });

    ApplySpellFix({
        17941,  // Shadow Trance
        22008,  // Netherwind Focus
        31834,  // Light's Grace
        34754,  // Clearcasting
        34936,  // Backlash
        48108,  // Hot Streak
        51124,  // Killing Machine
        54741,  // Firestarter
        64823,  // Item - Druid T8 Balance 4P Bonus
        34477,  // Misdirection
        44401,  // Missile Barrage
        18820   // Insight
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcCharges = 1;
        });

    // Fireball
    ApplySpellFix({ 57761 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcCharges = 1;
            spellInfo->Priority = 50;
        });

    // Tidal Wave
    ApplySpellFix({ 53390 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcCharges = 2;
        });

    // Oscillation Field
    ApplySpellFix({ 37408 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Ascendance (Talisman of Ascendance trinket)
    ApplySpellFix({ 28200 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcCharges = 6;
        });

    // The Eye of Acherus (no spawn in phase 2 in db)
    ApplySpellFix({ 51852 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValue |= 1;
        });

    // Crafty's Ultra-Advanced Proto-Typical Shortening Blaster
    ApplySpellFix({ 51912 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Amplitude = 3000;
        });

    // Desecration Arm - 36 instead of 37 - typo?
    ApplySpellFix({ 29809 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_7_YARDS);
        });

    // Sic'em
    ApplySpellFix({ 42767 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_NEARBY_ENTRY);
        });

    // Master Shapeshifter: missing stance data for forms other than bear - bear version has correct data
    // To prevent aura staying on target after talent unlearned
    ApplySpellFix({ 48420 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Stances = 1 << (FORM_CAT - 1);
        });

    ApplySpellFix({ 48421 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Stances = 1 << (FORM_MOONKIN - 1);
        });

    ApplySpellFix({ 48422 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Stances = 1 << (FORM_TREE - 1);
        });

    // Elemental Oath
    ApplySpellFix({ 51466, 51470 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_ADD_FLAT_MODIFIER;
            spellInfo->_effects[EFFECT_1].MiscValue = SPELLMOD_EFFECT2;
            spellInfo->_effects[EFFECT_1].SpellClassMask = flag96(0x00000000, 0x00004000, 0x00000000);
        });

    // Improved Shadowform (Rank 1)
    ApplySpellFix({ 47569 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes &= ~SPELL_ATTR0_NOT_SHAPESHIFT;   // with this spell atrribute aura can be stacked several times
        });

    // Natural shapeshifter
    ApplySpellFix({ 16834, 16835 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(21);
        });

    // Ebon Plague
    ApplySpellFix({ 51735, 51734, 51726 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellFamilyFlags[2] = 0x10;
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Parasitic Shadowfiend Passive
    ApplySpellFix({ 41013 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;   // proc debuff, and summon infinite fiends
        });

    ApplySpellFix({
        27892,  // To Anchor 1
        27928,  // To Anchor 1
        27935,  // To Anchor 1
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS);
        });

    // Wrath of the Plaguebringer
    ApplySpellFix({ 29214, 54836 }, [](SpellInfo* spellInfo)
        {
            // target allys instead of enemies, target A is src_caster, spells with effect like that have ally target
            // this is the only known exception, probably just wrong data
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ALLY);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ALLY);
        });

    // Wind Shear
    ApplySpellFix({ 57994 }, [](SpellInfo* spellInfo)
        {
            // improper data for EFFECT_1 in 3.3.5 DBC, but is correct in 4.x
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_MODIFY_THREAT_PERCENT;
            spellInfo->_effects[EFFECT_1].BasePoints = -6; // -5%
        });

    // Improved Devouring Plague
    ApplySpellFix({ 63675 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BonusMultiplier = 0;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
        });

    ApplySpellFix({
        8145,   // Tremor Totem (instant pulse)
        6474    // Earthbind Totem (instant pulse)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx5 |= SPELL_ATTR5_HASTE_AFFECT_DURATION;
        });

    // Marked for Death
    ApplySpellFix({ 53241, 53243, 53244, 53245, 53246 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].SpellClassMask = flag96(399361, 276955137, 1);
        });

    ApplySpellFix({
        70728,  // Exploit Weakness
        70840   // Devious Minds
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_PET);
        });

    // Culling The Herd
    ApplySpellFix({ 70893 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_MASTER);
        });

    // Sigil of the Frozen Conscience
    ApplySpellFix({ 54800 }, [](SpellInfo* spellInfo)
        {
            // change class mask to custom extended flags of Icy Touch
            // this is done because another spell also uses the same SpellFamilyFlags as Icy Touch
            // SpellFamilyFlags[0] & 0x00000040 in SPELLFAMILY_DEATHKNIGHT is currently unused (3.3.5a)
            // this needs research on modifier applying rules, does not seem to be in Attributes fields
            spellInfo->_effects[EFFECT_0].SpellClassMask = flag96(0x00000040, 0x00000000, 0x00000000);
        });

    // Idol of the Flourishing Life
    ApplySpellFix({ 64949 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].SpellClassMask = flag96(0x00000000, 0x02000000, 0x00000000);
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_ADD_FLAT_MODIFIER;
        });

    ApplySpellFix({
        34231,  // Libram of the Lightbringer
        60792,  // Libram of Tolerance
        64956   // Libram of the Resolute
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].SpellClassMask = flag96(0x80000000, 0x00000000, 0x00000000);
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_ADD_FLAT_MODIFIER;
        });

    ApplySpellFix({
        28851,  // Libram of Light
        28853,  // Libram of Divinity
        32403   // Blessed Book of Nagrand
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].SpellClassMask = flag96(0x40000000, 0x00000000, 0x00000000);
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_ADD_FLAT_MODIFIER;
        });

    // Ride Carpet
    ApplySpellFix({ 45602 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 0; // force seat 0, vehicle doesn't have the required seat flags for "no seat specified (-1)"
        });

    ApplySpellFix({
        64745,  // Item - Death Knight T8 Tank 4P Bonus
        64936   // Item - Warrior T8 Protection 4P Bonus
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 100; // 100% chance of procc'ing, not -10% (chance calculated in PrepareTriggersExecutedOnHit)
        });

    // Easter Lay Noblegarden Egg Aura
    ApplySpellFix({ 61719 }, [](SpellInfo* spellInfo)
        {
            // Interrupt flags copied from aura which this aura is linked with
            spellInfo->AuraInterruptFlags = AURA_INTERRUPT_FLAG_HITBYSPELL | AURA_INTERRUPT_FLAG_TAKE_DAMAGE;
        });

    // Bleh, need to change FamilyFlags :/ (have the same as original aura - bad!)
    ApplySpellFix({ 63510 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellFamilyFlags[EFFECT_0] = 0;
            spellInfo->SpellFamilyFlags[EFFECT_2] = 0x4000000;
        });

    ApplySpellFix({ 63514 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellFamilyFlags[EFFECT_0] = 0;
            spellInfo->SpellFamilyFlags[EFFECT_2] = 0x2000000;
        });

    ApplySpellFix({ 63531 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellFamilyFlags[EFFECT_0] = 0;
            spellInfo->SpellFamilyFlags[EFFECT_2] = 0x8000000;
        });

    // And affecting spells
    ApplySpellFix({
        20138,  // Improved Devotion Aura
        20139,
        20140
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].SpellClassMask[0] = 0;
            spellInfo->_effects[EFFECT_1].SpellClassMask[2] = 0x2000000;
        });

    ApplySpellFix({
        20254,  // Improved concentration aura
        20255,
        20256
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].SpellClassMask[0] = 0;
            spellInfo->_effects[EFFECT_1].SpellClassMask[2] = 0x4000000;
            spellInfo->_effects[EFFECT_2].SpellClassMask[0] = 0;
            spellInfo->_effects[EFFECT_2].SpellClassMask[2] = 0x4000000;
        });

    ApplySpellFix({
        53379,  // Swift Retribution
        53484,
        53648
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].SpellClassMask[0] = 0;
            spellInfo->_effects[EFFECT_0].SpellClassMask[2] = 0x8000000;
        });

    // Sanctified Retribution
    ApplySpellFix({ 31869 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].SpellClassMask[0] = 0;
            spellInfo->_effects[EFFECT_0].SpellClassMask[2] = 0x8000000;
        });

    // Seal of Light trigger
    ApplySpellFix({ 20167 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellLevel = 0;
            spellInfo->BaseLevel = 0;
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
        });

    // Hand of Reckoning
    ApplySpellFix({ 62124 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR1_CANT_BE_REDIRECTED;
        });

    // Redemption
    ApplySpellFix({ 7328, 10322, 10324, 20772, 20773, 48949, 48950 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellFamilyName = SPELLFAMILY_PALADIN;
        });

    ApplySpellFix({
        20184,  // Judgement of Justice
        20185,  // Judgement of Light
        20186,  // Judgement of Wisdom
        68055   // Judgements of the Just
        }, [](SpellInfo* spellInfo)
        {
            // hack for seal of light and few spells, judgement consists of few single casts and each of them can proc
            // some spell, base one has disabled proc flag but those dont have this flag
            spellInfo->AttributesEx3 |= SPELL_ATTR3_CANT_TRIGGER_PROC;
        });

    // Blessing of sanctuary stats
    ApplySpellFix({ 67480 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValue = -1;
            spellInfo->SpellFamilyName = SPELLFAMILY_UNK1; // allows stacking
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY; // just a marker
        });

    ApplySpellFix({
        6940, // Hand of Sacrifice
        64205 // Divine Sacrifice
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx7 |= SPELL_ATTR7_NO_PUSHBACK_ON_DAMAGE;
        });

    // Seal of Command trigger
    ApplySpellFix({ 20424 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 &= ~SPELL_ATTR3_CANT_TRIGGER_PROC;
        });

    ApplySpellFix({
        54968,  // Glyph of Holy Light, Damage Class should be magic
        53652,  // Beacon of Light heal, Damage Class should be magic
        53654
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
        });

    // Wild Hunt
    ApplySpellFix({ 62758, 62762 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
        });

    // Intervene
    ApplySpellFix({ 3411 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_STOP_ATTACK_TARGET;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Roar of Sacrifice
    ApplySpellFix({ 53480 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_SPLIT_DAMAGE_PCT;
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
            spellInfo->_effects[EFFECT_1].DieSides = 1;
            spellInfo->_effects[EFFECT_1].BasePoints = 19;
            spellInfo->_effects[EFFECT_1].BasePoints = 127; // all schools
        });

    // Silencing Shot
    ApplySpellFix({ 34490, 41084, 42671 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Speed = 0.0f;
        });

    // Monstrous Bite
    ApplySpellFix({ 54680, 55495, 55496, 55497, 55498, 55499 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        });

    // Hunter's Mark
    ApplySpellFix({ 1130, 14323, 14324, 14325, 53338 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    // Cobra Strikes
    ApplySpellFix({ 53257 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcCharges = 2;
            spellInfo->StackAmount = 0;
        });

    // Kill Command
    // Kill Command, Overpower
    ApplySpellFix({ 34027, 37529 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcCharges = 0;
        });

    // Kindred Spirits, damage aura
    ApplySpellFix({ 57458 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx4 |= SPELL_ATTR4_DONT_REMOVE_IN_ARENA;
        });

    // Chimera Shot - Serpent trigger
    ApplySpellFix({ 53353 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
        });

    // Entrapment trigger
    ApplySpellFix({ 19185, 64803, 64804 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_TARGET_ENEMY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_DEST_AREA_ENEMY);
            spellInfo->AttributesEx5 |= SPELL_ATTR5_SKIP_CHECKCAST_LOS_CHECK;
        });

    // Improved Stings (Rank 2)
    ApplySpellFix({ 19465 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        });

    // Heart of the Phoenix (triggered)
    ApplySpellFix({ 54114 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx &= ~SPELL_ATTR1_DISMISS_PET;
            spellInfo->RecoveryTime = 8 * 60 * IN_MILLISECONDS; // prev 600000
        });

    // Master of Subtlety
    ApplySpellFix({ 31221, 31222, 31223 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellFamilyName = SPELLFAMILY_ROGUE;
        });

    ApplySpellFix({
        31666,  // Master of Subtlety triggers
        58428   // Overkill
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_SCRIPT_EFFECT;
        });

    // Honor Among Thieves
    ApplySpellFix({ 51698, 51700, 51701 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 51699;
        });

    ApplySpellFix({
        5171,   // Slice and Dice
        6774    // Slice and Dice
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Envenom
    ApplySpellFix({ 32645, 32684, 57992, 57993 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Dispel = DISPEL_NONE;
        });

    ApplySpellFix({
        64014,  // Expedition Base Camp Teleport
        64032,  // Formation Grounds Teleport
        64028,  // Colossal Forge Teleport
        64031,  // Scrapyard Teleport
        64030,  // Antechamber Teleport
        64029,  // Shattered Walkway Teleport
        64024,  // Conservatory Teleport
        64025,  // Halls of Invention Teleport
        65042   // Prison of Yogg-Saron Teleport
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo(TARGET_DEST_DB);
        });

    // Killing Spree (teleport)
    ApplySpellFix({ 57840 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(6); // 100 yards
        });

    // Killing Spree
    ApplySpellFix({ 51690 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR1_NOT_BREAK_STEALTH;
        });

    // Blood Tap visual cd reset
    ApplySpellFix({ 47804 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->RuneCostID = 442;
        });

    // Chains of Ice
    ApplySpellFix({ 45524 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    // Impurity
    ApplySpellFix({ 49220, 49633, 49635, 49636, 49638 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->SpellFamilyName = SPELLFAMILY_NECROMANCER;
        });

    // Deadly Aggression (Deadly Gladiator's Death Knight Relic, item: 42620)
    ApplySpellFix({ 60549 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Magic Suppression
    ApplySpellFix({ 49224, 49610, 49611 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcCharges = 0;
        });

    // Wandering Plague
    ApplySpellFix({ 50526 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Dancing Rune Weapon
    ApplySpellFix({ 49028 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
            spellInfo->ProcFlags |= PROC_FLAG_DONE_MELEE_AUTO_ATTACK;
        });

    // Death and Decay
    ApplySpellFix({ 52212 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->AttributesEx6 |= SPELL_ATTR6_CAN_TARGET_INVISIBLE;
        });

    // T9 blood plague crit bonus
    ApplySpellFix({ 67118 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Pestilence
    ApplySpellFix({ 50842 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].TargetA = TARGET_DEST_TARGET_ENEMY;
        });

    // Horn of Winter, stacking issues
    ApplySpellFix({ 57330, 57623 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].TargetA = 0;
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Scourge Strike trigger
    ApplySpellFix({ 70890 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_CANT_TRIGGER_PROC;
        });

    // Blood-caked Blade - Blood-caked Strike trigger
    ApplySpellFix({ 50463 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_CANT_TRIGGER_PROC;
        });

    // Blood Gorged
    ApplySpellFix({ 61274, 61275, 61276, 61277,61278 }, [](SpellInfo* spellInfo)
        {
            // ARP affect Death Strike and Rune Strike
            spellInfo->_effects[EFFECT_0].SpellClassMask = flag96(0x1400011, 0x20000000, 0x0);
        });

    // Death Grip
    ApplySpellFix({ 49576, 49560 }, [](SpellInfo* spellInfo)
        {
            // remove main grip mechanic, leave only effect one
            //  should fix taunt on bosses and not break the pull protection at the same time (no aura provides immunity to grip mechanic)
            spellInfo->Mechanic = 0;
        });

    // Death Grip Jump Dest
    ApplySpellFix({ 57604 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Death Pact
    ApplySpellFix({ 48743 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx &= ~SPELL_ATTR1_CANT_TARGET_SELF;
        });

    // Raise Ally (trigger)
    ApplySpellFix({ 46619 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes &= ~SPELL_ATTR0_CANT_CANCEL;
        });

    // Frost Strike
    ApplySpellFix({ 49143, 51416, 51417, 51418, 51419, 55268 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_COMPLETELY_BLOCKED;
        });

    // Death Knight T10 Tank 2p Bonus
    ApplySpellFix({ 70650 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_ADD_PCT_MODIFIER;
        });

    ApplySpellFix({ 45297, 45284 }, [](SpellInfo* spellInfo)
        {
            spellInfo->CategoryRecoveryTime = 0;
            spellInfo->RecoveryTime = 0;
            spellInfo->AttributesEx6 |= SPELL_ATTR6_LIMIT_PCT_DAMAGE_MODS;
        });

    // Improved Earth Shield
    ApplySpellFix({ 51560, 51561 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].MiscValue = SPELLMOD_DAMAGE;
        });

    // Tidal Force
    ApplySpellFix({ 55166, 55198 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(18);
            spellInfo->ProcCharges = 0;
        });

    // Healing Stream Totem
    ApplySpellFix({ 52042 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellLevel = 0;
            spellInfo->BaseLevel = 0;
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(5); // 40yd
        });

    // Earth Shield
    ApplySpellFix({ 379 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellLevel = 0;
            spellInfo->BaseLevel = 0;
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
        });

    // Stormstrike
    ApplySpellFix({ 17364 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Strength of Earth totem effect
    ApplySpellFix({ 8076, 8162, 8163, 10441, 25362, 25527, 57621, 58646 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].RadiusEntry = spellInfo->_effects[EFFECT_0].RadiusEntry;
            spellInfo->_effects[EFFECT_2].RadiusEntry = spellInfo->_effects[EFFECT_0].RadiusEntry;
        });

    // Flametongue Totem effect
    ApplySpellFix({ 52109, 52110, 52111, 52112, 52113, 58651, 58654, 58655 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].TargetB = spellInfo->_effects[EFFECT_1].TargetB = spellInfo->_effects[EFFECT_0].TargetB = 0;
            spellInfo->_effects[EFFECT_2].TargetA = spellInfo->_effects[EFFECT_1].TargetA = spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        });

    // Sentry Totem
    ApplySpellFix({ 6495 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(0);
        });

    // Bind Sight (PT)
    ApplySpellFix({ 6277 }, [](SpellInfo* spellInfo)
        {
            // because it is passive, needs this to be properly removed at death in RemoveAllAurasOnDeath()
            spellInfo->AttributesEx &= ~SPELL_ATTR1_CHANNELED_1;
            spellInfo->Attributes |= SPELL_ATTR0_PASSIVE;
            spellInfo->AttributesEx7 |= SPELL_ATTR7_DISABLE_AURA_WHILE_DEAD;
        });

    // Ancestral Awakening Heal
    ApplySpellFix({ 52752 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
        });

    // Heroism
    ApplySpellFix({ 32182 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 57723; // Exhaustion
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Bloodlust
    ApplySpellFix({ 2825 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 57724; // Sated
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Improved Succubus
    ApplySpellFix({ 18754, 18755, 18756 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        });

    // Unstable Affliction
    ApplySpellFix({ 31117 }, [](SpellInfo* spellInfo)
        {
            spellInfo->PreventionType = SPELL_PREVENTION_TYPE_NONE;
        });

    // Shadowflame - trigger
    ApplySpellFix({
        47960,  // r1
        61291   // r2
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR1_CANT_BE_REDIRECTED;
        });

    // Curse of Doom
    ApplySpellFix({ 18662 }, [](SpellInfo* spellInfo)
        {
            // summoned doomguard duration fix
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(6);
        });

    // Everlasting Affliction
    ApplySpellFix({ 47201, 47202, 47203, 47204, 47205 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].SpellClassMask[0] |= 2; // add corruption to affected spells
        });

    // Death's Embrace
    ApplySpellFix({ 47198, 47199, 47200 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].SpellClassMask[0] |= 0x4000; // include Drain Soul
        });

    // Improved Demonic Tactics
    ApplySpellFix({ 54347, 54348, 54349 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = spellInfo->_effects[EFFECT_0].Effect;
            spellInfo->_effects[EFFECT_1].ApplyAuraName = spellInfo->_effects[EFFECT_0].ApplyAuraName;
            spellInfo->_effects[EFFECT_1].TargetA = spellInfo->_effects[EFFECT_0].TargetA;
            spellInfo->_effects[EFFECT_0].MiscValue = SPELLMOD_EFFECT1;
            spellInfo->_effects[EFFECT_1].MiscValue = SPELLMOD_EFFECT2;
        });

    // Rain of Fire (Doomguard)
    ApplySpellFix({ 42227 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Speed = 0.0f;
        });

    // Ritual Enslavement
    ApplySpellFix({ 22987 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_MOD_CHARM;
        });

    // Combustion, make this passive
    ApplySpellFix({ 11129 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Dispel = DISPEL_NONE;
        });

    // Magic Absorption
    ApplySpellFix({ 29441, 29444 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellLevel = 0;
        });

    // Living Bomb
    ApplySpellFix({ 44461, 55361, 55362 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
            spellInfo->AttributesEx4 |= SPELL_ATTR4_DAMAGE_DOESNT_BREAK_AURAS;
        });

    // Evocation
    ApplySpellFix({ 12051 }, [](SpellInfo* spellInfo)
        {
            spellInfo->InterruptFlags |= SPELL_INTERRUPT_FLAG_INTERRUPT;
        });

    // MI Fireblast, WE Frostbolt, MI Frostbolt
    ApplySpellFix({ 59637, 31707, 72898 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
        });

    // Blazing Speed
    ApplySpellFix({ 31641, 31642 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 31643;
        });

    // Summon Water Elemental (permanent)
    ApplySpellFix({ 70908 }, [](SpellInfo* spellInfo)
        {
            // treat it as pet
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_SUMMON_PET;
        });

    // // Burnout, trigger
    ApplySpellFix({ 44450 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_POWER_BURN;
        });

    // Mirror Image - Summon Spells
    ApplySpellFix({ 58831, 58833, 58834, 65047 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_CASTER);
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(0);
        });

    // Initialize Images (Mirror Image)
    ApplySpellFix({ 58836 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        });

    // Arcane Blast, can't be dispelled
    ApplySpellFix({ 36032 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
        });

    // Chilled (frost armor, ice armor proc)
    ApplySpellFix({ 6136, 7321 }, [](SpellInfo* spellInfo)
        {
            spellInfo->PreventionType = SPELL_PREVENTION_TYPE_NONE;
        });

    // Mirror Image Frostbolt
    ApplySpellFix({ 59638 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
            spellInfo->SpellFamilyName = SPELLFAMILY_MAGE;
            spellInfo->SpellFamilyFlags = flag96(0x20, 0x0, 0x0);
        });

    // Fingers of Frost
    ApplySpellFix({ 44544 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Dispel = DISPEL_NONE;
            spellInfo->AttributesEx4 |= SPELL_ATTR4_NOT_STEALABLE;
            spellInfo->_effects[EFFECT_0].SpellClassMask = flag96(685904631, 1151040, 32);
        });

    // Fingers of Frost visual buff
    ApplySpellFix({ 74396 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcCharges = 2;
            spellInfo->StackAmount = 0;
        });

    // Glyph of blocking
    ApplySpellFix({ 58375 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 58374;
        });

    // Sweeping Strikes stance change
    ApplySpellFix({ 12328 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR2_NOT_NEED_SHAPESHIFT;
        });

    // Damage Shield
    ApplySpellFix({ 59653 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
            spellInfo->SpellLevel = 0;
        });

    ApplySpellFix({
        20230,  // Retaliation
        871,    // Shield Wall
        1719    // Recklessness
        }, [](SpellInfo* spellInfo)
        {
            // Strange shared cooldown
            spellInfo->AttributesEx6 |= SPELL_ATTR6_IGNORE_CATEGORY_COOLDOWN_MODS;
        });

    // Vigilance
    ApplySpellFix({ 50720 }, [](SpellInfo* spellInfo)
        {
            // fixes bug with empowered renew, single target aura
            spellInfo->SpellFamilyName = SPELLFAMILY_WARRIOR;
        });

    // Sunder Armor - Old Ranks
    ApplySpellFix({ 7405, 8380, 11596, 11597, 25225, 47467 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 11971;
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE;
        });

    // Improved Spell Reflection
    ApplySpellFix({ 59725 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER_AREA_PARTY);
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Hymn of Hope
    ApplySpellFix({ 64904 }, [](SpellInfo* spellInfo)
        {
            // rewrite part of aura system or swap effects...
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_MOD_INCREASE_ENERGY_PERCENT;
            spellInfo->_effects[EFFECT_2].Effect = spellInfo->_effects[EFFECT_0].Effect;
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].DieSides = spellInfo->_effects[EFFECT_0].DieSides;
            spellInfo->_effects[EFFECT_2].TargetA = spellInfo->_effects[EFFECT_0].TargetB;
            spellInfo->_effects[EFFECT_2].RadiusEntry = spellInfo->_effects[EFFECT_0].RadiusEntry;
            spellInfo->_effects[EFFECT_2].BasePoints = spellInfo->_effects[EFFECT_0].BasePoints;
        });

    // Divine Hymn
    ApplySpellFix({ 64844 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
            spellInfo->SpellLevel = 0;
        });

    ApplySpellFix({
        14898,  // Spiritual Healing affects prayer of mending
        15349,
        15354,
        15355,
        15356,
        47562,  // Divine Providence affects prayer of mending
        47564,
        47565,
        47566,
        47567,
        47586,  // Twin Disciplines affects prayer of mending
        47587,
        47588,
        52802,
        52803
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].SpellClassMask[1] |= 0x20; // prayer of mending
        });

    // Power Infusion
    ApplySpellFix({ 10060 }, [](SpellInfo* spellInfo)
        {
            // hack to fix stacking with arcane power
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_2].ApplyAuraName = SPELL_AURA_ADD_PCT_MODIFIER;
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
        });

    // Lifebloom final bloom
    ApplySpellFix({ 33778 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
            spellInfo->SpellLevel = 0;
            spellInfo->SpellFamilyFlags = flag96(0, 0x10, 0);
        });

    // Owlkin Frenzy
    ApplySpellFix({ 48391 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR4_NOT_STEALABLE;
        });

    // Item T10 Restoration 4P Bonus
    ApplySpellFix({ 70691 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
        });

    ApplySpellFix({
        770,    // Faerie Fire
        16857   // Faerie Fire (Feral)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx &= ~SPELL_ATTR1_UNAFFECTED_BY_SCHOOL_IMMUNE;
        });

    ApplySpellFix({ 49376 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_3_YARDS); // 3yd
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Feral Charge - Cat
    ApplySpellFix({ 61138, 61132, 50259 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Glyph of Barkskin
    ApplySpellFix({ 63058 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_CHANCE;
        });

    // Resurrection Sickness
    ApplySpellFix({ 15007 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellFamilyName = SPELLFAMILY_GENERIC;
        });

    // Luck of the Draw
    ApplySpellFix({ 72221 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags |= AURA_INTERRUPT_FLAG_CHANGE_MAP;
        });

    // Bind
    ApplySpellFix({ 3286 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Targets = 0; // neutral innkeepers not friendly?
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
        });

    // Playback Speech
    ApplySpellFix({ 74209 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(1);
        });

    ApplySpellFix({
        2641, // Dismiss Pet
        23356 // Taming Lesson
        }, [](SpellInfo* spellInfo)
        {
            // remove creaturetargettype
            spellInfo->TargetCreatureType = 0;
        });

    // Aspect of the Viper
    ApplySpellFix({ 34074 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(1);
            spellInfo->_effects[EFFECT_2].ApplyAuraName = SPELL_AURA_DUMMY;
        });

    // Strength of Wrynn
    ApplySpellFix({ 60509 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].BasePoints = 1500;
            spellInfo->_effects[EFFECT_1].BasePoints = 150;
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_PERIODIC_HEAL;
        });

    // Winterfin First Responder
    ApplySpellFix({ 48739 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 1;
            spellInfo->_effects[EFFECT_0].RealPointsPerLevel = 0;
            spellInfo->_effects[EFFECT_0].DieSides = 0;
            spellInfo->_effects[EFFECT_0].DamageMultiplier = 0;
            spellInfo->_effects[EFFECT_0].BonusMultiplier = 0;
        });

    // Army of the Dead (trigger npc aura)
    ApplySpellFix({ 49099 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Amplitude = 15000;
        });

    // Frightening Shout
    ApplySpellFix({ 19134 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_DUMMY;
        });

    // Isle of Conquest
    ApplySpellFix({ 66551 }, [](SpellInfo* spellInfo)
        {
            // Teleport in, missing range
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13); // 50000yd
        });

    // A'dal's Song of Battle
    ApplySpellFix({ 39953 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ALLY);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ALLY);
            spellInfo->_effects[EFFECT_2].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ALLY);
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(367); // 2 Hours
        });

    ApplySpellFix({
        57607,  // WintergraspCatapult - Spell Plague Barrel - EffectRadiusIndex
        57619,  // WintergraspDemolisher - Spell Hourl Boulder - EffectRadiusIndex
        57610   // Cannon (Siege Turret)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_25_YARDS); // SPELL_EFFECT_WMO_DAMAGE
        });

    // WintergraspCannon - Spell Fire Cannon - EffectRadiusIndex
    ApplySpellFix({ 51422 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS); // SPELL_EFFECT_SCHOOL_DAMAGE
        });

    // WintergraspDemolisher - Spell Ram -  EffectRadiusIndex
    ApplySpellFix({ 54107 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_3_YARDS); // SPELL_EFFECT_KNOCK_BACK
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_3_YARDS); // SPELL_EFFECT_SCHOOL_DAMAGE
            spellInfo->_effects[EFFECT_2].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_3_YARDS); // SPELL_EFFECT_WEAPON_DAMAGE
        });

    // WintergraspSiegeEngine - Spell Ram - EffectRadiusIndex
    ApplySpellFix({ 51678 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS); // SPELL_EFFECT_KNOCK_BACK
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS); // SPELL_EFFECT_SCHOOL_DAMAGE
            spellInfo->_effects[EFFECT_2].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_20_YARDS); // SPELL_EFFECT_WEAPON_DAMAGE
        });

    // WintergraspCatapult - Spell Plague Barrell - Range
    ApplySpellFix({ 57606 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(164); // "Catapult Range"
        });

    // Boulder (Demolisher)
    ApplySpellFix({ 50999 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS); // 10yd
        });

    // Flame Breath (Catapult)
    ApplySpellFix({ 50990 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_18_YARDS); // 18yd
        });

    // Jormungar Bite
    ApplySpellFix({ 56103 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Throw Proximity Bomb
    ApplySpellFix({ 34095 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_TARGET_ENEMY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    ApplySpellFix({
        53348,  // DEATH KNIGHT SCARLET FIRE ARROW
        53117   // BALISTA
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->RecoveryTime = 5000;
            spellInfo->CategoryRecoveryTime = 5000;
        });

    // Teleport To Molten Core
    ApplySpellFix({ 25139 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_DEATH_PERSISTENT;
        });

    // Landen Stilwell Transform
    ApplySpellFix({ 31310 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_PASSIVE;
        });

    // Shadowstalker Stealth
    ApplySpellFix({ 5916 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RealPointsPerLevel = 5.0f;
        });

    // Sneak
    ApplySpellFix({ 22766 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RealPointsPerLevel = 5.0f;
        });

    // Murmur's Touch
    ApplySpellFix({ 38794, 33711 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
            spellInfo->_effects[EFFECT_0].TriggerSpell = 33760;
        });

    // Negaton Field
    ApplySpellFix({
        36729,  // Normal
        38834   // Heroic
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Curse of the Doomsayer NORMAL
    ApplySpellFix({ 36173 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 36174; // Currently triggers heroic version...
        });

    // Crystal Channel
    ApplySpellFix({ 34156 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(35); // 35yd;
            spellInfo->ChannelInterruptFlags |= AURA_INTERRUPT_FLAG_MOVE;
        });

    // Debris - Debris Visual
    ApplySpellFix({ 36449, 30632 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_NEGATIVE_1;
        });

    // Soul Channel
    ApplySpellFix({ 30531 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Activate Sunblade Protecto
    ApplySpellFix({ 46475, 46476 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(14); // 60yd
        });

    // Break Ice
    ApplySpellFix({ 46638 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 &= ~SPELL_ATTR3_ONLY_TARGET_PLAYERS; // Obvious fail, it targets gameobject...
        });

    // Sinister Reflection Clone
    ApplySpellFix({ 45785 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Speed = 0.0f;
        });

    // Armageddon
    ApplySpellFix({ 45909 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Speed = 8.0f;
        });

    // Spell Absorption
    ApplySpellFix({ 41034 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_2].ApplyAuraName = SPELL_AURA_SCHOOL_ABSORB;
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->_effects[EFFECT_2].MiscValue = SPELL_SCHOOL_MASK_MAGIC;
        });

    // Shared Bonds
    ApplySpellFix({ 41363 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx &= ~SPELL_ATTR1_CHANNELED_1;
        });

    ApplySpellFix({
        41485,  // Deadly Poison
        41487   // Envenom
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx6 |= SPELL_ATTR6_CAN_TARGET_INVISIBLE;
        });

    // Parasitic Shadowfiend
    ApplySpellFix({ 41914 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_NEGATIVE_1;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Teleport Maiev
    ApplySpellFix({ 41221 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13); // 0-50000yd
        });

    // Watery Grave Explosion
    ApplySpellFix({ 37852 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx5 |= SPELL_ATTR5_USABLE_WHILE_STUNNED;
        });

    // Amplify Damage
    ApplySpellFix({ 39095 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
        });

    // Energy Feedback
    ApplySpellFix({ 44335 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags |= AURA_INTERRUPT_FLAG_CHANGE_MAP;
        });

    ApplySpellFix({
        31984,  // Finger of Death
        35354   // Hand of Death
        }, [](SpellInfo* spellInfo)
        {
            // Spell doesn't need to ignore invulnerabilities
            spellInfo->Attributes = SPELL_ATTR0_ABILITY;
        });

    // Finger of Death
    ApplySpellFix({ 32111 }, [](SpellInfo* spellInfo)
        {
            spellInfo->CastTimeEntry = sSpellCastTimesStore.LookupEntry(0); // We only need the animation, no damage
        });

    // Flame Breath, catapult spell
    ApplySpellFix({ 50989 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes &= ~SPELL_ATTR0_LEVEL_DAMAGE_CALCULATION;
        });

    // Koralon, Flaming Cinder
    ApplySpellFix({ 66690 }, [](SpellInfo* spellInfo)
        {
            // missing radius index
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yd
            spellInfo->MaxAffectedTargets = 1;
        });

    // Acid Volley
    ApplySpellFix({ 54714, 29325 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
        });

    // Summon Plagued Warrior
    ApplySpellFix({ 29237 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_DUMMY;
            spellInfo->_effects[EFFECT_1].Effect = spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    // Icebolt
    ApplySpellFix({ 28526 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
        });

    // Infected Wound
    ApplySpellFix({ 29306 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Hopeless
    ApplySpellFix({ 29125 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENTRY);
        });

    // Jagged Knife
    ApplySpellFix({ 55550 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_REQ_AMMO;
        });

    // Moorabi - Transformation
    ApplySpellFix({ 55098 }, [](SpellInfo* spellInfo)
        {
            spellInfo->InterruptFlags |= SPELL_INTERRUPT_FLAG_INTERRUPT;
        });

    ApplySpellFix({
        55521,  // Poisoned Spear (Normal)
        58967,  // Poisoned Spear (Heroic)
        55348,  // Throw (Normal)
        58966   // Throw (Heroic)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_REQ_AMMO;
        });

    // Charged Chaotic rift aura, trigger
    ApplySpellFix({ 47737 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(37); // 50yd
        });

    // Vanish
    ApplySpellFix({ 55964 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    // Trollgore - Summon Drakkari Invader
    ApplySpellFix({ 49456, 49457, 49458 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DB);
        });

    ApplySpellFix({
        48278,  // Paralyse
        47669   // Awaken subboss
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[0].TargetB = SpellImplicitTargetInfo();
        });

    // Flame Breath
    ApplySpellFix({ 47592 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Amplitude = 200;
        });

    // Skarvald, Charge
    ApplySpellFix({ 43651 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13); // 0-50000yd
        });

    // Ingvar the Plunderer, Woe Strike
    ApplySpellFix({ 42730 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].TriggerSpell = 42739;
        });

    ApplySpellFix({ 59735 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].TriggerSpell = 59736;
        });

    // Ingvar the Plunderer, Ingvar transform
    ApplySpellFix({ 42796 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_DEATH_PERSISTENT;
        });

    ApplySpellFix({
        42772,  // Hurl Dagger (Normal)
        59685   // Hurl Dagger (Heroic)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_REQ_AMMO;
        });

    // Control Crystal Activation
    ApplySpellFix({ 57804 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(1);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Destroy Door Seal
    ApplySpellFix({ 58040 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ChannelInterruptFlags &= ~(AURA_INTERRUPT_FLAG_HITBYSPELL | AURA_INTERRUPT_FLAG_TAKE_DAMAGE);
        });

    // Ichoron, Water Blast
    ApplySpellFix({ 54237, 59520 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    // Krik'thir - Mind Flay
    ApplySpellFix({ 52586, 59367 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ChannelInterruptFlags |= AURA_INTERRUPT_FLAG_MOVE;
        });

    // Glare of the Tribunal
    ApplySpellFix({ 50988, 59870 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Static Charge
    ApplySpellFix({ 50835, 59847 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ALLY);
        });

    // Lava Strike damage
    ApplySpellFix({ 57697 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
        });

    // Lava Strike trigger
    ApplySpellFix({ 57578 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
        });

    // Gift of Twilight Shadow/Fire
    ApplySpellFix({ 57835, 58766 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx &= ~SPELL_ATTR1_CHANNELED_1;
        });

    // Pyrobuffet
    ApplySpellFix({ 57557 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 56911;
        });

    // Arcane Barrage
    ApplySpellFix({ 56397 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
            spellInfo->_effects[EFFECT_2].TargetB = SpellImplicitTargetInfo();
        });

    ApplySpellFix({
        55849,  // Power Spark (ground +50% dmg aura)
        56438,  // Arcane Overload (-50% dmg taken) - this is to prevent apply -> unapply -> apply ... dunno whether it's correct
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Vortex (Control Vehicle)
    ApplySpellFix({ 56263 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    // Haste (Nexus Lord, increase run Speed of the disk)
    ApplySpellFix({ 57060 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_VEHICLE);
        });

    // Arcane Overload
    ApplySpellFix({ 56430 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_TRIGGER_MISSILE;
            spellInfo->_effects[EFFECT_0].TriggerSpell = 56429;
        });

    // Summon Arcane Bomb
    ApplySpellFix({ 56429 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_2].TargetB = SpellImplicitTargetInfo();
        });

    // Destroy Platform Event
    ApplySpellFix({ 59099 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(22);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo(15);
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(22);
            spellInfo->_effects[EFFECT_2].TargetB = SpellImplicitTargetInfo(15);
        });

    // Surge of Power (Phase 3)
    ApplySpellFix({
        57407,  // N
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
            spellInfo->InterruptFlags = 0;
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS);
            spellInfo->AttributesEx4 |= SPELL_ATTR4_CAN_CAST_WHILE_CASTING;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENEMY);
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Surge of Power (Phase 3)
    ApplySpellFix({
        60936   // H
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 3;
            spellInfo->InterruptFlags = 0;
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS);
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENEMY);
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Wyrmrest Drake - Life Burst
    ApplySpellFix({ 57143 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ALLY);
            spellInfo->_effects[EFFECT_1].PointsPerComboPoint = 2500;
            spellInfo->_effects[EFFECT_1].BasePoints = 2499;
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(1);
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    //Alexstrasza - Gift
    ApplySpellFix({ 61028 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
        });

    // Vortex (freeze anim)
    ApplySpellFix({ 55883 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags |= AURA_INTERRUPT_FLAG_CHANGE_MAP;
        });

    // Hurl Pyrite
    ApplySpellFix({ 62490 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Ulduar, Mimiron, Magnetic Core (summon)
    // Meeting Stone Summon
    ApplySpellFix({ 64444, 23598 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_CASTER);
        });

    // Ulduar, Mimiron, bomb bot explosion
    ApplySpellFix({ 63801 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].MiscValue = 17286;
        });

    // Ulduar, Mimiron, Summon Flames Initial
    ApplySpellFix({ 64563 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Ulduar, Mimiron, Flames (damage)
    ApplySpellFix({ 64566 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->AttributesEx4 &= ~SPELL_ATTR4_IGNORE_RESISTANCES;
        });

    // Ulduar, Hodir, Starlight
    ApplySpellFix({ 62807 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_1_YARD); // 1yd
        });

    // Hodir Shatter Cache
    ApplySpellFix({ 62502 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENTRY);
        });

    // Ulduar, General Vezax, Mark of the Faceless
    ApplySpellFix({ 63278 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;
        });

    // Boom (XT-002)
    ApplySpellFix({ 62834 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Supercharge
    ApplySpellFix({ 61920 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Lightning Whirl
    ApplySpellFix({ 61916 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 3;
        });

    ApplySpellFix({ 63482 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 8;
        });

    // Stone Grip, remove absorb aura
    ApplySpellFix({ 62056, 63985 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Sentinel Blast
    ApplySpellFix({ 64389, 64678 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Dispel = DISPEL_MAGIC;
        });

    // Potent Pheromones
    ApplySpellFix({ 62619 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
        });

    // Healthy spore summon periodic
    ApplySpellFix({ 62566 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Amplitude = 2000;
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_PERIODIC_TRIGGER_SPELL;
        });

    // Brightleaf Essence trigger
    ApplySpellFix({ 62968 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;  // duplicate
        });

    // Potent Pheromones
    ApplySpellFix({ 64321 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_ONLY_TARGET_PLAYERS;
            spellInfo->AttributesEx |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
        });

    // Lightning Orb Charged
    ApplySpellFix({ 62186 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Amplitude = 5000; // Duration 5 secs, amplitude 8 secs...
        });

    // Lightning Pillar
    ApplySpellFix({ 62976 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(6);
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(28); // 5 seconds, wrong DBC data?
        });

    // Sif's Blizzard
    ApplySpellFix({ 62576, 62602 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_8_YARDS); // 8yd
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_8_YARDS); // 8yd
        });

    // Protective Gaze
    ApplySpellFix({ 64175 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RecoveryTime = 25000;
        });

    // Shadow Beacon
    ApplySpellFix({ 64465 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 64467; // why do they need two script effects :/ (this one has visual effect)
        });

    // Sanity
    ApplySpellFix({ 63050 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx6 |= SPELL_ATTR6_CAN_TARGET_INVISIBLE;
        });

    // Shadow Nova
    ApplySpellFix({ 62714, 65209 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    // Cosmic Smash (Algalon the Observer)
    ApplySpellFix({ 62293 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_DEST_CASTER);
        });

    // Cosmic Smash (Algalon the Observer)
    ApplySpellFix({ 62311, 64596 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yd
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13);  // 50000yd
        });

    // Constellation Phase Effect
    ApplySpellFix({ 65509 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
        });

    // Black Hole
    ApplySpellFix({ 62168, 65250, 62169 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_NEGATIVE_1;
        });

    // Ground Slam
    ApplySpellFix({ 62625 }, [](SpellInfo* spellInfo)
        {
            spellInfo->InterruptFlags |= SPELL_INTERRUPT_FLAG_INTERRUPT;
        });

    // Onyxia's Lair, Onyxia, Flame Breath (TriggerSpell = 0 and spamming errors in console)
    ApplySpellFix({ 18435 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Onyxia's Lair, Onyxia, Create Onyxia Spawner
    ApplySpellFix({ 17647 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(37);
        });

    ApplySpellFix({
        17646,  // Onyxia's Lair, Onyxia, Summon Onyxia Whelp
        68968   // Onyxia's Lair, Onyxia, Summon Lair Guard
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->Targets |= TARGET_FLAG_DEST_LOCATION;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13); // 50000yd
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(5);
        });

    // Onyxia's Lair, Onyxia, Eruption
    ApplySpellFix({ 17731, 69294 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_DUMMY;
            spellInfo->CastTimeEntry = sSpellCastTimesStore.LookupEntry(3);
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_18_YARDS); // 18yd instead of 13yd to make sure all cracks erupt
        });

    // Onyxia's Lair, Onyxia, Breath
    ApplySpellFix({
        18576, 18578, 18579, 18580, 18581, 18582, 18583, 18609, 18611, 18612, 18613, 18614, 18615, 18616, 18584,
        18585, 18586, 18587, 18588, 18589, 18590, 18591, 18592, 18593, 18594, 18595, 18564, 18565, 18566, 18567,
        18568, 18569, 18570, 18571, 18572, 18573, 18574, 18575, 18596, 18597, 18598, 18599, 18600, 18601, 18602,
        18603, 18604, 18605, 18606, 18607, 18617, 18619, 18620, 18621, 18622, 18623, 18624, 18625, 18626, 18627,
        18628, 18618, 18351, 18352, 18353, 18354, 18355, 18356, 18357, 18358, 18359, 18360, 18361, 17086, 17087,
        17088, 17089, 17090, 17091, 17092, 17093, 17094, 17095, 17097, 22267, 22268, 21132, 21133, 21135, 21136,
        21137, 21138, 21139
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(328); // 250ms
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(1);
            if (spellInfo->_effects[EFFECT_1].Effect)
            {
                spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_APPLY_AURA;
                spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_PERIODIC_TRIGGER_SPELL;
                spellInfo->_effects[EFFECT_1].Amplitude = ((spellInfo->CastTimeEntry == sSpellCastTimesStore.LookupEntry(170)) ? 50 : 215);
            }
        });

    ApplySpellFix({
        48760,  // Oculus, Teleport to Coldarra DND
        49305   // Oculus, Teleport to Boss 1 DND
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(25);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(17);
        });

    // Oculus, Drake spell Stop Time
    ApplySpellFix({ 49838 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
            spellInfo->ExcludeTargetAuraSpell = 51162; // exclude planar shift
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_150_YARDS);
        });

    // Oculus, Varos Cloudstrider, Energize Cores
    ApplySpellFix({ 61407, 62136, 56251, 54069 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CONE_ENTRY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Halls of Lightning, Arc Weld
    ApplySpellFix({ 59086 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(1);
        });

    // Halls of Lightning, Arcing Burn
    ApplySpellFix({ 52671, 59834 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Trial of the Champion, Death's Respite
    ApplySpellFix({ 68306 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(25);
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(25);
        });

    // Trial of the Champion, Eadric Achievement (The Faceroller)
    ApplySpellFix({ 68197 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ALLY);
            spellInfo->Attributes |= SPELL_ATTR0_CASTABLE_WHILE_DEAD;
        });

    // Trial of the Champion, Earth Shield
    ApplySpellFix({ 67530 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_PROC_TRIGGER_SPELL; // will trigger 67537
        });

    // Trial of the Champion, Hammer of the Righteous
    ApplySpellFix({ 66867 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_DUMMY;
        });

    // Trial of the Champion, Summon Risen Jaeren/Arelas
    ApplySpellFix({ 67705, 67715 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_DEAD;
        });

    // Trial of the Champion, Ghoul Explode
    ApplySpellFix({ 67751 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENTRY);
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS);
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENTRY);
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS);
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_2].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENTRY);
            spellInfo->_effects[EFFECT_2].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS);
        });

    // Trial of the Champion, Desecration
    ApplySpellFix({ 67778, 67877 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 68766;
        });

    // Killing Spree (Off hand damage)
    ApplySpellFix({ 57842 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(2); // Melee Range
        });

    // Trial of the Crusader, Jaraxxus Intro spell
    ApplySpellFix({ 67888 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_STOP_ATTACK_TARGET;
            spellInfo->AttributesEx |= SPELL_ATTR1_NO_THREAT;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Trial of the Crusader, Lich King Intro spell
    ApplySpellFix({ 68193 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENEMY);
        });

    // Trial of the Crusader, Gormok, player vehicle spell, CUSTOM! (default jump to hand, not used)
    ApplySpellFix({ 66342 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_SET_VEHICLE_ID;
            spellInfo->_effects[EFFECT_0].MiscValue = 496;
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(21);
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13);
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(25);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->AuraInterruptFlags = AURA_INTERRUPT_FLAG_CHANGE_MAP;
        });

    // Trial of the Crusader, Gormok, Fire Bomb
    ApplySpellFix({ 66313 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_DEST_TARGET_ANY);
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo(TARGET_DEST_TARGET_ANY);
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    ApplySpellFix({ 66317 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_STOP_ATTACK_TARGET;
            spellInfo->AttributesEx |= SPELL_ATTR1_NO_THREAT;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    ApplySpellFix({ 66318 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->Speed = 14.0f;
            spellInfo->Attributes |= SPELL_ATTR0_STOP_ATTACK_TARGET;
            spellInfo->AttributesEx |= SPELL_ATTR1_NO_THREAT;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    ApplySpellFix({ 66320, 67472, 67473, 67475 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_2_YARDS);
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_2_YARDS);
        });

    // Trial of the Crusader, Acidmaw & Dreadscale, Emerge
    ApplySpellFix({ 66947 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx5 |= SPELL_ATTR5_USABLE_WHILE_STUNNED;
        });

    // Trial of the Crusader, Jaraxxus, Curse of the Nether
    ApplySpellFix({ 66211 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 66209; // exclude Touch of Jaraxxus
        });

    // Trial of the Crusader, Jaraxxus, Summon Volcano
    ApplySpellFix({ 66258, 67901 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(85); // summon for 18 seconds, 15 not enough
        });

    // Trial of the Crusader, Jaraxxus, Spinning Pain Spike
    ApplySpellFix({ 66281 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_4_YARDS);
        });

    ApplySpellFix({ 66287 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_MOD_TAUNT;
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_NEARBY_ENTRY);
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_2].ApplyAuraName = SPELL_AURA_MOD_STUN;
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(35); // 4 secs
        });

    // Trial of the Crusader, Jaraxxus, Fel Fireball
    ApplySpellFix({ 66532, 66963, 66964, 66965 }, [](SpellInfo* spellInfo)
        {
            spellInfo->InterruptFlags |= SPELL_INTERRUPT_FLAG_INTERRUPT;
        });

    // tempfix, make Nether Power not stealable
    ApplySpellFix({ 66228, 67106, 67107, 67108 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx4 |= SPELL_ATTR4_NOT_STEALABLE;
        });

    // Trial of the Crusader, Faction Champions, Druid - Tranquality
    ApplySpellFix({ 66086, 67974, 67975, 67976 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_APPLY_AREA_AURA_FRIEND;
        });

    // Trial of the Crusader, Faction Champions, Shaman - Earth Shield
    ApplySpellFix({ 66063 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_PROC_TRIGGER_SPELL;
            spellInfo->_effects[EFFECT_0].TriggerSpell = 66064;
        });

    // Trial of the Crusader, Faction Champions, Priest - Mana Burn
    ApplySpellFix({ 66100 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 5;
            spellInfo->_effects[EFFECT_0].DieSides = 0;
        });

    ApplySpellFix({ 68026 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 8;
            spellInfo->_effects[EFFECT_0].DieSides = 0;
        });

    ApplySpellFix({ 68027 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 6;
            spellInfo->_effects[EFFECT_0].DieSides = 0;
        });

    ApplySpellFix({ 68028 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 10;
            spellInfo->_effects[EFFECT_0].DieSides = 0;
        });

    // Trial of the Crusader, Twin Valkyr, Touch of Light/Darkness, Light/Dark Surge
    ApplySpellFix({
        65950   // light 0
        }, [](SpellInfo* spellInfo)
        {
            //spellInfo->EffectApplyAuraName[0] = SPELL_AURA_PERIODIC_DUMMY;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(6);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    ApplySpellFix({
        65767   // light surge 0
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 65686;
        });

    ApplySpellFix({
        67296   // light 1
        }, [](SpellInfo* spellInfo)
        {
            //spellInfo->_effects[EFFECT_0].ApplyAuraNames = SPELL_AURA_PERIODIC_DUMMY;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(6);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    ApplySpellFix({
        67274   // light surge 1
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 67222;
        });

    ApplySpellFix({
        67297   // light 2
        }, [](SpellInfo* spellInfo)
        {
            //spellInfo->_effects[EFFECT_0].ApplyAuraNames = SPELL_AURA_PERIODIC_DUMMY;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(6);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    ApplySpellFix({
        67275   // light surge 2
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 67223;
        });

    ApplySpellFix({
        67298   // light 3
        }, [](SpellInfo* spellInfo)
        {
            //spellInfo->_effects[EFFECT_0].ApplyAuraNames = SPELL_AURA_PERIODIC_DUMMY;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(6);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    ApplySpellFix({
        67276   // light surge 3
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 67224;
        });

    ApplySpellFix({
        66001   // dark 0
        }, [](SpellInfo* spellInfo)
        {
            //spellInfo->_effects[EFFECT_0].ApplyAuraNames = SPELL_AURA_PERIODIC_DUMMY;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(6);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    ApplySpellFix({
        65769   // dark surge 0
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 65684;
        });

    ApplySpellFix({
        67281   // dark 1
        }, [](SpellInfo* spellInfo)
        {
            //spellInfo->_effects[EFFECT_0].ApplyAuraNames = SPELL_AURA_PERIODIC_DUMMY;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(6);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    ApplySpellFix({
        67265   // dark surge 1
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 67176;
        });

    ApplySpellFix({
        67282   // dark 2
        }, [](SpellInfo* spellInfo)
        {
            //spellInfo->_effects[EFFECT_0].ApplyAuraNames = SPELL_AURA_PERIODIC_DUMMY;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(6);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    ApplySpellFix({
        67266   // dark surge 2
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 67177;
        });

    ApplySpellFix({
        67283   // dark 3
        }, [](SpellInfo* spellInfo)
        {
            //spellInfo->_effects[EFFECT_0].ApplyAuraNames = SPELL_AURA_PERIODIC_DUMMY;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(6);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    ApplySpellFix({
        67267   // dark surge 3
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 67178;
        });

    // Trial of the Crusader, Twin Valkyr, Twin's Pact
    ApplySpellFix({ 65875, 67303, 67304, 67305, 65876, 67306, 67307, 67308 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    // Trial of the Crusader, Anub'arak, Emerge
    ApplySpellFix({ 65982 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx5 |= SPELL_ATTR5_USABLE_WHILE_STUNNED;
        });

    // Trial of the Crusader, Anub'arak, Penetrating Cold
    ApplySpellFix({ 66013, 67700, 68509, 68510 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yd
        });

    // Trial of the Crusader, Anub'arak, Shadow Strike
    ApplySpellFix({ 66134 }, [](SpellInfo* spellInfo)
        {
            spellInfo->InterruptFlags |= SPELL_INTERRUPT_FLAG_INTERRUPT;
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;
        });

    // Trial of the Crusader, Anub'arak, Pursuing Spikes
    ApplySpellFix({ 65920, 65922, 65923 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_PERIODIC_DUMMY;
            //spellInfo->EffectTriggerSpell[0] = 0;
        });

    // Trial of the Crusader, Anub'arak, Summon Scarab
    ApplySpellFix({ 66339 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(35);
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(25);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Trial of the Crusader, Anub'arak, Achievements: The Traitor King
    ApplySpellFix({
        68186,  // Anub'arak Scarab Achievement 10
        68515   // Anub'arak Scarab Achievement 25
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENEMY);
            spellInfo->Attributes |= SPELL_ATTR0_CASTABLE_WHILE_DEAD;
        });

    // Trial of the Crusader, Anub'arak, Spider Frenzy
    ApplySpellFix({ 66129 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Soul Sickness
    ApplySpellFix({ 69131 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_PERIODIC_TRIGGER_SPELL;
            spellInfo->_effects[EFFECT_0].Amplitude = 8000;
            spellInfo->_effects[EFFECT_0].TriggerSpell = 69133;
        });

    // Phantom Blast
    ApplySpellFix({ 68982, 70322 }, [](SpellInfo* spellInfo)
        {
            spellInfo->InterruptFlags |= SPELL_INTERRUPT_FLAG_INTERRUPT;
        });

    // Empowered Blizzard
    ApplySpellFix({ 70131 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
        });

    // Ice Lance Volley
    ApplySpellFix({ 70464 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENTRY);
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_70_YARDS);
        });

    ApplySpellFix({
        70513,   // Multi-Shot
        59514    // Shriek of the Highborne
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CONE_ENTRY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Icicle
    ApplySpellFix({ 69428, 69426 }, [](SpellInfo* spellInfo)
        {
            spellInfo->InterruptFlags = 0;
            spellInfo->AuraInterruptFlags = 0;
            spellInfo->ChannelInterruptFlags = 0;
        });

    ApplySpellFix({ 70525, 70639 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_2].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENTRY);
            spellInfo->_effects[EFFECT_2].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_500_YARDS); // 500yd
        });

    // Frost Nova
    ApplySpellFix({ 68198 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13);
            spellInfo->Targets |= TARGET_FLAG_DEST_LOCATION;
        });

    // Blight
    ApplySpellFix({ 69604, 70286 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
            spellInfo->AttributesEx3 |= (SPELL_ATTR3_IGNORE_HIT_RESULT | SPELL_ATTR3_ONLY_TARGET_PLAYERS);
        });

    // Chilling Wave
    ApplySpellFix({ 68778, 70333 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_TARGET_ENEMY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    ApplySpellFix({ 68786, 70336 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= (SPELL_ATTR3_IGNORE_HIT_RESULT | SPELL_ATTR3_ONLY_TARGET_PLAYERS);
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_DUMMY;
        });

    // Pursuit
    ApplySpellFix({ 68987 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->_effects[EFFECT_2].TargetB = SpellImplicitTargetInfo();
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(6); // 100yd
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    ApplySpellFix({ 69029, 70850 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    // Explosive Barrage
    ApplySpellFix({ 69263 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_MOD_STUN;
        });

    // Overlord's Brand
    ApplySpellFix({ 69172 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcFlags = DONE_HIT_PROC_FLAG_MASK & ~PROC_FLAG_DONE_PERIODIC;
            spellInfo->ProcChance = 100;
        });

    // Icy Blast
    ApplySpellFix({ 69232 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].TriggerSpell = 69238;
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    ApplySpellFix({ 69233, 69646 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    ApplySpellFix({ 69238, 69628 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_DEST_DYNOBJ_NONE);
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo(TARGET_DEST_DYNOBJ_NONE);
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Hoarfrost
    ApplySpellFix({ 69246, 69245, 69645 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Devour Humanoid
    ApplySpellFix({ 69503 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ChannelInterruptFlags |= 0;
            spellInfo->AuraInterruptFlags = AURA_INTERRUPT_FLAG_MOVE | AURA_INTERRUPT_FLAG_TURNING;
        });

    // Falric: Defiling Horror
    ApplySpellFix({ 72435, 72452 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS);
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS);
        });

    // Frostsworn General - Throw Shield
    ApplySpellFix({ 69222, 73076 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
        });

    // Halls of Reflection Clone
    ApplySpellFix({ 69828 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    // Summon Ice Wall
    ApplySpellFix({ 69768 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->AttributesEx5 |= SPELL_ATTR5_CAN_CHANNEL_WHEN_MOVING;
        });

    ApplySpellFix({ 69767 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_TARGET_ANY);
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
        });

    // Essence of the Captured
    ApplySpellFix({ 73035, 70719 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13);
        });

    // Achievement Check
    ApplySpellFix({ 72830 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS);
        });

    ApplySpellFix({
        70781,  // Light's Hammer Teleport
        70856,  // Oratory of the Damned Teleport
        70857,  // Rampart of Skulls Teleport
        70858,  // Deathbringer's Rise Teleport
        70859,  // Upper Spire Teleport
        70860,  // Frozen Throne Teleport
        70861   // Sindragosa's Lair Teleport
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DB); // this target is for SPELL_EFFECT_TELEPORT_UNITS
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_2].TargetB = SpellImplicitTargetInfo();
        });

    ApplySpellFix({
        70960,  // Bone Flurry
        71258   // Adrenaline Rush (Ymirjar Battle-Maiden)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx &= ~SPELL_ATTR1_CHANNELED_2;
        });

    // Saber Lash (Lord Marrowgar)
    ApplySpellFix({ 69055, 70814 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_5_YARDS); // 5yd
        });

    // Impaled (Lord Marrowgar)
    ApplySpellFix({ 69065 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;    // remove stun so Dispersion can be used
        });

    // Cold Flame (Lord Marrowgar)
    ApplySpellFix({ 72701, 72702, 72703, 72704 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo();
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(9);   // 30 secs instead of 12, need him for longer, but will stop his actions after 12 secs
        });

    ApplySpellFix({ 69138 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(9);   // 30 secs instead of 12, need him for longer, but will stop his actions after 12 secs
        });

    ApplySpellFix({ 69146, 70823, 70824, 70825 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_3_YARDS); // 3yd instead of 5yd
            spellInfo->AttributesEx4 &= ~SPELL_ATTR4_IGNORE_RESISTANCES;
        });

    // Dark Martyrdom (Lady Deathwhisper)
    ApplySpellFix({ 70897 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_DEAD;
        });

    ApplySpellFix({
        69075,  // Bone Storm (Lord Marrowgar)
        70834,  // Bone Storm (Lord Marrowgar)
        70835,  // Bone Storm (Lord Marrowgar)
        70836,  // Bone Storm (Lord Marrowgar)
        72378,  // Blood Nova (Deathbringer Saurfang)
        73058,  // Blood Nova (Deathbringer Saurfang)
        72769,  // Scent of Blood (Deathbringer Saurfang)
        72385,  // Boiling Blood (Deathbringer Saurfang)
        72441,  // Boiling Blood (Deathbringer Saurfang)
        72442,  // Boiling Blood (Deathbringer Saurfang)
        72443,  // Boiling Blood (Deathbringer Saurfang)
        71160,  // Plague Stench (Stinky)
        71161,  // Plague Stench (Stinky)
        71123,  // Decimate (Stinky & Precious)
        71464   // Divine Surge (Sister Svalna)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS);   // 100yd
        });

    // Shadow's Fate
    ApplySpellFix({ 71169 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Lock Players and Tap Chest
    ApplySpellFix({ 72347 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 &= ~SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Award Reputation - Boss Kill
    ApplySpellFix({ 73843, 73844, 73845, 73846 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS);
        });

    // Death Plague (Rotting Frost Giant)
    ApplySpellFix({ 72864 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 0;
        });

    // Gunship Battle, spell Below Zero
    ApplySpellFix({ 69705 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    // Resistant Skin (Deathbringer Saurfang adds)
    ApplySpellFix({ 72723 }, [](SpellInfo* spellInfo)
        {
            // this spell initially granted Shadow damage immunity, however it was removed but the data was left in client
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
        });

    // Mark of the Fallen Champion (Deathbringer Saurfang)
    ApplySpellFix({ 72255, 72444, 72445, 72446 }, [](SpellInfo* spellInfo)
        {
            // Patch 3.3.2 (2010-01-02): Deathbringer Saurfang will no longer gain blood power from Mark of the Fallen Champion.
            // prevented in script, effect needed for Prayer of Mending
            spellInfo->AttributesEx3 &= ~SPELL_ATTR3_CANT_TRIGGER_PROC;
        });

    // Coldflame Jets (Traps after Saurfang)
    ApplySpellFix({ 70460 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(1); // 10 seconds
        });

    ApplySpellFix({
        70461,  // Coldflame Jets (Traps after Saurfang)
        71289   // Dominate Mind (Lady Deathwhisper)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Severed Essence (Val'kyr Herald)
    ApplySpellFix({ 71906, 71942 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    ApplySpellFix({
        71159,  // Awaken Plagued Zombies (Precious)
        71302   // Awaken Ymirjar Fallen (Ymirjar Deathbringer)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(21);
        });

    // Blood Prince Council, Invocation of Blood
    ApplySpellFix({ 70981, 70982, 70952 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;  // clear share health aura
        });

    // Ymirjar Frostbinder, Frozen Orb
    ApplySpellFix({ 71274 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(6);
        });

    // Ooze Flood (Rotface)
    ApplySpellFix({ 69783, 69797, 69799, 69802 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR1_CANT_TARGET_SELF;
        });

    // Volatile Ooze Beam Protection
    ApplySpellFix({ 70530 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_APPLY_AURA; // blizzard typo, 65 instead of 6, aura itself is defined (dummy)
        });

    // Professor Putricide, Gaseous Bloat (Orange Ooze Channel)
    ApplySpellFix({ 70672, 72455, 72832, 72833 }, [](SpellInfo* spellInfo)
        {
            // copied attributes from Green Ooze Channel
            spellInfo->Attributes |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    ApplySpellFix({
        71412,  // Green Ooze Summon (Professor Putricide)
        71415   // Orange Ooze Summon (Professor Putricide)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
        });

    ApplySpellFix({
        71621,  // Create Concoction (Professor Putricide)
        72850,
        72851,
        72852,
        71893,  // Guzzle Potions (Professor Putricide)
        73120,
        73121,
        73122
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->CastTimeEntry = sSpellCastTimesStore.LookupEntry(15); // 4 sec
        });

    // Mutated Plague (Professor Putricide)
    ApplySpellFix({ 72454, 72464, 72506, 72507 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx4 |= SPELL_ATTR4_IGNORE_RESISTANCES;
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
        });

    // Unbound Plague (Professor Putricide) (needs target selection script)
    ApplySpellFix({ 70911, 72854, 72855, 72856 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
        });

    // Mutated Transformation (Professor Putricide)
    ApplySpellFix({ 70402, 72511, 72512, 72513 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
        });

    // Empowered Flare (Blood Prince Council)
    ApplySpellFix({ 71708, 72785, 72786, 72787 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
        });

    ApplySpellFix({
        71518,  // Unholy Infusion Quest Credit (Professor Putricide)
        72934,  // Blood Infusion Quest Credit (Blood-Queen Lana'thel)
        72289   // Frost Infusion Quest Credit (Sindragosa)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS);   // another missing radius
        });

    // Swarming Shadows
    ApplySpellFix({ 71266, 72890 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AreaGroupId = 0; // originally, these require area 4522, which is... outside of Icecrown Citadel
        });

    ApplySpellFix({
        71301,  // Summon Dream Portal (Valithria Dreamwalker)
        71977   // Summon Nightmare Portal (Valithria Dreamwalker)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Column of Frost (visual marker)
    ApplySpellFix({ 70715 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(32); // 6 seconds (missing)
        });

    // Mana Void (periodic aura)
    ApplySpellFix({ 71085 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(9); // 30 seconds (missing)
        });

    // Summon Suppressor (needs target selection script)
    ApplySpellFix({ 70936 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Corruption
    ApplySpellFix({ 70602 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    ApplySpellFix({
        72706,  // Achievement Check (Valithria Dreamwalker)
        71357   // Order Whelp
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS);   // 200yd
        });

    // Sindragosa's Fury
    ApplySpellFix({ 70598 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
        });

    // Tail Smash (Sindragosa)
    ApplySpellFix({ 71077 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_CASTER_BACK);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_DEST_AREA_ENEMY);
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_DEST_CASTER_BACK);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_DEST_AREA_ENEMY);
        });

    // Frost Bomb
    ApplySpellFix({ 69846 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Speed = 0.0f;    // This spell's summon happens instantly
        });

    // Mystic Buffet (Sindragosa)
    ApplySpellFix({ 70127, 72528, 72529, 72530 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;  // remove obsolete spell effect with no targets
        });

    // Sindragosa, Frost Aura
    ApplySpellFix({ 70084, 71050, 71051, 71052 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes &= ~SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
        });

    // Ice Lock
    ApplySpellFix({ 71614 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Mechanic = MECHANIC_STUN;
        });

    // Lich King, Infest
    ApplySpellFix({ 70541, 73779, 73780, 73781 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Lich King, Necrotic Plague
    ApplySpellFix({ 70337, 73912, 73913, 73914, 70338, 73785, 73786, 73787 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    ApplySpellFix({
        69099,  // Ice Pulse 10n
        73776,  // Ice Pulse 25n
        73777,  // Ice Pulse 10h
        73778   // Ice Pulse 25h
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            spellInfo->AttributesEx4 &= ~SPELL_ATTR4_IGNORE_RESISTANCES;
        });

    // Fury of Frostmourne
    ApplySpellFix({ 72350 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
        });

    ApplySpellFix({
        72351,  // Fury of Frostmourne
        72431,  // Jump (removes Fury of Frostmourne debuff)
        72429,  // Mass Resurrection
        73159   // Play Movie
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
        });

    // Raise Dead
    ApplySpellFix({ 72376 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 4;
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
        });

    // Jump
    ApplySpellFix({ 71809 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(5); // 40yd
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS); // 10yd
            spellInfo->_effects[EFFECT_0].MiscValue = 190;
        });

    // Broken Frostmourne
    ApplySpellFix({ 72405 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
        });

    // Summon Shadow Trap
    ApplySpellFix({ 73540 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(3); // 60 seconds
        });

    // Shadow Trap (visual)
    ApplySpellFix({ 73530 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(28); // 5 seconds
        });

    // Shadow Trap
    ApplySpellFix({ 73529 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS); // 10yd
        });

    // Shadow Trap (searcher)
    ApplySpellFix({ 74282 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_3_YARDS); // 3yd
        });

    // Raging Spirit Visual
    ApplySpellFix({ 69198 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13); // 50000yd
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    // Defile
    ApplySpellFix({ 72762 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(559); // 53 seconds
            spellInfo->ExcludeCasterAuraSpell = 0;
            spellInfo->Attributes |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
            spellInfo->AttributesEx6 |= (SPELL_ATTR6_CAN_TARGET_INVISIBLE | SPELL_ATTR6_CAN_TARGET_UNTARGETABLE);
        });

    // Defile
    ApplySpellFix({ 72743 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(22); // 45 seconds
        });

    ApplySpellFix({ 72754, 73708, 73709, 73710 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
        });

    // Val'kyr Target Search
    ApplySpellFix({ 69030 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
            spellInfo->Attributes |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
        });

    // Harvest Souls
    ApplySpellFix({ 73654, 74295, 74296, 74297 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
            spellInfo->_effects[EFFECT_2].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
        });

    // Restore Soul
    ApplySpellFix({ 72595, 73650 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
        });

    // Kill Frostmourne Players
    ApplySpellFix({ 75127 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
        });

    // Harvest Soul
    ApplySpellFix({ 73655 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_DONE_BONUS;
        });

    // Destroy Soul
    ApplySpellFix({ 74086 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
        });

    // Summon Spirit Bomb
    ApplySpellFix({ 74302, 74342 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
            spellInfo->MaxAffectedTargets = 1;
        });

    // Summon Spirit Bomb
    ApplySpellFix({ 74341, 74343 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_200_YARDS); // 200yd
            spellInfo->MaxAffectedTargets = 3;
        });

    // Summon Spirit Bomb
    ApplySpellFix({ 73579 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_25_YARDS); // 25yd
        });

    // Trigger Vile Spirit (Inside, Heroic)
    ApplySpellFix({ 73582 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS); // 50000yd
        });

    // Scale Aura (used during Dominate Mind from Lady Deathwhisper)
    ApplySpellFix({ 73261 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags |= AURA_INTERRUPT_FLAG_CHANGE_MAP;
        });

    // Leap to a Random Location
    ApplySpellFix({ 70485 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(6); // 100yd
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS);
            spellInfo->_effects[EFFECT_0].MiscValue = 100;
        });

    // Empowered Blood
    ApplySpellFix({ 70227, 70232 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AreaGroupId = 2452; // Whole icc instead of Crimson Halls only, remove when area calculation is fixed
        });

    ApplySpellFix({ 74509 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_20_YARDS); // 20yd
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_20_YARDS); // 20yd
        });

    // Rallying Shout
    ApplySpellFix({ 75414 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_20_YARDS); // 20yd
        });

    // Barrier Channel
    ApplySpellFix({ 76221 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ChannelInterruptFlags &= ~(AURA_INTERRUPT_FLAG_TURNING | AURA_INTERRUPT_FLAG_MOVE);
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_NEARBY_ENTRY);
        });

    // Intimidating Roar
    ApplySpellFix({ 74384 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yd
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yd
        });

    ApplySpellFix({
        74562,  // Fiery Combustion
        74792   // Soul Consumption
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR1_CANT_BE_REDIRECTED;
        });

    // Combustion
    ApplySpellFix({ 75883, 75884 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_6_YARDS); // 6yd
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_6_YARDS); // 6yd
        });

    // Consumption
    ApplySpellFix({ 75875, 75876 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_6_YARDS); // 6yd
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_6_YARDS); // 6yd
            spellInfo->_effects[EFFECT_0].Mechanic = MECHANIC_NONE;
            spellInfo->_effects[EFFECT_1].Mechanic = MECHANIC_SNARE;
        });

    // Soul Consumption
    ApplySpellFix({ 74799 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_12_YARDS); // 12yd
        });

    // Twilight Cutter
    ApplySpellFix({ 74769, 77844, 77845, 77846 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yd
        });

    // Twilight Mending
    ApplySpellFix({ 75509 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx6 |= SPELL_ATTR6_CAN_TARGET_INVISIBLE;
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yd
            spellInfo->_effects[EFFECT_1].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_100_YARDS); // 100yd
        });

    // Meteor Strike
    ApplySpellFix({ 74637 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Speed = 0;
        });

    //Blazing Aura
    ApplySpellFix({ 75885, 75886 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx4 &= ~SPELL_ATTR4_IGNORE_RESISTANCES;
        });

    ApplySpellFix({
        75952,  //Meteor Strike
        74629   //Combustion Periodic
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx4 &= ~SPELL_ATTR4_IGNORE_RESISTANCES;
        });

    // Going Bearback
    ApplySpellFix({ 54897 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_DUMMY;
            spellInfo->_effects[EFFECT_1].RadiusEntry = spellInfo->_effects[EFFECT_0].RadiusEntry;
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_DEST_AREA_ENTRY);
        });

    // Still At It
    ApplySpellFix({ 51931, 51932, 51933 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(38);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Rallying the Troops
    ApplySpellFix({ 47394 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 47394;
        });

    // A Tangled Skein
    ApplySpellFix({ 51165, 51173 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    ApplySpellFix({
        69563,  // A Cloudlet of Classy Cologne
        69445,  // A Perfect Puff of Perfume
        69489   // Bonbon Blitz
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        });

    // Control
    ApplySpellFix({ 30790 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].MiscValue = 0;
        });

    // Reclusive Runemaster
    ApplySpellFix({ 48028 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENEMY);
        });

    // Mastery of
    ApplySpellFix({ 65147 }, [](SpellInfo* spellInfo)
        {
            spellInfo->CategoryEntry = sSpellCategoryStore.LookupEntry(1244);
            spellInfo->CategoryRecoveryTime = 1500;
        });

    // Weakness to Lightning
    ApplySpellFix({ 46432 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 &= ~SPELL_ATTR3_DEATH_PERSISTENT;
        });

    // Wrangle Some Aether Rays!
    ApplySpellFix({ 40856 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(27); // 3000ms
        });

    // The Black Knight's Orders
    ApplySpellFix({ 63163 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 52390;
        });

    // The Warp Rifts
    ApplySpellFix({ 34888 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(5); // 300 secs
        });

    // The Smallest Creatures
    ApplySpellFix({ 38544 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValueB = 427;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(1);
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Ridding the red rocket
    ApplySpellFix({ 49177 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 1; // corrects seat id (points - 1 = seatId)
        });

    // Jormungar Strike
    ApplySpellFix({ 56513 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RecoveryTime = 2000;
        });

    ApplySpellFix({
        37851, // Tag Greater Felfire Diemetradon
        37918  // Arcano-pince
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->RecoveryTime = 3000;
        });

    ApplySpellFix({
        54997, // Cast Net (tooltip says 10s but sniffs say 6s)
        56524  // Acid Breath
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->RecoveryTime = 6000;
        });

    ApplySpellFix({
        47911, // EMP
        48620, // Wing Buffet
        51752  // Stampy's Stompy-Stomp
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->RecoveryTime = 10000;
        });

    ApplySpellFix({
        37727, // Touch of Darkness
        54996  // Ice Slick (tooltip says 20s but sniffs say 12s)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->RecoveryTime = 12000;
        });

    // Signal Helmet to Attack
    ApplySpellFix({ 51748 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RecoveryTime = 15000;
        });

    ApplySpellFix({
        51756, // Charge
        37919, //Arcano-dismantle
        37917  //Arcano-Cloak
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->RecoveryTime = 20000;
        });

    // Kaw the Mammoth Destroyer
    ApplySpellFix({ 46260 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // That's Abominable
    ApplySpellFix({ 59565 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValueB = 1721; // controlable guardian
        });

    // Investigate the Blue Recluse (1920)
    // Investigate the Alchemist Shop (1960)
    ApplySpellFix({ 9095 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS); // 10yd
        });

    // Gauging the Resonant Frequency (10594)
    ApplySpellFix({ 37390 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValueB = 181;
        });

    // Where in the World is Hemet Nesingwary? (12521)
    ApplySpellFix({ 50860 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 50860;
        });

    ApplySpellFix({ 50861 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 0;
        });

    // Riding Jokkum
    ApplySpellFix({ 56606 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 1;
        });

    // Blightbeasts be Damned! (12072)
    ApplySpellFix({ 47424 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags &= ~AURA_INTERRUPT_FLAG_NOT_ABOVEWATER;
        });

    // Dark Horizon (12664), Reunited (12663)
    ApplySpellFix({ 52190 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 52391 - 1;
        });

    // The Sum is Greater than the Parts (13043) - Chained Grip
    ApplySpellFix({ 60540 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValue = 300;
        });

    // Not a Bug (13342)
    ApplySpellFix({ 60531 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_DEAD;
        });

    // Frankly,  It Makes No Sense... (10672)
    ApplySpellFix({ 37851 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Honor Challenge (12939)
    ApplySpellFix({ 21855 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Convocation at Zol'Heb (12730)
    ApplySpellFix({ 52956 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_DEST_AREA_ENTRY);
        });

    // Mangletooth
    ApplySpellFix({
        7764,   // Wisdom of Agamaggan
        10767,  // Rising Spirit
        16610,  // Razorhide
        16612,  // Agamaggan's Strength
        16618,  // Spirit of the Wind
        17013   // Agamaggan's Agility
        }, [](SpellInfo* spellInfo)

        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->AttributesEx5 |= SPELL_ATTR5_SKIP_CHECKCAST_LOS_CHECK;
        });

    //Crushing the Crown
    ApplySpellFix({ 71024 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DYNOBJ_NONE);
        });

    // Battle for the Undercity
    ApplySpellFix({
        59892   // Cyclone fall
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_APPLY_AREA_AURA_FRIEND;
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_10_YARDS); // 10yd
            spellInfo->AttributesEx &= ~SPELL_ATTR0_CANT_CANCEL;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_ONLY_TARGET_PLAYERS;
        });

    // enchant Lightweave Embroidery
    ApplySpellFix({ 55637 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].MiscValue = 126;
        });

    ApplySpellFix({
        47977, // Magic Broom
        65917  // Magic Rooster
        }, [](SpellInfo* spellInfo)
        {
            // First two effects apply auras, which shouldn't be there
            // due to NO_TARGET applying aura on current caster (core bug)
            // Just wipe effect data, to mimic blizz-behavior
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Titanium Seal of Dalaran, Toss your luck
    ApplySpellFix({ 60476 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        });

    // Mind Amplification Dish, change charm aura
    ApplySpellFix({ 26740 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_MOD_CHARM;
        });

    // Persistent Shield
    ApplySpellFix({ 26467 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE;
            spellInfo->_effects[EFFECT_0].TriggerSpell = 26470;
        });

    // Deadly Swiftness
    ApplySpellFix({ 31255 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 22588;
        });

    // Black Magic enchant
    ApplySpellFix({ 59630 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_PASSIVE;
        });

    // Precious's Ribbon
    ApplySpellFix({ 72968 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_DEATH_PERSISTENT;
        });

    ApplySpellFix({
        71646,  // Item - Bauble of True Blood 10m
        71607,  // Item - Bauble of True Blood 25m
        71610,  // Item - Althor's Abacus trigger 10m
        71641   // Item - Althor's Abacus trigger 25m
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->DmgClass = SPELL_DAMAGE_CLASS_MAGIC;
            spellInfo->SpellLevel = 0;
        });

    ApplySpellFix({
        6789,  // Warlock - Death Coil (Rank 1)
        17925, // Warlock - Death Coil (Rank 2)
        17926, // Warlock - Death Coil (Rank 3)
        27223, // Warlock - Death Coil (Rank 4)
        47859, // Warlock - Death Coil (Rank 5)
        71838, // Drain Life - Bryntroll Normal
        71839  // Drain Life - Bryntroll Heroic
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
        });

    // Alchemist's Stone
    ApplySpellFix({ 17619 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_DEATH_PERSISTENT;
        });

    // Stormchops
    ApplySpellFix({ 43730 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(1);
            spellInfo->_effects[EFFECT_1].TargetB = SpellImplicitTargetInfo();
        });

    // Savory Deviate Delight (transformations), allow to mount while transformed
    ApplySpellFix({ 8219, 8220, 8221, 8222 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes &= ~SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
        });

    // Clamlette Magnifique
    ApplySpellFix({ 72623 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = spellInfo->_effects[EFFECT_1].BasePoints;
        });

    // Compact Harvest Reaper
    ApplySpellFix({ 4078 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(6); // 10 minutes
        });

    // Dragon Kite, Tuskarr Kite - Kite String
    ApplySpellFix({ 45192 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(6); // 100yd
        });

    // Frigid Frostling, Infrigidate
    ApplySpellFix({ 74960 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_20_YARDS); // 20yd
        });

    // Apple Trap
    ApplySpellFix({ 43450 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENEMY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_DUMMY;
        });

    // Dark Iron Attack - spawn mole machine
    ApplySpellFix({ 43563 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;  // summon GO's manually
        });

    // Throw Mug visual
    ApplySpellFix({ 42300 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
        });

    // Dark Iron knockback Aura
    ApplySpellFix({ 42299 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
            spellInfo->_effects[EFFECT_0].MiscValue = 100;
            spellInfo->_effects[EFFECT_0].BasePoints = 79;
        });

    // Chug and Chuck!
    ApplySpellFix({ 42436 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_SRC_CASTER);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENTRY);
            spellInfo->MaxAffectedTargets = 0;
            spellInfo->ExcludeCasterAuraSpell = 42299;
        });

    // Brewfest quests
    ApplySpellFix({ 47134, 51798 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;
        });

    // The Heart of The Storms (12998)
    ApplySpellFix({ 43528 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(18);
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(25);
        });

    // Water splash
    ApplySpellFix({ 42348 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_NONE;
        });

    // Summon Lantersn
    ApplySpellFix({ 44255, 44231 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_DEST);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Throw Head Back
    ApplySpellFix({ 42401 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_NEARBY_ENTRY);
        });

    // Food
    ApplySpellFix({ 65418 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].TriggerSpell = 65410;
        });

    ApplySpellFix({ 65422 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].TriggerSpell = 65414;
        });

    ApplySpellFix({ 65419 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].TriggerSpell = 65416;
        });

    ApplySpellFix({ 65420 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].TriggerSpell = 65412;
        });

    ApplySpellFix({ 65421 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_2].TriggerSpell = 65415;
        });

    // Stamp Out Bonfire
    ApplySpellFix({ 45437 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_DUMMY;
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_NEARBY_ENTRY);
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Light Bonfire (DND)
    ApplySpellFix({ 29831 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Infernal
    ApplySpellFix({ 33240 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_2].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
        });

    ApplySpellFix({
        47476,  // Deathknight - Strangulate
        15487,  // Priest - Silence
        5211,   // Druid - Bash  - R1
        6798,   // Druid - Bash  - R2
        8983    // Druid - Bash  - R3
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx7 |= SPELL_ATTR7_INTERRUPT_ONLY_NONPLAYER;
        });

    // Shadowmeld
    ApplySpellFix({ 58984 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_ONLY_TARGET_PLAYERS;
        });

    // Flare activation speed
    ApplySpellFix({ 1543 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Speed = 0.0f;
        });

    // Light's Beacon
    ApplySpellFix({ 53651 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR1_NO_THREAT;
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Shadow Hunter Vosh'gajin - Hex
    ApplySpellFix({ 16097 }, [](SpellInfo* spellInfo)
        {
            spellInfo->CastTimeEntry = sSpellCastTimesStore.LookupEntry(16);
        });

    // Sacred Cleansing
    ApplySpellFix({ 53659 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(5); // 40yd
        });

    // Silithyst
    ApplySpellFix({ 29519 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ApplyAuraName = SPELL_AURA_MOD_DECREASE_SPEED;
            spellInfo->_effects[EFFECT_0].BasePoints = -25;
        });

    // Focused Eyebeam Summon Trigger
    ApplySpellFix({ 63342 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Luffa
    ApplySpellFix({ 23595 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 1; // Remove only 1 bleed effect
        });

    // Eye of Kilrogg Passive (DND)
    ApplySpellFix({ 2585 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags |= AURA_INTERRUPT_FLAG_HITBYSPELL | AURA_INTERRUPT_FLAG_TAKE_DAMAGE;
        });

    // Nefarius Corruption
    ApplySpellFix({ 23642 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
        });

    // Conflagration, Horseman's Cleave
    ApplySpellFix({ 42380, 42587 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    // Serverside - Summon Arcane Disruptor
    ApplySpellFix({ 49591 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcChance = 101;
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_CREATE_ITEM;
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(25);
            spellInfo->_effects[EFFECT_1].ItemType = 37888;
        });

    // Serverside - Create Rocket Pack
    ApplySpellFix({ 70055 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcChance = 101;
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_CREATE_ITEM;
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(25);
            spellInfo->_effects[EFFECT_1].ItemType = 49278;
        });

    // Ashenvale Outrunner Sneak
    // Stealth
    ApplySpellFix({ 20540, 32199 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags |= (AURA_INTERRUPT_FLAG_MELEE_ATTACK | AURA_INTERRUPT_FLAG_CAST);
        });

    // Arcane Bolt
    ApplySpellFix({ 15979 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(3); // 20y
        });

    // Mortal Shots
    ApplySpellFix({ 19485, 19487, 19488, 19489, 19490 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].SpellClassMask[0] |= 0x00004000;
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Item - Death Knight T9 Melee 4P Bonus
    // Item - Hunter T9 2P Bonus
    // Item - Paladin T9 Retribution 2P Bonus (Righteous Vengeance)
    ApplySpellFix({ 67118, 67150, 67188 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Green Beam
    ApplySpellFix({ 31628, 31630, 31631 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
            spellInfo->MaxAffectedTargets = 1;
        });

    ApplySpellFix({
        20271, 57774,  // Judgement of Light
        20425,         // Judgement of Command
        32220,         // Judgement of Blood
        53407,         // Judgement of Justice
        53408,         // Judgement of Wisdom
        53725          // Judgement of the Martyr
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 &= ~SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Chaos Bolt Passive
    ApplySpellFix({ 58284 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_MOD_ABILITY_IGNORE_TARGET_RESIST;
            spellInfo->_effects[EFFECT_1].BasePoints = 100;
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->_effects[EFFECT_1].MiscValue = 127;
            spellInfo->_effects[EFFECT_1].SpellClassMask[1] = 0x00020000;
        });

    // Nefarian: Shadowbolt, Shadow Command
    ApplySpellFix({ 22667, 22677 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(152); // 150 yards
        });

    // Manastorm
    ApplySpellFix({ 21097 }, [](SpellInfo* spellInfo)
        {
            spellInfo->InterruptFlags &= ~SPELL_INTERRUPT_FLAG_INTERRUPT;
        });

    // Arcane Vacuum
    ApplySpellFix({ 21147 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(4); // 30 yards
            spellInfo->AttributesEx3 |= SPELL_ATTR3_ONLY_TARGET_PLAYERS;
        });

    // Reflection
    ApplySpellFix({ 22067 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Dispel = DISPEL_NONE;
        });

    // Focused Assault
    // Brutal Assault
    ApplySpellFix({ 46392, 46393 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags |= AURA_INTERRUPT_FLAG_CHANGE_MAP;
        });

    // Improved Blessing Protection (Nefarian Class Call)
    ApplySpellFix({ 23415 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_TARGET_ENEMY);
        });

    // Bestial Wrath
    ApplySpellFix({ 19574 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx4 |= SPELL_ATTR4_FADES_WHILE_LOGGED_OUT;
        });

    // Shadowflame
    ApplySpellFix({ 22539 }, [](SpellInfo* spellInfo)
        {
            spellInfo->InterruptFlags &= ~SPELL_INTERRUPT_FLAG_INTERRUPT;
        });

    // PX-238 Winter Wondervolt
    ApplySpellFix({ 26157, 26272, 26273, 26274 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Mechanic = 0;
        });

    // Calm Dragonkin
    ApplySpellFix({ 19872 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR1_CANT_TARGET_SELF;
        });

    // Cosmetic - Lightning Beam Channel
    ApplySpellFix({ 45537 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Burning Adrenaline
    ApplySpellFix({ 23478 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].BasePoints = 4374;
            spellInfo->_effects[EFFECT_0].DieSides = 1250;
        });

    // Explosion - Razorgore
    ApplySpellFix({ 20038 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_50000_YARDS);
            spellInfo->Attributes |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Brood Power : Bronze
    ApplySpellFix({ 22311 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_CANT_TRIGGER_PROC;
        });

    // Rapture
    ApplySpellFix({ 63652, 63653, 63654, 63655 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR1_NO_THREAT;
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Everlasting Affliction
    ApplySpellFix({ 47422 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SchoolMask = SPELL_SCHOOL_MASK_SHADOW;
        });

    // Flametongue Weapon (Passive) (Rank 6)
    ApplySpellFix({ 16312 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_PASSIVE;
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(21);
        });

    // Mana Tide Totem
    // Cleansing Totem Effect
    ApplySpellFix({ 39609, 52025 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(5); // 40yd
        });

    // Increased Totem Radius
    ApplySpellFix({ 21895 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[0].SpellClassMask = flag96(0x0603E000, 0x00200100);
        });

    // Jokkum Summon
    ApplySpellFix({ 56541 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValueB = 844;
        });

    // Hakkar Cause Insanity
    ApplySpellFix({ 24327 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Dispel = DISPEL_NONE;
        });

    // Summon Nightmare Illusions
    ApplySpellFix({ 24681, 24728, 24729 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValueB = 64;
        });

    // Blood Siphon
    ApplySpellFix({ 24322, 24323 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_MOD_STUN;
            spellInfo->_effects[EFFECT_2].Effect = SPELL_EFFECT_NONE;
            spellInfo->Attributes |= SPELL_ATTR0_CANT_CANCEL;
            spellInfo->AttributesEx5 |= SPELL_ATTR5_CAN_CHANNEL_WHEN_MOVING;
            spellInfo->ChannelInterruptFlags &= ~AURA_INTERRUPT_FLAG_MOVE;
        });

    // Place Fake Fur
    ApplySpellFix({ 46085 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValue = 8;
        });

    // Smash Mammoth Trap
    ApplySpellFix({ 46201 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValue = 8;
        });

    // Elemental Mastery
    ApplySpellFix({ 16166 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].SpellClassMask = flag96(0x00000003, 0x00001000);
        });

    // Elemental Vulnerability
    ApplySpellFix({ 28772 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Speed = 1;
        });

    // Find the Ancient Hero: Kill Credit
    ApplySpellFix({ 25729 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = TARGET_UNIT_SUMMONER;
        });

    // Artorius Demonic Doom
    ApplySpellFix({ 23298 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx4 |= SPELL_ATTR4_FIXED_DAMAGE;
            spellInfo->AttributesEx6 |= SPELL_ATTR6_LIMIT_PCT_DAMAGE_MODS;
        });

    // Lash
    ApplySpellFix({ 25852 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // Explosion
    ApplySpellFix({ 5255 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    // Death's Respite
    ApplySpellFix({ 67731, 68305 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Wyvern Sting DoT
    ApplySpellFix({ 24131, 24134, 24135 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
        });

    // Feed Pet
    ApplySpellFix({ 1539, 51284 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_CASTABLE_WHILE_SITTING;
        });

    // Judgement (Paladin T2 8P Bonus)
    // Battlegear of Eternal Justice
    ApplySpellFix({ 23591, 26135 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcFlags = PROC_FLAG_DONE_SPELL_MELEE_DMG_CLASS;
        });

    // Gift of Arthas
    ApplySpellFix({ 11371 }, [](SpellInfo* spellInfo)
        {
            spellInfo->SpellFamilyName = SPELLFAMILY_POTION;
        });

    // Refocus (Renataki's charm of beasts)
    ApplySpellFix({ 24531 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        });

    // Collect Rookery Egg
    ApplySpellFix({ 15958 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_NONE;
        });

    // WotLK Prologue Frozen Shade Visual, temp used to restore visual after Dispersion
    ApplySpellFix({ 53444 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(27);
        });

    // Rental Racing Ram
    ApplySpellFix({ 43883 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags &= ~AURA_INTERRUPT_FLAG_NOT_ABOVEWATER;
        });

    // Summon Worm
    ApplySpellFix({ 518, 25831, 25832 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValueB = 64;
        });

    // Uppercut
    ApplySpellFix({ 26007 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_CANT_TRIGGER_PROC;
        });

    // Digestive Acid (Temporary)
    ApplySpellFix({ 26476 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY;
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_IGNORE_HIT_RESULT;
        });

    // Drums of War/Battle/Speed/Restoration
    ApplySpellFix({ 35475, 35476, 35477, 35478 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ExcludeTargetAuraSpell = 51120;
        });

    // Slap!
    ApplySpellFix({ 6754 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR1_NO_THREAT;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Summon Cauldron Stuff
    ApplySpellFix({ 36549 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(28); // 5 seconds
            spellInfo->_effects[EFFECT_0].TargetB = TARGET_DEST_CASTER;
        });

    // Hunter's Mark
    ApplySpellFix({ 31615 }, [](SpellInfo* spellInfo)
        {
            for (uint8 index = EFFECT_0; index <= EFFECT_1; ++index)
            {
                spellInfo->_effects[index].TargetA = TARGET_UNIT_TARGET_ENEMY;
                spellInfo->_effects[index].TargetB = 0;
            }
        });

    // Self Visual - Sleep Until Cancelled(DND)
    ApplySpellFix({ 6606, 14915, 16093 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags &= ~AURA_INTERRUPT_FLAG_NOT_SEATED;
        });

    // Cleansing Totem, Healing Stream Totem, Mana Tide Totem
    ApplySpellFix({ 8171,52025, 52041, 52042, 52046, 52047, 52048, 52049, 52050, 58759, 58760, 58761, 39610, 39609 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });
    // Game In Session
    ApplySpellFix({ 39331 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->Attributes |= SPELL_ATTR0_CANT_CANCEL;
            spellInfo->AuraInterruptFlags |= AURA_INTERRUPT_FLAG_CHANGE_MAP;
        });
    // Death Ray Warning Visual, Death Ray Damage Visual
    ApplySpellFix({ 63882, 63886 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx5 |= SPELL_ATTR5_CAN_CHANNEL_WHEN_MOVING;
        });

    // Buffeting Winds of Susurrus
    ApplySpellFix({ 32474 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(556); // 28 seconds
        });

    // Quest - Healing Salve
    ApplySpellFix({ 29314 }, [](SpellInfo* spellInfo)
        {
            spellInfo->CastTimeEntry = sSpellCastTimesStore.LookupEntry(1); // 0s
        });

    // Seed of Corruption
    ApplySpellFix({ 27285, 47833, 47834 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx |= SPELL_ATTR1_CANT_BE_REFLECTED;
        });

    // Turn the Tables
    ApplySpellFix({ 51627, 51628, 51629 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_STACK_FOR_DIFF_CASTERS;
        });

    // Silence
    ApplySpellFix({ 18278 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx4 |= SPELL_ATTR4_NOT_USABLE_IN_ARENA;
        });

    // Absorb Life
    ApplySpellFix({ 34239 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].ValueMultiplier = 1;
        });

    // Summon a Warp Rift in Void Ridge
    ApplySpellFix({ 35036 }, [](SpellInfo* spellInfo)
        {
            spellInfo->CastTimeEntry = sSpellCastTimesStore.LookupEntry(1); // 0s
        });

    // Hit Rating (Dungeon T3 - 2P Bonus - Wastewalker, Doomplate)
    ApplySpellFix({ 37608, 37610 }, [](SpellInfo* spellInfo)
        {
            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(0);
            spellInfo->_effects[EFFECT_0].MiscValue = 224;
        });

    // Target Fissures
    ApplySpellFix({ 30745 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
        });

    // Acid Spit
    ApplySpellFix({ 34290 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
        });

    // Mulgore Hatchling (periodic)
    ApplySpellFix({ 62586 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 62585; // Mulgore Hatchling (fear)
        });

    // Poultryized!
    ApplySpellFix({ 30504 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags |= AURA_INTERRUPT_FLAG_TAKE_DAMAGE;
        });

    // Torment of the Worgen
    ApplySpellFix({ 30567 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ProcChance = 3;
        });

    // Summon Water Elementals
    ApplySpellFix({ 29962, 37051, 37052, 37053 }, [](SpellInfo* spellInfo)
        {
            spellInfo->RangeEntry = sSpellRangeStore.LookupEntry(13); // 50000yd
        });

    // Instill Lord Valthalak's Spirit DND
    ApplySpellFix({ 27360 }, [](SpellInfo* spellInfo)
        {
            spellInfo->ChannelInterruptFlags |= AURA_INTERRUPT_FLAG_MOVE;
        });

    // Holiday - Midsummer, Ribbon Pole Periodic Visual
    ApplySpellFix({ 45406 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags |= (AURA_INTERRUPT_FLAG_MOUNT | AURA_INTERRUPT_FLAG_CAST | AURA_INTERRUPT_FLAG_TALK);
        });

    // Improved Mind Flay and Smite
    ApplySpellFix({ 37571 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].SpellClassMask[0] = 8388736;
        });

    // Improved Corruption and Immolate (Updated)
    ApplySpellFix({ 61992 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_ADD_PCT_MODIFIER;
            spellInfo->_effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            spellInfo->_effects[EFFECT_1].BasePoints = 4;
            spellInfo->_effects[EFFECT_1].DieSides = 1;
            spellInfo->_effects[EFFECT_1].MiscValue = 22;
            spellInfo->_effects[EFFECT_1].SpellClassMask[0] = 6;
        });

    // 46747 Fling torch
    ApplySpellFix({ 46747 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_CASTER);
        });

    // Chains of Naberius
    ApplySpellFix({ 36146 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 1;
        });

    // Force of Neltharaku
    ApplySpellFix({ 38762 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
        });

    // Spotlight
    ApplySpellFix({ 29683, 32214 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx5 |= SPELL_ATTR5_HIDE_DURATION;
        });

    // Haunted
    ApplySpellFix({ 53768 }, [](SpellInfo* spellInfo)
        {
            spellInfo->Attributes |= SPELL_ATTR0_CANT_CANCEL;
        });

    // Tidal Wave
    ApplySpellFix({ 37730 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Commanding Shout
    ApplySpellFix({ 469, 47439, 47440 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Battle Shout
    ApplySpellFix({ 2048, 5242, 6192, 6673, 11549, 11550, 11551, 25289, 47436 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Plague Effect
    ApplySpellFix({ 19594, 26557 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Prayer of Fortitude
    ApplySpellFix({ 21562, 21564, 25392, 48162 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Gift of the Wild
    ApplySpellFix({ 21849, 21850, 26991, 48470, 69381 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Arcane Brilliance
    ApplySpellFix({ 23028, 27127, 43002 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Prayer of Spirit
    ApplySpellFix({ 27681, 32999, 48074 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Prayer of Shadow Protection
    ApplySpellFix({ 27683, 39374, 48170 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Nagrand Fort Buff Reward Raid
    ApplySpellFix({ 33006 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Ancestral Awakening
    ApplySpellFix({ 52759 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Turn the Tables
    ApplySpellFix({ 52910, 52914, 52915 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Judgements of the Wise
    ApplySpellFix({ 54180 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Replenishment
    ApplySpellFix({ 57669 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Dalaran Brilliance
    ApplySpellFix({ 61316 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // [DND] Dalaran Brilliance
    ApplySpellFix({ 61332 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Infinite Replenishment + Wisdom
    ApplySpellFix({ 61782 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Renewed Hope
    ApplySpellFix({ 63944 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Fortitude
    ApplySpellFix({ 69377 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Blessing of Forgotten Kings
    ApplySpellFix({ 69378 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Lucky Charm
    ApplySpellFix({ 69511 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Shiny Shard of the Scale Heal Targeter
    ApplySpellFix({ 69749 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Purified Shard of the Scale Heal Targeter
    ApplySpellFix({ 69754 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Brilliance
    ApplySpellFix({ 69994 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx2 |= SPELL_ATTR2_CAN_TARGET_NOT_IN_LOS;
        });

    // Domination
    ApplySpellFix({ 37135 }, [](SpellInfo* spellInfo)
        {
            spellInfo->MaxAffectedTargets = 5;
        });

    // Presence Of Mind
    ApplySpellFix({ 12043 }, [](SpellInfo* spellInfo)
        {
            // It should not share cooldown mods with category[1151] spells (Arcane Power [12042], Decimate [47271])
            spellInfo->AttributesEx6 |= SPELL_ATTR6_IGNORE_CATEGORY_COOLDOWN_MODS;
        });

    // Eye of Grillok
    ApplySpellFix({ 38495 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TriggerSpell = 38530; // Quest Credit for Eye of Grillok
        });

    // Greater Fireball
    ApplySpellFix({ 33051 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx4 |= SPELL_ATTR4_IGNORE_RESISTANCES;
        });

    // Gor'drek's Ointment
    ApplySpellFix({ 32578 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_CANT_TRIGGER_PROC;
            spellInfo->AttributesEx3 |= SPELL_ATTR3_NO_INITIAL_AGGRO;
        });

    // Shadow Grasp
    ApplySpellFix({ 30410 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx6 |= SPELL_ATTR6_UNK15;
        });

    ApplySpellFix({
        471, // Palamino
        8980, // Skeletal Horse
        10788, // Leopard
        10790, // Tiger
        10792, // Spotted Panther
        60136, // Grand Caravan Mammoth
        60140 // Grand Caravan Mammoth
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AuraInterruptFlags &= ~AURA_INTERRUPT_FLAG_NOT_ABOVEWATER;
        });

    // Molten Punch
    ApplySpellFix({ 40126 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(TARGET_DEST_CASTER);
        });

    // Wing Buffet
    ApplySpellFix({ 37319 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CONE_ENEMY_24);
            spellInfo->_effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(0);
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_20_YARDS);
        });

    // Flame Wave
    ApplySpellFix({ 33800 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_1].Effect = SPELL_EFFECT_APPLY_AURA;
            spellInfo->_effects[EFFECT_1].ApplyAuraName = SPELL_AURA_PERIODIC_TRIGGER_SPELL;
            spellInfo->_effects[EFFECT_1].Amplitude = 500;
        });

    // Chromatic Resistance Aura
    ApplySpellFix({ 41453 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].MiscValue = 124;
        });

    // Power of the Guardian
    ApplySpellFix({ 28142, 28143, 28144, 28145 }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_DEATH_PERSISTENT;
        });

    // Warrior stances passives
    ApplySpellFix({
        2457, // Battle Stance
        2458, // Berserker Stance
        7376  // Defensive Stance Passive
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->AttributesEx3 |= SPELL_ATTR3_CANT_TRIGGER_PROC;
        });

    // Conjure Refreshment Table (Rank 1, Rank 2)
    ApplySpellFix({ 43985, 58661 }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_DEST_CASTER_FRONT);
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_5_YARDS);
        });

    ApplySpellFix({
        698, // Ritual of Summoning (portal for clicking)
        61993 // Ritual of Summoning (summons the closet)
        }, [](SpellInfo* spellInfo)
        {
            spellInfo->_effects[EFFECT_0].RadiusEntry = sSpellRadiusStore.LookupEntry(EFFECT_RADIUS_3_YARDS);
        });

    for (uint32 i = 0; i < GetSpellInfoStoreSize(); ++i)
    {
        SpellInfo* spellInfo = mSpellInfoMap[i];
        if (!spellInfo)
        {
            continue;
        }

        for (uint8 j = 0; j < MAX_SPELL_EFFECTS; ++j)
        {
            switch (spellInfo->_effects[j].Effect)
            {
            case SPELL_EFFECT_CHARGE:
            case SPELL_EFFECT_CHARGE_DEST:
            case SPELL_EFFECT_JUMP:
            case SPELL_EFFECT_JUMP_DEST:
            case SPELL_EFFECT_LEAP_BACK:
                if (!spellInfo->Speed && !spellInfo->SpellFamilyName)
                {
                    spellInfo->Speed = 42.1f;
                }
                break;
            }

            // Xinef: i hope this will fix the problem with not working resurrection
            if (spellInfo->_effects[j].Effect == SPELL_EFFECT_SELF_RESURRECT)
            {
                spellInfo->_effects[j].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            }
        }

        // Fix range for trajectory triggered spell
        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
        {
            if (spellEffectInfo.IsEffect() && (spellEffectInfo.TargetA.GetTarget() == TARGET_DEST_TRAJ || spellEffectInfo.TargetB.GetTarget() == TARGET_DEST_TRAJ))
            {
                // Get triggered spell if any
                if (SpellInfo* spellInfoTrigger = const_cast<SpellInfo*>(GetSpellInfo(spellEffectInfo.TriggerSpell)))
                {
                    float maxRangeMain = spellInfo->RangeEntry ? spellInfo->RangeEntry->RangeMax[0] : 0.0f;
                    float maxRangeTrigger = spellInfoTrigger->RangeEntry ? spellInfoTrigger->RangeEntry->RangeMax[0] : 0.0f;

                    // check if triggered spell has enough max range to cover trajectory
                    if (maxRangeTrigger < maxRangeMain)
                        spellInfoTrigger->RangeEntry = spellInfo->RangeEntry;
                }
            }
        }

        if (spellInfo->ActiveIconID == 2158)  // flight
        {
            spellInfo->Attributes |= SPELL_ATTR0_PASSIVE;
        }

        switch (spellInfo->SpellFamilyName)
        {
        case SPELLFAMILY_PALADIN:
            // Seals of the Pure should affect Seal of Righteousness
            if (spellInfo->SpellIconID == 25 && (spellInfo->Attributes & SPELL_ATTR0_PASSIVE))
                spellInfo->_effects[EFFECT_0].SpellClassMask[1] |= 0x20000000;
            break;
        case SPELLFAMILY_NECROMANCER:
            // Icy Touch - extend FamilyFlags (unused value) for Sigil of the Frozen Conscience to use
            if (spellInfo->SpellIconID == 2721 && spellInfo->SpellFamilyFlags[0] & 0x2)
                spellInfo->SpellFamilyFlags[0] |= 0x40;
            break;
        case SPELLFAMILY_HUNTER:
            // Aimed Shot not affected by category cooldown modifiers
            if (spellInfo->SpellFamilyFlags[0] & 0x00020000)
            {
                spellInfo->AttributesEx6 |= SPELL_ATTR6_IGNORE_CATEGORY_COOLDOWN_MODS;
                spellInfo->RecoveryTime = 10 * IN_MILLISECONDS;
            }
            break;
        }
    }
    /// <summary>
    /// 
    /// </summary>

    for (uint32 i = 0; i < GetSpellInfoStoreSize(); ++i)
    {
        SpellInfo* spellInfo = mSpellInfoMap[i];
        if (!spellInfo)
            continue;

        // Fix range for trajectory triggered spell
        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
        {
            if (spellEffectInfo.IsEffect() && (spellEffectInfo.TargetA.GetTarget() == TARGET_DEST_TRAJ || spellEffectInfo.TargetB.GetTarget() == TARGET_DEST_TRAJ))
            {
                // Get triggered spell if any
                if (SpellInfo* spellInfoTrigger = const_cast<SpellInfo*>(GetSpellInfo(spellEffectInfo.TriggerSpell)))
                {
                    float maxRangeMain = spellInfo->RangeEntry ? spellInfo->RangeEntry->RangeMax[0] : 0.0f;
                    float maxRangeTrigger = spellInfoTrigger->RangeEntry ? spellInfoTrigger->RangeEntry->RangeMax[0] : 0.0f;

                    // check if triggered spell has enough max range to cover trajectory
                    if (maxRangeTrigger < maxRangeMain)
                        spellInfoTrigger->RangeEntry = spellInfo->RangeEntry;
                }
            }
        }

        for (SpellEffectInfo& spellEffectInfo : spellInfo->_GetEffects())
        {
            switch (spellEffectInfo.Effect)
            {
                case SPELL_EFFECT_CHARGE:
                case SPELL_EFFECT_CHARGE_DEST:
                case SPELL_EFFECT_JUMP:
                case SPELL_EFFECT_JUMP_DEST:
                case SPELL_EFFECT_LEAP_BACK:
                    if (!spellInfo->Speed && !spellInfo->SpellFamilyName)
                        spellInfo->Speed = SPEED_CHARGE;
                    break;
                case SPELL_EFFECT_APPLY_AURA:
                    // special aura updates each 30 seconds
                    if (spellEffectInfo.ApplyAuraName == SPELL_AURA_MOD_ATTACK_POWER_OF_ARMOR)
                        spellEffectInfo.Amplitude = 30 * IN_MILLISECONDS;
                    break;
                default:
                    break;
            }

            // Passive talent auras cannot target pets
            if (spellInfo->IsPassive() && GetTalentSpellCost(i))
                if (spellEffectInfo.TargetA.GetTarget() == TARGET_UNIT_PET)
                    spellEffectInfo.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);

            // Area auras may not target area (they're self cast)
            if (spellEffectInfo.IsAreaAuraEffect() && spellEffectInfo.IsTargetingArea())
            {
                spellEffectInfo.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
                spellEffectInfo.TargetB = SpellImplicitTargetInfo(0);
            }
        }

        // disable proc for magnet auras, they're handled differently
        if (spellInfo->HasAura(SPELL_AURA_SPELL_MAGNET))
            spellInfo->ProcFlags = 0;

        // due to the way spell system works, unit would change orientation in Spell::_cast
        if (spellInfo->HasAura(SPELL_AURA_CONTROL_VEHICLE))
            spellInfo->AttributesEx5 |= SPELL_ATTR5_DONT_TURN_DURING_CAST;

        if (spellInfo->ActiveIconID == 2158)  // flight
            spellInfo->Attributes |= SPELL_ATTR0_PASSIVE;

        switch (spellInfo->SpellFamilyName)
        {
            case SPELLFAMILY_PALADIN:
                // Seals of the Pure should affect Seal of Righteousness
                if (spellInfo->SpellIconID == 25 && spellInfo->HasAttribute(SPELL_ATTR0_PASSIVE))
                    spellInfo->_GetEffect(EFFECT_0).SpellClassMask[1] |= 0x20000000;
                break;
            case SPELLFAMILY_NECROMANCER:
                // Icy Touch - extend FamilyFlags (unused value) for Sigil of the Frozen Conscience to use
                if (spellInfo->SpellIconID == 2721 && spellInfo->SpellFamilyFlags[0] & 0x2)
                    spellInfo->SpellFamilyFlags[0] |= 0x40;
                break;
        }
    }

    if (SummonPropertiesEntry* properties = const_cast<SummonPropertiesEntry*>(sSummonPropertiesStore.LookupEntry(121)))
        properties->Title = SUMMON_TYPE_TOTEM;
    if (SummonPropertiesEntry* properties = const_cast<SummonPropertiesEntry*>(sSummonPropertiesStore.LookupEntry(647))) // 52893
        properties->Title = SUMMON_TYPE_TOTEM;
    if (SummonPropertiesEntry* properties = const_cast<SummonPropertiesEntry*>(sSummonPropertiesStore.LookupEntry(628))) // Hungry Plaguehound
        properties->Control = SUMMON_CATEGORY_PET;

    if (LockEntry* entry = const_cast<LockEntry*>(sLockStore.LookupEntry(36))) // 3366 Opening, allows to open without proper key
        entry->Type[2] = LOCK_KEY_NONE;

    TC_LOG_INFO("server.loading", ">> Loaded SpellInfo corrections in {} ms", GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellInfoSpellSpecificAndAuraState()
{
    uint32 oldMSTime = getMSTime();

    for (SpellInfo* spellInfo : mSpellInfoMap)
    {
        if (!spellInfo)
            continue;

        // AuraState depends on SpellSpecific
        spellInfo->_LoadSpellSpecific();
        spellInfo->_LoadAuraState();
    }

    TC_LOG_INFO("server.loading", ">> Loaded SpellInfo SpellSpecific and AuraState in {} ms", GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellInfoDiminishing()
{
    uint32 oldMSTime = getMSTime();

    for (SpellInfo* spellInfo : mSpellInfoMap)
    {
        if (!spellInfo)
            continue;

        spellInfo->_LoadSpellDiminishInfo();
    }

    TC_LOG_INFO("server.loading", ">> Loaded SpellInfo diminishing infos in {} ms", GetMSTimeDiffToNow(oldMSTime));
}

void SpellMgr::LoadSpellInfoImmunities()
{
    uint32 oldMSTime = getMSTime();

    for (SpellInfo* spellInfo : mSpellInfoMap)
    {
        if (!spellInfo)
            continue;

        spellInfo->_LoadImmunityInfo();
    }

    TC_LOG_INFO("server.loading", ">> Loaded SpellInfo immunity infos in {} ms", GetMSTimeDiffToNow(oldMSTime));
}
