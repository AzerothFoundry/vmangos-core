// Copyright 2026. GPL-2.0-or-later. Shared assignments contain GUIDs, never actor pointers.
#include "RaidCoordination.h"
#include "BotDamagePolicy.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PartyBotAI.h"
#include "PartyBotPreset.h"
#include "PathFinder.h"
#include "Pet.h"
#include "Player.h"
#include "RaidCoordinationPolicy.h"
#include "Server/Packets/Group.h"
#include "Spell.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
namespace RaidCoordination
{
namespace
{
using Key = std::tuple<uint32, uint32, uint64, uint32>;
struct State
{
    bool enabled = true;
    bool autoPull = false;
    bool autoAuthorized = false;
    uint64 autoOwner = 0;
    float anchorX = 0, anchorY = 0, anchorZ = 0;
    uint64 pull = 0;
    uint32 pullAt = 0;
    std::string pullReason = "尚未开始";
    std::map<uint64, unsigned> ready;
    std::set<uint64> botReplies;
    uint32 touched = 0;
    uint32 massTauntAt = 0;
    std::map<uint64, uint64> owners;
    std::map<uint64, uint32> plannedOwners;
    std::map<std::pair<uint64, uint64>, bool> throttled;
};
std::map<Key, State> states;
std::recursive_mutex stateMutex;
Key GetKey(Player *p)
{
    return Key(p->GetMapId(), p->GetInstanceId(), p->GetGroup()->GetLeaderGuid().GetRawValue(), p->GetGroup()->GetId());
}
State &Get(Player *p)
{
    uint32 now = WorldTimer::getMSTime();
    for (auto i = states.begin(); i != states.end();)
        if (uint32(now - i->second.touched) > 300000)
            i = states.erase(i);
        else
            ++i;
    State &s = states[GetKey(p)];
    s.touched = now;
    return s;
}
std::vector<Player *> Tanks(Player *p)
{
    std::vector<Player *> v;
    for (GroupReference *r = p->GetGroup()->GetFirstMember(); r; r = r->next())
    {
        Player *q = r->getSource();
        if (!q || q->GetMap() != p->GetMap() || !q->IsAlive() || q->IsBeingTeleported() ||
            q->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
            continue;
        auto *ai = dynamic_cast<PartyBotAI *>(q->AI());
        if ((ai && ai->m_initialized && ai->GetRole() == ROLE_TANK) ||
            (!q->IsBot() && PartyPreset::PreferredRole(q) == ROLE_TANK) ||
            q->GetObjectGuid() == p->GetGroup()->GetMainTankGuid())
            v.push_back(q);
    }
    std::sort(v.begin(), v.end(), [p](Player *a, Player *b) {
        auto mt = p->GetGroup()->GetMainTankGuid();
        if ((a->GetObjectGuid() == mt) != (b->GetObjectGuid() == mt))
            return a->GetObjectGuid() == mt;
        return a->GetObjectGuid() < b->GetObjectGuid();
    });
    return v;
}
bool Reach(Player *p, Unit const *e)
{
    return p && p->IsAlive() && p->GetMap() == e->GetMap() && p->IsWithinDistInMap(e, 40) && p->IsWithinLOSInMap(e);
}
} // namespace
bool Enabled(Player *p)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!p || !p->GetGroup() || !p->GetMap()->IsDungeon() || p->InBattleGround())
        return false;
    bool quick = false;
    for (GroupReference *r = p->GetGroup()->GetFirstMember(); r; r = r->next())
        if (Player *q = r->getSource())
            if (auto *a = dynamic_cast<PartyBotAI *>(q->AI()))
                if (a->m_quickRaid)
                    quick = true;
    return quick && Get(p).enabled;
}
void SetEnabled(Player *p, bool on)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (p && p->GetGroup())
    {
        State &state = Get(p);
        state.enabled = on;
        if (!on)
        {
            state.pull = 0;
            state.autoPull = false;
            state.ready.clear();
        }
    }
}
bool Reset(Player *p)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!p || !p->GetGroup())
        return false;
    for (GroupReference *r = p->GetGroup()->GetFirstMember(); r; r = r->next())
        if (Player *q = r->getSource())
            if (q->IsInCombat() || (q->GetPet() && q->GetPet()->IsInCombat()))
                return false;
    states.erase(GetKey(p));
    return true;
}
void CancelPull(Player *p)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!p || !p->GetGroup())
        return;
    State &state = Get(p);
    state.pull = 0;
    state.autoPull = false;
    state.ready.clear();
    state.pullReason = "拉怪已取消";
}
Player *AssignedTank(Player *p, Unit const *e)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!Enabled(p) || !e || e->GetMap() != p->GetMap())
        return nullptr;
    auto tanks = Tanks(p);
    auto &s = Get(p);
    auto &owner = s.owners[e->GetGUID()];
    if (Unit *victim = e->GetVictim())
        if (Player *human = victim->ToPlayer())
            if (!human->IsBot() && human->GetGroup() == p->GetGroup() &&
                (PartyPreset::PreferredRole(human) == ROLE_TANK ||
                 human->GetObjectGuid() == p->GetGroup()->GetMainTankGuid()) &&
                Reach(human, e))
            {
                owner = human->GetGUID();
                return human;
            }
    for (Player *t : tanks)
        if (t->GetGUID() == owner && Reach(t, e))
            return t;
    // Preserve a tank already holding this target, including a real player.
    if (Unit *victim = e->GetVictim())
        if (victim->IsPlayer() && victim->ToPlayer()->GetGroup() == p->GetGroup())
        {
            Player *v = victim->ToPlayer();
            auto *ai = dynamic_cast<PartyBotAI *>(v->AI());
            if ((ai && ai->GetRole() == ROLE_TANK) || (!v->IsBot() && PartyPreset::PreferredRole(v) == ROLE_TANK) ||
                v->GetObjectGuid() == p->GetGroup()->GetMainTankGuid())
            {
                if (Reach(v, e))
                {
                    unsigned currentLoad = 0;
                    bool idleAvailable = false;
                    for (auto row : s.owners)
                        if (row.first != e->GetGUID() && row.second == v->GetGUID())
                            ++currentLoad;
                    for (Player *candidate : tanks)
                        if (candidate != v && Reach(candidate, e))
                        {
                            bool busy = false;
                            for (auto row : s.owners)
                                if (row.first != e->GetGUID() && row.second == candidate->GetGUID())
                                {
                                    busy = true;
                                    break;
                                }
                            if (!busy)
                            {
                                idleAvailable = true;
                                break;
                            }
                        }
                    if (RaidCoordinationPolicy::PreserveCurrentTank(!v->IsBot(), currentLoad, idleAvailable))
                    {
                        owner = v->GetGUID();
                        return v;
                    }
                }
            }
        }
    Player *best = nullptr;
    unsigned load = ~0u;
    for (Player *t : tanks)
    {
        if (!Reach(t, e))
            continue;
        unsigned n = 0;
        for (auto row : s.owners)
            if (row.second == t->GetGUID())
                ++n;
        if (!best || n < load)
        {
            best = t;
            load = n;
        }
    }
    owner = best ? best->GetGUID() : 0;
    return best;
}
Unit *TankTarget(PartyBotAI const &ai)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    Player *p = ai.me;
    if (!Enabled(p) || ai.GetRole() != ROLE_TANK)
        return nullptr;
    std::vector<Unit *> enemies;
    for (GroupReference *r = p->GetGroup()->GetFirstMember(); r; r = r->next())
        if (Player *q = r->getSource())
            if (q->GetMap() == p->GetMap())
            {
                for (Unit *e : q->GetAttackers())
                    if (e && e->IsAlive() && e->IsValidAttackTarget(p) && Reach(p, e))
                        enemies.push_back(e);
                if (Unit *e = q->GetVictim())
                    if (e->IsAlive() && e->IsInCombat() && e->IsValidAttackTarget(p) && Reach(p, e))
                        enemies.push_back(e);
            }
    std::sort(enemies.begin(), enemies.end(), [](Unit *a, Unit *b) {
        return a->GetMaxHealth() != b->GetMaxHealth() ? a->GetMaxHealth() > b->GetMaxHealth()
                                                      : a->GetGUID() < b->GetGUID();
    });
    enemies.erase(std::unique(enemies.begin(), enemies.end()), enemies.end());
    auto &s = Get(p);
    for (auto i = s.owners.begin(); i != s.owners.end();)
    {
        Creature *old = p->GetMap()->GetCreature(ObjectGuid(i->first));
        auto planned = s.plannedOwners.find(i->first);
        bool hasLease = planned != s.plannedOwners.end();
        uint32 elapsed = hasLease ? uint32(WorldTimer::getMSTime() - planned->second) : 0;
        bool exists = old && old->IsAlive() && old->GetMap() == p->GetMap();
        bool combat = exists && old->IsInCombat();
        bool keep = RaidCoordinationPolicy::KeepPlannedOwner(exists, combat, hasLease, elapsed);
        if (!keep || combat || (hasLease && elapsed >= 20000))
            s.plannedOwners.erase(i->first);
        if (!keep)
            i = s.owners.erase(i);
        else
            ++i;
    }
    Unit *best = nullptr;
    for (Unit *e : enemies)
        if (AssignedTank(p, e) == p)
        {
            if (!best || e == p->GetVictim())
                best = e;
            if (e->GetVictim() != p)
                return e;
        }
    return best;
}
static bool AllowOne(PartyBotAI const &ai, Unit const *e, SpellEntry const *spell)
{
    if (spell && !BotDamagePolicy::IsDamage(spell))
        return true;
    if (PullPending(ai.me))
        return false;
    Player *p = ai.me;
    if (!Enabled(p) || !e || e->IsPlayer() || !e->CanHaveThreatList() || !e->IsInCombat() ||
        ai.GetRole() == ROLE_TANK || (spell && spell->IsPositiveSpell()))
        return true;
    Player *tank = AssignedTank(p, e);
    if (!tank)
        return false;
    float tt = const_cast<Unit *>(e)->GetThreatManager().getThreat(tank);
    float mine = const_cast<Unit *>(e)->GetThreatManager().getThreat(p);
    if (Pet *pet = p->GetPet())
        mine = std::max(mine, const_cast<Unit *>(e)->GetThreatManager().getThreat(pet));
    auto &hold = Get(p).throttled[std::make_pair(p->GetGUID(), e->GetGUID())];
    hold = RaidCoordinationPolicy::HoldDamage(mine, tt, p->CanReachWithMeleeSpellAttack(e), hold);
    return !hold;
}
bool AllowDamage(PartyBotAI const &ai, Unit const *e, SpellEntry const *spell)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!AllowOne(ai, e, spell))
        return false;
    if (Enabled(ai.me) && e && spell && BotDamagePolicy::IsDamage(spell) && !spell->IsPositiveSpell() &&
        spell->IsAreaOfEffectSpell())
    {
        float radius = 10.0f;
        for (unsigned i = 0; i < 3; ++i)
            if (spell->EffectRadiusIndex[i])
                radius = std::max(radius,
                                  Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i])));
        std::list<Unit *> targets;
        ai.me->GetEnemyListInRadiusAround(const_cast<Unit *>(e), radius, targets);
        for (Unit *target : targets)
            if (!target->IsInCombat() || !AllowOne(ai, target, spell))
                return false;
    }
    return true;
}
bool PullPending(Player *p)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return Enabled(p) && Get(p).pull;
}
std::string AutoPull(Player *p, bool on)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!Enabled(p) || p->GetMapId() != 409 || !p->GetGroup()->IsLeader(p->GetObjectGuid()))
        return "自主拉怪拒绝：需要MC快速编队的队长";
    State &state = Get(p);
    state.pull = 0;
    state.autoPull = on;
    state.autoAuthorized = false;
    state.ready.clear();
    state.botReplies.clear();
    if (!on)
    {
        state.pullReason = "自主拉怪已关闭";
        return state.pullReason;
    }
    state.autoOwner = p->GetGUID();
    state.anchorX = p->GetPositionX();
    state.anchorY = p->GetPositionY();
    state.anchorZ = p->GetPositionZ();
    state.pullAt = WorldTimer::getMSTime();
    for (GroupReference *r = p->GetGroup()->GetFirstMember(); r; r = r->next())
    {
        Player *q = r->getSource();
        if (!q || !q->IsInWorld() || q->IsBeingTeleported() || !q->IsAlive() || q->GetMap() != p->GetMap() ||
            q->IsInCombat())
        {
            state.autoPull = false;
            state.pullReason = "自主拉怪拒绝：成员离线/死亡/异图/战斗";
            return state.pullReason;
        }
        state.ready[q->GetGUID()] = q->IsBot() ? 1 : 2;
    }
    if (state.ready.size() != p->GetGroup()->GetMembersCount())
    {
        state.autoPull = false;
        state.pullReason = "自主拉怪拒绝：编队中存在离线成员";
        return state.pullReason;
    }
    WorldPacket request(MSG_RAID_READY_CHECK, 0);
    p->GetGroup()->BroadcastPacket(&request, false);
    state.pullReason = "等待全体真人确认：固定45码区域内恶犬自主拉怪";
    return state.pullReason;
}
std::string BeginPull(Player *p, Unit *target)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!Enabled(p) || p->GetMapId() != 409 || !target || target->GetMap() != p->GetMap() || !target->IsAlive() ||
        target->IsInCombat())
        return "开怪拒绝：仅支持当前MC有效未进战目标";
    if (target->GetEntry() != 11671 && target->GetEntry() != 12056 && target->GetEntry() != 11988)
        return "开怪拒绝：目标尚未纳入验证范围";
    if (!p->GetGroup()->IsLeader(p->GetObjectGuid()))
        return "开怪拒绝：需要队长";
    auto tanks = Tanks(p);
    if (tanks.empty() || !target->IsValidAttackTarget(tanks.front()))
        return "开怪拒绝：当前主坦无法合法攻击该目标（检查阵营与GM模式）";
    State &state = Get(p);
    state.autoPull = false;
    state.pull = target->GetGUID();
    state.pullAt = WorldTimer::getMSTime();
    state.ready.clear();
    state.botReplies.clear();
    state.pullReason = "等待全员就位";
    for (GroupReference *r = p->GetGroup()->GetFirstMember(); r; r = r->next())
        if (Player *q = r->getSource())
            state.ready[q->GetGUID()] = q->IsBot() ? 1 : 2;
    WorldPacket request(MSG_RAID_READY_CHECK, 0);
    p->GetGroup()->BroadcastPacket(&request, false);
    return "已发原生就位确认；等待所有真人确认与机器人恢复（30秒有效）";
}
void ReadyResponse(Player *p, unsigned answer)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!Enabled(p) || p->IsBot())
        return;
    State &state = Get(p);
    auto i = state.ready.find(p->GetGUID());
    if ((state.pull || state.autoPull) && i != state.ready.end())
        i->second = answer == 1 ? 1 : 0;
}
bool UpdatePull(PartyBotAI &ai)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!Enabled(ai.me))
        return false;
    State &state = Get(ai.me);
    if (state.autoPull)
    {
        unsigned members = 0;
        bool consent = true;
        bool combat = false;
        auto stop = [&](char const *why) {
            state.autoPull = false;
            state.pull = 0;
            state.ready.clear();
            state.pullReason = why;
        };
        for (GroupReference *ref = ai.me->GetGroup()->GetFirstMember(); ref; ref = ref->next())
        {
            ++members;
            Player *member = ref->getSource();
            if (!member || !member->IsInWorld() || member->IsBeingTeleported() || !member->IsAlive() ||
                member->GetMap() != ai.me->GetMap() || !state.ready.count(member->GetGUID()))
            {
                stop("自主拉怪停止：成员死亡/离线/异图/变更");
                return false;
            }
            if (!member->IsBot() && member->GetDistance(state.anchorX, state.anchorY, state.anchorZ) > 45)
            {
                stop("自主拉怪停止：真人离开授权区域");
                return false;
            }
            if (state.ready[member->GetGUID()] == 0)
            {
                stop("自主拉怪停止：真人拒绝就位");
                return false;
            }
            if (!member->IsBot() && state.ready[member->GetGUID()] != 1)
                consent = false;
            if (member->IsInCombat())
                combat = true;
        }
        if (members != state.ready.size() || members != ai.me->GetGroup()->GetMembersCount())
        {
            stop("自主拉怪停止：编队变更");
            return false;
        }
        if (!consent)
        {
            if (uint32(WorldTimer::getMSTime() - state.pullAt) > 30000)
                stop("自主拉怪停止：真人确认超时");
            return false;
        }
        state.autoAuthorized = true;
        if (combat)
        {
            if (state.pull)
                stop("自主拉怪停止：拉怪前出现额外战斗");
            return false;
        }
        auto tanks = Tanks(ai.me);
        if (tanks.empty())
        {
            stop("自主拉怪停止：没有可用坦克");
            return false;
        }
        if (tanks.front() != ai.me)
            return false;
        // Attack() returns false for an already active melee target. Do not
        // select and issue that same pull again while the tank is walking in.
        if(!state.pull)
            if(Unit* approaching=ai.me->GetVictim())
            {
                auto planned=state.plannedOwners.find(approaching->GetGUID());
                if(approaching->IsAlive() && !approaching->IsInCombat() && planned!=state.plannedOwners.end() &&
                   uint32(WorldTimer::getMSTime()-planned->second)<20000)
                {
                    if(approaching->GetDistance(state.anchorX,state.anchorY,state.anchorZ)>45)
                        stop("自主拉怪停止：既有接近目标离开授权区域");
                    else state.pullReason="拉怪已发起：主坦正在接近目标";
                    return false;
                }
            }
        if (!state.pull)
        {
            std::list<Unit *> candidates;
            ai.me->GetEnemyListInRadiusAround(ai.me, 30, candidates);
            Unit *chosen = nullptr;
            for (Unit *u : candidates)
                if (u->GetEntry() == 11671 && u->IsAlive() && !u->IsInCombat() &&
                    RaidCoordinationPolicy::InsideAutoRegion(
                        u->GetDistance(state.anchorX, state.anchorY, state.anchorZ), ai.me->GetDistance(u)) &&
                    Reach(ai.me, u))
                    if (!chosen || ai.me->GetDistance(u) < ai.me->GetDistance(chosen))
                        chosen = u;
            if (!chosen)
            {
                state.pullReason = "自主拉怪等待：授权区内无可拉恶犬";
                return false;
            }
            state.pull = chosen->GetGUID();
            state.pullAt = WorldTimer::getMSTime();
            state.botReplies.clear();
            state.pullReason = "自主拉怪等待：全员恢复";
        }
    }
    if (!state.pull)
        return false;
    auto cancel = [&]() {
        state.pull = 0;
        if (!state.autoPull)
            state.ready.clear();
    };
    auto unsafe = [&](char const *why) {
        state.autoPull = false;
        cancel();
        state.pullReason = why;
    };
    if (uint32(WorldTimer::getMSTime() - state.pullAt) > 30000)
    {
        unsafe("拉怪停止：就位确认超时");
        return false;
    }
    unsigned members = 0;
    bool ready = true;
    std::string blocked;
    for (GroupReference *r = ai.me->GetGroup()->GetFirstMember(); r; r = r->next())
    {
        ++members;
        Player *q = r->getSource();
        if (!q || !q->IsInWorld() || q->IsBeingTeleported() || !state.ready.count(q->GetGUID()) || !q->IsAlive() ||
            q->GetMap() != ai.me->GetMap() || q->IsInCombat())
        {
            cancel();
            return false;
        }
        if (state.ready[q->GetGUID()] == 0)
        {
            cancel();
            return false;
        }
        bool recovered = q->GetDistance(ai.me) <= 50 && !q->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                         !q->IsNonMeleeSpellCasted() && q->GetHealthPercent() >= 90 &&
                         (q->GetPowerType() != POWER_MANA || q->GetPowerPercent(POWER_MANA) >= 70);
        if (auto *actor = dynamic_cast<PartyBotAI *>(q->AI()))
        {
            recovered = recovered && !actor->m_isBuffing;
            if (q->GetClass() == CLASS_HUNTER)
                recovered = recovered && q->GetUInt32Value(PLAYER_AMMO_ID) && q->GetPet() && q->GetPet()->IsAlive();
            if (recovered && state.botReplies.insert(q->GetGUID()).second)
            {
                WorldPackets::Group::RaidReadyCheckFromClient answer;
                answer.state = uint8(1);
                q->GetSession()->HandleRaidReadyCheckOpcode(answer);
            }
        }
        if (state.ready[q->GetGUID()] != 1 || !recovered)
        {
            ready = false;
            if (blocked.empty())
            {
                std::ostringstream reason;
                reason << "等待就位：" << q->GetName() << " 确认=" << state.ready[q->GetGUID()]
                       << " 血=" << q->GetHealthPercent() << " 蓝=" << q->GetPowerPercent(POWER_MANA)
                       << " 距离=" << q->GetDistance(ai.me) << " 施法=" << q->IsNonMeleeSpellCasted();
                if (auto *actor = dynamic_cast<PartyBotAI *>(q->AI()))
                    reason << " 增益中=" << actor->m_isBuffing;
                if (q->GetClass() == CLASS_HUNTER)
                    reason << " 弹药=" << q->GetUInt32Value(PLAYER_AMMO_ID)
                           << " 活宠物=" << (q->GetPet() && q->GetPet()->IsAlive());
                blocked = reason.str();
            }
        }
    }
    if (members != state.ready.size() || members != ai.me->GetGroup()->GetMembersCount())
    {
        cancel();
        return false;
    }
    Creature *target = ai.me->GetMap()->GetCreature(ObjectGuid(state.pull));
    if (!target || !target->IsAlive() || target->IsInCombat())
    {
        unsafe("拉怪停止：目标已失效或进入额外战斗");
        return false;
    }
    if (!ready)
        state.pullReason = blocked;
    if (!ready || ai.GetRole() != ROLE_TANK)
        return false;
    auto tanks = Tanks(ai.me);
    if (tanks.empty() || tanks.front() != ai.me)
        return false;
    if (!Reach(ai.me, target))
    {
        unsafe("拉怪停止：主坦距离或视线不满足");
        return false;
    }
    PathInfo path(ai.me);
    path.calculate(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    if (path.getPathType() != PATHFIND_NORMAL)
    {
        unsafe("拉怪停止：未找到完整导航路径");
        return false;
    }
    AssignedTank(ai.me, target);
    std::list<Unit *> nearby;
    ai.me->GetEnemyListInRadiusAround(target, 100, nearby);
    std::set<uint64> pack{target->GetGUID()};
    if (target->GetEntry() == 11671)
    {
        std::vector<RaidCoordinationPolicy::HoundPoint> candidates{
            {target->GetGUID(), target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()}};
        for (Unit *enemy : nearby)
            if (enemy->GetMap() == target->GetMap() && enemy->GetEntry() == 11671 && enemy->IsAlive() &&
                !enemy->IsInCombat())
                candidates.push_back(
                    {enemy->GetGUID(), enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ()});
        auto hounds = RaidCoordinationPolicy::ConnectedHounds(candidates, target->GetGUID());
        pack.insert(hounds.begin(), hounds.end());
    }
    else
    {
        for (Unit *enemy : nearby)
            if (enemy->IsAlive() && !enemy->IsInCombat() &&
                RaidCoordinationPolicy::AuthorizedPack(target->GetEntry(), enemy->GetEntry(),
                                                       enemy->GetDistance(target)))
                pack.insert(enemy->GetGUID());
    }
    if (state.autoPull)
        for (Unit *enemy : nearby)
            if (pack.count(enemy->GetGUID()) && enemy->GetDistance(state.anchorX, state.anchorY, state.anchorZ) > 45)
            {
                unsafe("自主拉怪停止：同包怪物超出授权区域");
                return false;
            }
    if (tanks.size() < pack.size())
    {
        unsafe("拉怪停止：可用坦克不足以接住授权怪物包");
        return false;
    }
    for (Unit *enemy : nearby)
        if (pack.count(enemy->GetGUID()) && !AssignedTank(ai.me, enemy))
        {
            unsafe("拉怪停止：授权怪物无可达坦克");
            return false;
        }
    if (state.autoPull)
        for (auto const &point : path.getPath())
        {
            float dx = point.x - state.anchorX, dy = point.y - state.anchorY, dz = point.z - state.anchorZ;
            if (!RaidCoordinationPolicy::InsideAutoRegion(std::sqrt(dx * dx + dy * dy + dz * dz), 0))
            {
                unsafe("自主拉怪停止：路径超出授权区域");
                return false;
            }
        }
    for (auto const &point : path.getPath())
        for (Unit *other : nearby)
            if (!pack.count(other->GetGUID()) && !other->IsInCombat() && other->IsAlive() &&
                other->GetDistance(point.x, point.y, point.z) < 25)
            {
                unsafe("拉怪停止：路径可能引到额外怪物");
                return false;
            }
    cancel();
    bool started = ai.AttackStart(target);
    if (started)
    {
        uint32 now = WorldTimer::getMSTime();
        for (uint64 guid : pack)
            state.plannedOwners[guid] = now;
    }
    state.pullReason = started ? "拉怪已发起" : "拉怪未发起：原生攻击检查拒绝";
    if (!started)
        state.autoPull = false;
    return true;
}
bool AllowAreaDamage(PartyBotAI const &ai, SpellEntry const *spell, float x, float y, float z)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!Enabled(ai.me) || !BotDamagePolicy::IsDamage(spell))
        return true;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || ai.me->GetDistance(x, y, z) > 80)
        return false;
    float radius = 10.0f;
    for (unsigned i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (spell->EffectRadiusIndex[i])
            radius =
                std::max(radius, Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i])));
    // Scan around the caster, then filter at the original ground destination.
    // No temporary world object or dummy is needed to represent that point.
    std::list<Unit *> enemies;
    ai.me->GetEnemyListInRadiusAround(ai.me, ai.me->GetDistance(x, y, z) + radius + 5, enemies);
    for (Unit *enemy : enemies)
        if (enemy->IsWithinDist3d(x, y, z, radius + enemy->GetObjectBoundingRadius()) &&
            (!enemy->IsInCombat() || !AllowOne(ai, enemy, spell)))
            return false;
    return true;
}
bool TryMassTaunt(PartyBotAI &ai)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!Enabled(ai.me) || ai.GetRole() != ROLE_TANK)
        return false;
    State &state = Get(ai.me);
    uint32 now = WorldTimer::getMSTime();
    if (state.massTauntAt && uint32(now - state.massTauntAt) < 8000)
        return false;
    for (Player *tank : Tanks(ai.me))
    {
        auto *actor = dynamic_cast<PartyBotAI *>(tank->AI());
        if (!actor)
            continue;
        uint32 spellId = tank->GetClass() == CLASS_WARRIOR ? 1161 : tank->GetClass() == CLASS_DRUID ? 5209 : 0;
        if (!spellId || !tank->HasSpell(spellId))
            continue;
        SpellEntry const *spell = sSpellMgr.GetSpellEntry(spellId);
        if (!spell || !actor->CanTryToCastSpell(tank, spell))
            continue;
        std::list<Unit *> enemies;
        tank->GetEnemyListInRadiusAround(tank, 10.0f, enemies);
        unsigned leaks = 0;
        for (Unit *e : enemies)
        {
            if (!e->IsInCombat() || !e->GetVictim())
                continue;
            Player *owner = AssignedTank(tank, e);
            if (owner && e->GetVictim() != owner && e->GetVictim()->IsPlayer())
            {
                auto *targetAi = dynamic_cast<PartyBotAI *>(e->GetVictim()->ToPlayer()->AI());
                if (!targetAi || targetAi->GetRole() != ROLE_TANK)
                    ++leaks;
            }
        }
        if (leaks < 3)
            continue;
        if (tank != ai.me)
            return false;
        if (ai.DoCastSpell(tank, spell) == SPELL_CAST_OK)
        {
            state.massTauntAt = now;
            return true;
        }
        return false;
    }
    return false;
}
Player *HealingTank(PartyBotAI const &ai)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!Enabled(ai.me))
        return nullptr;
    auto tanks = Tanks(ai.me);
    if (tanks.empty())
        return nullptr;
    std::vector<Player *> healers;
    for (GroupReference *r = ai.me->GetGroup()->GetFirstMember(); r; r = r->next())
        if (Player *p = r->getSource())
        {
            auto *other = dynamic_cast<PartyBotAI *>(p->AI());
            if (other && other->GetRole() == ROLE_HEALER && p->GetMap() == ai.me->GetMap() && p->IsAlive())
                healers.push_back(p);
        }
    std::sort(healers.begin(), healers.end(), [](Player *a, Player *b) { return a->GetGUID() < b->GetGUID(); });
    for (unsigned n = 0; n < healers.size(); ++n)
        if (healers[n] == ai.me)
            return tanks[n % tanks.size()];
    return nullptr;
}
Unit *HealTarget(PartyBotAI const &ai, float self, float group)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    Player *p = ai.me;
    if (!Enabled(p) || ai.GetRole() != ROLE_HEALER)
        return nullptr;
    auto tanks = Tanks(p);
    std::vector<Player *> healers;
    for (GroupReference *r = p->GetGroup()->GetFirstMember(); r; r = r->next())
        if (Player *q = r->getSource())
        {
            auto *a = dynamic_cast<PartyBotAI *>(q->AI());
            if (q->GetMap() == p->GetMap() && q->IsAlive() && a && a->GetRole() == ROLE_HEALER)
                healers.push_back(q);
        }
    std::sort(healers.begin(), healers.end(), [](Player *a, Player *b) { return a->GetGUID() < b->GetGUID(); });
    unsigned index = 0;
    for (unsigned i = 0; i < healers.size(); ++i)
        if (healers[i] == p)
            index = i;
    for (Player *t : tanks)
        if (t->GetHealthPercent() < 25 && ai.IsValidHealTarget(t, group))
            return t;
    if (!tanks.empty())
    {
        Player *t = tanks[index % tanks.size()];
        if (ai.IsValidHealTarget(t, group))
        {
            uint32 incoming = 0;
            for (Player *healer : healers)
                if (healer != p)
                    if (Spell *cast = healer->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                        if (cast->getState() == SPELL_STATE_PREPARING && cast->m_targets.getUnitTarget() == t)
                            for (unsigned i = 0; i < 3; ++i)
                                if (cast->m_spellInfo->Effect[i] == SPELL_EFFECT_HEAL)
                                    incoming += std::max(0.0f, cast->CalculateDamage(SpellEffectIndex(i), t));
            if (incoming < t->GetMaxHealth() - t->GetHealth())
                return t;
        }
    }
    return nullptr;
}
std::string Status(Player *p)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!p || !p->GetGroup())
        return "团队协调：未组队";
    auto &s = Get(p);
    std::ostringstream o;
    o << "团队协调=" << (Enabled(p) ? "开启" : "关闭/非副本") << " 地图=" << p->GetMapId()
      << " 实例=" << p->GetInstanceId() << " 目标分配=" << s.owners.size() << " 可用坦克=" << Tanks(p).size()
      << " 自主拉怪=" << (s.autoPull ? "开启" : "关闭") << " 授权中心=" << s.anchorX << "," << s.anchorY << ","
      << s.anchorZ << " 授权半径=45" << " 待开怪=" << s.pull << " 确认名册=" << s.ready.size()
      << " 开怪状态=" << s.pullReason;
    for (auto row : s.owners)
        o << " [目标=" << row.first << " 坦克=" << row.second << "]";
    for (auto row : s.throttled)
        if (row.second)
            o << " [停手角色=" << row.first.first << " 目标=" << row.first.second << "]";
    return o.str();
}
} // namespace RaidCoordination
