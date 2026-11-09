/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "BattleBotAI.h"
#include "AvRouteRecoveryPolicy.h"
#include "BotMountPolicy.h"
#include "PvPCombatPolicy.h"
#include "BattleBotWaypoints.h"
#include "BattleGround.h"
#include "Player.h"
#include "Group.h"
#include "CreatureAI.h"
#include "Log.h"
#include "MotionMaster.h"
#include "PointMovementGenerator.h"
#include "ObjectMgr.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "World.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "Utilities/Random.h"
#include "TargetedMovementGenerator.h"
#include "BattleGroundAB.h"
#include "PathFinder.h"
#include "GridMap.h"
#include <iomanip>
#include <sstream>
#include "MoveSpline.h"
#include "BattleGroundWS.h"
#include "BattleGroundMgr.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include <chrono>
#include <mutex>
#include <limits>

enum BattleBotSpells
{
    BB_SPELL_FOOD = 1131,
    BB_SPELL_DRINK = 1137,
    BB_SPELL_AUTO_SHOT = 75,
    BB_SPELL_SHOOT_WAND = 5019,

    BB_SPELL_MOUNT_40_HUMAN = 470,
    BB_SPELL_MOUNT_40_NELF = 10787,
    BB_SPELL_MOUNT_40_DWARF = 6896,
    BB_SPELL_MOUNT_40_GNOME = 17456,
    BB_SPELL_MOUNT_40_TROLL = 10795,
    BB_SPELL_MOUNT_40_ORC = 581,
    BB_SPELL_MOUNT_40_TAUREN = 18363,
    BB_SPELL_MOUNT_40_UNDEAD = 8980,

    BB_SPELL_MOUNT_60_HUMAN = 22717,
    BB_SPELL_MOUNT_60_NELF = 22723,
    BB_SPELL_MOUNT_60_DWARF = 22720,
    BB_SPELL_MOUNT_60_GNOME = 22719,
    BB_SPELL_MOUNT_60_TROLL = 22721,
    BB_SPELL_MOUNT_60_ORC = 22724,
    BB_SPELL_MOUNT_60_TAUREN = 22718,
    BB_SPELL_MOUNT_60_UNDEAD = 22722,

    BB_SPELL_MOUNT_40_PALADIN = 13819,
    BB_SPELL_MOUNT_60_PALADIN = 23214,

    BB_SPELL_MOUNT_40_WARLOCK = 5784,
    BB_SPELL_MOUNT_60_WARLOCK = 23161,

    BB_ITEM_ARROW  = 2512,
    BB_ITEM_BULLET = 2516,
};

#define BB_UPDATE_INTERVAL 1000

#define GO_WSG_DROPPED_SILVERWING_FLAG 179785
#define GO_WSG_DROPPED_WARSONG_FLAG 179786
#define GO_WSG_SILVERWING_FLAG 179830
#define GO_WSG_WARSONG_FLAG 179831

uint32 BattleBotAI::GetMountSpellId() const
{
    return BotMountPolicy::Select(me->GetGUIDLow(),me->GetRace(),me->GetClass(),me->GetLevel());
}

bool BattleBotAI::AllowLegacyFormExit() const
{
    for(auto* aura:me->GetAurasByType(SPELL_AURA_MOD_SHAPESHIFT))
        if(!BattleBotPolicyRuntime::CanLegacy(*this,aura->GetSpellProto()))return false;
    return true;
}
bool BattleBotAI::AllowLegacySpell(SpellEntry const* spell) const
{
    bool allowed=BattleBotPolicyRuntime::CanLegacy(*this,spell);
    if(allowed && spell && m_initialized && me && me->InBattleGround()){
        std::lock_guard<std::recursive_mutex> guard(m_policy.mutex);
        m_policy.legacy="legacy shared-helper spell="+std::to_string(spell->Id);
    }
    return allowed;
}
bool BattleBotAI::CanTryToCastSpell(Unit const* target,SpellEntry const* spell) const
{
    if(!BattleBotPolicyRuntime::CanLegacy(*this,spell))return false;
    // Unmanaged utility preserves every native legacy guard, including duplicate
    // aura prevention. Managed casts never pass this path or its base-range limit.
    bool result=CombatBotBaseAI::CanTryToCastSpell(target,spell);
    if(result&&m_preparationUpdating)m_preparationTriedSpell=true;
    return result;
}

SpellCastResult BattleBotAI::PolicyAwareCast(Unit* target,uint32 id,bool triggered)
{return PolicyAwareCast(target,sSpellMgr.GetSpellEntry(id),triggered);}
SpellCastResult BattleBotAI::PolicyAwareCast(Unit* target,SpellEntry const* spell,bool triggered)
{
    if(!spell || !BattleBotPolicyRuntime::CanLegacy(*this,spell))return SPELL_FAILED_DONT_REPORT;
    if(m_initialized && me->InBattleGround()){
        std::lock_guard<std::recursive_mutex> guard(m_policy.mutex);
        m_policy.legacy="legacy spell="+std::to_string(spell->Id);
    }
    return me->CastSpell(target,spell,triggered);
}
bool BattleBotAI::UseMount(bool preparation)
{
    ++m_mountAttempts;m_mountPreparation=preparation;m_mountCastResult=-1;
    auto reject=[&](char const* reason){m_mountReason=reason;return false;};
    if(me->IsMounted())return reject("mounted");
    if(me->IsInCombat())return reject("combat");
    if(me->IsNonMeleeSpellCasted())return reject("casting");
    if(me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL|UNIT_STATE_CAN_NOT_MOVE))return reject("control");
    if(me->HasAuraType(SPELL_AURA_MOD_STEALTH))return reject("stealth");
    BattleGround* bg=me->GetBattleGround();
    if(!bg || (bg->GetStatus()!=STATUS_IN_PROGRESS && !(preparation&&bg->GetStatus()==STATUS_WAIT_JOIN)) || me->HasAura(AURA_WARSONG_FLAG) || me->HasAura(AURA_SILVERWING_FLAG))return reject("scene_or_flag");
    if(!me->GetTerrain()->IsOutdoors(me->GetPositionX(),me->GetPositionY(),me->GetPositionZ()))return reject("indoors");
    // Conservative native-water preflight: keep the egress stream running.
    if(me->IsSwimming()||me->IsInWater()||me->GetTerrain()->IsInWater(me->GetPositionX(),me->GetPositionY(),me->GetPositionZ()))return reject("water");
    std::list<Unit*> close;me->GetEnemyListInRadiusAround(me,20.0f,close);
    for(Unit* enemy:close)if(enemy->IsAlive() && me->IsValidAttackTarget(enemy) && me->IsWithinLOSInMap(enemy))return reject("enemy_near");
    Position goal;
    if(preparation&&bg->GetTypeID()==BATTLEGROUND_AB){
        static Position const nodes[5]={{1169.52f,1198.71f,-56.2742f,0},{978.122f,1043.87f,-44.4682f,0},{803.826f,874.909f,-55.2547f,0},{857.674f,1146.16f,11.529f,0},{1145.14f,849.895f,-110.523f,0}};
        if(m_abOpeningNode<0)return reject("no_opening");goal=nodes[m_abOpeningNode];
    }
    else if(bg->GetTypeID()==BATTLEGROUND_WS && m_wsgGoalValid)goal=m_wsgGoal;
    else if(bg->GetTypeID()==BATTLEGROUND_AB && m_arathiGuardAnchorValid)goal=m_arathiGuardAnchor;
    else if(bg->GetTypeID()==BATTLEGROUND_AB && m_arathiAdvanceValid)goal=m_arathiAdvanceGoal;
    else if(m_currentPath && !m_currentPath->empty()){
        auto const& end=m_movingInReverse?m_currentPath->front():m_currentPath->back();goal=Position{end.x,end.y,end.z,0};
    }else return reject("no_route"); // No known route: do not invent a travel distance.
    PathInfo route(me);route.calculate(goal.x,goal.y,goal.z);
    float travel=route.getPathType()==PATHFIND_NORMAL?route.Length():preparation?me->GetDistance(goal):0;
    if(travel<=3)return reject("short_route");
    SpellEntry const* spell=sSpellMgr.GetSpellEntry(GetMountSpellId());if(!spell)return reject("no_mount_spell");
    if(!BattleBotPolicyRuntime::CanLegacy(*this,spell))return reject("policy_gate");
    float increase=0;
    for(unsigned i=0;i<3;++i)if(spell->EffectApplyAuraName[i]==SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)increase=std::max(increase,float(spell->EffectBasePoints[i]+1));
    if(increase<=0)return reject("no_speed_aura");
    float bonus=std::max(me->GetTotalAuraMultiplier(SPELL_AURA_MOD_MOUNTED_SPEED_ALWAYS),(100.0f+me->GetMaxPositiveAuraModifier(SPELL_AURA_MOD_MOUNTED_SPEED_NOT_STACK))/100.0f);
    float base=me->GetSpeed(MOVE_RUN)/std::max(0.01f,me->GetSpeedRate(MOVE_RUN));
    float mounted=base*bonus*(100.0f+increase)/100.0f;
    if(int32 cap=me->GetMaxPositiveAuraModifier(SPELL_AURA_USE_NORMAL_MOVEMENT_SPEED))mounted=std::min(mounted,float(cap));
    mounted*=(100.0f+me->GetMaxNegativeAuraModifier(SPELL_AURA_MOD_DECREASE_SPEED))/100.0f;
        uint32 castTime=BattleBotPolicyRuntime::ActualCastTime(me,spell);
    if(preparation&&bg->GetStartDelayTime()<=int32(castTime+1000))return reject("late_cast");
    // During preparation the cast is paid by otherwise idle waiting time. If
    // closed gates prevent a full route query, direct distance is a conservative
    // lower bound for the later journey; no move is issued along that estimate.
    if(!BattleBotTactics::MountSavesTime(travel,me->GetSpeed(MOVE_RUN),mounted,preparation?0:castTime/1000.0f))return reject("no_time_gain");
    // Read-only native readiness/cost checks precede all movement/form changes.
    if(!me->IsSpellReady(spell))return reject("cooldown");
    if(me->HasGCD(spell))return reject("gcd");
    m_mountCost=Spell::CalculatePowerCost(spell,me,nullptr,nullptr,false);
    if(spell->powerType==POWER_HEALTH?me->GetHealth()<=m_mountCost:me->GetPower(Powers(spell->powerType))<m_mountCost)return reject("resource");
    // Stop a healthy movement stream only for a worthwhile legal mount cast.
    StopMoving();
    if(me->IsInDisallowedMountForm())me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
    m_mountCastResult=int32(PolicyAwareCast(me,spell->Id,false));
    m_mountReason=m_mountCastResult==SPELL_CAST_OK?"cast_ok":"native_reject";
    return m_mountCastResult==SPELL_CAST_OK;
}

bool BattleBotAI::DrinkAndEat()
{
    // Never let a recovery caller seize a live native capture cast.
    if(Spell* opening=me->GetCurrentSpell(CURRENT_GENERIC_SPELL))if((opening->m_spellInfo->Id==21651||opening->m_spellInfo->Id==23932))return true;
    if(Spell* opening=me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))if((opening->m_spellInfo->Id==21651||opening->m_spellInfo->Id==23932))return true;

    if(me->GetCurrentSpell(CURRENT_GENERIC_SPELL)||me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))return true;
    if(me->IsInCombat()||me->IsInWater())return false;

    if (m_isBuffing)
        return false;

    if (me->IsMounted())
        return false;

    if (me->GetVictim())
        return false;

    BattleGround* bg;
    bool const needToEat = me->GetHealthPercent() < 100.0f && !((bg = me->GetBattleGround()) && bg->GetStatus() == STATUS_WAIT_JOIN);
    bool const needToDrink = (me->GetPowerType() == POWER_MANA) && (me->GetPowerPercent(POWER_MANA) < 100.0f);

    if (!needToEat && !needToDrink)
        return false;

    // Do not stop while carrying flag.
    if (me->HasAura(AURA_WARSONG_FLAG) ||
        me->HasAura(AURA_SILVERWING_FLAG))
        return false;

    bool const isEating = me->HasAura(BB_SPELL_FOOD);
    bool const isDrinking = me->HasAura(BB_SPELL_DRINK);

    if (!isEating && needToEat)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            ClearPath();
            StopMoving();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(BB_SPELL_FOOD))
        {
            PolicyAwareCast(me, pSpellEntry, true);
            me->RemoveSpellCooldown(pSpellEntry);
        }
        return true;
    }

    if (!isDrinking && needToDrink)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            ClearPath();
            StopMoving();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(BB_SPELL_DRINK))
        {
            PolicyAwareCast(me, pSpellEntry, true);
            me->RemoveSpellCooldown(pSpellEntry);
        }
        return true;
    }

    return needToEat || needToDrink;
}

float BattleBotAI::GetMaxAggroDistanceForMap() const
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AV)
        return 50.0f;

    return 30.0f;
}

bool BattleBotAI::AttackStart(Unit* pVictim)
{
    m_isBuffing = false;

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    if (me->Attack(pVictim, true))
    {
        ClearPath();
        BeginChasing(pVictim);
        return true;
    }

    return false;
}

extern BattleBotPath vPath_WSG_HordeFlagRoom_to_HordeGraveyard;
extern BattleBotPath vPath_WSG_AllianceFlagRoom_to_AllianceGraveyard;
namespace
{
std::uint64_t TacticalNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool HasWsgFlag(Unit const* player) { return player->HasAura(AURA_WARSONG_FLAG) || player->HasAura(AURA_SILVERWING_FLAG); }
bool Threatens(Unit const* enemy,Player const* friendly)
{
    if(enemy->GetVictim()==friendly || friendly->GetAttackers().count(const_cast<Unit*>(enemy)))return true;
    for(CurrentSpellTypes type:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL,CURRENT_AUTOREPEAT_SPELL})
        if(Spell* spell=enemy->GetCurrentSpell(type))
            if(!spell->m_spellInfo->IsPositiveSpell() && spell->m_targets.getUnitTargetGuid()==friendly->GetObjectGuid())return true;
    return false;
}
void StopTacticalAttack(Player* player)
{
    if(Unit* victim=player->GetVictim())
        for(CurrentSpellTypes type:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL,CURRENT_AUTOREPEAT_SPELL})
            if(Spell* spell=player->GetCurrentSpell(type))
                if(!spell->m_spellInfo->IsPositiveSpell() && spell->m_targets.getUnitTargetGuid()==victim->GetObjectGuid())player->InterruptSpell(type,true);
    player->AttackStop();player->ClearTarget();
}
bool HealingSignal(Player* player)
{
    auto const* ai=player->AI() ? dynamic_cast<BattleBotAI const*>(player->AI()) : nullptr;
    if(player->IsBot() && ai && ai->m_initialized && ai->GetRole()==ROLE_HEALER)return true;
    for(CurrentSpellTypes type:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL})
        if(Spell* spell=player->GetCurrentSpell(type))if(spell->m_spellInfo->IsHealSpell())return true;
    return false;
}
struct ArathiGuardState { std::array<BattleBotTactics::AbCaptureLease,5> capture{}; BattleBotTactics::AbOpeningPlan opening;bool openingFrozen=false; int side=-1;uint64 sideAt=0; BattleBotTactics::Assignments assignments{};std::uint64_t touched=0; };
std::mutex arathiGuardMutex;
std::map<std::pair<uint32,uint32>,ArathiGuardState> arathiGuards;
}
bool BattleBotAI::IsTacticalBattleGround() const
{
    BattleGround* bg=me ? me->GetBattleGround() : nullptr;
    return bg && bg->GetStatus()==STATUS_IN_PROGRESS && (bg->GetTypeID()==BATTLEGROUND_WS || bg->GetTypeID()==BATTLEGROUND_AB);
}
void BattleBotAI::RefreshArathiOpeningPlan()
{
    BattleGround* bg=me->GetBattleGround();if(!bg||bg->GetTypeID()!=BATTLEGROUND_AB)return;
    std::vector<BattleBotTactics::AbOpeningMember> roster;Group* raid=bg->GetBgRaid(me->GetBGTeam());
    for(auto const& row:bg->GetPlayers())if(Player* p=ObjectAccessor::FindPlayer(row.first)){
        if(!p->IsInWorld()||p->GetBattleGround()!=bg||p->GetBGTeam()!=me->GetBGTeam()||p->IsGameMaster())continue;
        auto* ai=p->AI()?dynamic_cast<BattleBotAI*>(p->AI()):nullptr;
        if(p->IsBot()&&(!ai||!ai->m_initialized))continue;
        roster.push_back({p->GetGUID(),unsigned(raid?raid->GetMemberGroup(p->GetObjectGuid()):99),p->IsBot(),ai&&ai->GetRole()==ROLE_HEALER});
    }
    std::lock_guard<std::mutex> lock(arathiGuardMutex);auto& shared=arathiGuards[{bg->GetInstanceID(),uint32(me->GetBGTeam())}];
    if(!shared.openingFrozen){
        shared.opening=BattleBotTactics::PlanAbOpening(roster,me->GetBGTeam()==ALLIANCE);
        shared.openingFrozen=bg->GetStatus()==STATUS_IN_PROGRESS;
    }
    shared.touched=TacticalNow();auto own=shared.opening.nodes.find(me->GetGUID());
    m_abOpeningNode=own==shared.opening.nodes.end()?-1:own->second;
    m_abOpeningActualRaid=shared.opening.actualRaid;m_abHomeOpener=shared.opening.home==me->GetGUID();
    if(m_abOpeningNode>=0){unsigned team=BattleGround::GetTeamIndexByTeamId(me->GetBGTeam());
        if(bg->IsActiveEvent(m_abOpeningNode,1+team)||bg->IsActiveEvent(m_abOpeningNode,3+team))m_abOpeningComplete=true;
    }
}
namespace {
GameObject* ResolveArathiBanner(BattleGround* bg,Map* map,unsigned node,unsigned state)
{
 if(!bg||!map||bg->GetTypeID()!=BATTLEGROUND_AB||!bg->IsActiveEvent(node,state))return nullptr;
 GameObject* selected=nullptr;
 for(auto guid:bg->GetEventGameObjectGuids(node,state))if(GameObject* go=map->GetGameObject(guid)){
  bool type=go->GetGoType()==(state?GAMEOBJECT_TYPE_BUTTON:GAMEOBJECT_TYPE_GOOBER);
  if(std::find(vFlagsAB.begin(),vFlagsAB.end(),go->GetEntry())==vFlagsAB.end()||!BattleBotTactics::AbBannerMatches(node,state,go->GetEntry(),go->GetGOInfo()->GetLockId()!=0,type,go->isSpawned()))continue;
  if(selected)return nullptr; // Ambiguous event data: never pick an arbitrary decoration/banner.
  selected=go;
 }
 return selected;
}
bool IsArathiOpening(Player* observer,Unit* caster,Spell* spell)
{
    if(!observer||!caster||!spell||!caster->IsPlayer()||!(spell->m_spellInfo->Id==21651||spell->m_spellInfo->HasEffect(SPELL_EFFECT_OPEN_LOCK)))return false;
    auto* bg=observer->GetBattleGround();if(!bg||bg->GetTypeID()!=BATTLEGROUND_AB)return false;
    if(!caster->IsVisibleForOrDetect(observer,observer,false)||!observer->IsWithinLOSInMap(caster)||!observer->IsWithinDistInMap(caster,80))return false;
    auto* go=observer->GetMap()->GetGameObject(spell->m_targets.getGOTargetGuid());if(!go)return false;
    for(auto const& event:sBattleGroundMgr.GetGameObjectEventsVector(go->GetGUIDLow()))
        if(event.event1<BG_AB_NODES_MAX&&ResolveArathiBanner(bg,observer->GetMap(),event.event1,event.event2)==go)return true;
    return false;
}
bool ArathiDryPath(Player* player,Movement::PointsArray const& points,bool allowWetPrefix,float limit)
{
    if(points.empty())return false;
    bool drySeen=!allowWetPrefix;float prefix=0;
    for(size_t i=0;i<points.size();++i){
        auto const& p=points[i];auto const& prev=points[i?i-1:i];float length=(p-prev).length();
        if(!std::isfinite(length))return false;
        unsigned steps=std::max(1u,unsigned(std::ceil(length/3.0f)));
        for(unsigned j=1;j<=steps;++j){auto q=prev+(p-prev)*(float(j)/steps);
            if(!BattleBotTactics::AbLandSample(player->GetTerrain()->IsInWater(q.x,q.y,q.z),drySeen,prefix,length/steps,limit))return false;
        }
    }
    auto const& end=points.back();return !player->GetTerrain()->IsInWater(end.x,end.y,end.z);
}
// An actor already in water gets its own normal-native-water egress leg. It is
// never planned with the dry filter that would reject its start polygon.
bool ArathiEgress(Player* player,Position const& goal,Position& destination,float& limit)
{
    float best=std::numeric_limits<float>::infinity();
    for(float radius:{10.f,20.f})for(unsigned i=0;i<8;++i){
        float angle=float(i)*float(M_PI)/4,px,py,pz;
        if(!player->GetRandomPoint(player->GetPositionX()+std::cos(angle)*radius,player->GetPositionY()+std::sin(angle)*radius,player->GetPositionZ(),1,px,py,pz)||player->GetTerrain()->IsInWater(px,py,pz))continue;
        float dx=px-player->GetPositionX(),dy=py-player->GetPositionY(),dz=pz-player->GetPositionZ();if(dx*dx+dy*dy+dz*dz>400)continue;
        PathInfo path(player);path.ExcludeSteepSlopes();
        if(!path.calculate(px,py,pz)||path.getPathType()!=PATHFIND_NORMAL||!ArathiDryPath(player,path.getPath(),true,20))continue;
        if(path.Length()<best){best=path.Length();destination={px,py,pz,0};limit=20;}
    }
    if(std::isfinite(best))return true;
    // Deep-water compatibility: preserve a safe native egress rather than freeze
    // the actor. Pick the first real dry sample of the original native path, not
    // an invented pose; this fallback does not claim a 20-yard escape guarantee.
    PathInfo original(player);original.ExcludeSteepSlopes();
    if(!original.calculate(goal.x,goal.y,goal.z)||original.getPathType()!=PATHFIND_NORMAL)return false;
    auto const& points=original.getPath();
    for(size_t i=1;i<points.size();++i){auto const& prev=points[i-1];auto const& p=points[i];float length=(p-prev).length();if(!std::isfinite(length))return false;
        unsigned steps=std::max(1u,unsigned(std::ceil(length/3.f)));
        for(unsigned j=1;j<=steps;++j){auto q=prev+(p-prev)*(float(j)/steps);if(player->GetTerrain()->IsInWater(q.x,q.y,q.z))continue;
            PathInfo escape(player);escape.ExcludeSteepSlopes();
            if(!escape.calculate(q.x,q.y,q.z)||escape.getPathType()!=PATHFIND_NORMAL||escape.getPath().empty())return false;
            limit=std::max(20.f,escape.Length()+3.f);
            if(!ArathiDryPath(player,escape.getPath(),true,limit))return false;
            destination={q.x,q.y,q.z,0};return true;
        }
    }
    return false;
}
struct StartingGate {Position safe;Position gate;uint32 entry,guid;};
StartingGate GateFor(BattleGround* bg,Team team){
    if(bg->GetTypeID()==BATTLEGROUND_AB)return team==ALLIANCE?StartingGate{AB_WAITING_POS_ALLIANCE,{1284.6f,1281.17f,-15.9779f,0},180255,90054}:StartingGate{AB_WAITING_POS_HORDE,{708.09f,708.448f,-17.3899f,0},180256,90055};
    return team==ALLIANCE?StartingGate{WS_WAITING_POS_ALLIANCE_1,{1503.34f,1493.47f,352.189f,0},179918,90008}:StartingGate{WS_WAITING_POS_HORDE_1,{949.166f,1423.77f,345.624f,0},179916,90012};
}
}
std::string BattleBotAI::OpeningStatus() const
{
    return std::string("prepWaiting=")+(m_preparationWaiting?"1":"0")+" buffDone="+(m_preparationBuffDone?"1":"0")+" openingNode="+std::to_string(m_abOpeningNode)+" grouping="+(m_abOpeningActualRaid?"actual_ui_subgroup":"logical_scaled")+" homeOpener="+(m_abHomeOpener?"1":"0");
}
std::string BattleBotAI::PreparationStatus() const
{
 std::ostringstream out;out<<std::fixed<<std::setprecision(1);
 out<<"prepWaiting="<<m_preparationWaiting<<" prepPhase="<<m_prepPhase<<" prepTargetX="<<(m_prepGoalReady?m_prepTarget.x:0)<<" prepTargetY="<<(m_prepGoalReady?m_prepTarget.y:0)<<" prepTargetZ="<<(m_prepGoalReady?m_prepTarget.z:0)<<" prepMoves="<<m_prepMoves<<" prepRejectReason="<<m_prepReject<<" prepCandidateAttempts="<<m_prepAttempts<<" startDelayMs="<<(me&&me->GetBattleGround()?me->GetBattleGround()->GetStartDelayTime():0);
 return out.str();
}
std::string BattleBotAI::MountStatus() const
{
 std::ostringstream out;
 out<<"mountReason="<<m_mountReason<<" mountCalls="<<m_mountAttempts<<" mountPrep="<<m_mountPreparation
    <<" mountCost="<<m_mountCost<<" mountResult="<<m_mountCastResult<<" class="<<uint32(me->GetClass())<<" race="<<uint32(me->GetRace())
    <<" stealth="<<me->HasAuraType(SPELL_AURA_MOD_STEALTH)<<" form="<<uint32(me->GetShapeshiftForm())
    <<" outdoors="<<me->GetTerrain()->IsOutdoors(me->GetPositionX(),me->GetPositionY(),me->GetPositionZ())
    <<" mountSpell="<<GetMountSpellId();
 return out.str();
}
std::vector<std::string> BattleBotAI::ArathiFlagStatus(Player* me)
{
 std::vector<std::string> rows;if(!me)return rows;auto* bg=me->GetBattleGround();
 if(!bg||bg->GetTypeID()!=BATTLEGROUND_AB)return rows;
 std::ostringstream actor;actor<<"GMUI|ABFLAGACTOR|guid="<<me->GetGUID()<<" gm="<<me->IsGameMaster()<<" mounted="<<me->IsMounted()<<" immune="<<me->IsTotalImmune()<<" control="<<me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)<<" canUse="<<me->CanUseBattleGroundObject()<<" bgStatus="<<uint32(bg->GetStatus())<<" map="<<me->GetMapId()<<" instance="<<me->GetInstanceId()<<" bgInstance="<<bg->GetInstanceID()<<" teleport="<<me->IsBeingTeleported();rows.push_back(actor.str());
 std::string reason="ok";
 if(me->GetClass()==CLASS_PRIEST&&me->HasAura(27827))reason="spirit_redemption";else if(me->IsGameMaster())reason="gm";else if(me->IsMounted())reason="mounted";else if(me->IsTotalImmune())reason="immune";else if(!me->IsAlive())reason="dead";else if(me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))reason="controlled";
 auto spellId=[&](CurrentSpellTypes lane){Spell* s=me->GetCurrentSpell(lane);return s?s->m_spellInfo->Id:0u;};
 std::ostringstream activity;activity<<"GMUI|ABACTIVITY|guid="<<me->GetGUID()<<" alive="<<me->IsAlive()<<" spirit="<<me->HasAura(27827)<<" combat="<<me->IsInCombat()<<" selfMover="<<me->IsSelfMover()<<" canUseReason="<<reason<<" generic="<<spellId(CURRENT_GENERIC_SPELL)<<" channel="<<spellId(CURRENT_CHANNELED_SPELL)<<" stand="<<uint32(me->GetStandState())<<" food="<<me->HasAura(BB_SPELL_FOOD)<<" drink="<<me->HasAura(BB_SPELL_DRINK)<<" hp="<<me->GetHealthPercent()<<" mana="<<me->GetPowerPercent(POWER_MANA);rows.push_back(activity.str());
 if(auto* ai=me->AI()?dynamic_cast<BattleBotAI*>(me->AI()):nullptr){
  std::string phase=spellId(CURRENT_GENERIC_SPELL)==21651||spellId(CURRENT_CHANNELED_SPELL)==21651||spellId(CURRENT_GENERIC_SPELL)==23932||spellId(CURRENT_CHANNELED_SPELL)==23932?"opening":ai->m_preparationWaiting?"prep":me->HasAura(BB_SPELL_FOOD)||me->HasAura(BB_SPELL_DRINK)?"recovery":me->IsInCombat()?"combat":ai->m_arathiGuardAnchorValid?"guard":ai->m_arathiAdvanceValid?"advance":"idle";
  std::ostringstream state;state<<"GMUI|ABAI|guid="<<me->GetGUID()<<" phase="<<phase<<" capture="<<ai->m_abCaptureTrace<<" captureEnd="<<ai->m_abCaptureEnd<<" result="<<ai->m_abCaptureResult;rows.push_back(state.str());
  auto now=TacticalNow();std::ostringstream timing;timing<<"GMUI|ABTIMING|guid="<<me->GetGUID()<<" hurtAt="<<ai->m_abCaptureHurtAt<<" retryAt="<<ai->m_abCaptureRetryAt<<" attemptedAt="<<ai->m_abCaptureAttemptAt<<" rushMs="<<(now<ai->m_abRushUntil?ai->m_abRushUntil-now:0);rows.push_back(timing.str());
  std::ostringstream route;route<<"GMUI|ABROUTESTATE|guid="<<me->GetGUID()<<" node="<<ai->m_arathiAdvanceNode<<" land="<<ai->m_abLandTrace<<" water="<<me->IsInWater()<<" terrainWet="<<me->GetTerrain()->IsInWater(me->GetPositionX(),me->GetPositionY(),me->GetPositionZ());rows.push_back(route.str());
 }
 for(unsigned node=0;node<5;++node)for(unsigned state=0;state<5;++state)if(bg->IsActiveEvent(node,state)){
  auto* flag=ResolveArathiBanner(bg,me->GetMap(),node,state);auto guid=flag?flag->GetObjectGuid():ObjectGuid();
  std::ostringstream out;out<<"GMUI|ABFLAG|node="<<node<<" state="<<state<<" guid="<<guid.GetRawValue()<<" exists="<<(flag!=nullptr);
  if(flag)out<<" level="<<flag->GetUInt32Value(GAMEOBJECT_LEVEL)<<" entry="<<flag->GetEntry()<<" type="<<uint32(flag->GetGoType())<<" goState="<<uint32(flag->GetGoState())<<" flags="<<flag->GetUInt32Value(GAMEOBJECT_FLAGS)<<" lock="<<flag->GetGOInfo()->GetLockId()<<" spawn="<<flag->isSpawned()<<" near="<<flag->IsAtInteractDistance(me);
  rows.push_back(out.str());
 }
 return rows;
}
bool BattleBotAI::OwnsPreparationMovement() const
{
 if(!m_prepPoint||me->movespline->GetId()!=m_prepSpline)return false;
 auto const* point=dynamic_cast<PointMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent());
 return point&&point->GetMovementGeneratorType()==POINT_MOTION_TYPE&&point->GetId()==m_prepPoint;
}
bool BattleBotAI::UpdateBattleGroundPreparation()
{
 BattleGround* bg=me->GetBattleGround();if(!bg||(bg->GetTypeID()!=BATTLEGROUND_WS&&bg->GetTypeID()!=BATTLEGROUND_AB))return false;
 if(bg->GetTypeID()==BATTLEGROUND_AB)RefreshArathiOpeningPlan();
 auto gate=GateFor(bg,me->GetBGTeam());auto now=TacticalNow();
 bool recovery=(me->HasAura(BB_SPELL_FOOD)&&me->GetHealthPercent()<100)||(me->HasAura(BB_SPELL_DRINK)&&me->GetPowerPercent(POWER_MANA)<100);
 if(bg->GetStatus()!=STATUS_WAIT_JOIN){
  if(!m_preparationWaiting)return false;
  if(bg->GetStatus()!=STATUS_IN_PROGRESS)return true;
  auto* door=me->GetMap()->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT,gate.entry,gate.guid));
  if(bg->IsActiveEvent(BG_EVENT_GHOST_GATE,0)||(door&&door->GetGoState()==GO_STATE_READY)){m_prepPhase="gate_wait";return true;}
  if(bg->GetTypeID()==BATTLEGROUND_AB||bg->GetTypeID()==BATTLEGROUND_WS){
   // Doors are open: only cancel our preparation activity, never an objective/heal/control.
   if(Spell* cast=me->GetCurrentSpell(CURRENT_GENERIC_SPELL))if(m_preparationUpdating||m_preparationTriedSpell||m_isBuffing)if(cast->m_spellInfo->IsPositiveSpell()&&!cast->m_spellInfo->IsHealSpell())me->InterruptSpell(CURRENT_GENERIC_SPELL,true);
   me->RemoveAurasDueToSpell(BB_SPELL_FOOD);me->RemoveAurasDueToSpell(BB_SPELL_DRINK);
   if(me->GetStandState()!=UNIT_STAND_STATE_STAND)me->SetStandState(UNIT_STAND_STATE_STAND);
   m_preparationBuffDone=true;m_isBuffing=false;
  }else if(me->IsNonMeleeSpellCasted()||recovery){m_prepPhase=recovery?"recover":"buff";return true;}
  if(bg->GetTypeID()==BATTLEGROUND_AB)m_abRushUntil=now+60000;
  m_preparationWaiting=false;m_prepPhase="released";
  if(OwnsPreparationMovement()){ClearPath();StopMoving();}return false;
 }
 m_preparationWaiting=true;if(!m_preparationAt)m_preparationAt=now;
 if(me->IsNonMeleeSpellCasted()){m_prepPhase="buff";return true;}
 if(recovery){m_prepPhase="recover";return true;}
 if(me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE)){m_prepReject="controlled";return true;}
 // Foreign distraction/charge/follow generators own their activity, even when stationary.
 if(me->GetMotionMaster()->GetCurrentMovementGeneratorType()!=IDLE_MOTION_TYPE&&!OwnsPreparationMovement()){m_prepReject="external_motion";return true;}
 bool final=bg->GetStartDelayTime()<=15000;
 float sx,sy,sz,so;bg->GetTeamStartLoc(me->GetBGTeam(),sx,sy,sz,so);
 auto safePoint=[&](float x,float y,float z){float dx=x-sx,dy=y-sy,dz=z-sz;return dx*dx+dy*dy+dz*dz<=10000&&BattleBotTactics::InsideStartingGate(x,y,gate.gate.x,gate.gate.y,gate.safe.x,gate.safe.y);};
 auto actualSafe=[&](){
  if(!me->movespline->Initialized()||me->movespline->Finalized())return true;
  auto current=me->movespline->ComputePosition();bool safe=safePoint(current.x,current.y,current.z);
  for(int32 i=me->movespline->_currentSplineIdx()+1;safe&&i<=int32(me->movespline->CountSplinePoints());++i){auto point=me->movespline->GetPoint(i);safe=safePoint(point.x,point.y,point.z);}return safe;
 };
 auto stopOwned=[&](){if(OwnsPreparationMovement()){ClearPath();StopMoving();}};
 auto sampleGoals=[&](bool converge){
  stopOwned();m_prepSpreadArrived=false;m_prepGoalReady=false;m_prepCandidates.clear();m_prepCandidate=0;++m_prepGeneration;
  m_prepNextIdle=now+BattleBotTactics::PreparationIdleInterval(me->GetGUID(),bg->GetInstanceID(),m_prepGeneration);
  float nx=gate.safe.x-gate.gate.x,ny=gate.safe.y-gate.gate.y,length=std::sqrt(nx*nx+ny*ny);if(length<=.01f){m_prepReject="plane";return;}
  nx/=length;ny/=length;
  for(unsigned i=0;i<8;++i){auto offset=BattleBotTactics::PreparationOffset(me->GetGUID(),bg->GetInstanceID(),m_prepGeneration,i,converge);
   Position point{gate.gate.x+nx*offset.depth-ny*offset.lateral,gate.gate.y+ny*offset.depth+nx*offset.lateral,gate.safe.z,gate.safe.o};
   float x,y,z;bool ground=me->GetRandomPoint(point.x,point.y,point.z,.35f,x,y,z);
   if(ground)point=Position{x,y,z,gate.safe.o};m_prepCandidates.push_back({point,ground});
  }
 };
 auto moveGoal=[&](){
  if(m_prepGoalReady){
   if(me->IsMoving()&&OwnsPreparationMovement()){
    if(!actualSafe()){m_prepReject="actual_path";stopOwned();m_prepGoalReady=false;}
    else {m_prepPhase="moving";return true;}
   }
   else if(me->GetDistance(m_prepTarget)<=1.2f){m_prepSpreadArrived=true;return false;}
   else {m_prepGoalReady=false;m_prepReject="stopped";}
  }
  if(me->IsMoving()){m_prepReject="external_motion";return true;}
  if(m_prepCandidates.empty()&&now>=m_preparationMoveAt)sampleGoals(final);
  while(m_prepCandidate<m_prepCandidates.size()){
   auto candidate=m_prepCandidates[m_prepCandidate++];++m_prepAttempts;
   if(!candidate.second){m_prepReject="ground";continue;}
   auto const& point=candidate.first;if(!safePoint(point.x,point.y,point.z)){m_prepReject="bounds";continue;}
   PathInfo path(me);path.ExcludeSteepSlopes();bool calculated=path.calculate(point.x,point.y,point.z);
   if(!calculated||path.getPath().empty()||path.getPathType()!=PATHFIND_NORMAL){m_prepReject="nav";continue;}
   if(!BattleBotTactics::PreparationDetour(path.Length(),me->GetDistance(point))){m_prepReject="detour";continue;}
   bool safe=true;for(auto const& p:path.getPath())if(!safePoint(p.x,p.y,p.z)){safe=false;break;}
   if(!safe){m_prepReject="plane";continue;}
   m_prepTarget=point;m_prepGoalReady=true;ClearPath();StopMoving();
   m_prepPoint=0x42500000u+(m_prepAttempts&0xfffffu);
   me->GetMotionMaster()->MovePoint(m_prepPoint,point.x,point.y,point.z,MOVE_PATHFINDING|MOVE_EXCLUDE_STEEP_SLOPES,0,point.o);m_prepSpline=me->movespline->GetId();
   if(!OwnsPreparationMovement()||!me->movespline->Initialized()||me->movespline->Finalized()){m_prepReject="stopped";m_prepGoalReady=false;continue;}
   if(!actualSafe()){m_prepReject="actual_path";stopOwned();m_prepGoalReady=false;continue;}
   ++m_prepMoves;m_prepReject="none";m_prepPhase="moving";return true;
  }
  m_prepReject="exhausted";m_preparationMoveAt=now+8000;m_prepCandidates.clear();m_prepSpreadArrived=true;m_prepPhase="idle";return false;
 };
 if(final&&!m_prepConverge){m_prepConverge=true;sampleGoals(true);}
 // First acquire a safe different preparation position, not 45-60s of spawn idle.
 if(!m_prepSpreadArrived||final){if(moveGoal())return true;}
 if(!m_preparationBuffDone){
  bool budget=now-m_preparationAt<45000&&bg->GetStartDelayTime()>12000;
  if(budget){m_prepPhase="buff";m_preparationUpdating=true;m_preparationTriedSpell=false;UpdateOutOfCombatAI();m_preparationUpdating=false;
   if(me->IsNonMeleeSpellCasted()||m_preparationTriedSpell||m_isBuffing){m_preparationQuiet=0;return true;}
   if(++m_preparationQuiet<3)return true;
  }
  m_preparationBuffDone=true;m_isBuffing=false;
 }
 if(!me->IsMounted()&&me->GetPowerType()==POWER_MANA&&me->GetPowerPercent(POWER_MANA)<85&&now-m_preparationAt<60000&&bg->GetStartDelayTime()>12000)if(DrinkAndEat()){m_prepPhase="recover";return true;}
 if(me->GetStandState()!=UNIT_STAND_STATE_STAND)me->SetStandState(UNIT_STAND_STATE_STAND);
 if(UseMount(true)){m_prepPhase="mount";return true;}
 m_prepPhase=final?"converge":"idle";
 if(!final&&now>=m_prepNextIdle){sampleGoals(false);moveGoal();}
 return true;
}

namespace {
struct WsgPlace {uint64 id;Position point;float benefit;};
std::vector<WsgPlace> WsgWaitPlaces(bool alliance,bool pressed){
    // These are independently authored route points for each faction. Runtime
    // normal-nav and threat/healer LOS checks are mandatory, never mirrored XYZ.
    if(alliance)return pressed?std::vector<WsgPlace>{{11,{1484.42f,1396.69f,346.117f,0},4},{12,{1471.86f,1456.65f,342.794f,0},2}}:
        std::vector<WsgPlace>{{1,{1503.73f,1457.67f,373.684f,0},6},{2,{1492.61f,1494.72f,373.721f,0},5},{3,WS_FLAG_POS_ALLIANCE,1}};
    return pressed?std::vector<WsgPlace>{{11,{951.188f,1520.99f,356.377f,0},4},{12,{981.948f,1459.07f,336.154f,0},2}}:
        std::vector<WsgPlace>{{1,{956.972f,1459.48f,367.291f,0},6},{2,{978.632f,1430.71f,367.125f,0},5},{3,WS_FLAG_POS_HORDE,1}};
}
bool WsgSeen(Player* observer,Unit* unit){return unit&&unit->IsAlive()&&unit->IsInWorld()&&unit->GetMap()==observer->GetMap()&&observer->IsWithinDistInMap(unit,80)&&unit->IsVisibleForOrDetect(observer,observer,false)&&observer->IsWithinLOSInMap(unit);}
}
std::string BattleBotAI::WarsongStatus() const
{
    std::ostringstream out;out<<"scenario="<<m_wsgScenario<<" task="<<uint32(m_wsgTask)<<" squad="<<uint32(m_wsgSquad)<<" choice="<<m_wsgChoice.candidate<<" draws="<<m_wsgChoice.draws<<" until="<<m_wsgChoice.until<<" enemySeen="<<m_wsgEnemySeenAt<<" move="<<m_wsgMotionReason;return out.str();
}
bool BattleBotAI::OwnsLegacyWaypointMovement() const
{
    auto* point=dynamic_cast<PointMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent());
    return point&&point->GetMovementGeneratorType()==POINT_MOTION_TYPE&&m_currentPath&&m_waypointTicket.pending&&point->GetId()==m_waypointTicket.pending;
}
bool BattleBotAI::OwnsWarsongMovement()
{
    auto* point=dynamic_cast<PointMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent());
    if(!point||point->GetMovementGeneratorType()!=POINT_MOTION_TYPE||point->GetId()!=0x5753474f)return false;
    float x,y,z;point->GetDestination(x,y,z);if(std::abs(x-m_wsgMotionGoal.x)>.01f||std::abs(y-m_wsgMotionGoal.y)>.01f||std::abs(z-m_wsgMotionGoal.z)>.01f)return false;
    m_wsgMotionSpline=me->movespline->GetId();return true;
}
std::vector<std::string> BattleBotAI::WarsongRouteStatus(Player* actor)
{
    std::vector<std::string> rows;if(!actor||!actor->GetBattleGround()||actor->GetBattleGround()->GetTypeID()!=BATTLEGROUND_WS)return rows;
    for(bool alliance:{true,false})for(bool pressed:{false,true})for(auto const& place:WsgWaitPlaces(alliance,pressed)){
        Position start=alliance?WS_FLAG_POS_ALLIANCE:WS_FLAG_POS_HORDE;PathInfo path(actor);path.ExcludeSteepSlopes();
        bool ok=path.calculate(Movement::Vector3{start.x,start.y,start.z},Movement::Vector3{place.point.x,place.point.y,place.point.z});
        float endpointError=-1,zError=-1,endZ=0;if(!path.getPath().empty()){auto const& end=path.getPath().back();endpointError=(end-Movement::Vector3{place.point.x,place.point.y,place.point.z}).length();zError=std::abs(end.z-place.point.z);endZ=end.z;}
        std::ostringstream out;out<<"GMUI|WSGROUTE|team="<<(alliance?469:67)<<" pressed="<<pressed<<" id="<<place.id<<" calculated="<<ok<<" type="<<uint32(path.getPathType())<<" length="<<(path.getPath().size()>1?path.Length():0)<<" x="<<place.point.x<<" y="<<place.point.y<<" z="<<place.point.z<<" endZ="<<endZ<<" error="<<endpointError<<" zError="<<zError;rows.push_back(out.str());
    }
    for(bool alliance:{true,false}){
        Position exits[2]={alliance?Position{1510.502f,1493.385f,351.995f,0}:Position{944.981f,1423.478f,345.434f,0},alliance?Position{1415.33f,1554.79f,343.156f,0}:Position{1029.14f,1387.49f,340.836f,0}};
        for(unsigned scenario=0;scenario<2;++scenario){auto start=exits[scenario];Position dest{1233.88f,1476.29f,308.015f,0};PathInfo route(actor);route.ExcludeSteepSlopes();bool ok=route.calculate(Movement::Vector3{start.x,start.y,start.z},Movement::Vector3{dest.x,dest.y,dest.z});
            std::ostringstream out;out<<"GMUI|WSGEXIT|team="<<(alliance?469:67)<<" scene="<<(scenario?"revive":"opening")<<" normal="<<(ok&&route.getPathType()==PATHFIND_NORMAL)<<" length="<<(route.getPath().size()>1?route.Length():0)<<" outdoors="<<actor->GetTerrain()->IsOutdoors(start.x,start.y,start.z);rows.push_back(out.str());}
    }
    return rows;
}
void BattleBotAI::RefreshWarsongTask()
{
    using namespace BattleBotTactics;using namespace BattleBotObjectivePolicy;
    BattleGround* bg=me?me->GetBattleGround():nullptr;if(!bg||bg->GetTypeID()!=BATTLEGROUND_WS){m_wsgGoalValid=false;m_wsgTravel=false;return;}
    uint64 now=TacticalNow();std::vector<WsgCandidate> roster;
    for(auto const& entry:bg->GetPlayers())if(Player* p=ObjectAccessor::FindPlayer(entry.first))if(p->IsInWorld()&&p->GetBattleGround()==bg&&p->GetBGTeam()==me->GetBGTeam()&&p->IsBot())if(auto* ai=p->AI()?dynamic_cast<BattleBotAI*>(p->AI()):nullptr)if(ai->m_initialized)roster.push_back({p->GetGUID(),p->GetClass(),ai->GetRole()==ROLE_HEALER});
    auto squads=AssignWsgSquads(roster);auto mine=squads.find(me->GetGUID());if(mine==squads.end()){m_wsgGoalValid=false;return;}
    auto squad=mine->second;auto* ws=static_cast<BattleGroundWS*>(bg);bool alliance=me->GetBGTeam()==ALLIANCE;Team ours=alliance?ALLIANCE:HORDE,other=alliance?HORDE:ALLIANCE;
    ObjectGuid ownPicker=alliance?ws->GetAllianceFlagPickerGuid():ws->GetHordeFlagPickerGuid(),enemyPicker=alliance?ws->GetHordeFlagPickerGuid():ws->GetAllianceFlagPickerGuid();
    Player* friendly=enemyPicker.IsEmpty()?nullptr:ObjectAccessor::FindPlayer(enemyPicker);Player* enemy=ownPicker.IsEmpty()?nullptr:ObjectAccessor::FindPlayer(ownPicker);
    bool friendValid=friendly&&friendly->IsInWorld()&&friendly->IsAlive()&&friendly->GetBattleGround()==bg&&friendly->GetBGTeam()==ours;
    bool enemySeen=WsgSeen(me,enemy)&&enemy->GetBGTeam()!=ours;
    bool ownAway=ws->GetFlagState(ours)!=BG_WS_FLAG_STATE_ON_BASE;
    if(ownPicker!=m_wsgSeenEnemy||!ownAway){m_wsgEnemySeenAt=0;m_wsgSeenEnemy.Clear();}
    if(enemySeen&&ownAway&&me->IsValidAttackTarget(enemy)){m_wsgEnemyLast=enemy->GetPosition();m_wsgEnemySeenAt=now;m_wsgSeenEnemy=ownPicker;}
    Position home=alliance?WS_FLAG_POS_ALLIANCE:WS_FLAG_POS_HORDE;
    std::list<Player*> observed;me->GetAlivePlayerListInRange(me,observed,80);bool localThreat=false,pressed=false;
    for(Player* p:observed)if(p->GetBattleGround()==bg&&p->GetBGTeam()!=ours&&WsgSeen(me,p)&&me->IsValidAttackTarget(p)){
        if(p->GetDistance(home)<115||p->GetDistance(alliance?Position{1415.33f,1554.79f,343.156f,0}:Position{1029.14f,1387.49f,340.836f,0})<65){localThreat=true;m_wsgThreatLast=p->GetPosition();m_wsgThreatSeenAt=now;}
        if(me->GetDistance(p)<28||Threatens(p,me))pressed=true;
    }
    if(pressed)m_wsgPressureUntil=now+3000;
    pressed=pressed||now<m_wsgPressureUntil;
    if(!ObservationFresh(m_wsgThreatSeenAt,now,4000))m_wsgThreatSeenAt=0;
    m_wsgLocalDefense=m_wsgRevived&&(localThreat||m_wsgThreatSeenAt!=0);
    unsigned index=0;for(auto const& row:squads)if(row.second==WsgSquad::Flag){if(row.first==me->GetGUID())break;++index;}
    WsgTask task=PlanWsgTask(squad,HasWsgFlag(me),friendValid,ownAway,index);
    if(task==WsgTask::Escort&&!WsgSupportEligible(me->GetDistance(friendly),friendly->GetPositionX()))task=WsgTask::Midfield;
    if(m_wsgLocalDefense&&!HasWsgFlag(me))task=WsgTask::Recover;
    Position goal{};bool valid=true;ObjectGuid carrier;std::vector<WsgPlace> places;
    if(task==WsgTask::Return){if(!ownAway){goal=home;m_wsgScenario="score_now";}else{places=WsgWaitPlaces(alliance,pressed);m_wsgScenario=pressed?"carrier_ramp_retreat":"carrier_upper_hold";}}
    else if(task==WsgTask::Escort){goal=friendly->GetPosition();carrier=friendly->GetObjectGuid();m_wsgScenario="escort";}
    else if(task==WsgTask::Recover){
        if(m_wsgLocalDefense){goal=m_wsgThreatLast;m_wsgScenario="revive_defend";}
        else if(enemySeen){goal=m_wsgEnemyLast;carrier=ownPicker;m_wsgScenario="recover_seen";}
        else if(ObservationFresh(m_wsgEnemySeenAt,now,4000)){goal=m_wsgEnemyLast;carrier=m_wsgSeenEnemy;m_wsgScenario="recover_last_known";}
        else if(GameObject* flag=me->GetMap()->GetGameObject(ws->GetDroppedFlagGuid(ours))){if(flag->isSpawned()&&flag->IsVisibleForInState(me,me,false)&&me->IsWithinDistInMap(flag,80)&&me->IsWithinLOSInMap(flag)){goal=flag->GetPosition();m_wsgScenario="recover_dropped";}else valid=false;}
        else valid=false;
        if(!valid){task=WsgTask::Midfield;valid=true;}
    }
    if(task==WsgTask::Capture){goal=alliance?WS_FLAG_POS_HORDE:WS_FLAG_POS_ALLIANCE;m_wsgScenario="capture";
        if(GameObject* flag=me->GetMap()->GetGameObject(ws->GetDroppedFlagGuid(other)))if(flag->isSpawned()&&flag->IsVisibleForInState(me,me,false)&&me->IsWithinDistInMap(flag,80)&&me->IsWithinLOSInMap(flag))goal=flag->GetPosition();}
    if(task==WsgTask::Midfield){m_wsgScenario=m_wsgRevived?"revive_midfield":"midfield";if(GetRole()==ROLE_HEALER)places={{21,alliance?Position{1250.52f,1470.94f,309.8f,0}:Position{1212.99f,1477.94f,306.929f,0},4}};else places={{21,{1233.88f,1476.29f,308.015f,0},4},{22,{1250.52f,1470.94f,309.8f,0},2},{23,{1212.99f,1477.94f,306.929f,0},2}};}
    uint64 event=Hash(ownPicker.GetRawValue())^Hash(enemyPicker.GetRawValue())^uint64(ws->GetFlagState(ours))^uint64(ws->GetFlagState(other)<<8)^uint64(pressed<<16)^uint64(m_wsgLocalDefense<<17)^uint64(m_wsgRevived<<18);
    uint64 previousChoice=m_wsgChoice.candidate;
    if(!places.empty()){
        bool evaluate=m_wsgEvaluated.empty()||m_wsgEvaluatedEvent!=event||m_wsgEvaluatedTask!=uint64(task)||now-m_wsgEvaluatedAt>=1000||me->GetDistance(m_wsgEvaluatedPosition)>=5;
        if(evaluate){m_wsgEvaluated.clear();m_wsgEvaluatedAt=now;m_wsgEvaluatedEvent=event;m_wsgEvaluatedTask=uint64(task);m_wsgEvaluatedPosition=me->GetPosition();
        for(auto const& place:places){PathInfo route(me);route.ExcludeSteepSlopes();bool reachable=route.calculate(place.point.x,place.point.y,place.point.z)&&route.getPathType()==PATHFIND_NORMAL&&!route.getPath().empty();
            if(reachable){auto const& end=route.getPath().back();if(std::abs(end.z-place.point.z)>2.5f||(end-Movement::Vector3{place.point.x,place.point.y,place.point.z}).length()>4)reachable=false;}float distance=me->GetDistance(place.point);if(reachable&&route.Length()>std::max(80.f,distance*3+30))reachable=false;
            double friendlyForce=0,enemyForce=0;bool healerLos=false,healerNearby=false;
            for(auto const& row:bg->GetPlayers())if(Player* p=ObjectAccessor::FindPlayer(row.first))if(p->GetBGTeam()==ours&&p!=me&&p->IsAlive()&&p->IsInWorld()&&p->GetBattleGround()==bg){auto* ai=p->AI()?dynamic_cast<BattleBotAI*>(p->AI()):nullptr;if(ai&&ai->GetRole()==ROLE_HEALER&&me->IsWithinDistInMap(p,60)){healerNearby=true;if(p->GetDistance(place.point)<35&&p->IsWithinLOSAtPosition(p->GetPositionX(),p->GetPositionY(),p->GetPositionZ(),place.point.x,place.point.y,place.point.z))healerLos=true;}if(p->GetDistance(place.point)<35)friendlyForce+=p->GetHealthPercent()/100.;}
            for(Player* p:observed)if(p->GetBGTeam()!=ours&&WsgSeen(me,p)&&me->IsValidAttackTarget(p)&&p->GetDistance(place.point)<35)enemyForce+=p->GetHealthPercent()/100.;
            if(task==WsgTask::Return&&healerNearby&&!healerLos&&place.id!=3)reachable=false;
            if(task==WsgTask::Return&&enemyForce>friendlyForce+1&&me->GetDistance(place.point)<20)reachable=false;
            m_wsgEvaluated.push_back({place.id,double(place.benefit)*(healerLos?2.:1.),reachable?route.Length()/std::max(1.f,me->GetSpeed(MOVE_RUN)):0,friendlyForce,enemyForce,reachable,now+5000});
        }
        }
        uint64 pick=Choose(m_wsgChoice,me->GetGUID(),uint64(task)+1,event,now,5000,m_wsgEvaluated,false,task==WsgTask::Return);
        valid=false;for(auto const& place:places)if(place.id==pick){goal=place.point;valid=true;break;}
        if(!valid){m_wsgScenario="blocked_nav_candidates";m_wsgMotionReason="candidate_nav_unavailable";}
    }else {Forget(m_wsgChoice);m_wsgEvaluated.clear();}
    bool changed=!m_wsgGoalValid||task!=m_wsgTask||squad!=m_wsgSquad||carrier!=m_wsgCarrier||event!=m_wsgScenarioEvent;
    bool goalChanged=(previousChoice!=m_wsgChoice.candidate&&!places.empty())||(m_wsgGoalValid&&(goal.x!=m_wsgGoal.x||goal.y!=m_wsgGoal.y||goal.z!=m_wsgGoal.z)&&task==WsgTask::Return);
    m_wsgSquad=squad;m_wsgTask=task;m_wsgGoal=goal;m_wsgGoalValid=valid;m_wsgCarrier=carrier;m_wsgOwnFlagAway=ownAway;m_wsgPressed=pressed;m_wsgScenarioEvent=event;
    if(changed||goalChanged){bool ownMotion=OwnsWarsongMovement()||OwnsLegacyWaypointMovement();m_wsgProgressAt=0;if(!me->IsNonMeleeSpellCasted(false,false,true)){ClearPath();if(me->GetVictim())StopTacticalAttack(me);if(ownMotion)StopMoving();}m_wsgTravel=false;m_wsgRepathAt=0;}
}
bool BattleBotAI::StartWarsongObjective()
{
    using namespace BattleBotTactics;
    if(!m_wsgGoalValid)return false;
    auto motion=me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if(motion!=IDLE_MOTION_TYPE&&motion!=CHASE_MOTION_TYPE&&motion!=FOLLOW_MOTION_TYPE&&!OwnsWarsongMovement()){m_wsgMotionReason="foreign_motion";return true;}
    if((m_wsgTask==WsgTask::Midfield || m_wsgTask==WsgTask::Escort || m_wsgTask==WsgTask::Recover) && me->GetDistance(m_wsgGoal)<WsgStandOff(m_wsgTask,GetRole()==ROLE_HEALER,m_wsgCarrier.IsEmpty()))
    {ClearPath();StopMoving();return true;}
    if(UseMount()){ClearPath();return true;}
    m_wsgPathGoal=m_wsgGoal;m_wsgRepathAt=TacticalNow()+3000;
    m_wsgProgressPosition=me->GetPosition();m_wsgProgressAt=TacticalNow();
    // The authored endpoint heuristic is not a shortest-path algorithm. Objective
    // movement uses the navigation mesh, without selecting a scenic whole path.
    PathInfo route(me);route.ExcludeSteepSlopes();
    if(!route.calculate(m_wsgGoal.x,m_wsgGoal.y,m_wsgGoal.z)||route.getPathType()!=PATHFIND_NORMAL||route.getPath().empty()||(route.getPath().back()-Movement::Vector3{m_wsgGoal.x,m_wsgGoal.y,m_wsgGoal.z}).length()>4){m_wsgMotionReason="nav_unavailable";return false;}
    m_wsgMotionGoal=m_wsgGoal;me->GetMotionMaster()->MovePoint(0x5753474f,m_wsgGoal.x,m_wsgGoal.y,m_wsgGoal.z,MOVE_PATHFINDING|MOVE_EXCLUDE_STEEP_SLOPES);
    m_wsgMotionSpline=me->movespline->GetId();m_wsgMotionReason="native_objective";
    return true;
}
bool BattleBotAI::UpdateWarsongMovement()
{
    using namespace BattleBotTactics;
    BattleGround* bg=me->GetBattleGround();
    if(!bg || bg->GetTypeID()!=BATTLEGROUND_WS || bg->GetStatus()!=STATUS_IN_PROGRESS )return false;
    auto sceneMotion=me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if(sceneMotion!=IDLE_MOTION_TYPE&&sceneMotion!=CHASE_MOTION_TYPE&&sceneMotion!=FOLLOW_MOTION_TYPE&&!OwnsWarsongMovement()&&!OwnsLegacyWaypointMovement()){m_wsgMotionReason="foreign_motion";return true;}
    // A late join/recovered actor already outside the flag room must not walk
    // backwards through the opening route. This route is only an opening action.
    Position home=me->GetBGTeam()==ALLIANCE ? WS_FLAG_POS_ALLIANCE : WS_FLAG_POS_HORDE;
    if(!m_wsgOpeningDone && !m_currentPath && me->GetDistance(home)>100.0f)m_wsgOpeningDone=true;
    if(!m_wsgOpeningDone && !HasWsgFlag(me)){
        auto openingMotion=me->GetMotionMaster()->GetCurrentMovementGeneratorType();
        if(openingMotion!=IDLE_MOTION_TYPE&&openingMotion!=CHASE_MOTION_TYPE&&openingMotion!=FOLLOW_MOTION_TYPE&&!OwnsLegacyWaypointMovement()&&!OwnsWarsongMovement()){m_wsgMotionReason="foreign_motion";return true;}
        if(me->IsNonMeleeSpellCasted(false,false,true)||me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))return true;
        // The current location decides outdoors legality; mount as soon as the
        // ordinary cast saves journey time, not only after reaching the GY.
        if(!me->IsMounted()&&UseMount()){ClearPath();return true;}
        Position exit=me->GetBGTeam()==ALLIANCE ? Position{1415.33f,1554.79f,343.156f,0} : Position{1029.14f,1387.49f,340.836f,0};
        if(me->GetDistance(exit)<12.0f){m_wsgOpeningDone=true;ClearPath();StopMoving();if(UseMount())return true;}
        else {
            if(!m_currentPath){m_currentPath=me->GetBGTeam()==ALLIANCE ? &vPath_WSG_AllianceFlagRoom_to_AllianceGraveyard : &vPath_WSG_HordeFlagRoom_to_HordeGraveyard;
                unsigned nearest=1;float distance=FLT_MAX;for(unsigned i=1;i<m_currentPath->size();++i){auto const& point=m_currentPath->at(i);float d=me->GetDistance(point.x,point.y,point.z);if(d<distance){distance=d;nearest=i;}}
                m_currentPoint=nearest-1;m_movingInReverse=false;MoveToNextPoint();}
            return true;
        }
    }
    if(!m_wsgGoalValid){if(HasWsgFlag(me)){ActivateNearbyAreaTrigger();if(!me->IsNonMeleeSpellCasted(false,false,true))UpdateFlagCarrierAI();}if(OwnsWarsongMovement()){ClearPath();StopMoving();}m_wsgTravel=false;return true;}
    if(HasWsgFlag(me)){
        ActivateNearbyAreaTrigger();
        if(!HasWsgFlag(me)){m_wsgGoalValid=false;return true;}
        if(!me->IsNonMeleeSpellCasted(false,false,true))UpdateFlagCarrierAI();
        if(me->IsNonMeleeSpellCasted(false,false,true)||me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))return true;
        if(me->GetVictim())StopTacticalAttack(me);
        if(me->GetClass()==CLASS_DRUID){
            SpellEntry const* form=me->GetTerrain()->IsOutdoors(me->GetPositionX(),me->GetPositionY(),me->GetPositionZ()) && m_spells.druid.pTravelForm ? m_spells.druid.pTravelForm : m_spells.druid.pCatForm;
            if(form&&!me->HasAura(form->Id)&&BattleBotPolicyRuntime::CanLegacy(*this,form)){me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);if(CanTryToCastSpell(me,form)&&DoCastSpell(me,form)==SPELL_CAST_OK)return true;}
        }
        // If our flag is away, hold the return point rather than oscillating
        // between route endpoints. The native trigger above scores once legal.
        auto ws=static_cast<BattleGroundWS*>(bg);
        Team ours=me->GetBGTeam()==ALLIANCE ? ALLIANCE : HORDE;
        if(ws->GetFlagState(ours)!=BG_WS_FLAG_STATE_ON_BASE && me->GetDistance(m_wsgGoal)<=3.0f){
            ClearPath();StopMoving();return true;
        }
        auto now=TacticalNow();
        bool refresh=BattleBotTactics::RouteNeedsRefresh(me->IsMoving(),me->GetDistance(m_wsgProgressPosition),m_wsgProgressAt?now-m_wsgProgressAt:0);
        if(m_wsgProgressAt && now-m_wsgProgressAt>=6000){m_wsgProgressPosition=me->GetPosition();m_wsgProgressAt=now;}
        bool reroute=me->GetDistance(m_wsgPathGoal)>3&&((m_wsgGoal.x-m_wsgPathGoal.x)*(m_wsgGoal.x-m_wsgPathGoal.x)+(m_wsgGoal.y-m_wsgPathGoal.y)*(m_wsgGoal.y-m_wsgPathGoal.y)+(m_wsgGoal.z-m_wsgPathGoal.z)*(m_wsgGoal.z-m_wsgPathGoal.z)>9);
        if(refresh||reroute){ClearPath();if(OwnsWarsongMovement())StopMoving();StartWarsongObjective();}return true;
    }
    if(me->IsNonMeleeSpellCasted(false,false,true) || me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE)){m_wsgProgressAt=0;return false;}
    // Reissue a failed navigation request even when the objective has not moved.
    // Never teleport or change the match state to recover a blocked actor.
    if(m_wsgTravel && m_wsgProgressAt && TacticalNow()-m_wsgProgressAt>=6000){
        bool stuck=me->GetDistance(m_wsgProgressPosition)<1.0f;
        m_wsgProgressPosition=me->GetPosition();m_wsgProgressAt=TacticalNow();
        if(stuck){ClearPath();StopMoving();StartWarsongObjective();return true;}
    }
    float distance=me->GetDistance(m_wsgGoal);
    // Fight a visible carrier or its attackers once in supporting distance.
    if(Unit* target=SelectTacticalTarget())
    {
        Rank rank;if(TacticalRank(target,rank) && (rank.tier<=1 || m_wsgTask==WsgTask::Midfield) && (m_wsgTask==WsgTask::Recover || distance<=30.0f))
        {m_wsgTravel=false;return false;}
    }
    float support=m_wsgTask==WsgTask::Capture?2.f:WsgStandOff(m_wsgTask,GetRole()==ROLE_HEALER,m_wsgCarrier.IsEmpty());
    if(distance<=support){if(m_wsgTravel){ClearPath();StopMoving();}m_wsgTravel=false;UpdateBattleGroundAI();return false;}
    if(me->GetVictim())StopTacticalAttack(me);
    if(!m_wsgTravel){ClearPath();StopMoving();m_wsgTravel=true;}
    if((!me->IsMoving() && !m_currentPath) || (TacticalNow()>=m_wsgRepathAt && ((m_wsgGoal.x-m_wsgPathGoal.x)*(m_wsgGoal.x-m_wsgPathGoal.x)+(m_wsgGoal.y-m_wsgPathGoal.y)*(m_wsgGoal.y-m_wsgPathGoal.y)+(m_wsgGoal.z-m_wsgPathGoal.z)*(m_wsgGoal.z-m_wsgPathGoal.z)>225.0f)))
    {ClearPath();StopMoving();StartWarsongObjective();}
    return true;
}
void BattleBotAI::RefreshArathiGuardAssignment()
{
    int32 previousNode=m_arathiGuardNode;
    m_arathiGuardNode=-1;m_arathiGuardAnchorValid=false;
    BattleGround* bg=me ? me->GetBattleGround() : nullptr;
    if(!bg || bg->GetTypeID()!=BATTLEGROUND_AB || bg->GetStatus()!=STATUS_IN_PROGRESS)return;
    std::array<bool,5> occupied{};std::array<Position,5> anchors;
    uint8 state=BG_AB_NODE_TYPE_OCCUPIED+BattleGround::GetTeamIndexByTeamId(me->GetBGTeam());
    for(unsigned node=0;node<5;++node)
    {
        uint8 active=bg->IsActiveEvent(node,state) ? state : uint8(BG_AB_NODE_TYPE_CONTESTED+BattleGround::GetTeamIndexByTeamId(me->GetBGTeam()));
        if(!bg->IsActiveEvent(node,active))continue;
        if(GameObject* go=ResolveArathiBanner(bg,me->GetMap(),node,active)){anchors[node]=go->GetPosition();occupied[node]=true;}
    }
    std::vector<BattleBotTactics::GuardCandidate> candidates;
    for(auto const& entry:bg->GetPlayers())
    {
        Player* player=ObjectAccessor::FindPlayer(entry.first);
        if(!player || !player->IsInWorld() || !player->IsAlive() || !player->IsBot() || player->GetBattleGround()!=bg || player->GetBGTeam()!=me->GetBGTeam())continue;
        auto const* ai=player->AI() ? dynamic_cast<BattleBotAI const*>(player->AI()) : nullptr;
        if(!ai || !ai->m_initialized)continue;
        BattleBotTactics::GuardCandidate candidate;candidate.guid=player->GetObjectGuid().GetRawValue();candidate.healer=ai->GetRole()==ROLE_HEALER;
        for(unsigned node=0;node<5;++node)candidate.distance[node]=occupied[node] ? player->GetDistance(anchors[node]) : std::numeric_limits<float>::infinity();
        candidates.push_back(candidate);
    }
    auto now=TacticalNow();
    std::lock_guard<std::mutex> lock(arathiGuardMutex);
    for(auto it=arathiGuards.begin();it!=arathiGuards.end();)if(now-it->second.touched>600000)it=arathiGuards.erase(it);else ++it;
    auto& stateCache=arathiGuards[{bg->GetInstanceID(),uint32(me->GetBGTeam())}];
    stateCache.assignments=BattleBotTactics::AssignGuards(occupied,stateCache.assignments,candidates);stateCache.touched=now;
    for(unsigned node=0;node<5;++node)if(occupied[node] && stateCache.assignments[node]==me->GetObjectGuid().GetRawValue())
    {m_arathiGuardNode=int32(node);m_arathiGuardAnchor=anchors[node];m_arathiGuardAnchorValid=true;break;}
    m_arathiGuardChanged=m_arathiGuardNode!=previousNode;
}
bool BattleBotAI::UpdateArathiGuardMovement()
{
    if(!m_arathiGuardAnchorValid || !me->IsAlive())return false;
    if(m_arathiGuardChanged){ClearPath();StopMoving();m_arathiGuardChanged=false;}
    Unit* victim=me->GetVictim();
    if(victim && !BattleBotTactics::InGuardLeash(victim->GetDistance(m_arathiGuardAnchor)))
    {StopTacticalAttack(me);ClearPath();StopMoving();}
    float distance=me->GetDistance(m_arathiGuardAnchor);
    if(distance>25.0f)
    {
        if(me->GetVictim())StopTacticalAttack(me);
        if(!me->IsMoving() || me->GetMotionMaster()->GetCurrentMovementGeneratorType()==CHASE_MOTION_TYPE || me->GetMotionMaster()->GetCurrentMovementGeneratorType()==FOLLOW_MOTION_TYPE)
        {
            ClearPath();StopMoving();
            if(!StartNewPathToPosition(m_arathiGuardAnchor,vPaths_AB))
                me->GetMotionMaster()->MovePoint(0,m_arathiGuardAnchor.x,m_arathiGuardAnchor.y,m_arathiGuardAnchor.z,MOVE_PATHFINDING|MOVE_EXCLUDE_STEEP_SLOPES);
        }
        return true;
    }
    if(!me->GetVictim() && !me->IsInCombat()) { ClearPath();StopMoving();me->GetMotionMaster()->MoveIdle(); }
    return false;
}
bool BattleBotAI::OwnsArathiAdvanceMovement()
{
    auto* point=dynamic_cast<PointMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent());
    if(!point||point->GetMovementGeneratorType()!=POINT_MOTION_TYPE||point->GetId()!=m_abAdvancePoint||(m_abAdvancePoint!=0x41424c44&&m_abAdvancePoint!=0x41424144))return false;
    float x,y,z;point->GetDestination(x,y,z);
    if(std::abs(x-m_abAdvanceDestination.x)>.01f||std::abs(y-m_abAdvanceDestination.y)>.01f||std::abs(z-m_abAdvanceDestination.z)>.01f)return false;
    // Native speed changes relaunch this same owned point goal with a new spline.
    // Adopt only its exact private ID+destination, never a foreign generator.
    m_abLandSpline=me->movespline->GetId();return true;
}

std::vector<std::string> BattleBotAI::ArathiRouteStatus(Player* player)
{
    std::vector<std::string> rows;if(!player||!player->GetBattleGround()||player->GetBattleGround()->GetTypeID()!=BATTLEGROUND_AB)return rows;
    if(player->GetMapId()!=player->GetBattleGround()->GetMapId()||player->GetInstanceId()!=player->GetBattleGround()->GetInstanceID())return rows;
    std::ostringstream context;context<<"GMUI|ABROUTEACTOR|guid="<<player->GetGUID()<<" map="<<player->GetMapId()<<" instance="<<player->GetInstanceId();rows.push_back(context.str());
    Position nodes[3]={{1166.79f,1200.13f,-56.7086f,0},{977.08f,1046.54f,-44.8303f,0},{806.182f,874.272f,-55.9937f,0}};
    for(auto pair:{std::make_pair(1u,0u),std::make_pair(0u,1u),std::make_pair(1u,2u),std::make_pair(2u,1u)})for(bool land:{false,true}){
        auto from=nodes[pair.first],to=nodes[pair.second];PathInfo path(player);path.ExcludeSteepSlopes();if(land)path.ExcludeLiquids();
        bool calculated=path.calculate(Movement::Vector3{from.x,from.y,from.z},Movement::Vector3{to.x,to.y,to.z});
        std::ostringstream out;out<<"GMUI|ABROUTE|from="<<pair.first<<" to="<<pair.second<<" filter="<<(land?"land":"native")<<" calculated="<<calculated<<" type="<<uint32(path.getPathType())<<" length="<<(path.getPath().size()>1?path.Length():0)<<" points="<<path.getPath().size()<<" dry="<<ArathiDryPath(player,path.getPath(),false,0);rows.push_back(out.str());
    }
    bool wet=player->GetTerrain()->IsInWater(player->GetPositionX(),player->GetPositionY(),player->GetPositionZ());Position goal;float limit=20;bool found=wet&&ArathiEgress(player,nodes[1],goal,limit);
    PathInfo escape(player);escape.ExcludeSteepSlopes();bool escapeCalculated=found&&escape.calculate(goal.x,goal.y,goal.z);
    std::ostringstream out;out<<"GMUI|ABEGRESS|wetStart="<<wet<<" found="<<found<<" mode="<<(limit<=20?"bounded20":"native_first_dry")<<" prefixLimit="<<limit<<" type="<<uint32(escape.getPathType())<<" length="<<(escape.getPath().size()>1?escape.Length():0)<<" safe="<<(escapeCalculated&&escape.getPathType()==PATHFIND_NORMAL&&ArathiDryPath(player,escape.getPath(),true,limit))<<" x="<<(found?goal.x:0)<<" y="<<(found?goal.y:0)<<" z="<<(found?goal.z:0);rows.push_back(out.str());return rows;
}
bool BattleBotAI::TryCaptureArathiFlag()
{
 using namespace BattleBotTactics;
 auto* bg=me->GetBattleGround();if(!bg||bg->GetTypeID()!=BATTLEGROUND_AB||bg->GetStatus()!=STATUS_IN_PROGRESS)return false;
 uint64 now=TacticalNow();uint32 hp=me->GetHealth();
 if(m_abCaptureHealth&&hp<m_abCaptureHealth)m_abCaptureHurtAt=now;
 m_abCaptureHealth=hp;
 auto key=std::make_pair(bg->GetInstanceID(),uint32(me->GetBGTeam()));
 if(Spell* current=me->GetCurrentSpell(CURRENT_GENERIC_SPELL))if(current->m_spellInfo->Id==21651)return true;
 if(m_abCaptureActiveFlag){
  std::lock_guard<std::mutex> lock(arathiGuardMutex);auto& shared=arathiGuards[key];
  for(auto& lease:shared.capture)if(lease.owner==me->GetGUID()&&lease.flag==m_abCaptureActiveFlag)lease={};
  m_abCaptureEnd="unclassified_cast_ended";m_abCaptureActiveFlag=0;m_abCaptureRetryAt=now+3000;
 }
 if(!me->IsAlive()||me->IsNonMeleeSpellCasted()||me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)||now<m_abCaptureRetryAt)return false;
 unsigned team=BattleGround::GetTeamIndexByTeamId(me->GetBGTeam());
 for(unsigned node=0;node<5;++node)for(unsigned state=0;state<5;++state){
  if(state==1+team||state==3+team||!bg->IsActiveEvent(node,state))continue;
  auto* flag=ResolveArathiBanner(bg,me->GetMap(),node,state);auto flagGuid=flag?flag->GetObjectGuid():ObjectGuid();
  if(!flag||!flag->isSpawned()||flag->GetGoState()!=GO_STATE_READY||!flag->IsAtInteractDistance(me)||me->GetReactionTo(flag)<REP_NEUTRAL)continue;
  AbCaptureRisk risk;risk.healer=GetRole()==ROLE_HEALER;risk.combat=me->IsInCombat();risk.threatened=false;
  risk.periodic=me->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE)||me->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT)||me->HasAuraType(SPELL_AURA_PERIODIC_LEECH);
  risk.recentDamage=m_abCaptureHurtAt&&now-m_abCaptureHurtAt<3000;
  std::list<Unit*> enemies;me->GetEnemyListInRadiusAround(me,40,enemies);
  for(Unit* enemy:enemies)if(enemy->IsAlive()&&me->IsValidAttackTarget(enemy)&&enemy->IsVisibleForOrDetect(me,me,false)&&me->IsWithinLOSInMap(enemy)){
   ++risk.enemies;if(Threatens(enemy,me))risk.threatened=true;
   for(CurrentSpellTypes lane:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL})if(IsArathiOpening(me,enemy,enemy->GetCurrentSpell(lane)))risk.threatened=true;
  }
  std::list<Player*> local;me->GetAlivePlayerListInRange(me,local,40);
  uint64 best=0;
  for(Player* p:local){
   if(p->GetBattleGround()!=bg||p->IsGameMaster()||!p->IsVisibleForOrDetect(me,me,false)||!me->IsWithinLOSInMap(p))continue;
   if(p->GetBGTeam()!=me->GetBGTeam())continue;
   ++risk.friends;
   if(Spell* cast=p->GetCurrentSpell(CURRENT_GENERIC_SPELL))if(cast->m_spellInfo->Id==21651&&cast->m_targets.getGOTargetGuid()==flagGuid){
    // A real or already-started friendly opener owns this action; everyone else protects it.
    if(p!=me)return false;
   }
   auto* ai=p->AI()?dynamic_cast<BattleBotAI*>(p->AI()):nullptr;
   if(!p->IsBot()||!ai||ai->GetRole()==ROLE_HEALER||p->IsNonMeleeSpellCasted()||p->GetHealthPercent()<80||!p->GetAttackers().empty()||p->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)||p->IsTotalImmune()||p->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE)||p->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT)||p->HasAuraType(SPELL_AURA_PERIODIC_LEECH)||!AbCaptureCandidateReady(now,ai->m_abCaptureRetryAt,ai->m_abCaptureHurtAt)||!flag->IsAtInteractDistance(p))continue;
   if(!best||p->GetGUID()<best)best=p->GetGUID();
  }
  if(!AbCaptureOpportunity(risk)||(risk.combat&&risk.enemies&&best!=me->GetGUID())){m_abCaptureTrace="risk_or_other_opener";continue;}
  {std::lock_guard<std::mutex> lock(arathiGuardMutex);auto& lease=arathiGuards[key].capture[node];
   if(!AbClaimCapture(lease,me->GetGUID(),flagGuid.GetRawValue(),now))return false;
  }
  me->RemoveAurasDueToSpell(BB_SPELL_FOOD);me->RemoveAurasDueToSpell(BB_SPELL_DRINK);
  me->SetStandState(UNIT_STAND_STATE_STAND);m_isBuffing=false;
  ClearPath();StopMoving();me->AttackStop();me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL,true); // no CombatStop: native combat and incoming damage remain authoritative.
  me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
  if(me->IsInDisallowedMountForm())me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
  m_abCaptureAttemptAt=now;SpellCastResult result=me->CastSpell(flag,21651,false);m_abCaptureResult=int32(result);m_abCaptureTrace="native_"+std::to_string(uint32(result));
  if(result==SPELL_CAST_OK){m_abCaptureEnd="pending_unobserved";m_abCaptureActiveFlag=flagGuid.GetRawValue();return true;}
  {std::lock_guard<std::mutex> lock(arathiGuardMutex);auto& lease=arathiGuards[key].capture[node];if(lease.owner==me->GetGUID())lease={};}
  m_abCaptureRetryAt=now+3000;return false;
 }
 return false;
}
bool BattleBotAI::TryCaptureNearbyBattleFlag()
{
    if(me->GetBattleGround()&&me->GetBattleGround()->GetTypeID()==BATTLEGROUND_AB)return TryCaptureArathiFlag();
    BattleGround* bg=me->GetBattleGround();
    if(!bg || bg->GetStatus()!=STATUS_IN_PROGRESS || me->IsInCombat() || me->IsNonMeleeSpellCasted() || me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))return false;
    if(bg->GetTypeID()!=BATTLEGROUND_AB && bg->GetTypeID()!=BATTLEGROUND_AV)return false;
    std::list<Player*> nearby;me->GetAlivePlayerListInRange(me,nearby,80.0f);
    for(Player* enemy:nearby)if(enemy->GetBattleGround()==bg && enemy->GetBGTeam()!=me->GetBGTeam() && me->IsValidAttackTarget(enemy) && enemy->IsVisibleForOrDetect(me,me,false) && me->IsWithinLOSInMap(enemy))return false;
    auto const& entries=bg->GetTypeID()==BATTLEGROUND_AB?vFlagsAB:vFlagsAV;
    for(uint32 entry:entries)if(GameObject* flag=me->FindNearestGameObject(entry,INTERACTION_DISTANCE)){
        if(!flag->isSpawned() || me->GetReactionTo(flag)<REP_NEUTRAL)continue;
        if(bg->GetTypeID()==BATTLEGROUND_AB){
            bool ours=false;unsigned team=BattleGround::GetTeamIndexByTeamId(me->GetBGTeam());
            for(unsigned n=0;n<5;++n)for(unsigned st:{1+team,3+team})if(bg->IsActiveEvent(n,st)&&ResolveArathiBanner(bg,me->GetMap(),n,st)==flag)ours=true;
            if(ours)continue;
        }
        Player* friend_=me->FindNearestFriendlyPlayer(INTERACTION_DISTANCE);
        Spell* cast=friend_?friend_->GetCurrentSpell(CURRENT_GENERIC_SPELL):nullptr;
        if(cast && cast->m_spellInfo->Id==21651)return true;
        ClearPath();StopMoving();me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
        if(me->IsInDisallowedMountForm())me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        return me->CastSpell(flag,21651,false)==SPELL_CAST_OK;
    }
    return false;
}
bool BattleBotAI::UpdateArathiAdvanceMovement()
{
    using namespace BattleBotTactics;
    BattleGround* bg=me->GetBattleGround();
    if(!bg || bg->GetTypeID()!=BATTLEGROUND_AB || bg->GetStatus()!=STATUS_IN_PROGRESS)return false;
    // Assigned defenders never enter the old follow/random-route fallback.
    if(m_arathiGuardAnchorValid)return true;
    if(me->IsNonMeleeSpellCasted() || me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))return true;
    auto currentMotion=me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if(currentMotion!=IDLE_MOTION_TYPE&&currentMotion!=CHASE_MOTION_TYPE&&currentMotion!=FOLLOW_MOTION_TYPE&&!OwnsArathiAdvanceMovement()){m_abLandTrace="foreign_motion";return true;}
    static Position const authored[5]={{1169.52f,1198.71f,-56.2742f,0},{978.122f,1043.87f,-44.4682f,0},{803.826f,874.909f,-55.2547f,0},{857.674f,1146.16f,11.529f,0},{1145.14f,849.895f,-110.523f,0}};
    std::array<AbState,5> states;std::array<Position,5> goals;std::array<ObjectGuid,5> flags;
    unsigned friendly=BattleGround::GetTeamIndexByTeamId(me->GetBGTeam());
    for(unsigned n=0;n<5;++n){
        goals[n]=authored[n];states[n]=AbState::Neutral;
        for(unsigned st=0;st<=4;++st)if(bg->IsActiveEvent(n,st)){
            states[n]=st==0?AbState::Neutral:st==1+friendly?AbState::FriendlyContested:st==3+friendly?AbState::FriendlyOccupied:st<3?AbState::EnemyContested:AbState::EnemyOccupied;
            if(GameObject* go=ResolveArathiBanner(bg,me->GetMap(),n,st)){flags[n]=go->GetObjectGuid();goals[n]=go->GetPosition();}
        }
    }
    auto now=TacticalNow();
    if(now>=m_arathiPlanAt || (m_arathiAdvanceNode>=0 && AbSecured(states[m_arathiAdvanceNode]))){
        std::array<float,5> costs;costs.fill(std::numeric_limits<float>::infinity());
        for(unsigned n=0;n<5;++n){
            PathInfo path(me);path.calculate(goals[n].x,goals[n].y,goals[n].z);
            if(path.getPathType()==PATHFIND_NORMAL)costs[n]=path.Length();
        }
        int side;
        {std::lock_guard<std::mutex> lock(arathiGuardMutex);
         auto& shared=arathiGuards[{bg->GetInstanceID(),uint32(me->GetBGTeam())}];
         if(shared.side<0 || (now>=shared.sideAt && !std::isfinite(costs[shared.side]))){
             shared.side=AbChooseSide(states,costs,shared.side);shared.sideAt=now+15000;
         }
         shared.touched=now;side=shared.side;
        }
        int next=(!m_abOpeningComplete&&m_abOpeningNode>=0&&!AbSecured(states[m_abOpeningNode])&&std::isfinite(costs[m_abOpeningNode]))?m_abOpeningNode:AbAdvanceNode(me->GetBGTeam()==ALLIANCE,states,costs,side);
        if(next!=m_arathiAdvanceNode){if(OwnsArathiAdvanceMovement()){ClearPath();StopMoving();}m_arathiMoveAt=0;m_abLandRoute=false;m_abLandViaValid=false;}
        m_arathiAdvanceNode=next;m_arathiAdvanceValid=true;
        // All five secured: a central reserve still responds to nearby combat.
        m_arathiAdvanceGoal=next>=0?goals[next]:authored[1];m_arathiPlanAt=now+5000;
    }
    if(!m_arathiAdvanceValid)return true;
    if(TryCaptureArathiFlag())return true;
    if(Unit* target=SelectTacticalTarget()){if(me->GetVictim()!=target)AttackStart(target);return true;}
    if(me->GetVictim())StopTacticalAttack(me);
    // Target selection has rejected the old combat/follow purpose. A moving
    // obsolete chase can keep making progress forever, so do not wait for stall.
    auto obsoleteMotion=me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if(obsoleteMotion==CHASE_MOTION_TYPE||obsoleteMotion==FOLLOW_MOTION_TYPE){ClearPath();StopMoving();m_arathiMoveAt=0;m_arathiProgressAt=0;}
    float distance=me->GetDistance(m_arathiAdvanceGoal);
    if(m_arathiAdvanceNode>=0 && distance<=INTERACTION_DISTANCE){
        ClearPath();StopMoving();
        TryCaptureArathiFlag();
        return true;
    }
    if(m_arathiAdvanceNode<0 && distance<=20){ClearPath();StopMoving();return true;}
    if(distance>25 && UseMount())return true;
    bool refresh=BattleBotTactics::RouteNeedsRefresh(me->IsMoving(),me->GetDistance(m_arathiProgress),m_arathiProgressAt?now-m_arathiProgressAt:0);
    if(m_arathiProgressAt && now-m_arathiProgressAt>=6000){m_arathiProgress=me->GetPosition();m_arathiProgressAt=now;}
    // Keep the BS/home corridor on real land navigation; never change a speed or pose.
    bool home=m_arathiAdvanceNode==0||m_arathiAdvanceNode==2;
    bool fromHome=me->GetDistance(authored[0])<180||me->GetDistance(authored[2])<180;
    if(!m_abLandRoute&&((home&&me->GetDistance(authored[1])<250)||(m_arathiAdvanceNode==1&&fromHome))){m_abLandRoute=true;m_abLandEscape=me->GetTerrain()->IsInWater(me->GetPositionX(),me->GetPositionY(),me->GetPositionZ());}
    m_abLandEscape=m_abLandRoute&&me->GetTerrain()->IsInWater(me->GetPositionX(),me->GetPositionY(),me->GetPositionZ());
    if(!m_abLandEscape)m_abLandEscapeLimit=20;
    auto dry=[&](Movement::PointsArray const& points){return ArathiDryPath(me,points,m_abLandEscape,m_abLandEscapeLimit);};
    auto* point=dynamic_cast<PointMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent());
    if(m_abLandRoute&&point&&point->GetId()==0x41424c44&&me->movespline->GetId()==m_abLandSpline&&!me->movespline->Finalized()){
        Movement::PointsArray remaining;auto current=me->movespline->ComputePosition();remaining.push_back({current.x,current.y,current.z});
        for(int32 i=me->movespline->_currentSplineIdx()+1;i<=int32(me->movespline->CountSplinePoints());++i)remaining.push_back(me->movespline->GetPoint(i));
        if(!dry(remaining)){ClearPath();StopMoving();m_abLandTrace="actual_wet_replan";m_arathiMoveAt=now+1000;return true;}
    }
    if(m_abLandViaValid&&me->GetDistance(m_abLandVia)<4){m_abLandViaValid=false;m_arathiMoveAt=0;refresh=true;}
    if(refresh && now>=m_arathiMoveAt){
        auto motion=me->GetMotionMaster()->GetCurrentMovementGeneratorType();
        if(motion!=IDLE_MOTION_TYPE&&motion!=CHASE_MOTION_TYPE&&motion!=FOLLOW_MOTION_TYPE&&!OwnsArathiAdvanceMovement()){m_abLandTrace="foreign_motion";return true;}
        Position moveGoal=m_abLandViaValid?m_abLandVia:m_arathiAdvanceGoal;
        if(m_abLandEscape){
            if(!ArathiEgress(me,m_arathiAdvanceGoal,moveGoal,m_abLandEscapeLimit)){m_abLandTrace="native_egress_unavailable";m_arathiMoveAt=now+5000;return false;}
            m_abLandVia=moveGoal;m_abLandViaValid=true;
        }
        PathInfo path(me);path.ExcludeSteepSlopes();if(m_abLandRoute&&!m_abLandEscape)path.ExcludeLiquids();
        bool valid=path.calculate(moveGoal.x,moveGoal.y,moveGoal.z)&&path.getPathType()==PATHFIND_NORMAL&&(!m_abLandRoute||dry(path.getPath()));
        if(!valid&&m_abLandRoute&&!m_abLandEscape&&!m_abLandViaValid){
            float dx=m_arathiAdvanceGoal.x-me->GetPositionX(),dy=m_arathiAdvanceGoal.y-me->GetPositionY(),length=std::sqrt(dx*dx+dy*dy),best=std::numeric_limits<float>::infinity();
            if(length>1)for(float offset:{35.f,-35.f,70.f,-70.f}){
                float x=(m_arathiAdvanceGoal.x+me->GetPositionX())*.5f-dy/length*offset,y=(m_arathiAdvanceGoal.y+me->GetPositionY())*.5f+dx/length*offset,z;
                float px,py;if(!me->GetRandomPoint(x,y,me->GetPositionZ(),2,px,py,z)||me->GetTerrain()->IsInWater(px,py,z))continue;
                PathInfo first(me),second(me);first.ExcludeSteepSlopes();first.ExcludeLiquids();second.ExcludeSteepSlopes();second.ExcludeLiquids();
                if(!first.calculate(px,py,z)||first.getPathType()!=PATHFIND_NORMAL||!dry(first.getPath()))continue;
                if(!second.calculate(Movement::Vector3{px,py,z},Movement::Vector3{m_arathiAdvanceGoal.x,m_arathiAdvanceGoal.y,m_arathiAdvanceGoal.z})||second.getPathType()!=PATHFIND_NORMAL||!dry(second.getPath()))continue;
                float total=first.Length()+second.Length();if(total<best&&total<=length*2.5f+40){best=total;moveGoal={px,py,z,0};m_abLandVia=moveGoal;m_abLandViaValid=true;}
            }
            if(m_abLandViaValid)valid=path.calculate(moveGoal.x,moveGoal.y,moveGoal.z)&&path.getPathType()==PATHFIND_NORMAL&&dry(path.getPath());
        }
        if(valid){
            ClearPath();StopMoving();
            m_abAdvancePoint=m_abLandRoute?0x41424c44:0x41424144;m_abAdvanceDestination=moveGoal;
            me->GetMotionMaster()->MovePoint(m_abAdvancePoint,moveGoal.x,moveGoal.y,moveGoal.z,MOVE_PATHFINDING|MOVE_EXCLUDE_STEEP_SLOPES|(m_abLandRoute&&!m_abLandEscape?MOVE_EXCLUDE_LIQUID:0));
            m_abLandSpline=me->movespline->GetId();m_abLandTrace=m_abLandEscape?(m_abLandEscapeLimit<=20?"egress20":"native_egress"):m_abLandRoute?(m_abLandViaValid?"dry_via":"dry_direct"):"native";
            if(m_abLandRoute&&(!me->movespline->Initialized()||me->movespline->Finalized()))m_abLandTrace="launch_no_motion";
            if(m_abLandRoute&&me->movespline->Initialized()&&!me->movespline->Finalized()){
                Movement::PointsArray actual;auto current=me->movespline->ComputePosition();actual.push_back({current.x,current.y,current.z});
                for(int32 i=me->movespline->_currentSplineIdx()+1;i<=int32(me->movespline->CountSplinePoints());++i)actual.push_back(me->movespline->GetPoint(i));
                if(!dry(actual)){ClearPath();StopMoving();m_abLandTrace="launch_wet_rejected";}
            }
            m_arathiProgress=me->GetPosition();m_arathiProgressAt=now;
        }else {m_abLandTrace="no_dry_path";m_abLandViaValid=false;if(m_abLandRoute&&point&&point->GetId()==0x41424c44&&me->movespline->GetId()==m_abLandSpline){ClearPath();StopMoving();}}
        m_arathiMoveAt=now+5000;
    }
    return true;
}
bool BattleBotAI::TacticalRank(Unit* target,BattleBotTactics::Rank& rank) const
{
    if(!IsTacticalBattleGround() || !target || target==me || !target->IsAlive() || target->GetMap()!=me->GetMap())return false;
    BattleGround* bg=me->GetBattleGround();
    if(bg->GetStatus()!=STATUS_IN_PROGRESS)return false;
    if(bg->GetTypeID()==BATTLEGROUND_WS && HasWsgFlag(me) && !me->IsRooted())return false;
    Player* enemy=target->GetAffectingPlayer();
    if(enemy && (enemy->GetBattleGround()!=bg || enemy->GetBGTeam()==me->GetBGTeam()))return false;
    bool enemyCarrier=bg->GetTypeID()==BATTLEGROUND_WS && (me->GetBGTeam()==HORDE ? target->HasAura(AURA_WARSONG_FLAG) : target->HasAura(AURA_SILVERWING_FLAG));
    if(!me->IsValidAttackTarget(target) || !target->IsVisibleForOrDetect(me,me,false) || target->IsTotalImmune() || target->GetTransport()!=me->GetTransport() || (!enemyCarrier && target->HasBreakableByDamageCrowdControlAura()))return false;
    if(bg->GetTypeID()==BATTLEGROUND_WS && !BattleBotTactics::WsgAttackAllowed(GetRole()==ROLE_HEALER,Threatens(target,me),me->GetDistance(target)))return false;
    float range=80.0f;
    if(m_arathiGuardAnchorValid)range=std::min(range,30.0f);
    if(!me->IsWithinDistInMap(target,range) || me->GetDistanceZ(target)>10.0f || !me->IsWithinLOSInMap(target))return false;
    if(m_arathiGuardAnchorValid && !BattleBotTactics::InGuardLeash(target->GetDistance(m_arathiGuardAnchor)))return false;
    bool humanThreat=false,carrierThreat=false,capper=false;
    std::list<Player*> nearby;me->GetAlivePlayerListInRange(me,nearby,80.0f);
    for(Player* friendly:nearby)
    {
        if(friendly->GetBattleGround()!=bg || friendly->GetBGTeam()!=me->GetBGTeam() || friendly->IsGameMaster())continue;
        if(Threatens(target,friendly))
        {
            if(!friendly->IsBot())humanThreat=true;
            if(bg->GetTypeID()==BATTLEGROUND_WS && HasWsgFlag(friendly))carrierThreat=true;
        }
    }
    if(bg->GetTypeID()==BATTLEGROUND_AB && target->IsPlayer())
        for(CurrentSpellTypes lane:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL})
            if(IsArathiOpening(me,target,target->GetCurrentSpell(lane)))capper=true;
    if(bg->GetTypeID()==BATTLEGROUND_WS && m_wsgGoalValid &&
        !BattleBotTactics::WsgObjectiveCombatAllowed(m_wsgTask,target->GetDistance(m_wsgGoal),me->GetDistance(m_wsgGoal),Threatens(target,me)))return false;
    if(bg->GetTypeID()==BATTLEGROUND_AB && !capper && !m_arathiGuardAnchorValid && m_arathiAdvanceValid && !BattleBotTactics::AbAdvanceAttackAllowed(target->GetDistance(m_arathiAdvanceGoal),Threatens(target,me)))return false;
    rank.guid=target->GetObjectGuid().GetRawValue();
    rank.playerBody=target->IsPlayer();
    rank.tier=bg->GetTypeID()!=BATTLEGROUND_WS ? (capper ? 0 : humanThreat ? 1 : 2) : BattleBotTactics::WsgPriority(enemyCarrier,carrierThreat,humanThreat);
    rank.distanceBucket=BattleBotTactics::DistanceBucket(me->GetDistance(target));
    rank.healer=enemy && HealingSignal(enemy);
    rank.damage=bg->GetTypeID()==BATTLEGROUND_AB ? BattleBotTactics::Damage15(target) : 0;
    return true;
}
Unit* BattleBotAI::SelectTacticalTarget(Unit* except) const
{
    if(!IsTacticalBattleGround())return nullptr;
    std::map<uint64,Unit*> candidates;
    std::list<Player*> players;me->GetAlivePlayerListInRange(me,players,80.0f);
    for(Player* player:players)
    {
        candidates[player->GetObjectGuid().GetRawValue()]=player;
        if(Pet* pet=player->GetPet())candidates[pet->GetObjectGuid().GetRawValue()]=pet;
        if(player->GetBGTeam()==me->GetBGTeam())for(Unit* attacker:player->GetAttackers())candidates[attacker->GetObjectGuid().GetRawValue()]=attacker;
    }
    if(Unit* current=me->GetVictim())candidates[current->GetObjectGuid().GetRawValue()]=current;
    for(Unit* attacker:me->GetAttackers())candidates[attacker->GetObjectGuid().GetRawValue()]=attacker;
    Unit* best=nullptr;BattleBotTactics::Rank bestRank;
    for(auto const& pair:candidates)
    {
        if(pair.second==except)continue;
        BattleBotTactics::Rank rank;
        if(TacticalRank(pair.second,rank) && (!best || BattleBotTactics::Better(rank,bestRank))){best=pair.second;bestRank=rank;}
    }
    return best;
}
bool BattleBotAI::UpdateTacticalTarget()
{
    if(!IsTacticalBattleGround() || (me->GetBattleGround()->GetTypeID()==BATTLEGROUND_WS && HasWsgFlag(me)))return false;
    Unit* current=me->GetVictim();
BattleBotTactics::Rank currentRank;
    bool currentValid=current && TacticalRank(current,currentRank);
    if(current && !currentValid)
    {
        // Existing attacks obey the same local/CC/leash checks as new candidates.
        StopTacticalAttack(me);ClearPath();StopMoving();
    }
    if(GetRole()==ROLE_HEALER || me->GetCurrentSpell(CURRENT_GENERIC_SPELL) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))return false;
    Unit* best=SelectTacticalTarget();if(!best)return false;
    BattleBotTactics::Rank bestRank;TacticalRank(best,bestRank);
    if(best==current)return false;
    if(currentValid && !BattleBotTactics::ShouldSwitch(bestRank,currentRank,TacticalNow()-m_lastTacticalSwitch))return false;
    if(me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL,true);
    if(AttackStart(best)){m_lastTacticalSwitch=TacticalNow();return true;}
    return false;
}
bool BattleBotAI::IsValidHostileTarget(Unit const* target) const
{
    if(IsTacticalBattleGround() && me->GetBattleGround()->GetTypeID()==BATTLEGROUND_WS && target &&
       (me->GetBGTeam()==HORDE ? target->HasAura(AURA_WARSONG_FLAG) : target->HasAura(AURA_SILVERWING_FLAG)))
    {
        BattleBotTactics::Rank rank;
        return TacticalRank(const_cast<Unit*>(target),rank);
    }
    return CombatBotBaseAI::IsValidHostileTarget(target);
}
bool BattleBotAI::FindAndHealInjuredAlly(float selfHealPercent,float groupHealPercent)
{
    if(!IsTacticalBattleGround())return CombatBotBaseAI::FindAndHealInjuredAlly(selfHealPercent,groupHealPercent);
    std::list<Player*> nearby;me->GetAlivePlayerListInRange(me,nearby,30.0f);nearby.push_back(me);
    Player* best=nullptr;unsigned bestTier=99;float bestHealth=101;
    for(Player* candidate:nearby)
    {
        if(candidate->GetBattleGround()!=me->GetBattleGround() || candidate->GetBGTeam()!=me->GetBGTeam() || candidate->IsGameMaster())continue;
        float threshold=candidate==me ? selfHealPercent : groupHealPercent;
        if(!IsValidHealTarget(candidate,threshold))continue;
        bool threatened=!candidate->GetAttackers().empty();
        unsigned tier=candidate->GetHealthPercent()<=25 ? 0 : !candidate->IsBot() && threatened ? 1 : HasWsgFlag(candidate) ? 2 : !candidate->IsBot() ? 3 : 4;
        if(!best || tier<bestTier || (tier==bestTier && (candidate->GetHealthPercent()<bestHealth || (candidate->GetHealthPercent()==bestHealth && candidate->GetObjectGuid()<best->GetObjectGuid()))))
        {best=candidate;bestTier=tier;bestHealth=candidate->GetHealthPercent();}
    }
    if(!best)return CombatBotBaseAI::FindAndHealInjuredAlly(selfHealPercent,groupHealPercent);
    if(me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL,true);
    return HealInjuredTarget(best);
}

bool BattleBotAI::ShouldIgnoreCombat() const
{
    if (m_battlegroundId == BATTLEGROUND_QUEUE_WS && !me->IsRooted() &&
       (me->HasAura(AURA_SILVERWING_FLAG) || me->HasAura(AURA_WARSONG_FLAG)))
        return true;
    return false;
}

Unit* BattleBotAI::SelectAttackTarget(Unit* pExcept) const
{
    if(IsTacticalBattleGround())
    {
        BattleBotTactics::Rank current;
        // A valid current target remains in the same ranking. pExcept is only
        // an actual exclusion when callers have already stopped/dropped it.
        Unit* excluded=pExcept;
        if(pExcept && pExcept==me->GetVictim() && TacticalRank(pExcept,current))excluded=nullptr;
        return SelectTacticalTarget(excluded);
    }

    // Ignore attackers while carrying flag, just keep running.
    if (ShouldIgnoreCombat())
        return nullptr;

    // 1. Check units we are currently in combat with.

    std::list<Unit*> targets;
    HostileReference* pReference = me->GetHostileRefManager().getFirst();

    while (pReference)
    {
        if (Unit* pTarget = pReference->getSourceUnit())
        {
            if (pTarget != pExcept &&
                IsValidHostileTarget(pTarget) &&
                me->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
            {
                if (me->GetTeam() == HORDE)
                {
                    if (pTarget->HasAura(AURA_WARSONG_FLAG))
                        return pTarget;
                }
                else
                {
                    if (pTarget->HasAura(AURA_SILVERWING_FLAG))
                        return pTarget;
                }

                targets.push_back(pTarget);
            }
        }
        pReference = pReference->next();
    }

    if (!targets.empty())
    {
        targets.sort([this](Unit* pUnit1, const Unit* pUnit2)
        {
            return me->GetDistance(pUnit1) < me->GetDistance(pUnit2);
        });

        return *targets.begin();
    }

    // 2. Find enemy player in range.

    std::list<Player*> players;
    me->GetAlivePlayerListInRange(me, players, VISIBILITY_DISTANCE_NORMAL);
    float const maxAggroDistance = GetMaxAggroDistanceForMap();

    for (const auto& pTarget : players)
    {
        if (pTarget == pExcept)
            continue;

        if (!IsValidHostileTarget(pTarget))
            continue;

        if (me->GetTeam() == HORDE)
        {
            if (pTarget->HasAura(AURA_WARSONG_FLAG))
                return pTarget;
        }
        else
        {
            if (pTarget->HasAura(AURA_SILVERWING_FLAG))
                return pTarget;
        }

        // Aggro weak enemies from further away.
        uint32 const aggroDistance = me->GetHealth() > pTarget->GetHealth() ? maxAggroDistance : 20.0f;
        if (!me->IsWithinDist(pTarget, aggroDistance))
            continue;

        if (me->GetDistanceZ(pTarget) > 10.0f)
            continue;

        if (me->IsWithinLOSInMap(pTarget))
            return pTarget;
    }

    // 3. Check party attackers.

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->getSource())
            {
                if (pMember == me)
                    continue;

                if (me->GetDistance(pMember) > 30.0f)
                    continue;

                if (Unit* pAttacker = pMember->GetAttackerForHelper())
                {
                    if (pAttacker != pExcept &&
                        IsValidHostileTarget(pAttacker) &&
                        me->IsWithinDist(pAttacker, maxAggroDistance * 2.0f) &&
                        me->GetDistanceZ(pAttacker) < 10.0f &&
                        me->IsWithinLOSInMap(pAttacker))
                        return pAttacker;
                }
            }
        }
    }

    return nullptr;
}

Unit* BattleBotAI::SelectFollowTarget() const
{
    if(IsTacticalBattleGround())
    {
        if(m_arathiGuardAnchorValid || HasWsgFlag(me) || me->GetBattleGround()->GetTypeID()==BATTLEGROUND_WS)return nullptr;
        std::list<Player*> nearby;me->GetAlivePlayerListInRange(me,nearby,35.0f);
        Player* best=nullptr;BattleBotTactics::Rank bestRank;
        for(Player* player:nearby)
        {
            if(player==me || player->GetBattleGround()!=me->GetBattleGround() || player->GetBGTeam()!=me->GetBGTeam() || player->IsGameMaster() || !me->IsWithinLOSInMap(player))continue;
            bool carrier=me->GetBattleGround()->GetTypeID()==BATTLEGROUND_WS && HasWsgFlag(player);
            if(!carrier && (m_role!=ROLE_HEALER || me->GetDistance2d(player)>20.0f || me->GetDistanceZ(player)>3.0f || player->HasAuraType(SPELL_AURA_MOD_STEALTH) || player->IsMounted()!=me->IsMounted()))continue;
            BattleBotTactics::Rank rank;rank.guid=player->GetObjectGuid().GetRawValue();rank.tier=carrier ? 0 : !player->IsBot() ? 1 : 2;rank.distanceBucket=BattleBotTactics::DistanceBucket(me->GetDistance(player));
            if(!best || BattleBotTactics::Better(rank,bestRank)){best=player;bestRank=rank;}
        }
        return best;
    }

    if (me->HasAura(AURA_WARSONG_FLAG) ||
        me->HasAura(AURA_SILVERWING_FLAG))
        return nullptr;

    std::list<Player*> players;
    me->GetAlivePlayerListInRange(me, players, VISIBILITY_DISTANCE_NORMAL);
    Player* pHealerFollowTarget = nullptr;

    for (const auto& pTarget : players)
    {
        if (pTarget == me)
            continue;

        if (me->GetTeam() != pTarget->GetTeam())
            continue;

        if (pTarget->IsGameMaster())
            continue;

        if (me->GetTeam() == ALLIANCE)
        {
            if (pTarget->HasAura(AURA_WARSONG_FLAG))
                return pTarget;
        }
        else
        {
            if (pTarget->HasAura(AURA_SILVERWING_FLAG))
                return pTarget;
        }

        if (m_role == ROLE_HEALER &&
           !IsHealerClass(pTarget->GetClass()) &&
           !IsStealthClass(pTarget->GetClass()) &&
           (pTarget->IsMounted() == me->IsMounted()) &&
           (me->GetDistance2d(pTarget) <= 20.0f) &&
           (me->GetDistanceZ(pTarget) <= 3.0f))
            pHealerFollowTarget = pTarget;
    }

    return pHealerFollowTarget;
}

void BattleBotAI::DoGraveyardJump()
{
    // Recorded absolute poses bypassed slows and retained raw actor/AI pointers
    // in delayed callbacks. Leave resurrection through ordinary navigation.
    m_doingGraveyardJump=false;
    BattleGround* bg=me->GetBattleGround();
    if(!bg || bg->GetTypeID()!=BATTLEGROUND_WS || bg->GetStatus()!=STATUS_IN_PROGRESS || !me->IsAlive() || me->IsInCombat() || HasWsgFlag(me) || me->HasUnitState(UNIT_STATE_NO_FREE_MOVE))return;
    RefreshWarsongTask();
    if(!m_wsgGoalValid)return;
    PathInfo route(me);route.calculate(m_wsgGoal.x,m_wsgGoal.y,m_wsgGoal.z);
    if(route.getPathType()!=PATHFIND_NORMAL)return;
    m_wsgOpeningDone=true;
    ClearPath();StopMoving();StartWarsongObjective();
}

void BattleBotAI::StopMoving()
{
    BattleBotWaypointPolicy::Invalidate(m_waypointTicket);
    me->StopMoving();
    me->GetMotionMaster()->Clear();
    me->GetMotionMaster()->MoveIdle();
}

void BattleBotAI::OnPacketReceived(WorldPacket const* packet)
{
    BattleBotPolicyRuntime::ObservePacket(*this,packet);
    //printf("Bot received %s\n", LookupOpcodeName(packet->GetOpcode()));
    switch (packet->GetOpcode())
    {
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_4_2
        case MSG_PVP_LOG_DATA:
        {
            if (!me)
                return;

            uint8 ended = *((uint8*)(*packet).contents());
            if (ended)
            {
                // Temporary battlebots are removed after bg ends.
                if (m_temporary)
                    botEntry->requestRemoval = true;
                else
                {
                    auto data = std::make_unique<WorldPackets::Battleground::LeaveBattlefield>();
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
                    data->mapId = me->GetMapId();
#endif
                    me->GetSession()->QueuePacket(std::move(data));
                }
            }
            return;
        }
#endif
    }

    CombatBotBaseAI::OnPacketReceived(packet);
}

void BattleBotAI::OnPlayerLogin()
{
    if (!m_initialized)
        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
}

std::string BattleBotAI::AvRouteStatus() const
{
 auto* follow=dynamic_cast<FollowMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent());
 std::ostringstream out;out<<"role="<<uint32(m_role)<<"|victim="<<(me->GetVictim()?me->GetVictim()->GetGUIDLow():0)<<"|follow="<<(follow&&follow->GetTarget()?follow->GetTarget()->GetGUIDLow():0)<<"|alive="<<me->IsAlive()<<"|deadObserved="<<m_wasDead<<"|motion="<<uint32(me->GetMotionMaster()->GetCurrentMovementGeneratorType())<<"|path="<<(m_currentPath?m_currentPath->size():0)<<"|point="<<m_currentPoint<<"|ticket="<<m_waypointTicket.pending<<"|owned="<<OwnsLegacyWaypointMovement()<<"|splineFinal="<<me->movespline->Finalized()<<"|staleAt="<<m_avRouteStaleAt<<"|buffing="<<m_isBuffing<<"|casting="<<me->IsNonMeleeSpellCasted()<<"|food="<<me->HasAura(BB_SPELL_FOOD)<<"|drink="<<me->HasAura(BB_SPELL_DRINK)<<"|stand="<<uint32(me->GetStandState())<<"|mana="<<me->GetPowerPercent(POWER_MANA)<<"|controlled="<<me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)<<"|jump="<<m_doingGraveyardJump;
 return out.str();
}

void BattleBotAI::UpdateWaypointMovement()
{
    // A route pointer alone does not prove its native movement still exists.
    // AV resurrection/cancelled splines may leave the authored route orphaned.
    if(m_currentPath && me->GetBattleGround() && me->GetBattleGround()->GetTypeID()==BATTLEGROUND_AV){
        auto motion=me->GetMotionMaster()->GetCurrentMovementGeneratorType();
        bool stopped= !me->IsMoving() && me->IsStopped() && me->movespline->Finalized();
        bool ours=motion==IDLE_MOTION_TYPE || (OwnsLegacyWaypointMovement()&&motion==POINT_MOTION_TYPE);
        bool protectedAction=me->IsInCombat()||me->IsNonMeleeSpellCasted()||me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE)||me->HasAura(BB_SPELL_FOOD)||me->HasAura(BB_SPELL_DRINK)||me->GetBattleGround()->GetStatus()!=STATUS_IN_PROGRESS;
        uint32 now=WorldTimer::getMSTime();
        if(AvRouteRecoveryPolicy::Observe(stopped,ours,protectedAction,now,m_avRouteStaleAt))ClearPath();
    }
    if (m_currentPath)
        return;

    if (me->IsMoving())
        return;

    if (!me->IsStopped())
        return;

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))
        return;

    switch (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
    {
        case IDLE_MOTION_TYPE:
        case CHASE_MOTION_TYPE:
        case POINT_MOTION_TYPE:
            break;
        default:
            return;
    }

    if (BattleGround* bg = me->GetBattleGround())
        if (bg->GetStatus() == STATUS_WAIT_JOIN)
            return;

    if (StartNewPathToObjective())
        return;

    if (StartNewPathFromBeginning())
        return;

    StartNewPathFromAnywhere();
}

void BattleBotAI::OnJustDied()
{
    BattleBotObjectivePolicy::Forget(m_wsgChoice);m_wsgEvaluated.clear();m_wsgEnemySeenAt=0;m_wsgThreatSeenAt=0;m_wsgPressureUntil=0;m_wsgSeenEnemy.Clear();
    BattleBotPolicyRuntime::Forget(*this);
    ClearPath();
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        StopMoving();
}

void BattleBotAI::OnJustRevived()
{
    m_avRouteStaleAt=0;
    if(me->GetBattleGround()&&me->GetBattleGround()->GetTypeID()==BATTLEGROUND_AV){
        bool own=OwnsLegacyWaypointMovement();
        ClearPath();
        // Only clear our own old route; retain foreign movement/control.
        if(me->GetMotionMaster()->GetCurrentMovementGeneratorType()==IDLE_MOTION_TYPE || own)StopMoving();
        SummonPetIfNeeded();
        if((own||me->GetMotionMaster()->GetCurrentMovementGeneratorType()==IDLE_MOTION_TYPE)&&me->GetBattleGround()->GetStatus()==STATUS_IN_PROGRESS&&!me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL|UNIT_STATE_CAN_NOT_MOVE)&&!me->HasAura(BB_SPELL_FOOD)&&!me->HasAura(BB_SPELL_DRINK)&&!me->IsNonMeleeSpellCasted()&&!me->IsInCombat())UpdateWaypointMovement();
        return;
    }
    SummonPetIfNeeded();
    if(me->GetBattleGround()&&me->GetBattleGround()->GetTypeID()==BATTLEGROUND_WS){m_wsgRevived=true;m_wsgOpeningDone=true;BattleBotObjectivePolicy::Forget(m_wsgChoice);m_wsgGoalValid=false;RefreshWarsongTask();return;}
    if (!me->SelectRandomUnfriendlyTarget(nullptr, 30.0f))
        DoGraveyardJump();
}

void BattleBotAI::OnEnterBattleGround()
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg)
        return;

    if (bg->GetStatus() != STATUS_WAIT_JOIN)
        return;

    SummonPetIfNeeded();
    m_wsgRevived=false;m_wsgEnemySeenAt=0;m_wsgThreatSeenAt=0;m_wsgPressureUntil=0;m_wsgSeenEnemy.Clear();m_wsgScenarioEvent=0;m_wsgEvaluated.clear();BattleBotObjectivePolicy::Forget(m_wsgChoice);
    m_wsgOpeningDone=false;
    RefreshWarsongTask();

    m_abRushUntil=0;
    m_abCaptureResult=-1;m_abCaptureAttemptAt=0;m_abCaptureTrace="none";m_abCaptureEnd="unobserved";
    m_abCaptureHealth=0;m_abCaptureHurtAt=0;m_abCaptureRetryAt=0;m_abCaptureActiveFlag=0;
    m_preparationWaiting=false;m_preparationBuffDone=false;m_preparationAt=0;m_preparationMoveAt=0;m_preparationQuiet=0;
    m_prepSpreadArrived=false;m_prepConverge=false;m_prepGoalReady=false;m_prepCandidates.clear();m_prepCandidate=0;m_prepGeneration=0;m_prepMoves=0;m_prepAttempts=0;m_prepSpline=0;m_prepPoint=0;m_prepNextIdle=0;m_prepPhase="idle";m_prepReject="none";
    m_abOpeningNode=-1;m_abOpeningComplete=false;m_abOpeningActualRaid=false;m_abHomeOpener=false;
    if(bg->GetTypeID()==BATTLEGROUND_WS||bg->GetTypeID()==BATTLEGROUND_AB){m_waitingSpot=BB_WSG_WAIT_SPOT_RIGHT;return;}
    if (bg->GetTypeID() == BATTLEGROUND_AV)
    {
        if (me->GetTeam() == HORDE)
            me->GetMotionMaster()->MovePoint(0, AV_WAITING_POS_HORDE.x + frand(-2.0f, 2.0f), AV_WAITING_POS_HORDE.y + frand(-2.0f, 2.0f), AV_WAITING_POS_HORDE.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES, 0, AV_WAITING_POS_HORDE.o);
        else
            me->GetMotionMaster()->MovePoint(0, AV_WAITING_POS_ALLIANCE.x + frand(-2.0f, 2.0f), AV_WAITING_POS_ALLIANCE.y + frand(-2.0f, 2.0f), AV_WAITING_POS_ALLIANCE.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES, 0, AV_WAITING_POS_ALLIANCE.o);
    }
}

void BattleBotAI::OnLeaveBattleGround()
{
    BattleBotPolicyRuntime::Forget(*this);
    m_preparationWaiting=false;m_preparationBuffDone=false;m_preparationAt=0;m_abOpeningNode=-1;m_abOpeningComplete=false;
    m_abLandRoute=false;m_abLandViaValid=false;m_abLandEscape=false;m_abLandSpline=0;m_abAdvancePoint=0;m_abLandTrace="none";
    m_arathiAdvanceNode=-1;m_arathiAdvanceValid=false;m_arathiPlanAt=0;m_arathiMoveAt=0;m_arathiProgressAt=0;
    m_arathiGuardNode=-1;m_arathiGuardAnchorValid=false;
    m_wsgGoalValid=false;m_wsgTravel=false;m_wsgCarrier.Clear();m_wsgEnemySeenAt=0;m_wsgThreatSeenAt=0;m_wsgPressureUntil=0;m_wsgSeenEnemy.Clear();m_wsgRevived=false;BattleBotObjectivePolicy::Forget(m_wsgChoice);
    ClearPath();
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        StopMoving();

    // Temporary battlebots are removed after bg ends.
    if (m_temporary)
        botEntry->requestRemoval = true;
}

bool BattleBotAI::CheckForUnreachableTarget()
{
    if(me->IsNonMeleeSpellCasted() || me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))return false;
    if(me->GetMotionMaster()->GetCurrentMovementGeneratorType()!=CHASE_MOTION_TYPE || me->GetMotionMaster()->GetCurrent()->IsReachable())return false;
    Unit* target=static_cast<ChaseMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent())->GetTarget();
    if(!target || !target->IsAlive() || target->GetMap()!=me->GetMap() || !IsValidHostileTarget(target) || !me->IsWithinDist(target,VISIBILITY_DISTANCE_NORMAL) || me->GetDistanceZ(target)>10.0f){
        StopTacticalAttack(me);ClearPath();StopMoving();return true;
    }
    if(me->CanReachWithMeleeAutoAttack(target))return false;
    auto now=TacticalNow();if(now<m_unreachableRetryAt)return true;
    m_unreachableRetryAt=now+2000;
    PathInfo route(me);route.calculate(target->GetPositionX(),target->GetPositionY(),target->GetPositionZ());
    ClearPath();StopMoving();
    if(route.getPathType()==PATHFIND_NORMAL)BeginChasing(target);
    else StopTacticalAttack(me); // Includes pets: never blink to a creature to repair pathing.
    return true;
}

void BattleBotAI::UpdateAI(uint32 const diff)
{
    // Protected phases can return before the legacy waypoint lane. They must
    // break stale-route observation rather than counting as stationary travel.
    if(me && (!me->IsInWorld()||me->IsBeingTeleported()||!me->IsAlive()||me->IsInCombat()||me->IsNonMeleeSpellCasted()||m_isBuffing||me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE|UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)||me->HasAura(BB_SPELL_FOOD)||me->HasAura(BB_SPELL_DRINK)||!me->GetBattleGround()||me->GetBattleGround()->GetStatus()!=STATUS_IN_PROGRESS||me->IsMoving()||!me->movespline->Finalized()||(me->GetMotionMaster()->GetCurrentMovementGeneratorType()!=IDLE_MOTION_TYPE&&!OwnsLegacyWaypointMovement())))m_avRouteStaleAt=0;

    // A native AB capture owns the whole action lane, including the 200ms policy prefix.
    // Incoming damage/control is still processed by the native Spell system.
    if(m_initialized&&botEntry&&!botEntry->requestRemoval&&me&&me->IsInWorld()&&!me->IsBeingTeleported()&&me->IsAlive()&&me->GetBattleGround()&&me->GetBattleGround()->GetTypeID()==BATTLEGROUND_AB&&me->GetBattleGround()->GetStatus()==STATUS_IN_PROGRESS)
        for(CurrentSpellTypes lane:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL})
            if(Spell* opening=me->GetCurrentSpell(lane))if(opening->m_spellInfo->Id==21651||opening->m_spellInfo->Id==23932||IsArathiOpening(me,me,opening))return;
    m_updateTimer.Update(diff);
    if(BattleBotPolicyRuntime::Tick(*this,true))return;
    if (m_updateTimer.Passed())
        m_updateTimer.Reset(BB_UPDATE_INTERVAL);
    else
        return;

    if (!me->IsInWorld() || me->IsBeingTeleported() || m_doingGraveyardJump)
        return;

    if (m_matchProfile && !BotMatch::Active(m_matchProfile->requestId))
    { botEntry->requestRemoval = true; return; }

    if (!m_initialized)
    {
        if (m_level && m_level != me->GetLevel())
        {
            me->GiveLevel(m_level);
            me->InitTalentForLevel();
            me->SetUInt32Value(PLAYER_XP, 0);
        }

        LearnPremadeSpecForClass();

        if (m_role == ROLE_INVALID)
            AutoAssignRole();

        if (m_matchProfile)
        {
            LearnArmorProficiencies();
            me->UpdateSkillsToMaxSkillsForLevel();
            std::string report;
            BotMatch::EquipMatched(me, m_role, *m_matchProfile, report);
            BotMatch::Report(m_matchProfile->requestId, report);
            UpdateVisualHonorRankBasedOnItems();
        }
        else
            AutoEquipGear(sWorld.getConfig(CONFIG_UINT32_BATTLE_BOT_AUTO_EQUIP));
        ResetSpellData();
        PopulateSpellData();
        AddAllSpellReagents();
        me->UpdateSkillsToMaxSkillsForLevel();
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
        SummonPetIfNeeded();
        me->SetHealthPercent(100.0f);
        me->SetPowerPercent(me->GetPowerType(), 100.0f);

        if (urand(0, 1))
        {
            me->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_HELM);
            me->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_CLOAK);
        }

        uint32 newzone, newarea;
        me->GetZoneAndAreaId(newzone, newarea);
        me->UpdateZone(newzone, newarea);

        m_initialized = true;
        return;
    }

    if (!me->InBattleGround())
    {
        if (m_wasInBG)
        {
            m_wasInBG = false;
            OnLeaveBattleGround();
            return;
        }

        if (m_receivedBgInvite)
        {
            if (m_matchProfile && !BotMatch::CanAcceptInvite(m_matchProfile->requestId, me))
                return;
            SendBattlefieldPortPacket();
            m_receivedBgInvite = false;
            return;
        }

        if (!me->InBattleGroundQueue())
        {
            bool canQueue;
            char args[] = "";
            switch (m_battlegroundId)
            {
                case BATTLEGROUND_QUEUE_AV:
                    canQueue = ChatHandler(me).HandleGoAlteracCommand(args);
                    break;
                case BATTLEGROUND_QUEUE_WS:
                    canQueue = ChatHandler(me).HandleGoWarsongCommand(args);
                    break;
                case BATTLEGROUND_QUEUE_AB:
                    canQueue = ChatHandler(me).HandleGoArathiCommand(args);
                    break;
                default:
                    sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "BattleBot: Invalid BG queue type!");
                    botEntry->requestRemoval = true;
                    return;
            }

            if (!canQueue)
            {
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "BattleBot: Attempt to queue for BG failed! Bot is too low level or BG is not available in this patch.");
                botEntry->requestRemoval = true;
                return;
            }

            SendBattlemasterJoinPacket(m_battlegroundId, m_matchProfile ? BotMatch::DesiredInstance(m_matchProfile->requestId) : 0);
            return;
        }

        // Remain idle until we can join battleground.
        return;
    }
    else
    {
        if (!m_wasInBG)
        {
            m_wasInBG = true;
            OnEnterBattleGround();
            return;
        }
        else if (m_temporary)
        {
            // Remove temporary battlebots if no real players in map.
            if (BattleGround* bg = me->GetBattleGround())
            {
                if (bg->GetStatus() == STATUS_IN_PROGRESS)
                {
                    if (!me->GetMap()->HaveRealPlayers() &&
                        !(m_matchProfile && BotMatch::HumansPendingArrival(m_matchProfile->requestId, bg->GetInstanceID())))
                    {
                        botEntry->requestRemoval = true;
                        return;
                    }
                }
            }
        }
    }

    if (me->IsDead())
    {
        if (!m_wasDead)
        {
            m_wasDead = true;
            OnJustDied();
            return;
        }

        if (me->InBattleGround())
        {
            if (me->GetDeathState() == CORPSE)
            {
                me->BuildPlayerRepop();
                me->RepopAtGraveyard();
            }
        }
        else
        {
            if (me->GetDeathState() == DEAD)
            {
                me->ResurrectPlayer(0.5f);
                me->SpawnCorpseBones();
                //me->SendCreateUpdateToPlayer(pLeader);
            }
        }

        return;
    }
    else
    {
        if (m_wasDead)
        {
            m_wasDead = false;
            m_arathiPlanAt=0;m_arathiMoveAt=0;m_arathiProgressAt=0;m_arathiAdvanceValid=false;m_abLandRoute=false;m_abLandViaValid=false;m_abLandEscape=false;m_abAdvancePoint=0;
            if(me->GetBattleGround()&&me->GetBattleGround()->GetTypeID()==BATTLEGROUND_AB)m_abOpeningComplete=true; // Rejoining uses home/BS/side priorities, not the initial lane assignment.
            OnJustRevived();
            return;
        }
    }

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        BreakCrowdControlEffects();
        return;
    }

    if(UpdateBattleGroundPreparation())return;
    if(TryCaptureNearbyBattleFlag())return;
    RefreshWarsongTask();
    RefreshArathiGuardAssignment();
    if(UpdateArathiGuardMovement())return;
    // A distant escort/recovery healer must catch up rather than restarting a
    // heal on every tick while the carrier disappears beyond support range.
    bool regroup=me->GetBattleGround()->GetTypeID()==BATTLEGROUND_WS && m_wsgGoalValid &&
        BattleBotTactics::WsgNeedsRegroup(m_wsgTask,me->GetDistance(m_wsgGoal));


    if(UpdateWarsongMovement())return;
    if(UpdateTacticalTarget())return;
    if(me->IsInCombat() && CheckForUnreachableTarget())return;

    if(BattleBotPolicyRuntime::Tick(*this,false))return;
    if(IsTacticalBattleGround() && GetRole()==ROLE_HEALER && !regroup && !me->GetCurrentSpell(CURRENT_GENERIC_SPELL) && !me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        if(FindAndHealInjuredAlly(40.0f,70.0f))return;
    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        // Stop auto shot if no target.
        if (!me->GetVictim())
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
        else if (me->GetClass() == CLASS_HUNTER)
        {
            if (me->GetCombatDistance(me->GetVictim()) < 8.0f)
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
            else
                UpdateInCombatAI_Hunter();
        }

        return;
    }

    if (me->IsNonMeleeSpellCasted(false, false, true))
        return;

    if (me->GetTargetGuid() == me->GetObjectGuid())
        me->ClearTarget();

    Unit* pVictim = me->GetVictim();

    // Prevent battelbot from chasing target entered stealth mode
    if (pVictim && !pVictim->IsVisibleForOrDetect(me, me, false))
    {
        me->AttackStop();
        me->ClearTarget();
        me->StopMoving();
        if (pVictim = SelectAttackTarget(pVictim))
            AttackStart(pVictim);
        return;
    }

    // A non-defender with safe travel resources advances before the legacy buff/rest loop.
    // Critical recovery remains possible; capture itself already owns the earliest action lane.
    if(me->GetBattleGround()->GetTypeID()==BATTLEGROUND_AB&&BattleBotTactics::AbAdvanceBeforeRecovery(m_arathiGuardAnchorValid,me->IsInCombat(),me->GetHealthPercent(),me->GetPowerPercent(POWER_MANA),me->GetMaxPower(POWER_MANA)>0,TacticalNow()<m_abRushUntil&&!m_abOpeningComplete&&(!m_arathiAdvanceValid||me->GetDistance(m_arathiAdvanceGoal)>INTERACTION_DISTANCE))&&!SelectTacticalTarget())
        if(UpdateArathiAdvanceMovement())return;

    if (!me->IsInCombat())
    {
        if ((!IsTacticalBattleGround() || !SelectTacticalTarget()) && DrinkAndEat())
            return;
    }

    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);

    if (me->GetSheath() == SHEATH_STATE_UNARMED && !me->IsMounted())
        me->SetSheath(SHEATH_STATE_MELEE);

    UpdateBattleGroundAI();

    if (!me->IsInCombat())
    {
        if (CheckForUnreachableTarget())
            return;

        UpdateOutOfCombatAI();

        if (m_isBuffing)
            return;

        // Can enter combat from UpdateOutOfCombatAI().
        if (me->IsInCombat())
            return;

        if (me->IsNonMeleeSpellCasted())
            return;

        if(UpdateArathiAdvanceMovement())return;

        if (!pVictim || !IsValidHostileTarget(pVictim))
        {
            if (pVictim = SelectAttackTarget(pVictim))
            {
                AttackStart(pVictim);
                return;
            }

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            {
                if (Unit* pFollowTarget = SelectFollowTarget())
                {
                    ClearPath();
                    me->GetMotionMaster()->MoveFollow(pFollowTarget, frand(3.0f, 5.0f), frand(0.0f, 3.0f));
                    return;
                }
            }
            else if (FollowMovementGenerator<Player> const* pMoveGen = dynamic_cast<FollowMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent()))
            {
                Unit* pTarget = pMoveGen->GetTarget();
                if (!pTarget || !pTarget->IsAlive() || !pTarget->IsWithinDist(me, VISIBILITY_DISTANCE_NORMAL))
                {
                    StopMoving();
                    return;
                }
            }

            if(m_arathiGuardAnchorValid)return;
            if(me->GetBattleGround()->GetTypeID()==BATTLEGROUND_WS)return; // WSG task owns the route; no unrelated legacy random path.
            if (UseMount())
                return;

            UpdateWaypointMovement();
        }
        return;
    }

    if(me->GetBattleGround()->GetTypeID()==BATTLEGROUND_AB && !SelectTacticalTarget() && UpdateArathiAdvanceMovement())return;

    if (ShouldIgnoreCombat())
    {
        UpdateFlagCarrierAI();
        UpdateWaypointMovement();
        return;
    }

    if (!pVictim || !IsValidHostileTarget(pVictim) ||
        !pVictim->IsWithinDist(me, VISIBILITY_DISTANCE_SMALL))
    {
        if (Unit* pNewVictim = SelectAttackTarget(pVictim))
        {
            if (pVictim != pNewVictim)
            {
                AttackStart(pNewVictim);
                return;
            }
        }

        if (me->GetVictim() &&
           (me != me->GetVictim()->GetVictim()))
        {
            me->AttackStop(false);
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                StopMoving();
            return;
        }
    }
    else
    {
        if (!me->HasInArc(pVictim, 2 * M_PI_F / 3) && !me->IsMoving())
        {
            me->SetInFront(pVictim);
            me->SendMovementPacket(MSG_MOVE_SET_FACING, false);
        }

        if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
           (m_role != ROLE_HEALER) &&
            IsValidHostileTarget(pVictim) &&
            AttackStart(pVictim))
            return;
    }

    if (me->IsInCombat())
        UpdateInCombatAI();
}

void BattleBotAI::UpdateBattleGroundAI()
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg)
        return;

    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_WS:
        {
            // Pick up dropped flags.
            if (GameObject* pGo = me->FindNearestGameObject(GO_WSG_DROPPED_SILVERWING_FLAG, INTERACTION_DISTANCE))
                pGo->Use(me);
            if (GameObject* pGo = me->FindNearestGameObject(GO_WSG_DROPPED_WARSONG_FLAG, INTERACTION_DISTANCE))
                pGo->Use(me);

            // Pick up stationary flags from bases.
            if (me->GetTeam() == HORDE)
            {
                if (GameObject* pGo = me->FindNearestGameObject(GO_WSG_SILVERWING_FLAG, INTERACTION_DISTANCE))
                    pGo->Use(me);
            }
            else
            {
                if (GameObject* pGo = me->FindNearestGameObject(GO_WSG_WARSONG_FLAG, INTERACTION_DISTANCE))
                    pGo->Use(me);
            }
            break;
        }
    }
}

void BattleBotAI::UpdateFlagCarrierAI()
{
    // First those that can be cast both in and out of combat.
    switch (me->GetClass())
    {
        case CLASS_PALADIN:
        {
            if (m_spells.paladin.pHolyShock && me->GetHealthPercent() < 90.0f &&
                CanTryToCastSpell(me, m_spells.paladin.pHolyShock))
            {
                PolicyAwareCast(me, m_spells.paladin.pHolyShock, false);
                return;
            }
            break;
        }
        case CLASS_SHAMAN:
        {
            if (m_spells.shaman.pGhostWolf && !me->IsMoving() &&
                CanTryToCastSpell(me, m_spells.shaman.pGhostWolf))
            {
                PolicyAwareCast(me, m_spells.shaman.pGhostWolf, false);
                return;
            }
            break;
        }
        case CLASS_MAGE:
        {
            if (m_spells.mage.pManaShield &&
                CanTryToCastSpell(me, m_spells.mage.pManaShield))
            {
                PolicyAwareCast(me, m_spells.mage.pManaShield, false);
                return;
            }
            if (m_spells.mage.pIceBarrier &&
                CanTryToCastSpell(me, m_spells.mage.pIceBarrier))
            {
                PolicyAwareCast(me, m_spells.mage.pIceBarrier, false);
                return;
            }
            break;
        }
        case CLASS_PRIEST:
        {
            if (m_spells.priest.pPowerWordShield &&
                CanTryToCastSpell(me, m_spells.priest.pPowerWordShield))
            {
                PolicyAwareCast(me, m_spells.priest.pPowerWordShield, false);
                return;
            }
            if (m_spells.priest.pHolyNova && me->GetHealthPercent() < 90.0f &&
                CanTryToCastSpell(me, m_spells.priest.pHolyNova))
            {
                PolicyAwareCast(me, m_spells.priest.pHolyNova, false);
                return;
            }
            break;
        }
        case CLASS_WARRIOR:
        {
            if (m_spells.warrior.pDefensiveStance &&
                CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
            {
                PolicyAwareCast(me, m_spells.warrior.pDefensiveStance, false);
                return;
            }
            break;
        }
        case CLASS_ROGUE:
        {
            if (m_spells.rogue.pSprint &&
                CanTryToCastSpell(me, m_spells.rogue.pSprint))
            {
                PolicyAwareCast(me, m_spells.rogue.pSprint, false);
                return;
            }
            break;
        }
        case CLASS_DRUID:
        {
            if (me->GetShapeshiftForm() == FORM_NONE)
            {
                if (m_spells.druid.pTravelForm &&
                    CanTryToCastSpell(me, m_spells.druid.pTravelForm))
                {
                    PolicyAwareCast(me, m_spells.druid.pTravelForm, false);
                    return;
                }
            }
            else if (me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) && AllowLegacyFormExit())
            {
                me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
            }
            break;
        }
    }

    Unit* pAttacker = me->GetAttackerForHelper();
    if (pAttacker)
    {
        switch (me->GetClass())
        {
            case CLASS_PALADIN:
            {
                if (m_spells.paladin.pHammerOfJustice &&
                    CanTryToCastSpell(pAttacker, m_spells.paladin.pHammerOfJustice))
                {
                    PolicyAwareCast(pAttacker, m_spells.paladin.pHammerOfJustice, false);
                    return;
                }
                break;
            }
            case CLASS_SHAMAN:
            {
                if (m_spells.shaman.pFrostShock &&
                    CanTryToCastSpell(pAttacker, m_spells.shaman.pFrostShock))
                {
                    PolicyAwareCast(pAttacker, m_spells.shaman.pFrostShock, false);
                    return;
                }
                break;
            }
            case CLASS_HUNTER:
            {
                if (m_spells.hunter.pAspectOfTheCheetah &&
                    me->HasAura(m_spells.hunter.pAspectOfTheCheetah->Id))
                {
                    me->RemoveAurasDueToSpellByCancel(m_spells.hunter.pAspectOfTheCheetah->Id);
                    return;
                }
                break;
            }
            case CLASS_MAGE:
            {
                if (m_spells.mage.pFrostNova &&
                    CanTryToCastSpell(me, m_spells.mage.pFrostNova) &&
                    m_spells.mage.pFrostNova->IsTargetInRange(me, pAttacker))
                {
                    PolicyAwareCast(me, m_spells.mage.pFrostNova, false);
                    return;
                }
                break;
            }
            case CLASS_PRIEST:
            {
                if (m_spells.priest.pPsychicScream &&
                    CanTryToCastSpell(me, m_spells.priest.pPsychicScream) &&
                    m_spells.priest.pPsychicScream->IsTargetInRange(me, pAttacker))
                {
                    PolicyAwareCast(me, m_spells.priest.pPsychicScream, false);
                    return;
                }
                break;
            }
            case CLASS_WARLOCK:
            {
                if (m_spells.warlock.pDeathCoil &&
                    CanTryToCastSpell(pAttacker, m_spells.warlock.pDeathCoil))
                {
                    PolicyAwareCast(pAttacker, m_spells.warlock.pDeathCoil, false);
                    return;
                }
                break;
            }
            case CLASS_WARRIOR:
            {
                if (m_spells.warrior.pShieldWall && me->GetHealthPercent() < 50.0f &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldWall))
                {
                    PolicyAwareCast(me, m_spells.warrior.pShieldWall, false);
                    return;
                }
                break;
            }
            case CLASS_ROGUE:
            {
                if (m_spells.rogue.pEvasion && me->GetHealthPercent() < 50.0f &&
                    CanTryToCastSpell(me, m_spells.rogue.pEvasion))
                {
                    PolicyAwareCast(me, m_spells.rogue.pEvasion, false);
                    return;
                }
                break;
            }
            case CLASS_DRUID:
            {
                if (m_spells.druid.pBarkskin && me->GetHealthPercent() < 50.0f &&
                    CanTryToCastSpell(me, m_spells.druid.pBarkskin))
                {
                    PolicyAwareCast(me, m_spells.druid.pBarkskin, false);
                    return;
                }
                break;
            }
        }
    }
    else // no attackers
    {
        switch (me->GetClass())
        {
            case CLASS_HUNTER:
            {
                if (m_spells.hunter.pAspectOfTheCheetah &&
                    CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheCheetah))
                {
                    PolicyAwareCast(me, m_spells.hunter.pAspectOfTheCheetah, false);
                    return;
                }
                break;
            }
        }
    }
}

void BattleBotAI::UpdateOutOfCombatAI()
{
    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateOutOfCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateOutOfCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateOutOfCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateOutOfCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateOutOfCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateOutOfCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateOutOfCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateOutOfCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateOutOfCombatAI_Druid();
            break;
    }
}

void BattleBotAI::UpdateInCombatAI()
{
    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateInCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateInCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateInCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateInCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateInCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateInCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateInCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateInCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateInCombatAI_Druid();
            break;
    }

    if (me->GetVictim())
        UseTrinketEffects();
}

void BattleBotAI::UpdateOutOfCombatAI_Paladin()
{
    if (m_spells.paladin.pAura &&
        CanTryToCastSpell(me, m_spells.paladin.pAura))
    {
        if (DoCastSpell(me, m_spells.paladin.pAura) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.paladin.pBlessingBuff)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.paladin.pBlessingBuff))
        {
            if (CanTryToCastSpell(pTarget, m_spells.paladin.pBlessingBuff))
            {
                if (DoCastSpell(pTarget, m_spells.paladin.pBlessingBuff) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    return;
                }
            }
        }
    }

    if (m_isBuffing &&
       (!m_spells.paladin.pBlessingBuff ||
        !me->HasGCD(m_spells.paladin.pBlessingBuff)))
    {
        m_isBuffing = false;
    }

    FindAndHealInjuredAlly();
}

void BattleBotAI::UpdateInCombatAI_Paladin()
{
    if (m_spells.paladin.pDivineShield &&
       (me->GetHealthPercent() < 20.0f) &&
       (me->GetPowerPercent(POWER_MANA) > 40.0f) &&
       !me->HasAura(AURA_WARSONG_FLAG) &&
        CanTryToCastSpell(me, m_spells.paladin.pDivineShield))
    {
        if (DoCastSpell(me, m_spells.paladin.pDivineShield) == SPELL_CAST_OK)
            return;
    }

    bool const hasSeal = m_spells.paladin.pSeal && me->HasAura(m_spells.paladin.pSeal->Id);

    if (!hasSeal &&
        m_spells.paladin.pSeal &&
        CanTryToCastSpell(me, m_spells.paladin.pSeal))
    {
        PolicyAwareCast(me, m_spells.paladin.pSeal, false);
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (hasSeal && m_spells.paladin.pJudgement &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pJudgement))
        {
            if (DoCastSpell(pVictim, m_spells.paladin.pJudgement) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pHammerOfJustice &&
            pVictim->IsNonMeleeSpellCasted() &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfJustice))
        {
            if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfJustice) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pHammerOfWrath &&
            pVictim->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfWrath))
        {
            if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfWrath) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pHolyShield &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShield) &&
           (IsMeleeDamageClass(pVictim->GetClass()) || (GetAttackersInRangeCount(8.0f) > 1)))
        {
            if (DoCastSpell(me, m_spells.paladin.pHolyShield) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pConsecration &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(me, m_spells.paladin.pConsecration))
        {
            if (DoCastSpell(me, m_spells.paladin.pConsecration) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pHolyShock &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHolyShock))
        {
            if (m_spells.paladin.pDivineFavor &&
                CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
            {
                DoCastSpell(me, m_spells.paladin.pDivineFavor);
            }

            if (DoCastSpell(pVictim, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pExorcism &&
            pVictim->IsCreature() &&
           (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD) &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pExorcism))
        {
            if (DoCastSpell(pVictim, m_spells.paladin.pExorcism) == SPELL_CAST_OK)
                return;
        }
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
           !me->CanReachWithMeleeAutoAttack(pVictim))
        {
            BeginChasing(pVictim);
        }
    }

    if (Unit* pFriend = me->FindLowestHpFriendlyUnit(30.0f, 70, true, me))
    {
        if (m_spells.paladin.pBlessingOfProtection &&
           !IsPhysicalDamageClass(pFriend->GetClass()) &&
           !pFriend->HasAura(AURA_WARSONG_FLAG) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pBlessingOfSacrifice &&
            pFriend->HasAura(AURA_WARSONG_FLAG) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pLayOnHands &&
           (pFriend->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pLayOnHands))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;
        }
    }

    if (m_spells.paladin.pBlessingOfFreedom &&
       (me->HasUnitState(UNIT_STATE_ROOT) || me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
        CanTryToCastSpell(me, m_spells.paladin.pBlessingOfFreedom))
    {
        if (DoCastSpell(me, m_spells.paladin.pBlessingOfFreedom) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.paladin.pCleanse)
    {
        if (Unit* pFriend = SelectDispelTarget(m_spells.paladin.pCleanse))
        {
            if (CanTryToCastSpell(pFriend, m_spells.paladin.pCleanse))
            {
                if (DoCastSpell(pFriend, m_spells.paladin.pCleanse) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    FindAndHealInjuredAlly(me->IsTotalImmune() ? 80.0f : 40.0f, 50.0f);
}

void BattleBotAI::UpdateOutOfCombatAI_Shaman()
{
    if (m_spells.shaman.pWeaponBuff &&
        CanTryToCastSpell(me, m_spells.shaman.pWeaponBuff))
    {
        if (CastWeaponBuff(m_spells.shaman.pWeaponBuff, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.shaman.pLightningShield &&
        CanTryToCastSpell(me, m_spells.shaman.pLightningShield))
    {
        if (DoCastSpell(me, m_spells.shaman.pLightningShield) == SPELL_CAST_OK)
            return;
    }

    if (me->GetVictim())
    {
        if (SummonShamanTotems())
            return;

        UpdateInCombatAI_Shaman();
    }
    else
    {
        if (m_spells.shaman.pGhostWolf &&
           !me->IsMoving() && !me->IsMounted() &&
           (!GetMountSpellId() || me->HasAura(AURA_WARSONG_FLAG) || me->HasAura(AURA_SILVERWING_FLAG)) &&
            CanTryToCastSpell(me, m_spells.shaman.pGhostWolf))
        {
            if (DoCastSpell(me, m_spells.shaman.pGhostWolf) == SPELL_CAST_OK)
                return;
        }
    }
}

void BattleBotAI::UpdateInCombatAI_Shaman()
{
    if (m_spells.shaman.pGhostWolf &&
        me->GetShapeshiftForm() == FORM_GHOSTWOLF)
        me->RemoveAurasDueToSpellByCancel(m_spells.shaman.pGhostWolf->Id);

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.shaman.pManaTideTotem &&
           (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
            CanTryToCastSpell(me, m_spells.shaman.pManaTideTotem))
        {
            if (DoCastSpell(me, m_spells.shaman.pManaTideTotem) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.shaman.pElementalMastery &&
            me->GetAttackers().empty() &&
            CanTryToCastSpell(me, m_spells.shaman.pElementalMastery))
        {
            if (DoCastSpell(me, m_spells.shaman.pElementalMastery) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.shaman.pEarthShock &&
            pVictim->IsNonMeleeSpellCasted(false, false, true) &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pEarthShock))
        {
            if (DoCastSpell(pVictim, m_spells.shaman.pEarthShock) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.shaman.pFrostShock &&
            pVictim->IsMoving() &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pFrostShock))
        {
            if (DoCastSpell(pVictim, m_spells.shaman.pFrostShock) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.shaman.pStormstrike &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pStormstrike))
        {
            if (DoCastSpell(pVictim, m_spells.shaman.pStormstrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.shaman.pChainLightning &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pChainLightning))
        {
            if (DoCastSpell(pVictim, m_spells.shaman.pChainLightning) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.shaman.pPurge &&
            IsValidDispelTarget(pVictim, m_spells.shaman.pPurge) &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pPurge))
        {
            if (DoCastSpell(pVictim, m_spells.shaman.pPurge) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.shaman.pFlameShock &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pFlameShock))
        {
            if (DoCastSpell(pVictim, m_spells.shaman.pFlameShock) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.shaman.pLightningBolt &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pLightningBolt))
        {
            if (DoCastSpell(pVictim, m_spells.shaman.pLightningBolt) == SPELL_CAST_OK)
                return;
        }
    }

    if (SummonShamanTotems())
        return;

    if (m_spells.shaman.pCureDisease &&
        CanTryToCastSpell(me, m_spells.shaman.pCureDisease) &&
        IsValidDispelTarget(me, m_spells.shaman.pCureDisease))
    {
        if (DoCastSpell(me, m_spells.shaman.pCureDisease) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.shaman.pCurePoison &&
        CanTryToCastSpell(me, m_spells.shaman.pCurePoison) &&
        IsValidDispelTarget(me, m_spells.shaman.pCurePoison))
    {
        if (DoCastSpell(me, m_spells.shaman.pCurePoison) == SPELL_CAST_OK)
            return;
    }

    FindAndHealInjuredAlly(40.0f);
}

void BattleBotAI::UpdateOutOfCombatAI_Hunter()
{
    if (m_spells.hunter.pAspectOfTheCheetah &&
       !me->IsMounted() &&
        CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheCheetah))
    {
        if (DoCastSpell(me, m_spells.hunter.pAspectOfTheCheetah) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.hunter.pHuntersMark &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pHuntersMark))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pHuntersMark) == SPELL_CAST_OK)
                return;
        }

        if (Pet* pPet = me->GetPet())
        {
            if (!pPet->GetVictim())
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Hunter();
    }
}

void BattleBotAI::UpdateInCombatAI_Hunter()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (me->HasSpell(BB_SPELL_AUTO_SHOT) &&
            !me->IsMoving() &&
            (me->GetCombatDistance(pVictim) > 8.0f) &&
            !me->IsNonMeleeSpellCasted())
        {
            switch (PolicyAwareCast(pVictim, BB_SPELL_AUTO_SHOT, false))
            {
                case SPELL_FAILED_NEED_AMMO:
                case SPELL_FAILED_NO_AMMO:
                {
                    AddHunterAmmo();
                    break;
                }
            }
        }

        if (m_spells.hunter.pConcussiveShot &&
            pVictim->IsMoving() && (pVictim->GetVictim() == me) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pConcussiveShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pConcussiveShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pAimedShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pAimedShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pAimedShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pArcaneShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pArcaneShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pArcaneShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pSerpentSting &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pSerpentSting))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pSerpentSting) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pMultiShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pMultiShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pMultiShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pAspectOfTheCheetah &&
            me->HasAura(m_spells.hunter.pAspectOfTheCheetah->Id))
        {
            if (pVictim->CanReachWithMeleeAutoAttack(me))
            {
                if (m_spells.hunter.pAspectOfTheMonkey &&
                    CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheMonkey))
                {
                    if (DoCastSpell(me, m_spells.hunter.pAspectOfTheMonkey) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.hunter.pAspectOfTheHawk &&
                    CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
                {
                    if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (pVictim->CanReachWithMeleeAutoAttack(me))
        {
            if (me->HasUnitState(UNIT_STATE_ROOT))
            {
                if (m_spells.hunter.pMongooseBite &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pMongooseBite))
                {
                    if (DoCastSpell(pVictim, m_spells.hunter.pMongooseBite) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.hunter.pRaptorStrike &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pRaptorStrike))
                {
                    if (DoCastSpell(pVictim, m_spells.hunter.pRaptorStrike) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.hunter.pWingClip &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pWingClip))
                {
                    DoCastSpell(pVictim, m_spells.hunter.pWingClip);
                }
            }
        }

        if (!me->HasUnitState(UNIT_STATE_ROOT) &&
            (me->GetCombatDistance(pVictim) < 8.0f) &&
             me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE)
        {
            if (!me->IsStopped())
                me->StopMoving();
            me->GetMotionMaster()->Clear();
            if (me->GetMotionMaster()->MoveDistance(pVictim, 25.0f))
                return;
        }
    }
}

void BattleBotAI::UpdateOutOfCombatAI_Mage()
{
    if (m_spells.mage.pArcaneBrilliance)
    {
        if (CanTryToCastSpell(me, m_spells.mage.pArcaneBrilliance))
        {
            if (DoCastSpell(me, m_spells.mage.pArcaneBrilliance) == SPELL_CAST_OK)
                return;
        }
    }
    else if (m_spells.mage.pArcaneIntellect)
    {
        if (CanTryToCastSpell(me, m_spells.mage.pArcaneIntellect))
        {
            if (DoCastSpell(me, m_spells.mage.pArcaneIntellect) == SPELL_CAST_OK)
                return;
        }
    }

    if (m_spells.mage.pIceArmor &&
        CanTryToCastSpell(me, m_spells.mage.pIceArmor))
    {
        if (DoCastSpell(me, m_spells.mage.pIceArmor) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.mage.pIceBarrier &&
        CanTryToCastSpell(me, m_spells.mage.pIceBarrier))
    {
        if (DoCastSpell(me, m_spells.mage.pIceBarrier) == SPELL_CAST_OK)
            return;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Mage();
}

void BattleBotAI::UpdateInCombatAI_Mage()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.mage.pCombustion &&
            CanTryToCastSpell(me, m_spells.mage.pCombustion))
        {
            if (DoCastSpell(me, m_spells.mage.pCombustion) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPyroblast &&
            m_spells.mage.pPresenceOfMind &&
            me->HasAura(m_spells.mage.pPresenceOfMind->Id) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pPyroblast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pPyroblast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pIceBlock &&
           (me->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pIceBlock))
        {
            if (DoCastSpell(me, m_spells.mage.pIceBlock) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pManaShield &&
            IsPhysicalDamageClass(pVictim->GetClass()) &&
           (me->GetPowerPercent(POWER_MANA) > 20.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pManaShield))
        {
            if (DoCastSpell(me, m_spells.mage.pManaShield) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pCounterspell &&
            pVictim->IsNonMeleeSpellCasted(false, false, true) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pCounterspell))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pCounterspell) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }
        else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                (pVictim->GetVictim() == me) &&
                (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
        {
            if (m_spells.mage.pConeofCold &&
                CanTryToCastSpell(me, m_spells.mage.pConeofCold))
            {
                if (DoCastSpell(pVictim, m_spells.mage.pConeofCold) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pBlink &&
               (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE) ||
                me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
                CanTryToCastSpell(me, m_spells.mage.pBlink))
            {
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
                    me->GetMotionMaster()->MoveIdle();

                if (DoCastSpell(me, m_spells.mage.pBlink) == SPELL_CAST_OK)
                    return;
            }

            if (!me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))
            {
                if (m_spells.mage.pFrostNova &&
                    !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
                    !pVictim->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                    CanTryToCastSpell(me, m_spells.mage.pFrostNova))
                {
                    DoCastSpell(me, m_spells.mage.pFrostNova);
                }

                if (me->GetMotionMaster()->MoveDistance(pVictim, 25.0f))
                    return;
            }
        }

        if (GetAttackersInRangeCount(10.0f) > 1)
        {
            if (m_spells.mage.pBlastWave &&
                CanTryToCastSpell(me, m_spells.mage.pBlastWave))
            {
                if (DoCastSpell(me, m_spells.mage.pBlastWave) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pArcaneExplosion &&
                CanTryToCastSpell(me, m_spells.mage.pArcaneExplosion))
            {
                if (DoCastSpell(me, m_spells.mage.pArcaneExplosion) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
            return;

        if (m_spells.mage.pRemoveLesserCurse &&
           (me->GetAttackers().size() < 3) &&
            CanTryToCastSpell(me, m_spells.mage.pRemoveLesserCurse) &&
            IsValidDispelTarget(me, m_spells.mage.pRemoveLesserCurse))
        {
            if (DoCastSpell(me, m_spells.mage.pRemoveLesserCurse) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPolymorph)
        {
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {
                if (CanTryToCastSpell(pVictim, m_spells.mage.pPolymorph))
                {
                    if (DoCastSpell(pVictim, m_spells.mage.pPolymorph) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.mage.pArcanePower &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pArcanePower))
        {
            if (DoCastSpell(me, m_spells.mage.pArcanePower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPresenceOfMind &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pPresenceOfMind))
        {
            if (DoCastSpell(me, m_spells.mage.pPresenceOfMind) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pScorch &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pScorch))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pScorch) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFrostbolt &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFrostbolt))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFrostbolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireBlast &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireBlast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireball &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireball))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireball) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pEvocation &&
           (me->GetPowerPercent(POWER_MANA) < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) == 0) &&
            CanTryToCastSpell(me, m_spells.mage.pEvocation))
        {
            if (DoCastSpell(me, m_spells.mage.pEvocation) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(BB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            PolicyAwareCast(pVictim, BB_SPELL_SHOOT_WAND, false);
    }
}

void BattleBotAI::UpdateOutOfCombatAI_Priest()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {
        if (m_spells.priest.pPrayerofFortitude)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPrayerofFortitude))
            {
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPrayerofFortitude))
                {
                    if (DoCastSpell(pTarget, m_spells.priest.pPrayerofFortitude) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.priest.pPrayerofSpirit)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPrayerofSpirit))
            {
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPrayerofSpirit))
                {
                    if (DoCastSpell(pTarget, m_spells.priest.pPrayerofSpirit) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.priest.pShadowProtection)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pShadowProtection))
            {
                if (CanTryToCastSpell(pTarget, m_spells.priest.pShadowProtection))
                {
                    if (DoCastSpell(pTarget, m_spells.priest.pShadowProtection) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }
    else if (bg && bg->GetStatus() == STATUS_IN_PROGRESS)
    {
        if (m_spells.priest.pPowerWordFortitude &&
            IsValidBuffTarget(me, m_spells.priest.pPowerWordFortitude) &&
            CanTryToCastSpell(me, m_spells.priest.pPowerWordFortitude))
        {
            if (DoCastSpell(me, m_spells.priest.pPowerWordFortitude) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                return;
            }
        }

        if (m_spells.priest.pDivineSpirit &&
            IsValidBuffTarget(me, m_spells.priest.pDivineSpirit) &&
            CanTryToCastSpell(me, m_spells.priest.pDivineSpirit))
        {
            if (DoCastSpell(me, m_spells.priest.pDivineSpirit) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                return;
            }
        }
    }

    if (m_spells.priest.pInnerFire &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFire))
    {
        if (DoCastSpell(me, m_spells.priest.pInnerFire) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.priest.pPowerWordFortitude ||
        !me->HasGCD(m_spells.priest.pPowerWordFortitude)))
    {
        m_isBuffing = false;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Priest();
}

void BattleBotAI::UpdateInCombatAI_Priest()
{
    if (m_spells.priest.pPowerWordShield &&
        CanTryToCastSpell(me, m_spells.priest.pPowerWordShield))
    {
        if (DoCastSpell(me, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.priest.pInnerFocus &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFocus))
    {
        DoCastSpell(me, m_spells.priest.pInnerFocus);
    }

    // Heal
    if (me->GetShapeshiftForm() == FORM_NONE &&
        FindAndHealInjuredAlly(40.0f))
        return;

    // Dispels
    if (m_spells.priest.pDispelMagic)
    {
        if (m_role == ROLE_HEALER)
        {
            if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pDispelMagic))
            {
                if (CanTryToCastSpell(pFriend, m_spells.priest.pDispelMagic))
                {
                    if (DoCastSpell(pFriend, m_spells.priest.pDispelMagic) == SPELL_CAST_OK)
                        return;
                }
            }
        }
        else if (IsValidDispelTarget(me, m_spells.priest.pDispelMagic) &&
                 CanTryToCastSpell(me, m_spells.priest.pDispelMagic))
        {
            if (DoCastSpell(me, m_spells.priest.pDispelMagic) == SPELL_CAST_OK)
                return;
        }
    }
    if (m_spells.priest.pAbolishDisease)
    {
        if (m_role == ROLE_HEALER)
        {
            if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pAbolishDisease))
            {
                if (CanTryToCastSpell(pFriend, m_spells.priest.pAbolishDisease))
                {
                    if (DoCastSpell(pFriend, m_spells.priest.pAbolishDisease) == SPELL_CAST_OK)
                        return;
                }
            }
        }
        else if (IsValidDispelTarget(me, m_spells.priest.pAbolishDisease) &&
                 CanTryToCastSpell(me, m_spells.priest.pAbolishDisease))
        {
            if (DoCastSpell(me, m_spells.priest.pAbolishDisease) == SPELL_CAST_OK)
                return;
        }
    }

    // Attack
    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.priest.pShadowform &&
            CanTryToCastSpell(me, m_spells.priest.pShadowform))
        {
            if (DoCastSpell(me, m_spells.priest.pShadowform) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pSilence &&
            pVictim->IsNonMeleeSpellCasted() &&
            CanTryToCastSpell(pVictim, m_spells.priest.pSilence))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pSilence) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pVampiricEmbrace &&
            CanTryToCastSpell(pVictim, m_spells.priest.pVampiricEmbrace))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pVampiricEmbrace) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindBlast &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindBlast))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pShadowWordPain &&
            CanTryToCastSpell(pVictim, m_spells.priest.pShadowWordPain))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pShadowWordPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pDevouringPlague &&
            CanTryToCastSpell(pVictim, m_spells.priest.pDevouringPlague))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pDevouringPlague) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pPsychicScream &&
            GetAttackersInRangeCount(10.0f) &&
            CanTryToCastSpell(me, m_spells.priest.pPsychicScream))
        {
            if (DoCastSpell(me, m_spells.priest.pPsychicScream) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pManaBurn &&
           (pVictim->GetPowerType() == POWER_MANA) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pManaBurn))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pManaBurn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindFlay &&
           (!GetAttackersInRangeCount(10.0f) || me->HasAuraType(SPELL_AURA_SCHOOL_ABSORB)) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindFlay))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindFlay) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (me->GetShapeshiftForm() == FORM_NONE)
        {
            if (m_spells.priest.pHolyNova &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                CanTryToCastSpell(me, m_spells.priest.pHolyNova))
            {
                if (DoCastSpell(me, m_spells.priest.pHolyNova) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.priest.pSmite &&
                CanTryToCastSpell(pVictim, m_spells.priest.pSmite))
            {
                if (DoCastSpell(pVictim, m_spells.priest.pSmite) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->HasSpell(BB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            PolicyAwareCast(pVictim, BB_SPELL_SHOOT_WAND, false);
    }

}

void BattleBotAI::UpdateOutOfCombatAI_Warlock()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {
        if (m_spells.warlock.pDetectInvisibility)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.warlock.pDetectInvisibility))
            {
                if (CanTryToCastSpell(pTarget, m_spells.warlock.pDetectInvisibility))
                {
                    if (DoCastSpell(pTarget, m_spells.warlock.pDetectInvisibility) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }

    if (m_spells.warlock.pDemonArmor &&
        CanTryToCastSpell(me, m_spells.warlock.pDemonArmor))
    {
        if (DoCastSpell(me, m_spells.warlock.pDemonArmor) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.warlock.pDetectInvisibility ||
        !me->HasGCD(m_spells.warlock.pDetectInvisibility)))
    {
        m_isBuffing = false;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (Pet* pPet = me->GetPet())
        {
            if (!pPet->GetVictim())
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Warlock();
    }
    else
        SummonPetIfNeeded();
}

void BattleBotAI::UpdateInCombatAI_Warlock()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.warlock.pDeathCoil &&
           (pVictim->CanReachWithMeleeAutoAttack(me) || pVictim->IsNonMeleeSpellCasted()) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDeathCoil))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDeathCoil) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowburn &&
           (pVictim->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowburn))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowburn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSearingPain &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSearingPain))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSearingPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowWard &&
           (pVictim->GetClass() == CLASS_WARLOCK) &&
            CanTryToCastSpell(me, m_spells.warlock.pShadowWard))
        {
            if (DoCastSpell(me, m_spells.warlock.pShadowWard) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDemonicSacrifice)
        {
            if (Pet* pPet = me->GetPet())
            {
                if (pPet->IsAlive() &&
                    CanTryToCastSpell(pPet, m_spells.warlock.pDemonicSacrifice))
                {
                    if (DoCastSpell(pPet, m_spells.warlock.pDemonicSacrifice) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.warlock.pImmolate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pImmolate))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pImmolate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pConflagrate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pConflagrate))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pConflagrate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pCorruption &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCorruption))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pCorruption) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSiphonLife &&
           (me->GetHealthPercent() < 80.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSiphonLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSiphonLife) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDrainLife &&
           (me->GetHealthPercent() < 30.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDrainLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDrainLife) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pFear &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pFear))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pFear) == SPELL_CAST_OK)
                return;
        }

        if (pVictim->IsCaster())
        {
            if (m_spells.warlock.pCurseofTongues &&
                CanTryToCastSpell(pVictim, m_spells.warlock.pCurseofTongues))
            {
                if (DoCastSpell(pVictim, m_spells.warlock.pCurseofTongues) == SPELL_CAST_OK)
                    return;
            }
        }
        else
        {
            if (m_spells.warlock.pCurseofExhaustion &&
                CanTryToCastSpell(pVictim, m_spells.warlock.pCurseofExhaustion))
            {
                if (DoCastSpell(pVictim, m_spells.warlock.pCurseofExhaustion) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (m_spells.warlock.pHowlofTerror &&
            GetAttackersInRangeCount(10.0f) > 1 &&
            CanTryToCastSpell(me, m_spells.warlock.pHowlofTerror))
        {
            if (DoCastSpell(me, m_spells.warlock.pHowlofTerror) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowBolt &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowBolt))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowBolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pLifeTap &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
           (me->GetHealthPercent() > 70.0f) &&
            CanTryToCastSpell(me, m_spells.warlock.pLifeTap))
        {
            if (DoCastSpell(me, m_spells.warlock.pLifeTap) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(BB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            PolicyAwareCast(pVictim, BB_SPELL_SHOOT_WAND, false);
    }
}

void BattleBotAI::UpdateOutOfCombatAI_Warrior()
{
    if (m_spells.warrior.pBattleStance &&
        CanTryToCastSpell(me, m_spells.warrior.pBattleStance))
    {
        if (DoCastSpell(me, m_spells.warrior.pBattleStance) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.warrior.pBattleShout &&
       !me->HasAura(m_spells.warrior.pBattleShout->Id))
    {
        if (CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
            DoCastSpell(me, m_spells.warrior.pBattleShout);
        else if (m_spells.warrior.pBloodrage &&
            (me->GetPower(POWER_RAGE) < 10) &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
        {
            DoCastSpell(me, m_spells.warrior.pBloodrage);
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.warrior.pCharge &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pCharge))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pCharge) == SPELL_CAST_OK)
                return;
        }
    }
}

void BattleBotAI::UpdateInCombatAI_Warrior()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (pVictim->IsNonMeleeSpellCasted(false, false, true))
        {
            if (m_spells.warrior.pPummel &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pPummel))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pPummel) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pShieldBash &&
                IsWearingShield(me) &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldBash))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldBash) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pExecute &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pExecute))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pExecute) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pOverpower &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pOverpower))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pOverpower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pLastStand &&
            me->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(me, m_spells.warrior.pLastStand))
        {
            if (DoCastSpell(me, m_spells.warrior.pLastStand) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pConcussionBlow &&
           (pVictim->IsNonMeleeSpellCasted() || pVictim->IsMoving() || (me->GetHealthPercent() < 50.0f)) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pConcussionBlow))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pConcussionBlow) == SPELL_CAST_OK)
                return;
        }

        if (me->GetShapeshiftForm() == FORM_DEFENSIVESTANCE &&
            IsWearingShield(me))
        {
            if (!me->GetAttackers().empty() &&
                IsPhysicalDamageClass(pVictim->GetClass()))
            {
                if (m_spells.warrior.pShieldBlock &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldBlock))
                {
                    if (DoCastSpell(me, m_spells.warrior.pShieldBlock) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.warrior.pShieldWall &&
                    (me->GetHealthPercent() < 40.0f) &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldWall))
                {
                    if (DoCastSpell(me, m_spells.warrior.pShieldWall) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.warrior.pShieldSlam &&
                CanTryToCastSpell(me, m_spells.warrior.pShieldSlam))
            {
                if (DoCastSpell(me, m_spells.warrior.pShieldSlam) == SPELL_CAST_OK)
                    return;
            }
        }

        if (pVictim->IsMoving() &&
           !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
           !pVictim->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED))
        {
            if (m_spells.warrior.pHamstring &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pHamstring))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pHamstring) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.warrior.pPiercingHowl &&
               (me->GetCombatDistance(pVictim) <= 10.0f) &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pPiercingHowl))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pPiercingHowl) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pRend &&
           (pVictim->GetClass() == CLASS_ROGUE) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pRend))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pRend) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pIntimidatingShout &&
           (me->GetHealthPercent() < 50.0f) &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntimidatingShout))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pIntimidatingShout) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pRetaliation &&
            IsMeleeDamageClass(pVictim->GetClass()) &&
           (me->GetHealthPercent() > 70.0f) &&
           ((GetAttackersInRangeCount(10.0f) > 1) || (pVictim->GetClass() == CLASS_ROGUE)) &&
            CanTryToCastSpell(me, m_spells.warrior.pRetaliation))
        {
            if (DoCastSpell(me, m_spells.warrior.pRetaliation) == SPELL_CAST_OK)
                return;
        }

        if ((me->GetHealthPercent() > 60.0f) && (pVictim->GetHealthPercent() > 40.0f) &&
            (pVictim->GetClass() == CLASS_WARLOCK || pVictim->GetClass() == CLASS_PRIEST) &&
            !me->HasUnitState(UNIT_STATE_ROOT) &&
            !me->IsImmuneToMechanic(MECHANIC_FEAR))
        {
            if (m_spells.warrior.pRecklessness &&
                CanTryToCastSpell(me, m_spells.warrior.pRecklessness))
            {
                if (DoCastSpell(me, m_spells.warrior.pRecklessness) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pDeathWish &&
                CanTryToCastSpell(me, m_spells.warrior.pDeathWish))
            {
                if (DoCastSpell(me, m_spells.warrior.pDeathWish) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pBerserkerRage &&
                CanTryToCastSpell(me, m_spells.warrior.pBerserkerRage))
            {
                if (DoCastSpell(me, m_spells.warrior.pBerserkerRage) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pMortalStrike &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pMortalStrike))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pMortalStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pBloodthirst &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pBloodthirst))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pBloodthirst) == SPELL_CAST_OK)
                return;
        }

        if (me->GetHealthPercent() < 20.0f)
        {
            if (m_spells.warrior.pDefensiveStance &&
                CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
            {
                DoCastSpell(me, m_spells.warrior.pDefensiveStance);
            }
        }
        else
        {
            if (m_spells.warrior.pBerserkerStance &&
               (pVictim->GetClass() != CLASS_ROGUE) &&
                CanTryToCastSpell(me, m_spells.warrior.pBerserkerStance))
            {
                DoCastSpell(me, m_spells.warrior.pBerserkerStance);
            }
        }

        if (m_spells.warrior.pIntercept &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntercept))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pIntercept) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pWhirlwind &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pWhirlwind))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pWhirlwind) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pDisarm &&
            IsMeleeWeaponClass(pVictim->GetClass()) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pDisarm))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pDisarm) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && !me->CanReachWithMeleeAutoAttack(pVictim))
        {
            BeginChasing(pVictim);
        }

        if (m_spells.warrior.pHeroicStrike &&
           (me->GetPower(POWER_RAGE) > 30) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pHeroicStrike))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pHeroicStrike) == SPELL_CAST_OK)
                return;
        }
    }
    else // no victim
    {
        if (m_spells.warrior.pBattleShout &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
                return;
        }
    }
}

void BattleBotAI::UpdateOutOfCombatAI_Rogue()
{
    if (m_spells.rogue.pMainHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pMainHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pMainHandPoison, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.rogue.pOffHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pOffHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pOffHandPoison, EQUIPMENT_SLOT_OFFHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.rogue.pStealth &&
        CanTryToCastSpell(me, m_spells.rogue.pStealth) &&
       !me->HasAura(AURA_WARSONG_FLAG) &&
       !me->HasAura(AURA_SILVERWING_FLAG))
    {
        if (DoCastSpell(me, m_spells.rogue.pStealth) == SPELL_CAST_OK)
            return;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Rogue();
}

void BattleBotAI::UpdateInCombatAI_Rogue()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pPremeditation &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pPremeditation))
            {
                DoCastSpell(pVictim, m_spells.rogue.pPremeditation);
            }

            if (pVictim->IsCaster())
            {
                if (m_spells.rogue.pGarrote &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pGarrote))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pGarrote) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.rogue.pAmbush &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pAmbush))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pAmbush) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.rogue.pCheapShot &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pCheapShot))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pCheapShot) == SPELL_CAST_OK)
                        return;
                }
            }
        }
        else
        {
            if (m_spells.rogue.pVanish &&
                (me->GetHealthPercent() < 10.0f))
            {
                if (m_spells.rogue.pPreparation &&
                    !me->IsSpellReady(m_spells.rogue.pVanish) &&
                    CanTryToCastSpell(me, m_spells.rogue.pPreparation))
                {
                    if (DoCastSpell(me, m_spells.rogue.pPreparation) == SPELL_CAST_OK)
                        return;
                }

                if (CanTryToCastSpell(me, m_spells.rogue.pVanish))
                {
                    if (DoCastSpell(me, m_spells.rogue.pVanish) == SPELL_CAST_OK)
                    {
                        if (me->GetMotionMaster()->MoveDistance(pVictim, 40.0f))
                            return;
                    }
                }
            }
        }

        if (me->GetComboPoints() > 4)
        {
            std::vector<SpellEntry const*> vSpells;
            if (m_spells.rogue.pSliceAndDice)
                vSpells.push_back(m_spells.rogue.pSliceAndDice);
            if (m_spells.rogue.pEviscerate)
                vSpells.push_back(m_spells.rogue.pEviscerate);
            if (m_spells.rogue.pKidneyShot)
                vSpells.push_back(m_spells.rogue.pKidneyShot);
            if (m_spells.rogue.pExposeArmor)
                vSpells.push_back(m_spells.rogue.pExposeArmor);
            if (m_spells.rogue.pRupture)
                vSpells.push_back(m_spells.rogue.pRupture);
            if (!vSpells.empty())
            {
                SpellEntry const* pComboSpell = SelectRandomContainerElement(vSpells);
                if (CanTryToCastSpell(pVictim, pComboSpell))
                {
                    if (DoCastSpell(pVictim, pComboSpell) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.rogue.pBlind)
        {
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {
                if (CanTryToCastSpell(pTarget, m_spells.rogue.pBlind))
                {
                    if (DoCastSpell(pTarget, m_spells.rogue.pBlind) == SPELL_CAST_OK)
                    {
                        me->AttackStop();
                        AttackStart(pVictim);
                        return;
                    }
                }
            }
        }

        if (m_spells.rogue.pAdrenalineRush &&
           !me->GetPower(POWER_ENERGY) &&
            CanTryToCastSpell(me, m_spells.rogue.pAdrenalineRush))
        {
            if (DoCastSpell(me, m_spells.rogue.pAdrenalineRush) == SPELL_CAST_OK)
                return;
        }

        if (pVictim->IsNonMeleeSpellCasted())
        {
            if (m_spells.rogue.pGouge &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pGouge))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pGouge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pKick &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pKick))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pKick) == SPELL_CAST_OK)
                    return;
            }
        }

        if (!me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pEvasion &&
               (me->GetHealthPercent() < 80.0f) &&
               ((GetAttackersInRangeCount(10.0f) > 2) || !IsRangedDamageClass(pVictim->GetClass())) &&
                CanTryToCastSpell(me, m_spells.rogue.pEvasion))
            {
                if (DoCastSpell(me, m_spells.rogue.pEvasion) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pColdBlood &&
                CanTryToCastSpell(me, m_spells.rogue.pColdBlood))
            {
                DoCastSpell(me, m_spells.rogue.pColdBlood);
            }

            if (m_spells.rogue.pBladeFlurry &&
                CanTryToCastSpell(me, m_spells.rogue.pBladeFlurry))
            {
                if (DoCastSpell(me, m_spells.rogue.pBladeFlurry) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.rogue.pBackstab &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pBackstab))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pBackstab) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pGhostlyStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pGhostlyStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pGhostlyStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pHemorrhage &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pHemorrhage))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pHemorrhage) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSinisterStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pSinisterStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pSinisterStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSprint &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(me, m_spells.rogue.pSprint))
        {
            if (DoCastSpell(me, m_spells.rogue.pSprint) == SPELL_CAST_OK)
                return;
        }
    }
}

void BattleBotAI::UpdateOutOfCombatAI_Druid()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {
        if (m_spells.druid.pGiftoftheWild)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.druid.pGiftoftheWild))
            {
                if (CanTryToCastSpell(pTarget, m_spells.druid.pGiftoftheWild))
                {
                    if (DoCastSpell(pTarget, m_spells.druid.pGiftoftheWild) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.druid.pThorns)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.druid.pThorns))
            {
                if (CanTryToCastSpell(pTarget, m_spells.druid.pThorns))
                {
                    if (DoCastSpell(pTarget, m_spells.druid.pThorns) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }
    else
    {
        if (m_spells.druid.pMarkoftheWild && CanTryToCastSpell(me, m_spells.druid.pMarkoftheWild))
        {
            if (DoCastSpell(me, m_spells.druid.pMarkoftheWild) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                return;
            }
        }

        if (m_spells.druid.pThorns && CanTryToCastSpell(me, m_spells.druid.pThorns))
        {
            if (DoCastSpell(me, m_spells.druid.pThorns) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                return;
            }
        }
    }

    if (m_spells.druid.pNaturesGrasp &&
        CanTryToCastSpell(me, m_spells.druid.pNaturesGrasp))
    {
        if (DoCastSpell(me, m_spells.druid.pNaturesGrasp) == SPELL_CAST_OK)
            return;
    }

    if (m_isBuffing &&
       (!m_spells.druid.pMarkoftheWild ||
        !me->HasGCD(m_spells.druid.pMarkoftheWild)))
    {
        m_isBuffing = false;
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {
        if (m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK)
        {
            if (m_spells.druid.pCatForm &&
                CanTryToCastSpell(me, m_spells.druid.pCatForm))
            {
                if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pBearForm &&
                CanTryToCastSpell(me, m_spells.druid.pBearForm))
            {
                if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
                    return;
            }
        }
        else
        {
            if ((me->GetPowerPercent(POWER_MANA) >  80.0f) &&
                FindAndHealInjuredAlly(80.0f))
                return;
        }
    }
    else if (me->GetShapeshiftForm() == FORM_CAT)
    {
        if (m_spells.druid.pProwl &&
            CanTryToCastSpell(me, m_spells.druid.pProwl) &&
            !me->HasAura(AURA_WARSONG_FLAG) &&
            !me->HasAura(AURA_SILVERWING_FLAG))
        {
            if (DoCastSpell(me, m_spells.druid.pProwl) == SPELL_CAST_OK)
                return;
        }
    }

    if (me->GetVictim())
    {
        if (m_spells.druid.pMoonkinForm &&
            CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
        {
            if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
                return;
        }

        UpdateInCombatAI_Druid();
    }
    else
    {
        if (m_spells.druid.pMoonkinForm &&
            me->GetShapeshiftForm() == FORM_MOONKIN)
            me->RemoveAurasDueToSpellByCancel(m_spells.druid.pMoonkinForm->Id);

        if (m_spells.druid.pTravelForm &&
           !me->IsMounted() &&
           (!GetMountSpellId() || me->HasAura(AURA_WARSONG_FLAG) || me->HasAura(AURA_SILVERWING_FLAG)) &&
            CanTryToCastSpell(me, m_spells.druid.pTravelForm))
        {
            if (DoCastSpell(me, m_spells.druid.pTravelForm) == SPELL_CAST_OK)
                return;
        }
    }
}

void BattleBotAI::UpdateInCombatAI_Druid()
{
    if (m_spells.druid.pTravelForm &&
        me->GetShapeshiftForm() == FORM_TRAVEL)
        me->RemoveAurasDueToSpellByCancel(m_spells.druid.pTravelForm->Id);

    if (me->GetShapeshiftForm() == FORM_NONE)
    {
        if (m_spells.druid.pHibernate &&
            !me->GetAttackers().empty())
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if (CanTryToCastSpell(pAttacker, m_spells.druid.pHibernate))
            {
                if (DoCastSpell(pAttacker, m_spells.druid.pHibernate) == SPELL_CAST_OK)
                    return;
            }
        }

        // Heal
        if (FindAndHealInjuredAlly(80.0f))
            return;

        // Dispels
       SpellEntry const* pDispelSpell = m_spells.druid.pAbolishPoison ?
                                         m_spells.druid.pAbolishPoison :
                                         m_spells.druid.pCurePoison;
       if (pDispelSpell)
       {
           if (m_role == ROLE_HEALER)
           {
               if (Unit* pFriend = SelectDispelTarget(pDispelSpell))
               {
                   if (CanTryToCastSpell(pFriend, pDispelSpell))
                   {
                       if (DoCastSpell(pFriend, pDispelSpell) == SPELL_CAST_OK)
                           return;
                   }
               }
           }
           else if (IsValidDispelTarget(me, pDispelSpell) &&
                    CanTryToCastSpell(me, pDispelSpell))
           {
               if (DoCastSpell(me, pDispelSpell) == SPELL_CAST_OK)
                   return;
           }
       }

        if (m_spells.druid.pRemoveCurse)
        {
            if (Unit* pFriend = SelectDispelTarget(m_spells.druid.pRemoveCurse))
            {
                if (CanTryToCastSpell(pFriend, m_spells.druid.pRemoveCurse))
                {
                    if (DoCastSpell(pFriend, m_spells.druid.pRemoveCurse) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.druid.pInnervate &&
            me->GetVictim() &&
           (me->GetHealthPercent() > 40.0f) &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
            CanTryToCastSpell(me, m_spells.druid.pInnervate))
        {
            if (DoCastSpell(me, m_spells.druid.pInnervate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.druid.pMoonkinForm &&
            CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
        {
            if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
                return;
        }

        if (m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK)
        {
            if (Unit* pVictim = me->GetVictim())
            {
                if (m_spells.druid.pBearForm &&
                    pVictim->CanReachWithMeleeAutoAttack(me) &&
                    IsPhysicalDamageClass(pVictim->GetClass()) &&
                    CanTryToCastSpell(me, m_spells.druid.pBearForm))
                {
                    if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pCatForm &&
                    CanTryToCastSpell(me, m_spells.druid.pCatForm))
                {
                    if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
                        return;
                }
            }
        }
    }
    else
    {
        if (me->HasUnitState(UNIT_STATE_ROOT) &&
            me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT) && AllowLegacyFormExit())
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
    }

    if (Unit* pVictim = me->GetVictim())
    {
        ShapeshiftForm const form = me->GetShapeshiftForm();
        if (m_spells.druid.pBarkskin &&
           (form == FORM_NONE || form == FORM_MOONKIN) &&
           (me->GetHealthPercent() < 50.0f) &&
            CanTryToCastSpell(me, m_spells.druid.pBarkskin))
        {
            if (DoCastSpell(me, m_spells.druid.pBarkskin) == SPELL_CAST_OK)
                return;
        }

        switch (form)
        {
            case FORM_CAT:
            {
                if (me->HasDistanceCasterMovement())
                    me->SetCasterChaseDistance(0.0f);

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                    && !me->CanReachWithMeleeAutoAttack(pVictim))
                {
                    BeginChasing(pVictim);
                }

                if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
                {
                    if (m_spells.druid.pPounce &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pPounce))
                    {
                        if (DoCastSpell(pVictim, m_spells.druid.pPounce) == SPELL_CAST_OK)
                            return;
                    }
                    if (m_spells.druid.pRavage &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pRavage))
                    {
                        if (DoCastSpell(pVictim, m_spells.druid.pRavage) == SPELL_CAST_OK)
                            return;
                    }
                    if (m_spells.druid.pTigersFury &&
                        CanTryToCastSpell(me, m_spells.druid.pTigersFury))
                    {
                        if (DoCastSpell(me, m_spells.druid.pTigersFury) == SPELL_CAST_OK)
                            return;
                    }
                    return;
                }

                if (me->GetComboPoints() > 4)
                {
                    if (m_spells.druid.pFerociousBite &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pFerociousBite))
                    {
                        if (DoCastSpell(pVictim, m_spells.druid.pFerociousBite) == SPELL_CAST_OK)
                            return;
                    }

                    if (m_spells.druid.pRip &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pRip))
                    {
                        if (DoCastSpell(pVictim, m_spells.druid.pRip) == SPELL_CAST_OK)
                            return;
                    }
                }

                if (!me->CanReachWithMeleeAutoAttack(pVictim))
                {
                    if (m_spells.druid.pFaerieFireFeral &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
                    {
                        if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                            return;
                    }

                    if (m_spells.druid.pDash &&
                        pVictim->IsMoving() &&
                        CanTryToCastSpell(me, m_spells.druid.pDash))
                    {
                        if (DoCastSpell(me, m_spells.druid.pDash) == SPELL_CAST_OK)
                            return;
                    }
                }

                if (m_spells.druid.pShred &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pShred))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pShred) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pRake &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRake))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pRake) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pClaw &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pClaw))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pClaw) == SPELL_CAST_OK)
                        return;
                }

                break;
            }
            case FORM_BEAR:
            case FORM_DIREBEAR:
            {
                if (me->HasDistanceCasterMovement())
                    me->SetCasterChaseDistance(0.0f);

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                    && !me->CanReachWithMeleeAutoAttack(pVictim))
                {
                    BeginChasing(pVictim);
                }

                if (m_spells.druid.pFeralCharge &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFeralCharge))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFeralCharge) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pBash &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pBash))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pBash) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pFrenziedRegeneration &&
                   (me->GetHealthPercent() < 30.0f) &&
                    CanTryToCastSpell(me, m_spells.druid.pFrenziedRegeneration))
                {
                    if (DoCastSpell(me, m_spells.druid.pFrenziedRegeneration) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pFaerieFireFeral &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pDemoralizingRoar &&
                    IsMeleeDamageClass(pVictim->GetClass()) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pDemoralizingRoar))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pDemoralizingRoar) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pSwipe &&
                   ((me->GetPower(POWER_RAGE) > 50) || (GetAttackersInRangeCount(10.0f) > 1)) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pSwipe))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pSwipe) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pMaul &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pMaul))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pMaul) == SPELL_CAST_OK)
                        return;
                }
                break;
            }
            case FORM_NONE:
            case FORM_MOONKIN:
            {
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                    me->GetDistance(pVictim) > 30.0f)
                {
                    BeginChasing(pVictim);
                }
                else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                        (pVictim->GetVictim() == me) &&
                        !me->HasUnitState(UNIT_STATE_ROOT) &&
                        (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
                {
                    if (m_spells.druid.pEntanglingRoots &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pEntanglingRoots))
                    {
                        if (DoCastSpell(pVictim, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
                            return;
                    }
                    me->SetCasterChaseDistance(25.0f);
                    if (me->GetMotionMaster()->MoveDistance(pVictim, 25.0f))
                        return;
                }

                if (m_spells.druid.pFaerieFire &&
                   (pVictim->GetClass() == CLASS_ROGUE) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFire))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFaerieFire) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pInsectSwarm &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pInsectSwarm))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pInsectSwarm) == SPELL_CAST_OK)
                        return;
                }

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
                    return;

                if (m_spells.druid.pMoonfire &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pMoonfire))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pMoonfire) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pStarfire &&
                   (pVictim->GetHealthPercent() > 50.0f) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pStarfire))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pStarfire) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pWrath &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pWrath))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pWrath) == SPELL_CAST_OK)
                        return;
                }

                break;
            }
        }
    }
}
