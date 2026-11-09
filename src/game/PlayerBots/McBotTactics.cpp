// Original 1.12 encounter policies; no AzerothCore implementation copied.
#include "McBotTactics.h"
#include "BotDamagePolicy.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Map.h"
#include "McBotPolicy.h"
#include "MotionMaster.h"
#include "PartyBotAI.h"
#include "PathFinder.h"
#include "Pet.h"
#include "Player.h"
#include "RaidCoordination.h"
#include "Spell.h"
#include "Timer.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>

namespace McBotTactics
{
namespace
{
using McBotPolicy::Point;
constexpr uint32 MapMc = 409, Geddon = 12056, Golemagg = 11988, Hound = 11671, Rager = 11672, Garr = 12057, Majordomo = 12018;
constexpr uint32 Bomb = 20475, Inferno = 19695;
struct State
{
    uint32 seen = 0, planned = 0, started = 0, progress = 0;
    unsigned mechanic = 0, failures = 0;
    Point goal, last;
    bool goalValid = false, issued = false;
    std::string reason = "常规战斗";
};
using Key = std::pair<uint32, uint64>;
std::map<Key, State> states;
struct Sight
{
    uint32 seen = 0;
    std::vector<uint64> enemies;
};
std::map<Key, Sight> sightings;
std::mutex stateMutex;
Point Pos(Unit const *u) { return {u->GetPositionX(), u->GetPositionY(), u->GetPositionZ()}; }
bool Active(PartyBotAI const &ai)
{
    return ai.me && ai.me->IsInWorld() && ai.me->GetMapId() == MapMc && !ai.me->InBattleGround() &&
           RaidCoordination::Enabled(ai.me);
}
std::list<Unit *> Nearby(PartyBotAI const &ai, Unit const *around, float radius)
{
    uint32 now = WorldTimer::getMSTime();
    Key key{ai.me->GetInstanceId(), ai.me->GetGUID()};
    Sight sight;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto it = sightings.find(key);
        if (it != sightings.end())
            sight = it->second;
    }
    if (!sight.seen || uint32(now - sight.seen) >= 1000)
    {
        std::list<Unit *> scan;
        ai.me->GetEnemyListInRadiusAround(ai.me, 110, scan);
        sight.seen = now;
        sight.enemies.clear();
        for (Unit *u : scan)
            if (u)
                sight.enemies.push_back(u->GetGUID());
        std::lock_guard<std::mutex> lock(stateMutex);
        sightings[key] = sight;
        if (sightings.size() > 1024)
            for (auto it = sightings.begin(); it != sightings.end();)
                if (uint32(now - it->second.seen) > 120000)
                    it = sightings.erase(it);
                else
                    ++it;
    }
    std::list<Unit *> out;
    for (uint64 guid : sight.enemies)
        if (Unit *u = ai.me->GetMap()->GetUnit(ObjectGuid(guid)))
            if (u->IsInWorld() && u->IsWithinDistInMap(around, radius))
                out.push_back(u);
    return out;
}
bool PartyEngaged(Player *actor, Unit *enemy)
{
    if (!enemy || !actor->GetGroup() || !enemy->IsInCombat())
        return false;
    if (Unit *victim = enemy->GetVictim())
    {
        if (victim == actor)
            return true;
        if (Player *owner = victim->GetAffectingPlayer())
            if (actor->IsInSameGroupWith(owner))
                return true;
    }
    for (GroupReference *it = actor->GetGroup()->GetFirstMember(); it; it = it->next())
        if (Player *p = it->getSource())
            if (p->GetMap() == actor->GetMap() && enemy->CanHaveThreatList() &&
                enemy->GetThreatManager().getThreat(p) > 0)
                return true;
    return false;
}
Unit *Encounter(PartyBotAI const &ai, unsigned entry)
{
    for (Unit *u : Nearby(ai, ai.me, 100))
        if (u && u->GetEntry() == entry && u->IsAlive() && PartyEngaged(ai.me, u))
            return u;
    return nullptr;
}
bool Damage(SpellEntry const *spell) { return BotDamagePolicy::IsDamage(spell); }
std::vector<Point> OtherMembers(PartyBotAI const &ai)
{
    std::vector<Point> members;
    for (GroupReference *it = ai.me->GetGroup()->GetFirstMember(); it; it = it->next())
        if (Player *p = it->getSource())
            if (p != ai.me && p->IsAlive() && p->IsInWorld() && p->GetMap() == ai.me->GetMap())
                members.push_back(Pos(p));
    return members;
}
bool Coverage(PartyBotAI const &ai, Point goal, Unit const *cover)
{
    return !cover ||
           (cover->IsAlive() && cover->GetMap() == ai.me->GetMap() && McBotPolicy::Distance(goal, Pos(cover)) <= 28 &&
            ai.me->GetMap()->isInLineOfSight(goal.x, goal.y, goal.z + 2, cover->GetPositionX(), cover->GetPositionY(),
                                             cover->GetPositionZ() + 2));
}
bool SafePath(PartyBotAI const &ai, Point goal, std::vector<Point> const &avoid, float separation,
              Unit const *cover = nullptr)
{
    if (!McBotPolicy::Finite(goal) || std::fabs(goal.z - ai.me->GetPositionZ()) > 12 ||
        !McBotPolicy::BombSafe(goal, avoid, separation) || !Coverage(ai, goal, cover))
        return false;
    PathInfo path(ai.me);
    path.ExcludeSteepSlopes();
    path.setPathLengthLimit(75);
    if (!path.calculate(goal.x, goal.y, goal.z) || path.getPathType() != PATHFIND_NORMAL || path.Length() > 70)
        return false;
    auto const end = path.getActualEndPosition();
    if (std::hypot(end.x - goal.x, end.y - goal.y) > 2 || std::fabs(end.z - goal.z) > 3)
        return false;
    // Refuse paths through an unengaged pack rather than solving a mechanic by
    // ADDing.
    auto enemies = Nearby(ai, ai.me, 95);
    for (auto const &point : path.getPath())
        for (Unit *enemy : enemies)
            if (enemy && enemy->IsAlive() && !PartyEngaged(ai.me, enemy) &&
                std::hypot(enemy->GetPositionX() - point.x, enemy->GetPositionY() - point.y) < 22 &&
                std::fabs(enemy->GetPositionZ() - point.z) < 8)
                return false;
    return true;
}
bool ChooseGoal(PartyBotAI const &ai, Point center, float radius, std::vector<Point> const &avoid, float separation,
                Point &out, Unit const *cover)
{
    float best = 1e9;
    float base = std::atan2(ai.me->GetPositionY() - center.y, ai.me->GetPositionX() - center.x);
    // Prefer the short escape in the actor's existing direction; only pathfind
    // plausible points, and keep a selected goal stable across ticks.
    for (unsigned i = 0; i < 16; ++i)
    {
        float angle = base + (i % 2 ? 1 : -1) * float((i + 1) / 2) * float(M_PI) / 8;
        Point p{center.x + radius * std::cos(angle), center.y + radius * std::sin(angle), ai.me->GetPositionZ()};
        ai.me->UpdateGroundPositionZ(p.x, p.y, p.z);
        float cost = McBotPolicy::Distance(Pos(ai.me), p);
        if (cost < best && SafePath(ai, p, avoid, separation, cover))
        {
            best = cost;
            out = p;
        }
    }
    return best < 1e9;
}
bool Move(PartyBotAI &ai, State &s, Point center, float radius, std::vector<Point> const &avoid, float separation,
          uint32 now, Unit const *cover = nullptr)
{
    Point here = Pos(ai.me);
    if (s.goalValid && (!McBotPolicy::BombSafe(s.goal, avoid, separation) || std::fabs(here.z - s.goal.z) > 12 ||
                        !Coverage(ai, s.goal, cover)))
        s.goalValid = false;
    if (s.goalValid && s.mechanic != 1 && McBotPolicy::Distance(s.goal, center) < radius - 2)
        s.goalValid = false;
    if (s.goalValid && McBotPolicy::Distance(here, s.goal) > 2 && uint32(now - s.planned) >= 2000)
    {
        s.planned = now;
        if (!SafePath(ai, s.goal, avoid, separation, cover))
            s.goalValid = false;
    }
    if (s.goalValid && McBotPolicy::Distance(here, s.last) > 1)
    {
        s.last = here;
        s.progress = now;
    }
    if (s.goalValid && uint32(now - s.progress) > 6000 && McBotPolicy::Distance(here, s.goal) > 2)
    {
        s.goalValid = false;
        ++s.failures;
        s.reason = "机制移动停滞，重新规划";
    }
    if (!s.goalValid && (!s.planned || uint32(now - s.planned) >= 2000))
    {
        s.planned = now;
        s.goalValid = ChooseGoal(ai, center, radius, avoid, separation, s.goal, cover);
        s.issued = false;
        s.last = here;
        s.progress = now;
        if (!s.goalValid)
        {
            ++s.failures;
            s.reason = "无安全机制路径，停止普通追击";
        }
    }
    if (s.mechanic != 3)
    {
        ai.me->AttackStop();
        if (Pet *pet = ai.me->GetPet())
            pet->AttackStop();
    }
    if (!s.goalValid || McBotPolicy::Distance(here, s.goal) > 2)
        ai.me->InterruptNonMeleeSpells(false);
    if (s.goalValid && McBotPolicy::Distance(here, s.goal) > 2)
    {
        ai.me->InterruptNonMeleeSpells(false);
        if (!s.issued || ai.me->IsStopped() ||
            ai.me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
        {
            ai.me->GetMotionMaster()->MovePoint(0x4D43, s.goal.x, s.goal.y, s.goal.z,
                                                MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
            s.issued = true;
        }
    }
    else
    {
        ai.me->StopMoving();
        if (!s.goalValid && ai.me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            ai.me->GetMotionMaster()->Clear(false, true);
            ai.me->GetMotionMaster()->MoveIdle();
        }
        // A healer at a safe destination may continue lawful healing; returning
        // true still prevents generic follow/chase from overriding that position.
        if (s.goalValid && ai.GetRole() == ROLE_HEALER && !ai.me->IsNonMeleeSpellCasted())
            ai.FindAndHealInjuredAlly();
        if (s.goalValid && s.mechanic == 3 && !ai.me->IsNonMeleeSpellCasted())
            ai.UpdateInCombatAI();
    }
    return true;
}
} // namespace

bool WantsAoe(PartyBotAI const &ai, Unit const *target)
{
    if (!Active(ai) || !target)
        return false;
    std::vector<McBotPolicy::Enemy> enemies;
    for (Unit *u : Nearby(ai, target, 10))
        if (u && u->IsAlive())
        {
            bool controlled = u->HasAuraType(SPELL_AURA_MOD_CONFUSE) || u->HasAuraType(SPELL_AURA_MOD_FEAR);
            for (auto mark : ai.m_marksToCC)
                if (ai.GetMarkedTarget(mark) == u)
                    controlled = true;
            enemies.push_back({u->GetGUID(), u->GetEntry(), u->GetHealthPercent(), controlled, PartyEngaged(ai.me, u),
                               u->GetHealth() == 1 && u->GetStandState() == UNIT_STAND_STATE_DEAD});
        }
    bool forbidden =
        Encounter(ai, Garr) || Encounter(ai, Majordomo) || Encounter(ai, Golemagg);
    return McBotPolicy::Aoe(enemies, forbidden);
}

bool AllowAttack(PartyBotAI const &ai, Unit *target)
{
    if (!Active(ai) || !target)
        return true;
    if (!McBotPolicy::GolemaggDamageAllowed(target->GetEntry(), Encounter(ai, Golemagg) != nullptr,
                                            ai.GetRole() == ROLE_TANK))
        return false;
    if (ai.me->HasAura(Bomb))
        return false;
    if (Unit *geddon = Encounter(ai, Geddon))
        if (geddon->HasAura(Inferno) &&
            (ai.GetRole() == ROLE_TANK || ai.GetRole() == ROLE_MELEE_DPS || ai.me->GetDistance(geddon) < 24))
            return false;
    return true;
}

bool AllowSpell(PartyBotAI const &ai, Unit const *target, SpellEntry const *spell)
{
    if (!Active(ai) || !spell)
        return true;
    if (Damage(spell))
    {
        if (ai.me->HasAura(Bomb))
            return false;
        if (Unit *boss = Encounter(ai, Geddon))
            if (boss->HasAura(Inferno) &&
                (ai.GetRole() == ROLE_TANK || ai.GetRole() == ROLE_MELEE_DPS || ai.me->GetDistance(boss) < 24))
                return false;
        if (target && !McBotPolicy::GolemaggDamageAllowed(target->GetEntry(), Encounter(ai, Golemagg) != nullptr,
                                                          ai.GetRole() == ROLE_TANK))
            return false;
    }
    if (Damage(spell) && spell->IsAreaOfEffectSpell() && ai.GetRole() != ROLE_TANK)
    {
        if (Encounter(ai, Golemagg) || Encounter(ai, Garr) || Encounter(ai, Majordomo))
            return false;
        // The pack policy overrides generic "three enemies" for mixed packs.
        if (!WantsAoe(ai, target ? target : ai.me))
            return false;
    }
    return true;
}

bool Update(PartyBotAI &ai)
{
    if (!Active(ai) || !ai.me->IsAlive() || !ai.me->GetGroup())
        return false;
    uint32 now = WorldTimer::getMSTime();
    Key key{ai.me->GetInstanceId(), ai.me->GetGUID()};
    State s;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        s = states[key];
    }
    unsigned previous = s.mechanic, mechanic = 0;
    Unit *danger = nullptr;
    Player *healingTank = ai.GetRole() == ROLE_HEALER ? RaidCoordination::HealingTank(ai) : nullptr;
    if (ai.me->HasAura(Bomb))
        mechanic = 1;
    else if ((danger = Encounter(ai, Geddon)) && danger->HasAura(Inferno) &&
             (ai.me->GetDistance(danger) < 24 || previous == 2))
        mechanic = 2;
    else if (ai.GetRole() == ROLE_TANK && ai.me->GetVictim() && ai.me->GetVictim()->GetEntry() == Rager &&
             ai.me->GetVictim()->GetVictim() == ai.me && (danger = Encounter(ai, Golemagg)) &&
             (ai.me->GetVictim()->GetDistance(danger) < 35 || previous == 3) && danger->GetHealthPercent() > 10)
        mechanic = 3;
    else if (ai.GetRole() == ROLE_HEALER && (danger = Encounter(ai, Golemagg)) &&
             (previous == 4 || ai.me->GetDistance(danger) < 25 ||
              (healingTank && (!ai.me->IsWithinDistInMap(healingTank, 27) || !ai.me->IsWithinLOSInMap(healingTank)))))
        mechanic = 4;
    if (s.mechanic != mechanic)
    {
        s.goalValid = false;
        s.planned = 0;
        s.started = now;
        s.progress = now;
        s.mechanic = mechanic;
    }
    if (previous && previous != mechanic &&
        ai.me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
    {
        ai.me->GetMotionMaster()->Clear(false, true);
        ai.me->GetMotionMaster()->MoveIdle();
    }
    bool occupied = false;
    if (mechanic)
    {
        Point center;
        float radius = 0, separation = 0;
        std::vector<Point> avoid;
        if (mechanic == 1)
        {
            avoid = OtherMembers(ai);
            center = Pos(ai.me);
            if (!avoid.empty())
            {
                center = {};
                for (auto const &p : avoid)
                {
                    center.x += p.x;
                    center.y += p.y;
                    center.z += p.z;
                }
                center.x /= avoid.size();
                center.y /= avoid.size();
                center.z /= avoid.size();
            }
            radius = 30;
            separation = 22;
            s.reason = "活体炸弹离群";
        }
        else
        {
            center = Pos(danger);
            radius = mechanic == 2 ? 24 : mechanic == 3 ? 43 : 33;
            s.reason = mechanic == 2 ? "地狱火避让" : mechanic == 3 ? "副坦拉离焚化者增益范围" : "焚化者治疗安全站位";
        }
        if (mechanic == 4 && healingTank)
        {
            center = Pos(healingTank);
            radius = 27;
            avoid.push_back(Pos(danger));
            separation = 22;
        }
        occupied = Move(ai, s, center, radius, avoid, separation, now, mechanic == 4 ? healingTank : nullptr);
    }
    else
    {
        if (s.goalValid && ai.me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            ai.me->GetMotionMaster()->Clear(false, true);
            ai.me->GetMotionMaster()->MoveIdle();
        }
        s.goalValid = false;
        s.reason = "常规战斗";
        if (ai.GetRole() != ROLE_HEALER && ai.GetRole() != ROLE_TANK && ai.me->IsInCombat())
        {
            auto nearby = Nearby(ai, ai.me, 60);
            std::vector<McBotPolicy::Enemy> pack;
            for (Unit *u : nearby)
                if (u && u->GetEntry() == Hound && u->IsAlive() && PartyEngaged(ai.me, u))
                    pack.push_back({u->GetGUID(), Hound, u->GetHealthPercent(), false, true,
                                    u->GetHealth() == 1 && u->GetStandState() == UNIT_STAND_STATE_DEAD});
            uint64 focus = McBotPolicy::HoundFocus(pack);
            Unit *highest = nullptr;
            for (Unit *u : nearby)
                if (u->GetGUID() == focus)
                    highest = u;
            if (highest && (!ai.me->GetVictim() || ai.me->GetVictim()->GetEntry() == Hound) &&
                highest != ai.me->GetVictim())
                ai.AttackStart(highest);
            if (Unit *boss = Encounter(ai, Golemagg))
            {
                if (ai.me->GetVictim() && ai.me->GetVictim()->GetEntry() == Rager)
                    ai.AttackStart(boss);
                if (Pet *pet = ai.me->GetPet())
                    if (pet->IsAlive() && pet->GetVictim() && pet->GetVictim()->GetEntry() == Rager)
                    {
                        pet->AttackStop();
                        if (pet->GetCharmInfo())
                            pet->GetCharmInfo()->SetIsCommandAttack(true);
                        pet->AI()->AttackStart(boss);
                    }
            }
        }
    }
    s.seen = now;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        states[key] = s;
        if (states.size() > 1024)
            for (auto it = states.begin(); it != states.end();)
                if (uint32(now - it->second.seen) > 120000)
                    it = states.erase(it);
                else
                    ++it;
    }
    return occupied;
}

std::string Status(PartyBotAI const &ai)
{
    if (!ai.me)
        return "mc=不可用";
    std::lock_guard<std::mutex> lock(stateMutex);
    auto it = states.find({ai.me->GetInstanceId(), ai.me->GetGUID()});
    if (it == states.end())
        return "mc=未激活";
    std::ostringstream out;
    char const *labels[] = {"常规战斗", "活体炸弹离群", "地狱火避让", "副坦拉离焚化者", "焚化者治疗站位"};
    out << "mc=" << labels[it->second.mechanic < 5 ? it->second.mechanic : 0] << " reason=" << it->second.reason
        << " pathFailures=" << it->second.failures << " pos=" << ai.me->GetPositionX() << "," << ai.me->GetPositionY()
        << "," << ai.me->GetPositionZ();
    if (it->second.goalValid)
        out << " goal=" << it->second.goal.x << "," << it->second.goal.y << "," << it->second.goal.z;
    Spell* active=ai.me->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if(!active)active=ai.me->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    out<<" role="<<unsigned(ai.GetRole())<<" victim="<<(ai.me->GetVictim()?ai.me->GetVictim()->GetGUID():0)
       <<" spell="<<(active?active->m_spellInfo->Id:0);
    return out.str();
}
} // namespace McBotTactics
