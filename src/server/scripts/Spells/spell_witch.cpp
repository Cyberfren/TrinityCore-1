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
 * Scripts for spells with SPELLFAMILY_NECROMANCER and SPELLFAMILY_GENERIC spells used by deathknight players.
 * Ordered alphabetically using scriptname.
 * Scriptnames of files in this file should be prefixed with "spell_dk_".
 */

#include "ScriptMgr.h"
#include "Containers.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerAI.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include "Unit.h"

enum WitchSpells
{
    SPELL_WITCH_SHADOWBOLT = 80742,
    SPELL_WITCH_DISPEL_FETISH_EFFECT = 82007
};

enum WitchSpellIcons
{
};

enum WitchMisc
{
    NPC_WITCH_DEATH_FETISH                      = 44206
};

enum DeathFetishMisc
{
    DATA_INITIAL_TARGET_GUID = 1,
};


class spell_witch_death_fetish : public AuraScript
{
    PrepareAuraScript(spell_witch_death_fetish);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({
            SPELL_WITCH_SHADOWBOLT
        });
    }

    void HandleTarget(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        std::list<Creature*> fetishDeath;
        caster->GetAllMinionsByEntry(fetishDeath, NPC_WITCH_DEATH_FETISH);
        for (Creature* temp : fetishDeath)
        {
            if (temp->IsAIEnabled())
                temp->AI()->SetGUID(GetTarget()->GetGUID(), DATA_INITIAL_TARGET_GUID);
            temp->GetThreatManager().RegisterRedirectThreat(GetId(), caster->GetGUID(), 100);
        }
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (SpellInfo const* procSpell = eventInfo.GetSpellInfo())
        {
            if (procSpell->IsRankOf(sSpellMgr->GetSpellInfo(SPELL_WITCH_SHADOWBOLT)))
                return true;
        }

        return false;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* owner = GetUnitOwner();
        if (!owner)
            return;

        SpellInfo const* procSpell = eventInfo.GetSpellInfo();
        Unit* fetishDeath = nullptr;
        for (auto itr = owner->m_Controlled.begin(); itr != owner->m_Controlled.end() && !fetishDeath; itr++)
            if ((*itr)->GetEntry() == NPC_WITCH_DEATH_FETISH)
                fetishDeath = *itr;

        if (!fetishDeath)
            return;

        if (fetishDeath->IsInCombat() && fetishDeath->GetVictim())
            fetishDeath->CastSpell(fetishDeath->GetVictim(), procSpell->Id, CastSpellExtraArgs(TriggerCastFlags::TRIGGERED_IGNORE_POWER_AND_REAGENT_COST));
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_witch_death_fetish::HandleTarget, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        DoCheckProc += AuraCheckProcFn(spell_witch_death_fetish::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_witch_death_fetish::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

class spell_witch_dispel_totem_pulse : public SpellScript
{
    PrepareSpellScript(spell_witch_dispel_totem_pulse);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_WITCH_DISPEL_FETISH_EFFECT });
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        if (GetCaster() && GetHitUnit() && GetOriginalCaster())
        {
            CastSpellExtraArgs args(GetOriginalCaster()->GetGUID());
            args.AddSpellMod(SPELLVALUE_BASE_POINT1, 1);
            GetCaster()->CastSpell(GetHitUnit(), SPELL_WITCH_DISPEL_FETISH_EFFECT, args);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_witch_dispel_totem_pulse::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

void AddSC_witch_spell_scripts()
{
    RegisterSpellScript(spell_witch_dispel_totem_pulse);
    RegisterSpellScript(spell_witch_death_fetish);
}
