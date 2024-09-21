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

#include "ScriptMgr.h"
#include "molten_core.h"
#include "ObjectMgr.h"
#include "ScriptedCreature.h"
#include "SpellScript.h"

enum Texts
{
    EMOTE_FRENZY        = 0
};

enum Spells
{
    SPELL_FRENZY        = 19451,
    SPELL_MAGMA_SPIT    = 19449,
    SPELL_PANIC         = 19408,
    SPELL_LAVA_BOMB     = 19428,
    SPELL_LAVA_BOMB_EFFECT = 20494,                    // Spawns trap GO 177704 which triggers 19428
    SPELL_LAVA_BOMB_RANGED = 20474,                    // This calls a dummy server side effect that cast spell 20495 to spawn GO 177704 for 60s
    SPELL_LAVA_BOMB_RANGED_EFFECT = 20495,
};

enum Events
{
    EVENT_FRENZY        = 1,
    EVENT_PANIC         = 2,
    EVENT_LAVA_BOMB     = 3,
    EVENT_LAVA_BOMB_RANGED = 4,
};

constexpr float MELEE_TARGET_LOOKUP_DIST = 10.0f;
struct boss_magmadar : public BossAI
{
    boss_magmadar(Creature* creature) : BossAI(creature, BOSS_MAGMADAR)
    {
    }

    void Reset() override
    {
        BossAI::Reset();
        DoCast(me, SPELL_MAGMA_SPIT, true);
    }

    void JustEngagedWith(Unit* victim) override
    {
        BossAI::JustEngagedWith(victim);
        events.ScheduleEvent(EVENT_FRENZY, 30s);
        events.ScheduleEvent(EVENT_PANIC, 20s);
        events.ScheduleEvent(EVENT_LAVA_BOMB, 12s);
        events.ScheduleEvent(EVENT_LAVA_BOMB_RANGED, 15s);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        events.Update(diff);

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_FRENZY:
                    Talk(EMOTE_FRENZY);
                    DoCast(me, SPELL_FRENZY);
                    events.ScheduleEvent(EVENT_FRENZY, 15s);
                    break;
                case EVENT_PANIC:
                    DoCastVictim(SPELL_PANIC);
                    events.ScheduleEvent(EVENT_PANIC, 35s);
                    break;
              //  case EVENT_LAVA_BOMB:
            //        if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 0.0f, true, true, -SPELL_LAVA_BOMB))
           //             DoCast(target, SPELL_LAVA_BOMB);
           //         events.ScheduleEvent(EVENT_LAVA_BOMB, 12s);
          //          break;
                case EVENT_LAVA_BOMB:
                {
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, MELEE_TARGET_LOOKUP_DIST, true))
                    {
                        DoCast(target, SPELL_LAVA_BOMB);
                    }

                    events.ScheduleEvent(EVENT_LAVA_BOMB_RANGED, 10s);
                    break;
                }
               case EVENT_LAVA_BOMB_RANGED:
            {
                std::list<Unit*> targets;
                SelectTargetList(targets, 1, SelectTargetMethod::Random, 1, [this](Unit* target)
                    {
                        return target && target->IsPlayer() && target->GetDistance(me) > MELEE_TARGET_LOOKUP_DIST && target->GetDistance(me) < 100.0f;
                    });

                if (!targets.empty())
                {
                    DoCast(targets.front(), SPELL_LAVA_BOMB_RANGED);
                }
                events.ScheduleEvent(EVENT_LAVA_BOMB_RANGED, 15s);
                break;
            }
                default:
                    break;
            }

            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
        }

        DoMeleeAttackIfReady();
    }
};


class spell_magmadar_lava_bomb : public SpellScript
{
    PrepareSpellScript(spell_magmadar_lava_bomb);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_LAVA_BOMB_EFFECT, SPELL_LAVA_BOMB_RANGED_EFFECT });
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        if (Unit* target = GetHitUnit())
        {
            uint32 spellId = 0;
            switch (m_scriptSpellId)
            {
            case SPELL_LAVA_BOMB:
            {
                spellId = SPELL_LAVA_BOMB_EFFECT;
                break;
            }
            case SPELL_LAVA_BOMB_RANGED:
            {
                spellId = SPELL_LAVA_BOMB_RANGED_EFFECT;
                break;
            }
            default:
            {
                return;
            }
            }
            target->CastSpell(target, spellId, true);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_magmadar_lava_bomb::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

void AddSC_boss_magmadar()
{
    RegisterSpellScript(spell_magmadar_lava_bomb);
    RegisterMoltenCoreCreatureAI(boss_magmadar);
}
