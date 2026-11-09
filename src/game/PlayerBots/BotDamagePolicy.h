#ifndef MANGOS_BOT_DAMAGE_POLICY_H
#define MANGOS_BOT_DAMAGE_POLICY_H
#include "SpellMgr.h"
namespace BotDamagePolicy
{
// Spell chains are bounded to avoid cycles in custom spell data. Positive
// utility spells remain usable; negative damage channels include their ticks.
inline bool IsDamage(SpellEntry const *spell, unsigned depth = 0)
{
    if (!spell || spell->IsPositiveSpell() || depth >= 4)
        return false;
    if (spell->IsDirectDamageSpell())
        return true;
    for (unsigned i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (Spells::IsDirectDamageEffect(spell->Effect[i]) ||
            spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE ||
            spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE_PERCENT ||
            spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_LEECH ||
            spell->EffectApplyAuraName[i] == SPELL_AURA_POWER_BURN_MANA)
            return true;
        if (spell->EffectTriggerSpell[i] &&
            (spell->Effect[i] == SPELL_EFFECT_TRIGGER_SPELL ||
             spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_TRIGGER_SPELL) &&
            IsDamage(sSpellMgr.GetSpellEntry(spell->EffectTriggerSpell[i]), depth + 1))
            return true;
    }
    return false;
}
} // namespace BotDamagePolicy
#endif
