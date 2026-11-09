#include "BattleBotPolicyRuntime.h"
#include "BattleBotAI.h"
#include "BattleGround.h"
#include "BattleGroundWS.h"
#include "Config/Config.h"
#include "GameObject.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerBotMgr.h"
#include "PvPCombatPolicy.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "Utilities/Random.h"
#include "WorldPacket.h"
#include <algorithm>
#include <cmath>
#include <sstream>
namespace BattleBotPolicyRuntime
{
namespace
{
uint32 Now() { return WorldTimer::getMSTime(); }
uint32 Root(uint32 id) { return sSpellMgr.GetFirstSpellInChain(id); }
struct RuntimeActor
{
    CombatBotBaseAI &base;
    State &m_policy;
    BattleBotAI *battle;
    bool externalDuel;
    Player *me;
    PlayerBotEntry *botEntry;
    bool m_initialized, m_doingGraveyardJump, m_wsgTravel, m_preventCasting;
    RuntimeActor(CombatBotBaseAI &a, State &state, BattleBotAI *bg, bool duel)
        : base(a), m_policy(state), battle(bg), externalDuel(duel), me(a.me), botEntry(a.botEntry),
          m_initialized(a.m_initialized), m_doingGraveyardJump(bg && bg->m_doingGraveyardJump),
          m_wsgTravel(bg && bg->m_wsgTravel), m_preventCasting(a.m_preventCasting)
    {
    }
    CombatBotRoles GetRole() const { return base.GetRole(); }
    bool IsWearingShield(Player *p) const { return base.IsWearingShield(p); }
    bool IsValidHostileTarget(Unit *u) const
    {
        return battle ? battle->IsValidHostileTarget(u) : base.IsValidHostileTarget(u);
    }
    Player *Opponent() const
    {
        if (!me || !me->m_duel || !me->m_duel->startTime || me->m_duel->outOfBound)
            return nullptr;
        Player *p = me->m_duel->opponent;
        if (!p || !p->m_duel || p->m_duel->opponent != me || !p->m_duel->startTime || p->m_duel->outOfBound ||
            !p->IsAlive() || p->GetMap() != me->GetMap())
            return nullptr;
        return p;
    }
    bool ObservedEnemy(Unit *u) const
    {
        if (!u || !me || u == me)
            return false;
        if (battle)
            return me->IsValidAttackTarget(u);
        Player *owner = u->GetAffectingPlayer();
        if (!externalDuel && me->m_duel)
            return false;
        auto *opponent = externalDuel ? Opponent() : nullptr;
        return ExternalEnemyAllowed(externalDuel, opponent != nullptr, owner && owner == opponent, owner != nullptr,
                                    me->IsValidAttackTarget(u));
    }
    bool TacticalRank(Unit *u, BattleBotTactics::Rank &rank) const
    {
        if (battle)
            return battle->TacticalRank(u, rank);
        if (!ObservedEnemy(u) || !u->IsAlive() || !u->IsInWorld() || u->GetMap() != me->GetMap() ||
            !me->IsWithinDistInMap(u, 80) || !u->IsVisibleForOrDetect(me, me, false) || !me->IsWithinLOSInMap(u) ||
            u->IsTotalImmune() || u->HasBreakableByDamageCrowdControlAura())
            return false;
        rank.guid = u->GetGUID();
        rank.playerBody = u->IsPlayer();
        rank.tier = 0;
        rank.distanceBucket = BattleBotTactics::DistanceBucket(me->GetDistance(u));
        return true;
    }
    Unit *SelectTacticalTarget() const
    {
        if (battle)
            return battle->SelectTacticalTarget();
        Unit *u = me->GetVictim();
        BattleBotTactics::Rank rank;
        return u && TacticalRank(u, rank) ? u : nullptr;
    }
    bool SameScene(Player *p) const
    {
        return battle ? p->GetBattleGround() == me->GetBattleGround()
                      : !p->InBattleGround() && p->GetMap() == me->GetMap();
    }
    bool Friendly(Player *p) const
    {
        if (battle)
            return p == me || p->GetBGTeam() == me->GetBGTeam();
        return ExternalAllyAllowed(externalDuel, p == me, me->IsInSameGroupWith(p), me->IsValidAttackTarget(p));
    }
    uint32 TeamKey() const { return uint32(battle ? me->GetBGTeam() : me->GetTeam()); }
};
bool Eligible(RuntimeActor const &ai)
{
    if ((ai.botEntry && ai.botEntry->requestRemoval) || !ai.me || !ai.m_initialized || ai.m_doingGraveyardJump ||
        !ai.me->IsInWorld() || ai.me->IsBeingTeleported() || !ai.me->IsAlive())
        return false;
    if (ai.battle)
        return ai.me->GetBattleGround() && ai.me->GetBattleGround()->GetStatus() == STATUS_IN_PROGRESS;
    if (ai.me->InBattleGround())
        return false;
    if (ai.externalDuel)
        return ai.Opponent() != nullptr;
    return ai.ObservedEnemy(ai.me->GetVictim());
}
Unit *Resolve(RuntimeActor const &ai, uint64 guid)
{
    return guid ? ObjectAccessor::GetUnit(*ai.me, ObjectGuid(guid)) : nullptr;
}
bool Seen(RuntimeActor const &ai, Unit *u)
{
    return u && u->IsInWorld() && u->GetMap() == ai.me->GetMap() && ai.me->IsWithinDistInMap(u, 80) &&
           u->IsVisibleForOrDetect(ai.me, ai.me, false) && ai.me->IsWithinLOSInMap(u);
}
bool LegalEnemy(RuntimeActor const &ai, Unit *target)
{
    if (!target)
        return false;
    BattleBotTactics::Rank rank;
    if (Seen(ai, target) && ai.TacticalRank(target, rank))
        return true;
    // AV already chose this PvE objective through its native route logic. Never scan
    // or invent NPC targets here, and never restore a disabled skill through legacy.
    return ai.battle && ai.me->GetBattleGround()->GetTypeID() == BATTLEGROUND_AV && ai.me->GetVictim() == target &&
           target->IsCreature() && !target->GetAffectingPlayer() && ai.IsValidHostileTarget(target) && Seen(ai, target);
}
int CastLength(RuntimeActor &ai, Unit *u, Spell *cast)
{
    if (!cast)
        return 0;
    if (cast != u->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return cast->GetCastTime();
    auto episode = ai.m_policy.starts.find(u->GetGUID());
    if (episode == ai.m_policy.starts.end())
        return 0;
    auto &length = ai.m_policy.channelLengths[u->GetGUID()];
    if (length.first != episode->second)
        length = {episode->second, cast->GetCastedTime()};
    return length.second; // Observable first remaining sample; never recalculate hidden spell mods.
}
Spell *Casting(Unit *u)
{
    if (!u)
        return nullptr;
    for (CurrentSpellTypes t : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
        if (auto *s = u->GetCurrentSpell(t))
            return s;
    return nullptr;
}
void Ranks(RuntimeActor &ai)
{
    auto &s = ai.m_policy;
    uint32 now = Now();
    if (s.cacheAt && uint32(now - s.cacheAt) < 1000)
        return;
    s.cacheAt = now;
    s.ownerRanks.clear();
    s.petRanks.clear();
    auto add = [](std::map<uint32, uint32> &out, uint32 id)
    {
        auto *e = sSpellMgr.GetSpellEntry(id);
        if (!e || e->IsPassiveSpell() || e->HasEffect(SPELL_EFFECT_LEARN_SPELL))
            return;
        uint32 root = Root(id);
        auto i = out.find(root);
        if (i == out.end() || sSpellMgr.GetSpellRank(id) > sSpellMgr.GetSpellRank(i->second))
            out[root] = id;
    };
    for (auto const &r : ai.me->GetSpellMap())
        if (r.second.state != PLAYERSPELL_REMOVED && !r.second.disabled)
            add(s.ownerRanks, r.first);
    if (Pet *pet = ai.me->GetPet())
        for (auto const &r : pet->m_petSpells)
            if (r.second.state != PETSPELL_REMOVED)
                add(s.petRanks, r.first);
}
void Auras(PvPCombatPolicy::Context &c, std::string const &key, Unit *u, Player *owner)
{
    if (!u)
        return;
    for (auto const &a : u->GetSpellAuraHolderMap())
    {
        auto *h = a.second;
        if (u == owner || h->GetAuraSlot() < MAX_AURAS)
            c.sets[key].insert(Root(h->GetId()));
        if ((key == "target.aura" || key == "ally.aura") && h->GetCasterGuid() == owner->GetObjectGuid())
            c.sets[key == "target.aura" ? "target.own_aura" : "ally.own_aura"].insert(Root(h->GetId()));
    }
}
bool Teaching(SpellEntry const *spell, std::set<uint32> &seen)
{
    if (!spell || seen.count(spell->Id))
        return false;
    if (seen.size() >= 16)
        return true;
    seen.insert(spell->Id);
    if (spell->HasEffect(SPELL_EFFECT_LEARN_SPELL) || spell->HasEffect(SPELL_EFFECT_LEARN_PET_SPELL) ||
        spell->HasEffect(SPELL_EFFECT_SKILL_STEP))
        return true;
    for (unsigned i = 0; i < 3; ++i)
        if (spell->EffectTriggerSpell[i] && Teaching(sSpellMgr.GetSpellEntry(spell->EffectTriggerSpell[i]), seen))
            return true;
    return false;
}
struct Roll
{
    double chance = 0, depth = 0;
    uint32 reaction = 0, created = 0;
};
std::mutex sharedMutex;
std::map<std::string, Roll> rolls;
std::map<std::string, std::pair<uint64, uint32>> leases;
void Drain(RuntimeActor &ai)
{
    auto &s = ai.m_policy;
    uint32 now = Now();
    std::deque<Event> pending;
    {
        std::lock_guard<std::mutex> queueLock(s.eventMutex);
        pending.swap(s.pending);
    }
    while (!pending.empty())
    {
        auto e = pending.front();
        pending.pop_front();
        if (uint32(now - e.time) > 5000)
            continue;
        Unit *u = Resolve(ai, e.caster);
        if (!Seen(ai, u) || !ai.ObservedEnemy(u))
            continue;
        if (e.start)
        {
            if (Spell *current = Casting(u))
                if (current->m_spellInfo->Id == e.spell)
                {
                    s.starts[e.caster] = current->GetCastInstanceId();
                    s.startSpells[e.caster] = e.spell;
                }
        }
        else
        {
            s.history.push_back(e);
            if (s.history.size() > 128)
                s.history.pop_front();
        }
    }
    while (!s.history.empty() && uint32(now - s.history.front().time) > 300000)
        s.history.pop_front();
    if (s.starts.size() > 128)
    {
        s.startSpells.erase(s.starts.begin()->first);
        s.channelLengths.erase(s.starts.begin()->first);
        s.starts.erase(s.starts.begin());
    }
}
PvPCombatPolicy::Context Context(RuntimeActor &ai, Unit *target, bool fast)
{
    auto &s = ai.m_policy;
    PvPCombatPolicy::Context c;
    auto *me = ai.me;
    for (auto key : {"self.aura", "self.knows", "pet.knows"})
        c.sets[key];
    auto unit = [&](Unit *u, std::string p)
    {
        c.numbers[p + "health_pct"] = u ? u->GetHealthPercent() : 0;
        c.numbers[p + "mana_pct"] = u ? u->GetPowerPercent(POWER_MANA) : 0;
        c.numbers[p + "class"] = u ? u->GetClass() : 0;
        c.flags[p + "moving"] = u && u->IsMoving();
        c.flags[p + "controlled"] = u && u->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL);
    };
    unit(me, "self.");
    if (target)
    {
        unit(target, "target.");
        c.sets["target.aura"];
        c.sets["target.own_aura"];
    }
    c.numbers["self.combo_points"] =
        target && me->GetComboTargetGuid() == target->GetObjectGuid() ? me->GetComboPoints() : 0;
    c.numbers["self.form"] = me->GetShapeshiftForm();
    c.flags["self.combat"] = me->IsInCombat();
    c.flags["self.stealthed"] = me->HasAuraType(SPELL_AURA_MOD_STEALTH);
    bool carrying = false;
    if (ai.battle && me->GetBattleGround()->GetTypeID() == BATTLEGROUND_WS)
    {
        auto *ws = static_cast<BattleGroundWS *>(me->GetBattleGround());
        carrying = ws->GetAllianceFlagPickerGuid() == me->GetObjectGuid() ||
                   ws->GetHordeFlagPickerGuid() == me->GetObjectGuid();
    }
    c.flags["self.carrying_flag"] = carrying;
    bool opening = false;
    if (Spell *cast = Casting(me))
        if (ai.battle && cast->m_spellInfo->Id == 21651)
            if (GameObject *go = me->GetMap()->GetGameObject(cast->m_targets.getGOTargetGuid()))
            {
                auto const &flags = me->GetBattleGround()->GetTypeID() == BATTLEGROUND_AB ? vFlagsAB : vFlagsAV;
                opening = std::find(flags.begin(), flags.end(), go->GetEntry()) != flags.end();
            }
    c.flags["self.opening_flag"] = opening;
    c.numbers["self.role"] = ai.GetRole();
    c.numbers["self.level"] = me->GetLevel();
    c.numbers["self.power"] = me->GetPower(me->GetPowerType());
    c.flags["self.casting"] = Casting(me);
    c.flags["self.mounted"] = me->IsMounted();
    c.flags["self.has_pet"] = me->GetPet();
    c.flags["self.has_shield"] = ai.IsWearingShield(me);
    if (target)
    {
        c.numbers["target.distance"] = target ? me->GetCombatDistance(target) : 99999;
        c.flags["target.player"] = target && target->IsPlayer();
        c.flags["target.pet"] = target && target->GetAffectingPlayer() && !target->IsPlayer();
        Spell *cast = Casting(target);
        c.flags["target.casting"] = cast;
        c.flags["target.healing"] = cast && cast->m_spellInfo->IsHealSpell();
        c.flags["target.interruptible"] =
            cast &&
            (cast->getState() == SPELL_STATE_CASTING ||
             (cast->getState() == SPELL_STATE_PREPARING && cast->GetCastTime() > 0)) &&
            cast->m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE &&
            (cast == target->GetCurrentSpell(CURRENT_GENERIC_SPELL)
                 ? cast->m_spellInfo->HasSpellInterruptFlag(SPELL_INTERRUPT_FLAG_DAMAGE_PUSHBACK)
                 : cast->m_spellInfo->HasChannelInterruptFlag(AURA_INTERRUPT_ACTION_CANCELS));
        c.numbers["target.cast_remaining_ms"] = cast ? cast->GetCastedTime() : 0;
        c.numbers["target.cast_progress_pct"] =
            cast && CastLength(ai, target, cast) > 0
                ? std::max(0.0, std::min(100.0,
                                         100.0 * (1.0 - double(cast->GetCastedTime()) / CastLength(ai, target, cast))))
                : 0;
    }
    for (auto const &r : s.ownerRanks)
        c.sets["self.knows"].insert(r.first);
    for (auto const &r : s.petRanks)
        c.sets["pet.knows"].insert(r.first);
    Auras(c, "self.aura", me, me);
    Auras(c, "target.aura", target, me);
    if (!fast)
    {
        std::list<Player *> near;
        me->GetAlivePlayerListInRange(me, near, 80);
        s.enemies = 0;
        s.sensedEnemies.clear();
        s.ally = me->GetGUID();
        float hp = me->GetHealthPercent();
        for (auto *p : near)
            if (Seen(ai, p) && ai.SameScene(p))
            {
                if (ai.ObservedEnemy(p))
                {
                    ++s.enemies;
                    s.sensedEnemies.push_back(p->GetGUID());
                    if (Pet *pet = p->GetPet())
                        if (Seen(ai, pet))
                            s.sensedEnemies.push_back(pet->GetGUID());
                }
                else if (ai.Friendly(p) && p->GetHealthPercent() < hp)
                {
                    hp = p->GetHealthPercent();
                    s.ally = p->GetGUID();
                }
            }
    }
    if (!fast)
    {
        for (Unit *enemy : me->GetAttackers())
            if (Seen(ai, enemy) && ai.ObservedEnemy(enemy))
                s.sensedEnemies.push_back(enemy->GetGUID());
        if (target && ai.ObservedEnemy(target))
            s.sensedEnemies.push_back(target->GetGUID());
        if (!ai.battle)
        {
            std::list<Unit *> units;
            me->GetEnemyListInRadiusAround(me, 80, units);
            for (Unit *u : units)
                if (Seen(ai, u) && me->IsValidAttackTarget(u))
                    s.sensedEnemies.push_back(u->GetGUID());
        }
        std::sort(s.sensedEnemies.begin(), s.sensedEnemies.end());
        s.sensedEnemies.erase(std::unique(s.sensedEnemies.begin(), s.sensedEnemies.end()), s.sensedEnemies.end());
        s.sensedOverflow = s.sensedEnemies.size() > 64;
        if (s.sensedOverflow)
            s.sensedEnemies.resize(64);
    }
    unsigned observed = 0;
    for (uint64 id : s.sensedEnemies)
        if (Unit *enemy = Resolve(ai, id))
            if (Seen(ai, enemy) && ai.ObservedEnemy(enemy))
                ++observed;
    Unit *ally = Resolve(ai, s.ally);
    if (Seen(ai, ally))
    {
        c.numbers["ally.health_pct"] = ally->GetHealthPercent();
        c.sets["ally.aura"];
        c.sets["ally.own_aura"];
        Auras(c, "ally.aura", ally, me);
    }
    c.numbers["enemies.nearby"] = observed;
    if (target)
        for (auto const &e : s.history)
            if (e.caster == target->GetGUID())
                c.recentTargetSpellsMs[Root(e.spell)] = uint32(Now() - e.time);
    return c;
}
bool AreaSafe(RuntimeActor &ai, Unit *actor, Unit *center, SpellEntry const *spell, std::string &why, Unit *&aim)
{
    if (spell->IsPositiveSpell() || !spell->IsAreaOfEffectSpell())
        return true;
    if (ai.m_policy.sensedOverflow)
    {
        why = "aoe_sense_capacity";
        return false;
    }
    float radius = 0;
    for (unsigned i = 0; i < 3; ++i)
        if (spell->EffectRadiusIndex[i])
            if (auto *row = sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))
                radius = std::max(radius, Spells::GetSpellRadius(row));
    if (Player *owner = actor->GetSpellModOwner())
        owner->ApplySpellMod(spell->Id, SPELLMOD_RADIUS, radius, nullptr, false);
    if (radius <= 0)
    {
        why = "aoe_radius_unsupported";
        return false;
    }
    bool legal = false;
    for (uint64 guid : ai.m_policy.sensedEnemies)
    {
        Unit *enemy = Resolve(ai, guid);
        if (!Seen(ai, enemy) || !enemy->IsAlive() || !actor->IsValidAttackTarget(enemy) ||
            !enemy->IsWithinDistInMap(center, radius))
            continue;
        if (!ai.battle && !enemy->GetAffectingPlayer())
        {
            why = "external_aoe_npc_risk";
            return false;
        }
        if (enemy->IsImmuneToSpell(spell, false))
            continue;
        if (enemy->HasBreakableByDamageCrowdControlAura() || enemy->HasAuraType(SPELL_AURA_MOD_CHARM) ||
            enemy->HasAuraType(SPELL_AURA_MOD_POSSESS))
        {
            why = "aoe_would_break_cc";
            return false;
        }
        if (LegalEnemy(ai, enemy))
        {
            legal = true;
            if (!aim || enemy == ai.me->GetVictim())
                aim = enemy;
        }
    }
    if (!legal)
    {
        why = "aoe_no_legal_observed_victim";
        return false;
    }
    return true;
}
bool Managed(PvPCombatPolicy::Snapshot const &p, RuntimeActor const &ai, uint32 root, std::string const &actor)
{
    return DeclaresManaged(p, ai.me->GetClass(), ai.GetRole(), root, actor, Root);
}

void AutoPet(RuntimeActor &ai, PvPCombatPolicy::Snapshot const *snapshot)
{
    auto &s = ai.m_policy;
    Pet *pet = ai.me->GetPet();
    if (!pet)
        return;
    if (s.petGuid != pet->GetGUID())
    {
        s.petAuto.clear();
        s.petGuid = pet->GetGUID();
    }
    if (!snapshot || !snapshot->enabled)
    {
        for (auto const &r : s.petAuto)
            if (pet->HasSpell(r.first))
                pet->ToggleAutocast(r.first, r.second);
        s.petAuto.clear();
        return;
    }
    for (auto i = s.petAuto.begin(); i != s.petAuto.end();)
        if (!Managed(*snapshot, ai, Root(i->first), "pet"))
        {
            if (pet->HasSpell(i->first))
                pet->ToggleAutocast(i->first, i->second);
            i = s.petAuto.erase(i);
        }
        else
            ++i;
    for (auto const &r : pet->m_petSpells)
        if (r.second.state != PETSPELL_REMOVED && Managed(*snapshot, ai, Root(r.first), "pet"))
        {
            if (!s.petAuto.count(r.first))
                s.petAuto[r.first] = r.second.active == ACT_ENABLED;
            pet->ToggleAutocast(r.first, false);
        }
}
} // namespace
bool ExternalEnemyAllowed(bool duel, bool active, bool opponent, bool controlled, bool attackable)
{
    return controlled && attackable && (!duel || (active && opponent));
}
bool ExternalAllyAllowed(bool duel, bool self, bool group, bool attackable)
{
    return self || (!duel && group && !attackable);
}
bool DeclaresManaged(PvPCombatPolicy::Snapshot const &p, unsigned classId, unsigned role, uint32_t root,
                     std::string const &actor, uint32_t (*normalize)(uint32_t))
{
    for (auto const &r : p.rules)
        if (r.classId == classId && (!r.role || r.role == role) && r.action.actor == actor &&
            normalize(r.action.spell) == root)
            return true;
    return false; // Rule.enabled intentionally does not release legacy ownership.
}
bool TimingReady(double u, double v, uint32_t reaction, double chance, double minPct, double maxPct, uint32_t minMs,
                 uint32_t maxMs, double progress, double elapsed)
{
    if (chance <= 0 || u > chance)
        return false;
    return progress >= minPct + v * (maxPct - minPct) &&
           elapsed >= minMs + double(reaction) / 1000000 * (maxMs - minMs);
}
void EnsureConfigured()
{
    static std::once_flag once;
    std::call_once(
        once,
        []
        {
            std::string path = sConfig.GetStringDefault("BattleBot.PvPPolicy.File",
                                                        "/workspace/vmangos/_install/etc/pvp-combat-policy.json");
            PvPCombatPolicy::Configure(path.empty() ? "/workspace/vmangos/_install/etc/pvp-combat-policy.json" : path);
        });
}
void ObserveExternal(State &state, WorldPacket const *packet)
{
    if (!packet || !state.acceptEvents.load())
        return;
    if (packet->GetOpcode() != SMSG_SPELL_GO && packet->GetOpcode() != SMSG_SPELL_START)
        return;
    try
    {
        WorldPacket p(*packet);
        p.rpos(0);
        p.readPackGUID();
        uint64 caster = p.readPackGUID();
        uint32 spell;
        p >> spell;
        std::lock_guard<std::mutex> lock(state.eventMutex);
        if (state.pending.size() >= 128)
            state.pending.pop_front();
        state.pending.push_back({caster, spell, Now(), packet->GetOpcode() == SMSG_SPELL_START});
    }
    catch (...)
    {
    }
}
uint32_t ActualCastTime(Unit *actor, SpellEntry const *spell)
{
    if (!actor || !spell)
        return 0;
    auto *row = sSpellCastTimesStore.LookupEntry(spell->CastingTimeIndex);
    if (!row)
        return 0;
    int32 rank = actor->GetSpellRank(spell);
    int32 time = row->CastTime + row->CastTimePerLevel * (rank / 5 - spell->baseLevel);
    time = std::max(time, row->MinCastTime);
    if (time)
        if (Player *owner = actor->GetSpellModOwner())
            owner->ApplySpellMod(spell->Id, SPELLMOD_CASTING_TIME, time, nullptr, false);
    if (!(spell->Attributes & (SPELL_ATTR_IS_ABILITY | SPELL_ATTR_IS_TRADESKILL)))
    {
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_11_2
        time = int32(time * actor->GetFloatValue(UNIT_MOD_CAST_SPEED));
#else
        time = int32(time * (1.0f + actor->GetInt32Value(UNIT_MOD_CAST_SPEED) / 100.0f));
#endif
    }
    else if (spell->IsRangedSpell() && !spell->IsAutoRepeatRangedSpell())
        time = int32(time * actor->m_modAttackSpeedPct[RANGED_ATTACK]);
    if ((spell->Attributes & SPELL_ATTR_USES_RANGED_SLOT) && !spell->IsAutoRepeatRangedSpell())
        time += 500;
    return time > 0 ? uint32(time) : 0;
}
bool NativeCheck(Unit *actor, Unit *target, SpellEntry const *spell, std::string &why)
{
    if (!actor || !target || !spell)
    {
        why = "missing_actor_target_spell";
        return false;
    }
    std::set<uint32> teaching;
    if (spell->IsPassiveSpell() || Teaching(spell, teaching))
    {
        why = "passive_or_teaching";
        return false;
    }
    if ((actor->IsPlayer() && !actor->ToPlayer()->HasSpell(spell->Id)) ||
        (actor->IsPet() && !static_cast<Pet *>(actor)->HasSpell(spell->Id)))
    {
        why = "not_known_by_actor";
        return false;
    }
    if (!actor->IsSpellReady(spell))
    {
        why = "cooldown";
        return false;
    }
    if (actor->HasGCD(spell))
    {
        why = "gcd";
        return false;
    }
    uint32 cost = Spell::CalculatePowerCost(spell, actor, nullptr, nullptr, false);
    if (spell->powerType == POWER_HEALTH ? actor->GetHealth() <= cost
                                         : actor->GetPower(Powers(spell->powerType)) < cost)
    {
        why = "resource";
        return false;
    }
    if (spell->TargetAuraState && !target->HasAuraState(AuraState(spell->TargetAuraState)))
    {
        why = "target_aura_state";
        return false;
    }
    if (spell->CasterAuraState && !actor->HasAuraState(AuraState(spell->CasterAuraState)))
    {
        why = "caster_aura_state";
        return false;
    }
    if (spell->GetErrorAtShapeshiftedCast(actor->GetShapeshiftForm()) != SPELL_CAST_OK)
    {
        why = "shape";
        return false;
    }
    auto control = Spell::CheckCasterAurasFor(actor, spell, ActualCastTime(actor, spell));
    if (control != SPELL_CAST_OK)
    {
        why = "control native=" + std::to_string(control);
        return false;
    }
    // Caster-source Nova/Scream does not hit self: never test self immunity.
    if (actor != target && target->IsImmuneToSpell(spell, false))
    {
        why = "immune";
        return false;
    }
    if (actor != target && !actor->IsWithinLOSInMap(target))
    {
        why = "los";
        return false;
    }
    if (actor != target && spell->rangeIndex != SPELL_RANGE_IDX_SELF_ONLY)
    {
        if (spell->rangeIndex == SPELL_RANGE_IDX_COMBAT)
        {
            float mod = 1, base = ATTACK_DISTANCE;
            if (Player *owner = actor->GetSpellModOwner())
                mod += owner->ApplySpellMod(spell->Id, SPELLMOD_RANGE, base, nullptr, false);
            if (!spell->IsNextMeleeSwingSpell() && !actor->CanReachWithMeleeSpellAttack(target, mod))
            {
                why = "range melee";
                return false;
            }
        }
        else
        {
            auto *range = sSpellRangeStore.LookupEntry(spell->rangeIndex);
            if (!range)
            {
                why = "range_missing";
                return false;
            }
            float max = Spells::GetSpellMaxRange(range), min = Spells::GetSpellMinRange(range);
            if (Player *owner = actor->GetSpellModOwner())
                owner->ApplySpellMod(spell->Id, SPELLMOD_RANGE, max, nullptr, false);
            max += (actor->IsPlayer() ? 1.25f : 0.0f) + actor->GetLeewayBonusRange(target, true);
            float distance = actor->GetCombatDistance(target);
            if (distance > max || (min && distance < min))
            {
                why = "range distance=" + std::to_string(distance) + " min=" + std::to_string(min) +
                      " max=" + std::to_string(max);
                return false;
            }
        }
    }
    if (Spell *current = actor->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        if (current->m_spellInfo->Id == spell->Id)
        {
            why = "autorepeat_already_active";
            return false;
        }
    if (Spell *current = actor->GetCurrentSpell(CURRENT_MELEE_SPELL))
        if (current->m_spellInfo->Id == spell->Id)
        {
            why = "next_melee_already_queued";
            return false;
        }
    if (ActualCastTime(actor, spell) > 0 && actor->IsMoving())
    {
        why = "moving_cast";
        return false;
    }
    why = "readonly_preflight_ok_final_native_pending";
    return true;
}

bool CanLegacy(RuntimeActor const &ai, SpellEntry const *spell)
{
    bool nativeDuelScope = !ai.battle && ai.externalDuel && ai.me && ai.me->m_duel && ai.me->m_duel->startTime;
    if (!spell || (!Eligible(ai) && !nativeDuelScope))
        return true;
    auto p = PvPCombatPolicy::Current();
    if (!p || !p->enabled)
        return true;
    std::lock_guard<std::recursive_mutex> lock(ai.m_policy.mutex);
    if (ai.m_policy.executing && ai.m_policy.permitActor == "owner" && ai.m_policy.permitRoot == Root(spell->Id))
        return true;
    bool allowed = !Managed(*p, ai, Root(spell->Id), "owner");
    if (!allowed)
        ai.m_policy.legacy = "legacy_blocked managedRoot=" + std::to_string(Root(spell->Id));
    return allowed;
}
void Forget(RuntimeActor &ai)
{
    std::lock_guard<std::recursive_mutex> lock(ai.m_policy.mutex);
    if (ai.me)
        AutoPet(ai, nullptr);
    auto &s = ai.m_policy;
    s.acceptEvents = false;
    s.mapId = 0;
    s.instanceId = 0;
    s.scopeTag = 0;
    s.duelFlag = 0;
    s.ally = 0;
    s.enemies = 0;
    s.sensedEnemies.clear();
    s.sensedOverflow = false;
    {
        std::lock_guard<std::mutex> queueLock(s.eventMutex);
        s.pending.clear();
    }
    s.history.clear();
    s.starts.clear();
    s.startSpells.clear();
    s.channelLengths.clear();
    s.lastUse.clear();
    s.ownerRanks.clear();
    s.petRanks.clear();
    s.cacheAt = 0;
    s.fastAt = 0;
    s.last = "状态已清理";
    s.lastCast.clear();
    s.legacy.clear();
}
std::string ExplainExternal(State const &state)
{
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    auto const &n = state.counters;
    return "lastDecision=" + state.last + " lastCast=" + state.lastCast + " legacy=" + state.legacy +
           " ownerCasts=" + std::to_string(n[State::OwnerCast]) + " petCasts=" + std::to_string(n[State::PetCast]) +
           " rejectCD=" + std::to_string(n[State::RejectCooldown]) +
           " rejectGCD=" + std::to_string(n[State::RejectGcd]) +
           " rejectRange=" + std::to_string(n[State::RejectRange]) +
           " rejectResource=" + std::to_string(n[State::RejectResource]) +
           " rejectControl=" + std::to_string(n[State::RejectControl]) +
           " rejectShape=" + std::to_string(n[State::RejectShape]) +
           " rejectUnknown=" + std::to_string(n[State::RejectUnknown]);
}
bool Tick(RuntimeActor &ai, bool fast)
{
    EnsureConfigured();
    PvPCombatPolicy::Poll();
    if (!Eligible(ai))
    {
        if (!ai.battle && ai.m_policy.scopeTag >= 2)
            Forget(ai);
        ai.m_policy.acceptEvents = false;
        return false;
    }
    uint32 tag = ai.battle ? 1 : ai.externalDuel ? 3 : 2;
    uint64 duelFlag = ai.externalDuel ? ai.me->GetUInt64Value(PLAYER_DUEL_ARBITER) : 0;
    if (ai.m_policy.scopeTag != tag || ai.m_policy.duelFlag != duelFlag || ai.m_policy.mapId != ai.me->GetMapId() ||
        ai.m_policy.instanceId != ai.me->GetInstanceId())
    {
        Forget(ai);
        ai.m_policy.mapId = ai.me->GetMapId();
        ai.m_policy.instanceId = ai.me->GetInstanceId();
        ai.m_policy.scopeTag = tag;
        ai.m_policy.duelFlag = duelFlag;
    }
    ai.m_policy.acceptEvents = true;
    auto p = PvPCombatPolicy::Current();
    auto &s = ai.m_policy;
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    AutoPet(ai, p.get());
    if (!p || !p->enabled)
        return false;
    uint32 now = Now();
    if (fast)
    {
        if (s.fastAt && uint32(now - s.fastAt) < 200)
            return false;
        s.fastAt = now;
    }
    Drain(ai);
    Ranks(ai);
    Unit *target = ai.me->GetVictim();
    BattleBotTactics::Rank rank;
    if (!LegalEnemy(ai, target))
        target = fast ? nullptr : ai.SelectTacticalTarget();
    if (!LegalEnemy(ai, target))
        target = nullptr;
    auto c = Context(ai, target, fast);
    for (auto candidate : PvPCombatPolicy::Rank(*p, c, fast))
    {
        auto const &r = *candidate.rule;
        if (!fast && r.fast)
            continue;
        if (s.lastUse.count(r.id) && uint32(now - s.lastUse[r.id]) < r.minIntervalMs)
            continue;
        Unit *actor = r.action.actor == "pet" ? static_cast<Unit *>(ai.me->GetPet()) : static_cast<Unit *>(ai.me);
        Unit *to = r.action.target == "self"    ? actor
                   : r.action.target == "enemy" ? target
                   : r.action.target == "pet"   ? static_cast<Unit *>(ai.me->GetPet())
                   : r.action.target == "ally"  ? Resolve(ai, s.ally)
                                                : static_cast<Unit *>(ai.me);
        auto const &known = r.action.actor == "pet" ? s.petRanks : s.ownerRanks;
        auto found = known.find(Root(r.action.spell));
        std::string why;
        auto reject = [&](std::string const &reason)
        {
            s.last = "rule=" + r.id + " rejected=" + reason + " revision=" + std::to_string(p->revision);
            auto slot = reason == "cooldown"          ? State::RejectCooldown
                        : reason == "gcd"             ? State::RejectGcd
                        : reason.find("range") == 0   ? State::RejectRange
                        : reason == "resource"        ? State::RejectResource
                        : reason.find("control") == 0 ? State::RejectControl
                        : reason == "shape"           ? State::RejectShape
                        : reason == "unknown_rank"    ? State::RejectUnknown
                                                      : State::RejectOther;
            ++s.counters[slot];
            if (target && target->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                s.last += " channel_progress=first_observed_estimate";
        };
        if (found == known.end())
        {
            reject("unknown_rank");
            continue;
        }
        auto *spell = sSpellMgr.GetSpellEntry(found->second);
        if (actor == ai.me && ai.m_preventCasting)
        {
            reject("casting_disabled_by_ai");
            continue;
        }
        if (!actor || !actor->IsAlive() || !to)
        {
            reject("actor_target_unavailable");
            continue;
        }
        if (to != ai.me && !Seen(ai, to))
        {
            reject("unobserved_or_range80");
            continue;
        }
        if (r.action.target == "enemy" && !LegalEnemy(ai, target))
        {
            reject("map_objective_target_gate");
            continue;
        }
        if (!ai.battle && ai.externalDuel)
        {
            Player *opponent = ai.Opponent();
            if (opponent && to->GetAffectingPlayer() == opponent &&
                (r.action.category == "heal" || spell->IsPositiveSpell()))
            {
                reject("duel_opponent_heal_or_buff_forbidden");
                continue;
            }
            bool multi = spell->IsAreaOfEffectSpell();
            for (unsigned i = 0; i < 3; ++i)
                multi |= spell->EffectChainTarget[i] > 1;
            bool healing = spell->HasEffect(SPELL_EFFECT_HEAL) || spell->HasEffect(SPELL_EFFECT_HEAL_MAX_HEALTH);
            for (unsigned i = 0; i < 3; ++i)
                healing |= spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL;
            if (multi && healing)
            {
                reject("duel_area_heal_safety_unproven");
                continue;
            }
        }
        bool harmfulArea = !spell->IsPositiveSpell() && spell->IsAreaOfEffectSpell();
        bool hostile = actor->IsValidAttackTarget(to) || harmfulArea;
        if (actor == ai.me && hostile && (c.flags["self.carrying_flag"] || ai.m_wsgTravel))
        {
            reject("flag_or_regroup_movement");
            continue;
        }
        Spell *own = Casting(actor);
        bool cancel = own && actor == ai.me && r.action.cancelOwnDamageCast && !own->m_spellInfo->IsPositiveSpell() &&
                      own->m_spellInfo->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE) &&
                      own == actor->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (own && !cancel)
        {
            reject("active_cast_preserved");
            continue;
        }
        std::string leaseKey;
        if (r.action.category == "interrupt")
        {
            Spell *enemy = Casting(target);
            if (!enemy)
            {
                reject("no_active_cast");
                continue;
            }
            auto episode = s.starts.find(target->GetGUID());
            if (episode == s.starts.end() || s.startSpells[target->GetGUID()] != enemy->m_spellInfo->Id ||
                episode->second != enemy->GetCastInstanceId())
            {
                reject("cast_start_not_observed");
                continue;
            }
            std::string key = std::to_string(ai.me->GetInstanceId()) + ":" + std::to_string(ai.TeamKey()) + ":" +
                              std::to_string(target->GetGUID()) + ":" + std::to_string(episode->second);
            leaseKey = key;
            Roll roll;
            {
                std::lock_guard<std::mutex> guard(sharedMutex);
                auto at = rolls.find(key);
                if (at == rolls.end())
                {
                    roll = {frand(0, 1), frand(0, 1), urand(0, 1000000), now};
                    if (rolls.size() >= 4096)
                    {
                        auto oldest = std::max_element(
                            rolls.begin(), rolls.end(), [now](auto const &a, auto const &b)
                            { return uint32(now - a.second.created) < uint32(now - b.second.created); });
                        if (uint32(now - oldest->second.created) < 30000)
                        {
                            reject("team_timing_capacity");
                            continue;
                        }
                        rolls.erase(oldest);
                    }
                    rolls[key] = roll;
                }
                else
                    roll = at->second;
            }
            double progress = c.numbers["target.cast_progress_pct"];
            double elapsed = CastLength(ai, target, enemy) - enemy->GetCastedTime();
            if (!TimingReady(roll.chance, roll.depth, roll.reaction, r.timing.chance, r.timing.progressMinPct,
                             r.timing.progressMaxPct, r.timing.reactionMinMs, r.timing.reactionMaxMs, progress,
                             elapsed))
            {
                reject("stable_interrupt_wait_or_skip");
                continue;
            }
        }
        Unit *areaAim = nullptr;
        if (harmfulArea)
        {
            Unit *center = (to == actor || spell->rangeIndex == SPELL_RANGE_IDX_SELF_ONLY) ? actor : to;
            if (!AreaSafe(ai, actor, center, spell, why, areaAim))
            {
                reject(why);
                continue;
            }
        }
        if (!NativeCheck(actor, to, spell, why))
        {
            if (actor == ai.me && ActualCastTime(actor, spell) > 0 && ai.me->IsMoving() && why == "moving_cast" &&
                !ai.m_wsgTravel)
            {
                ai.me->StopMoving();
                if (!NativeCheck(actor, to, spell, why))
                {
                    reject(why);
                    continue;
                }
            }
            else
            {
                reject(why);
                continue;
            }
        }
        if (actor == ai.me)
        {
            if (ActualCastTime(actor, spell) > 0 && ai.me->IsMoving())
            {
                reject("moving_cast");
                continue;
            }
            // Facing is committed only after all non-mutating preflight gates.
            if (to != actor)
                actor->SetFacingToObject(to);
            else if (harmfulArea && areaAim)
                actor->SetFacingToObject(areaAim);
        }
        if (!leaseKey.empty())
        {
            std::lock_guard<std::mutex> guard(sharedMutex);
            auto at = leases.find(leaseKey);
            if (at != leases.end() && at->second.first != ai.me->GetGUID() && int32(at->second.second - now) > 0)
            {
                reject("team_interrupt_reserved");
                continue;
            }
            leases[leaseKey] = {ai.me->GetGUID(), now + 500};
            if (leases.size() > 4096)
                leases.erase(leases.begin());
        }
        if (cancel)
            actor->InterruptSpell(CURRENT_GENERIC_SPELL, true);
        struct Permit
        {
            State &s;
            Permit(State &state, uint32 root, std::string actor) : s(state)
            {
                s.executing = true;
                s.permitRoot = root;
                s.permitActor = std::move(actor);
            }
            ~Permit()
            {
                s.executing = false;
                s.permitRoot = 0;
                s.permitActor.clear();
            }
        };
        SpellCastResult result;
        {
            Permit permit(s, Root(spell->Id), r.action.actor);
            result = actor->CastSpell(to, spell, false);
        }
        if (result != SPELL_CAST_OK)
        {
            reject("cast_result=" + std::to_string(result));
            continue;
        }
        ++s.counters[r.action.actor == "pet" ? State::PetCast : State::OwnerCast];
        s.lastUse[r.id] = now;
        s.last = "rule=" + r.id + " cast=" + std::to_string(spell->Id) + " actor=" + r.action.actor +
                 " target=" + std::to_string(to->GetGUID()) + " revision=" + std::to_string(p->revision) +
                 " generation=" + std::to_string(p->generation) + " score=" + std::to_string(candidate.score);
        if (target && target->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            s.last += " channel_progress=first_observed_estimate";
        s.lastCast = s.last;
        return true;
    }
    return false;
}
bool Tick(BattleBotAI &ai, bool fast)
{
    RuntimeActor actor(ai, ai.m_policy, &ai, false);
    return Tick(actor, fast);
}
bool TickExternal(CombatBotBaseAI &ai, State &state, bool duel, bool fast)
{
    RuntimeActor actor(ai, state, nullptr, duel);
    return Tick(actor, fast);
}
bool CanLegacy(BattleBotAI const &ai, SpellEntry const *spell)
{
    RuntimeActor actor(const_cast<BattleBotAI &>(ai), ai.m_policy, const_cast<BattleBotAI *>(&ai), false);
    return CanLegacy(actor, spell);
}
bool CanLegacyExternal(CombatBotBaseAI const &ai, State &state, SpellEntry const *spell, bool duel)
{
    RuntimeActor actor(const_cast<CombatBotBaseAI &>(ai), state, nullptr, duel);
    return CanLegacy(actor, spell);
}
void ObservePacket(BattleBotAI &ai, WorldPacket const *packet) { ObserveExternal(ai.m_policy, packet); }
void Forget(BattleBotAI &ai)
{
    RuntimeActor actor(ai, ai.m_policy, &ai, false);
    Forget(actor);
}
void ForgetExternal(CombatBotBaseAI &ai, State &state)
{
    RuntimeActor actor(ai, state, nullptr, false);
    Forget(actor);
}
std::string Explain(Player *p)
{
    auto *ai = p && p->AI() ? dynamic_cast<BattleBotAI *>(p->AI()) : nullptr;
    return ai ? ExplainExternal(ai->m_policy) : "非战场BattleBot";
}
} // namespace BattleBotPolicyRuntime
