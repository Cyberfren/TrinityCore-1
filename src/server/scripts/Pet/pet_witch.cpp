
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

/*
 * Ordered alphabetically using scriptname.
 * Scriptnames of files in this file should be prefixed with "npc_pet_dk_".
 */

#include "ScriptMgr.h"
#include "CombatAI.h"
#include "CellImpl.h"
#include "GridNotifiersImpl.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ScriptedCreature.h"
#include "SpellScript.h"

enum WitchSpells
{
    SPELL_WITCH_DEATH_FETISH = 80743,
    SPELL_WITCH_COPY_WEAPON = 80744,
    SPELL_WITCH_DEATH_FETISH_MARK = 80745,
    SPELL_WITCH_DEATH_FETISH_VISUAL = 80746,
    SPELL_WITCH_FAKE_AGGRO_RADIUS_8_YARD = 80747,
    SPELL_WITCH_DEATH_FETISH_SCALING_01 = 80748,
    SPELL_WITCH_DEATH_FETISH_SCALING = 80749,
    SPELL_WITCH_SCALING__MASTER_SPELL_06__SPELL_HIT_EXPERTISE_SPELL_PENETRATION = 80750,
    SPELL_WITCH_PET_SCALING_03 = 80751,
    SPELL_WITCH_AGGRO_8_YD_PBAE = 80752,
    SPELL_DISMISS_DEATH_FETISH = 80753, // Right now despawn is done by its duration
};

enum DeathFetishMisc
{
    TASK_GROUP_COMBAT = 1,
    DATA_INITIAL_TARGET_GUID = 1,
};

struct npc_pet_witch_death_fetish : ScriptedAI
{
    npc_pet_witch_death_fetish(Creature* creature) : ScriptedAI(creature) { }

    void IsSummonedBy(WorldObject* summoner) override
    {
        me->SetReactState(REACT_PASSIVE);

        if (summoner->GetTypeId() != TYPEID_UNIT)
            return;

        Unit* unitSummoner = summoner->ToUnit();

        DoCast(unitSummoner, SPELL_WITCH_COPY_WEAPON, true);
        DoCast(unitSummoner, SPELL_WITCH_DEATH_FETISH_MARK, true);
        DoCastSelf(SPELL_WITCH_DEATH_FETISH_VISUAL, true);
        DoCastSelf(SPELL_WITCH_FAKE_AGGRO_RADIUS_8_YARD, true);
        DoCastSelf(SPELL_WITCH_DEATH_FETISH_SCALING_01, true);
        DoCastSelf(SPELL_WITCH_DEATH_FETISH_SCALING, true);
        DoCastSelf(SPELL_WITCH_SCALING__MASTER_SPELL_06__SPELL_HIT_EXPERTISE_SPELL_PENETRATION, true);
        DoCastSelf(SPELL_WITCH_PET_SCALING_03, true);

        _scheduler.Schedule(500ms, [this](TaskContext /*activate*/)
        {
            me->SetReactState(REACT_AGGRESSIVE);
            if (!_targetGUID.IsEmpty())
            {
                if (Unit* target = ObjectAccessor::GetUnit(*me, _targetGUID))
                    me->EngageWithTarget(target);
            }
        }).Schedule(6s, [this](TaskContext visual)
        {
            // Cast every 6 seconds
            DoCastSelf(SPELL_WITCH_DEATH_FETISH_VISUAL, true);
            visual.Repeat();
        });
    }

    void SetGUID(ObjectGuid const& guid, int32 id) override
    {
        if (id == DATA_INITIAL_TARGET_GUID)
            _targetGUID = guid;
    }

    void JustEnteredCombat(Unit* who) override
    {
        ScriptedAI::JustEnteredCombat(who);

        // Investigate further if these casts are done by any owned aura, eitherway SMSG_SPELL_GO is sent every X seconds.
        _scheduler.Schedule(1s, TASK_GROUP_COMBAT, [this](TaskContext aggro8YD)
        {
            // Cast every second
            if (Unit* victim = me->GetVictim())
                DoCast(victim, SPELL_WITCH_AGGRO_8_YD_PBAE, true);
            aggro8YD.Repeat();
        });
    }

    void UpdateAI(uint32 diff) override
    {
        Unit* owner = me->GetOwner();
        if (!owner)
        {
            me->DespawnOrUnsummon();
            return;
        }

        _scheduler.Update(diff);

        if (!UpdateDeathFetishVictim())
            return;

        DoMeleeAttackIfReady();
    }

    bool CanAIAttack(Unit const* who) const override
    {
        Unit* owner = me->GetOwner();
        return owner && who->IsAlive() && me->IsValidAttackTarget(who) && !who->HasBreakableByDamageCrowdControlAura() && who->IsInCombatWith(owner) && ScriptedAI::CanAIAttack(who);
    }

    // Do not reload Creature templates on evade mode enter - prevent visual lost
    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        _scheduler.CancelGroup(TASK_GROUP_COMBAT);

        if (!me->IsAlive())
        {
            EngagementOver();
            return;
        }

        Unit* owner = me->GetCharmerOrOwner();

        me->CombatStop(true);
        me->SetLootRecipient(nullptr);
        me->ResetPlayerDamageReq();
        me->SetLastDamagedTime(0);
        me->SetCannotReachTarget(false);
        me->DoNotReacquireSpellFocusTarget();
        me->SetTarget(ObjectGuid::Empty);
        EngagementOver();

        if (owner && !me->HasUnitState(UNIT_STATE_FOLLOW))
        {
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, me->GetFollowAngle());
        }
    }

private:
    // custom UpdateVictim implementation to handle special target selection
    // we prioritize between things that are in combat with owner based on the owner's threat to them
    bool UpdateDeathFetishVictim()
    {
        Unit* owner = me->GetOwner();
        if (!owner)
            return false;

        if (!me->IsEngaged() && !owner->IsInCombat())
            return false;

        Unit* currentTarget = me->GetVictim();
        if (currentTarget && !CanAIAttack(currentTarget))
        {
            me->InterruptNonMeleeSpells(true); // do not finish casting on invalid targets
            me->AttackStop();
            currentTarget = nullptr;
        }

        Unit* selectedTarget = nullptr;

        // first, try to get the initial target
        if (Unit* initialTarget = ObjectAccessor::GetUnit(*me, _targetGUID))
        {
            if (CanAIAttack(initialTarget))
                selectedTarget = initialTarget;
        }
        else if (!_targetGUID.IsEmpty())
            _targetGUID.Clear();

        CombatManager const& mgr = owner->GetCombatManager();
        if (!selectedTarget)
        {
            if (mgr.HasPvPCombat())
            {
                // select pvp target
                float minDistance = 0.f;
                for (auto const& pair : mgr.GetPvPCombatRefs())
                {
                    Unit* target = pair.second->GetOther(owner);
                    if (target->GetTypeId() != TYPEID_PLAYER)
                        continue;
                    if (!CanAIAttack(target))
                        continue;

                    float dist = owner->GetDistance(target);
                    if (!selectedTarget || dist < minDistance)
                    {
                        selectedTarget = target;
                        minDistance = dist;
                    }
                }
            }
        }

        if (!selectedTarget)
        {
            // select pve target
            float maxThreat = 0.f;
            for (auto const& pair : mgr.GetPvECombatRefs())
            {
                Unit* target = pair.second->GetOther(owner);
                if (!CanAIAttack(target))
                    continue;

                float threat = target->GetThreatManager().GetThreat(owner);
                if (threat >= maxThreat)
                {
                    selectedTarget = target;
                    maxThreat = threat;
                }
            }
        }

        if (!selectedTarget)
        {
            EnterEvadeMode(EVADE_REASON_NO_HOSTILES);
            return false;
        }

        if (selectedTarget != me->GetVictim())
            AttackStart(selectedTarget);
        return true;
    }

    TaskScheduler _scheduler;
    ObjectGuid _targetGUID;
};

void AddSC_witch_pet_scripts()
{
    RegisterCreatureAI(npc_pet_witch_death_fetish);
}
