#include "../Battlegrounds/BattleGroundCapturePolicy.h"
#include "BattleBotTactics.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
namespace BattleBotTactics
{
bool WsgSupportEligible(float distance,float carrierX)
{return std::isfinite(distance)&&std::isfinite(carrierX)&&(distance<=100.0f||std::abs(carrierX-1233.88f)<=120.0f);}
unsigned WsgPriority(bool enemyCarrier,bool carrierThreat,bool humanThreat)
{
    return enemyCarrier ? 0 : carrierThreat ? 1 : humanThreat ? 2 : 3;
}
bool WsgAttackAllowed(bool healer,bool threatensSelf,float distance)
{
    return std::isfinite(distance) && (!healer || (threatensSelf && distance<=10.0f));
}
bool WsgObjectiveCombatAllowed(WsgTask task,float targetToGoal,float selfToGoal,bool threatensSelf)
{
    if(!std::isfinite(targetToGoal) || !std::isfinite(selfToGoal))return false;
    if(task==WsgTask::Escort)
        return targetToGoal<=30.0f && (selfToGoal<=30.0f || threatensSelf);
    if(task==WsgTask::Recover)return targetToGoal<=45.0f;
    if(task==WsgTask::Midfield)return targetToGoal<=80.0f || threatensSelf;
    return true;
}
bool WsgNeedsRegroup(WsgTask task,float distance)
{
    return (task==WsgTask::Escort || task==WsgTask::Recover) &&
        (!std::isfinite(distance) || distance>30.0f);
}
float WsgStandOff(WsgTask task,bool healer,bool droppedFlag)
{
    if(task==WsgTask::Recover && droppedFlag)return 3.0f;
    if(task==WsgTask::Midfield)return healer ? 28.0f : 18.0f;
    if(task==WsgTask::Recover)return healer ? 25.0f : 10.0f;
    return healer ? 18.0f : 10.0f;
}
std::map<Guid,WsgSquad> AssignWsgSquads(std::vector<WsgCandidate> candidates)
{
    // Roles use the whole roster, including dead members, so resurrection does
    // not shuffle both squads. Only joining/leaving can change the assignment.
    auto fast=[](unsigned cls){return cls==11 || cls==4 || cls==3;};
    std::sort(candidates.begin(),candidates.end(),[&](WsgCandidate const& a,WsgCandidate const& b){
        if(a.healer!=b.healer)return a.healer;
        if(fast(a.playerClass)!=fast(b.playerClass))return fast(a.playerClass);
        return a.guid<b.guid;
    });
    candidates.erase(std::unique(candidates.begin(),candidates.end(),[](WsgCandidate const& a,WsgCandidate const& b){return a.guid==b.guid;}),candidates.end());
    std::map<Guid,WsgSquad> result;
    unsigned flagCapacity=(candidates.size()+1)/2,midCapacity=candidates.size()/2;
    unsigned flags=0,mids=0,flagHealers=0,midHealers=0;
    for(auto const& c:candidates)
    {
        bool flag=c.healer ? (flags<flagCapacity && (mids>=midCapacity || flagHealers<=midHealers)) : flags<flagCapacity;
        result[c.guid]=flag ? WsgSquad::Flag : WsgSquad::Midfield;
        if(flag){++flags;if(c.healer)++flagHealers;}
        else {++mids;if(c.healer)++midHealers;}
    }
    return result;
}
WsgTask PlanWsgTask(WsgSquad squad,bool carrying,bool friendlyCarrier,bool ownFlagAway,unsigned flagIndex)
{
    if(carrying)return WsgTask::Return;
    if(squad==WsgSquad::Midfield)return WsgTask::Midfield;
    if(ownFlagAway && flagIndex<2)return WsgTask::Recover;
    if(friendlyCarrier)return flagIndex<4 ? WsgTask::Escort : WsgTask::Midfield;
    return WsgTask::Capture;
}

bool AbSecured(AbState state){return state==AbState::FriendlyOccupied || state==AbState::FriendlyContested;}
int AbChooseSide(std::array<AbState,5> const& states,std::array<float,5> const& costs,int previousSide)
{
    auto reachable=[&](unsigned n){return std::isfinite(costs[n]);};
    if((previousSide==3||previousSide==4)&&reachable(previousSide))return previousSide;
    if(AbSecured(states[3])&&reachable(3))return 3;
    if(AbSecured(states[4])&&reachable(4))return 4;
    if(reachable(3)&&(!reachable(4)||costs[3]<=costs[4]))return 3;
    return reachable(4)?4:-1;
}
int AbAdvanceNode(bool alliance,std::array<AbState,5> const& states,std::array<float,5> const& costs,int selectedSide)
{
    unsigned home=alliance?0:2,enemy=alliance?2:0;
    auto usable=[&](unsigned n){return !AbSecured(states[n]) && std::isfinite(costs[n]);};
    if(usable(home))return int(home);
    if(usable(1))return 1;
    if((selectedSide==3||selectedSide==4)&&usable(selectedSide))return selectedSide;
    if(usable(enemy))return int(enemy);
    unsigned remaining=selectedSide==3?4:3;
    return usable(remaining)?int(remaining):-1;
}
bool AbAdvanceAttackAllowed(float targetToGoal,bool threatensSelf)
{return threatensSelf || (std::isfinite(targetToGoal)&&targetToGoal<=55.0f);}
bool RouteNeedsRefresh(bool moving,float progress,std::uint64_t elapsedMs)
{return !moving || (elapsedMs>=6000 && (!std::isfinite(progress)||progress<1.0f));}
bool MountSavesTime(float pathLength,float currentSpeed,float mountedSpeed,float castSeconds)
{
    return std::isfinite(pathLength)&&std::isfinite(currentSpeed)&&std::isfinite(mountedSpeed)&&std::isfinite(castSeconds)&&pathLength>3.0f&&currentSpeed>0&&mountedSpeed>currentSpeed&&castSeconds>=0&&
        pathLength/currentSpeed-(castSeconds+pathLength/mountedSpeed)>1.0f;
}
namespace {
uint64_t PrepHash(uint64_t value){value+=0x9e3779b97f4a7c15ULL;value=(value^(value>>30))*0xbf58476d1ce4e5b9ULL;value=(value^(value>>27))*0x94d049bb133111ebULL;return value^(value>>31);}
}
bool AbBannerMatches(unsigned node,unsigned state,uint32_t entry,bool locked,bool properType,bool spawned)
{
 return BattleGroundCapturePolicy::ArathiBanner(node,state,entry,properType?(state?1u:10u):0u,locked,spawned);
}
bool AbClaimCapture(AbCaptureLease& lease,uint64_t owner,uint64_t flag,uint64_t now)
{
 if(!owner||!flag)return false;
 if(lease.flag!=flag||now>=lease.until)lease={};
 if(lease.owner&&lease.owner!=owner)return false;
 lease={owner,flag,now+10000};return true;
}
bool AbCaptureCandidateReady(uint64_t now,uint64_t retryAt,uint64_t hurtAt)
{return now>=retryAt&&(!hurtAt||(now>=hurtAt&&now-hurtAt>=3000));}
bool AbLandSample(bool wet,bool& drySeen,float& prefixWet,float step,float limit)
{
 if(!std::isfinite(step)||step<0||!std::isfinite(limit)||limit<0)return false;
 if(!wet){drySeen=true;return true;}
 if(drySeen)return false;
 prefixWet+=step;return prefixWet<=limit;
}
bool AbAdvanceBeforeRecovery(bool guard,bool combat,float health,float mana,bool usesMana,bool openingRush)
{return !guard&&!combat&&std::isfinite(health)&&health>=35&&(!usesMana||openingRush||(std::isfinite(mana)&&mana>=15));}
bool AbCaptureOpportunity(AbCaptureRisk const& r)
{
 if(r.threatened||r.periodic||r.recentDamage)return false;
 return !r.combat || r.enemies==0 || (!r.healer && r.friends>r.enemies && r.friends-r.enemies>=2);
}
PrepOffset PreparationOffset(Guid actor,uint32_t instance,uint32_t generation,unsigned candidate,bool converge)
{
 uint64_t hash=PrepHash(actor^(uint64_t(instance)<<32)^uint64_t(generation)*0x9e3779b97f4a7c15ULL^candidate);
 float a=float(hash&0xffffff)/float(0xffffff),b=float((hash>>24)&0xffffff)/float(0xffffff);
 return {(a*2-1)*(converge?3.5f:7.0f),converge?2.0f+b*1.0f:4.0f+b*10.0f};
}
uint32_t PreparationIdleInterval(Guid actor,uint32_t instance,uint32_t generation){return 8000+PrepHash(actor^(uint64_t(instance)<<32)^generation)%12001;}
bool PreparationDetour(float length,float direct){return std::isfinite(length)&&std::isfinite(direct)&&length<=std::max(12.0f,direct*1.75f+6.0f);}
bool InsideStartingGate(float x,float y,float gx,float gy,float sx,float sy,float margin)
{
 if(!std::isfinite(x)||!std::isfinite(y))return false;
 float nx=sx-gx,ny=sy-gy,length=std::sqrt(nx*nx+ny*ny);
 return length>0.01f&&((x-gx)*nx+(y-gy)*ny)/length>=margin;
}
AbOpeningPlan PlanAbOpening(std::vector<AbOpeningMember> members,bool alliance)
{
 std::sort(members.begin(),members.end(),[](auto const& a,auto const& b){return a.guid<b.guid;});
 members.erase(std::unique(members.begin(),members.end(),[](auto const& a,auto const& b){return a.guid==b.guid;}),members.end());
 AbOpeningPlan out;std::array<unsigned,3> groups{};bool ui=members.size()==15;
 for(auto const& m:members){if(m.group>=3)ui=false;else ++groups[m.group];}
 ui=ui&&groups[0]==5&&groups[1]==5&&groups[2]==5;
 auto pick=[&](bool third,bool nonheal){for(auto const& m:members)if(m.bot&&(!third||m.group==2)&&(!nonheal||!m.healer))return m.guid;return Guid(0);};
 out.home=ui?pick(true,true):0;if(!out.home&&ui)out.home=pick(true,false);
 if(!out.home){ui=false;out.home=pick(false,true);if(!out.home)out.home=pick(false,false);}
 out.actualRaid=ui;
 if(out.home)out.nodes[out.home]=alliance?0:2;
 std::vector<Guid> remaining;for(auto const& m:members)if(m.guid!=out.home){
  if(ui)out.nodes[m.guid]=m.group==0?1:m.group==1?3:4;else remaining.push_back(m.guid);
 }
 unsigned size=remaining.size(),bs=(size+2)/3,wood=(size+1)/3;
 for(unsigned i=0;i<size;++i)out.nodes[remaining[i]]=i<bs?1:i<bs+wood?3:4;
 return out;
}
unsigned DistanceBucket(float distance)
{
    return std::isfinite(distance) ? unsigned(std::max(0.0f,distance)/5.0f) : std::numeric_limits<unsigned>::max();
}
bool Better(Rank const& a,Rank const& b)
{
    if(a.playerBody!=b.playerBody)return a.playerBody;
    if(a.tier!=b.tier)return a.tier<b.tier;
    if(a.distanceBucket!=b.distanceBucket)return a.distanceBucket<b.distanceBucket;
    if(a.healer!=b.healer)return a.healer;
    if(a.damage!=b.damage)return a.damage>b.damage;
    return a.guid<b.guid;
}
bool ShouldSwitch(Rank const& candidate,Rank const& current,std::uint64_t heldMs)
{
    if(candidate.guid==current.guid)return false;
    if(!Better(candidate,current))return false;
    return (candidate.playerBody&&!current.playerBody) || candidate.tier<current.tier || heldMs>=2000;
}
bool InGuardLeash(float distanceToNode) { return std::isfinite(distanceToNode) && distanceToNode<=30.0f; }
Assignments AssignGuards(std::array<bool,5> const& occupied,Assignments previous,std::vector<GuardCandidate> const& candidates)
{
    std::set<Guid> used;
    for(unsigned node=0;node<5;++node)
    {
        auto candidate=std::find_if(candidates.begin(),candidates.end(),[&](GuardCandidate const& c){return c.guid==previous[node] && c.alive;});
        if(!occupied[node] || candidate==candidates.end() || !used.insert(previous[node]).second)previous[node]=0;
    }
    for(unsigned node=0;node<5;++node)
    {
        if(!occupied[node] || previous[node])continue;
        GuardCandidate const* best=nullptr;
        for(auto const& c:candidates)
        {
            if(!c.alive || used.count(c.guid) || !std::isfinite(c.distance[node]))continue;
            if(!best || (best->healer && !c.healer) || (best->healer==c.healer && (c.distance[node]<best->distance[node] || (c.distance[node]==best->distance[node] && c.guid<best->guid))))best=&c;
        }
        if(best){previous[node]=best->guid;used.insert(best->guid);}
    }
    return previous;
}
void RollingDamage::Prune(std::uint64_t now)
{
    for(auto it=rows.begin();it!=rows.end();)
    {
        while(!it->second.empty() && now>=it->second.front().time && now-it->second.front().time>=15000)it->second.pop_front();
        if(it->second.empty())it=rows.erase(it);else ++it;
    }
}
void RollingDamage::Add(std::uint32_t instance,Guid attacker,std::uint32_t damage,std::uint64_t now)
{
    if(!damage || !attacker)return;
    Prune(now);
    auto key=std::make_pair(instance,attacker);
    if(!rows.count(key) && rows.size()>=4096)return;
    auto& list=rows[key];
    list.push_back({now,damage});
    if(list.size()>4096)list.pop_front();
}
std::uint64_t RollingDamage::Get(std::uint32_t instance,Guid attacker,std::uint64_t now)
{
    Prune(now);auto it=rows.find(std::make_pair(instance,attacker));std::uint64_t total=0;
    if(it!=rows.end())for(auto const& e:it->second)total+=e.damage;
    return total;
}
}
#ifndef BATTLEBOT_TACTICS_POLICY_ONLY
#include "Unit.h"
#include "Player.h"
#include "BattleGround.h"
#include <mutex>
#include <chrono>
namespace BattleBotTactics
{
namespace { std::mutex damageMutex;RollingDamage damageWindow;
std::uint64_t Now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();} }
void RecordDamage(Unit const* attacker,Unit const* victim,std::uint32_t amount)
{
    try
    {
        if(!attacker || !victim || !amount)return;
        Player* source=attacker->GetAffectingPlayer();
        Player const* target=victim->IsPlayer() ? victim->ToPlayer() : nullptr;
        if(!source || !target || source==target)return;
        BattleGround* bg=source->GetBattleGround();
        if(!bg || bg->GetTypeID()!=BATTLEGROUND_AB || bg->GetStatus()!=STATUS_IN_PROGRESS || target->GetBattleGround()!=bg || source->GetBGTeam()==target->GetBGTeam())return;
        std::lock_guard<std::mutex> guard(damageMutex);
        damageWindow.Add(bg->GetInstanceID(),source->GetObjectGuid().GetRawValue(),amount,Now());
    }
    catch(...) { /* Observability must not alter an already-settled combat result. */ }
}
std::uint64_t Damage15(Unit const* attacker)
{
    if(!attacker)return 0;
    Player* player=attacker->GetAffectingPlayer();
    if(!player || !player->GetBattleGround() || player->GetBattleGround()->GetTypeID()!=BATTLEGROUND_AB)return 0;
    std::lock_guard<std::mutex> guard(damageMutex);
    return damageWindow.Get(player->GetBattleGround()->GetInstanceID(),player->GetObjectGuid().GetRawValue(),Now());
}
}
#endif
