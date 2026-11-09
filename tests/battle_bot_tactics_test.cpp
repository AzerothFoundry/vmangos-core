#include "../src/game/Battlegrounds/BattleGroundCapturePolicy.h"
// Compile the same production policy, without game-engine adapters:
// c++ -std=c++14 -DBATTLEBOT_TACTICS_POLICY_ONLY -Isrc/game/PlayerBots
//   src/game/PlayerBots/BattleBotTactics.cpp tests/battle_bot_tactics_test.cpp -o /tmp/battle_bot_tactics_test
#include "BattleBotTactics.h"
#include <cassert>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
using namespace BattleBotTactics;
int main()
{
    // Tiers are assigned by the engine adapter (AB capper/human/ordinary).
    Rank capper{100,0,9,false,0}, human{80,1,0,true,9000}, close{70,2,0,false,0};
    assert(Better(capper,human));assert(Better(human,close));
    Rank farHealer{60,2,1,true,9999};assert(Better(close,farHealer));
    Rank healer{80,2,0,true,0}, damage{90,2,0,false,9000};
    assert(Better(healer,damage));
    Rank damageMore{91,2,0,false,9001};assert(Better(damageMore,damage));
    Rank tie{89,2,0,false,9000};assert(Better(tie,damage));assert(!Better(damage,tie));
    assert(!ShouldSwitch(damage,damage,99999));
    assert(!ShouldSwitch(tie,damage,1999));assert(ShouldSwitch(tie,damage,2000));
    assert(ShouldSwitch(capper,damage,0));assert(!ShouldSwitch(damage,capper,99999));
    assert(DistanceBucket(9.99f)==1);assert(DistanceBucket(10)==2);
    assert(DistanceBucket(-1)==0);assert(DistanceBucket(std::numeric_limits<float>::infinity())==std::numeric_limits<unsigned>::max());
    // A high-priority distant rescue is ineligible for a defender before ranking.
    assert(InGuardLeash(30));assert(!InGuardLeash(30.01f));assert(!InGuardLeash(NAN));
    std::array<bool,5> owned{{true,true,false,false,false}};
    GuardCandidate a,b,c;a.guid=10;b.guid=11;c.guid=12;c.healer=true;
    a.distance.fill(100);b.distance.fill(10);c.distance.fill(1);
    auto guards=AssignGuards(owned,Assignments{}, {a,b,c});
    assert(guards[0]==11 && guards[1]==10 && !guards[2]);
    // Sticky ownership avoids reshuffling every second as actors move.
    a.distance.fill(0);b.distance.fill(900);
    assert(AssignGuards(owned,guards,{a,b,c})==guards);
    b.alive=false;auto replaced=AssignGuards(owned,guards,{a,b,c});
    assert(replaced[0]==12 && replaced[1]==10);
    auto left=AssignGuards(owned,guards,{a,c});assert(left==replaced);
    owned[1]=false;auto lost=AssignGuards(owned,replaced,{a,b,c});assert(lost[1]==0);
    auto shortage=AssignGuards({{true,true,true,true,true}},Assignments{}, {a});
    unsigned count=0;for(auto guid:shortage)if(guid){++count;assert(guid==10);}assert(count==1);
    assert(WsgPriority(true,true,true)==0);
    assert(WsgPriority(false,true,true)==1);
    assert(WsgPriority(false,false,true)==2);
    assert(WsgPriority(false,false,false)==3);
    assert(WsgAttackAllowed(false,false,50));
    assert(WsgAttackAllowed(true,true,10));assert(!WsgAttackAllowed(true,true,10.01f));
    assert(!WsgAttackAllowed(true,false,2));assert(!WsgAttackAllowed(false,true,NAN));
    assert(WsgStandOff(WsgTask::Recover,true)==25 && WsgStandOff(WsgTask::Recover,false)==10);
    assert(WsgStandOff(WsgTask::Recover,true,true)==3 && WsgStandOff(WsgTask::Recover,false,true)==3);
    assert(WsgStandOff(WsgTask::Escort,true)==18 && WsgStandOff(WsgTask::Midfield,true)==28);
    Rank flag{1,WsgPriority(true,false,false),9,false,0};
    Rank carrierAttacker{2,WsgPriority(false,true,true),0,false,0};
    Rank playerAttacker{3,WsgPriority(false,false,true),0,true,9999};
    assert(Better(flag,carrierAttacker) && Better(carrierAttacker,playerAttacker));
    assert(ShouldSwitch(flag,playerAttacker,0));
    std::vector<WsgCandidate> roster={{1,1,false},{2,8,false},{3,9,false},{4,4,false},{5,11,false},{6,3,false},{7,7,false},{8,1,false},{9,5,true},{10,11,true}};
    auto squads=AssignWsgSquads(roster);
    unsigned flagCount=0,midCount=0,flagHeal=0,midHeal=0;
    for(auto const& c:roster)if(squads.at(c.guid)==WsgSquad::Flag){++flagCount;if(c.healer)++flagHeal;}else{++midCount;if(c.healer)++midHeal;}
    assert(flagCount==5 && midCount==5 && flagHeal==1 && midHeal==1);
    assert(squads.at(4)==WsgSquad::Flag && squads.at(5)==WsgSquad::Flag && squads.at(6)==WsgSquad::Flag);
    std::reverse(roster.begin(),roster.end());assert(AssignWsgSquads(roster)==squads);
    assert(AssignWsgSquads({}).empty());
    assert(AssignWsgSquads({{1,1,false}}).at(1)==WsgSquad::Flag);
    auto odd=AssignWsgSquads({{1,1,false},{2,1,false},{3,1,false},{4,5,true},{5,7,true}});
    unsigned oddFlags=0;for(auto const& a:odd)if(a.second==WsgSquad::Flag)++oddFlags;assert(oddFlags==3);
    assert(PlanWsgTask(WsgSquad::Flag,false,false,false)==WsgTask::Capture);
    assert(PlanWsgTask(WsgSquad::Midfield,false,false,false)==WsgTask::Midfield);
    assert(PlanWsgTask(WsgSquad::Flag,false,false,true)==WsgTask::Recover);
    assert(PlanWsgTask(WsgSquad::Midfield,false,false,true)==WsgTask::Midfield);
    assert(PlanWsgTask(WsgSquad::Flag,false,true,true)==WsgTask::Recover);
    assert(PlanWsgTask(WsgSquad::Midfield,false,true,true)==WsgTask::Midfield);
    assert(PlanWsgTask(WsgSquad::Flag,false,true,true,2)==WsgTask::Escort);
    assert(PlanWsgTask(WsgSquad::Flag,false,true,true,4)==WsgTask::Midfield);
    assert(PlanWsgTask(WsgSquad::Flag,true,true,true)==WsgTask::Return);
    // Escort attacks are constrained to the carrier, not just distance to bot.
    assert(WsgObjectiveCombatAllowed(WsgTask::Escort,30,30,false));
    assert(!WsgObjectiveCombatAllowed(WsgTask::Escort,30.1f,10,true));
    assert(!WsgObjectiveCombatAllowed(WsgTask::Escort,10,31,false));
    assert(WsgObjectiveCombatAllowed(WsgTask::Escort,10,31,true));
    assert(!WsgObjectiveCombatAllowed(WsgTask::Recover,45.1f,10,true));
    assert(!WsgObjectiveCombatAllowed(WsgTask::Midfield,90,90,false));
    assert(!WsgObjectiveCombatAllowed(WsgTask::Escort,NAN,10,true));
    assert(WsgNeedsRegroup(WsgTask::Escort,30.1f));
    assert(WsgNeedsRegroup(WsgTask::Recover,31));
    assert(!WsgNeedsRegroup(WsgTask::Escort,30));
    assert(!WsgNeedsRegroup(WsgTask::Midfield,100));
    std::array<AbState,5> nodes;nodes.fill(AbState::Neutral);
    std::array<float,5> costs{{50,30,20,100,70}};
    assert(AbAdvanceNode(true,nodes,costs,4)==0);
    assert(AbAdvanceNode(false,nodes,costs,4)==2);
    nodes[0]=AbState::FriendlyContested;assert(AbAdvanceNode(true,nodes,costs,4)==1);
    nodes[1]=AbState::FriendlyOccupied;assert(AbChooseSide(nodes,costs,-1)==4);
    assert(AbAdvanceNode(true,nodes,costs,4)==4);
    costs[3]=1;assert(AbChooseSide(nodes,costs,4)==4); // shared sticky third point
    costs[4]=INFINITY;assert(AbChooseSide(nodes,costs,4)==3);costs[4]=70;
    nodes[3]=AbState::FriendlyContested;assert(AbChooseSide(nodes,costs,-1)==3);
    assert(AbAdvanceNode(true,nodes,costs,3)==2); // wood secured: enemy home before neutral mine
    nodes[3]=AbState::EnemyContested;assert(AbAdvanceNode(true,nodes,costs,3)==3); // retake chosen third
    nodes[0]=AbState::EnemyContested;assert(AbAdvanceNode(true,nodes,costs,3)==0);
    nodes[0]=AbState::FriendlyOccupied;nodes[1]=AbState::EnemyOccupied;assert(AbAdvanceNode(true,nodes,costs,3)==1);
    nodes[1]=nodes[2]=nodes[3]=AbState::FriendlyOccupied;assert(AbAdvanceNode(true,nodes,costs,3)==4);
    nodes.fill(AbState::FriendlyOccupied);assert(AbAdvanceNode(true,nodes,costs,3)==-1);
    assert(AbSecured(AbState::FriendlyContested));assert(!AbSecured(AbState::EnemyContested));
    assert(AbAdvanceAttackAllowed(55,false));assert(!AbAdvanceAttackAllowed(56,false));assert(AbAdvanceAttackAllowed(99,true));
    assert(WsgSupportEligible(100,1500));assert(!WsgSupportEligible(100.1f,1500));
    assert(WsgSupportEligible(500,1233.88f));assert(!WsgSupportEligible(500,1450));
    Rank body{500,3,15,false,0},pet{501,0,0,false,99999};pet.playerBody=false;
    assert(Better(body,pet));assert(!Better(pet,body));assert(ShouldSwitch(body,pet,0));
    assert(!WsgObjectiveCombatAllowed(WsgTask::Midfield,81,10,false));
    assert(!MountSavesTime(3,7,14,0));assert(!MountSavesTime(20,7,14,3));
    assert(MountSavesTime(100,7,14,3));assert(!MountSavesTime(100,14,14,0));
    assert(!MountSavesTime(100,20,14,3));assert(!MountSavesTime(NAN,7,14,3));
    assert(MountSavesTime(100,7,14,0));assert(!MountSavesTime(100,7,14,10));
    assert(!RouteNeedsRefresh(true,10,6000));assert(!RouteNeedsRefresh(true,0,5999));
    assert(RouteNeedsRefresh(true,0.9f,6000));assert(RouteNeedsRefresh(false,10,0));
    std::vector<AbOpeningMember> opening;
    for(unsigned i=1;i<=15;++i)opening.push_back({i,(i-1)/5,i!=1,i%5==2});
    auto ao=PlanAbOpening(opening,true);assert(ao.actualRaid&&ao.home>=11);
    std::array<unsigned,5> openingCounts{};for(auto const& row:ao.nodes)++openingCounts[row.second];
    assert(openingCounts[0]==1&&openingCounts[1]==5&&openingCounts[3]==5&&openingCounts[4]==4);
    assert(ao.nodes.at(1)==1); // human receives a displayed mission, never an AI control action
    std::reverse(opening.begin(),opening.end());assert(PlanAbOpening(opening,true).nodes==ao.nodes);
    auto ho=PlanAbOpening(opening,false);assert(ho.nodes.at(ho.home)==2);
    for(unsigned n:{1u,2u,5u,10u,14u}){
        std::vector<AbOpeningMember> small;for(unsigned i=1;i<=n;++i)small.push_back({i,99,true,i==2});
        auto plan=PlanAbOpening(small,true);assert(!plan.actualRaid&&plan.home!=0&&plan.nodes.size()==n);
        std::array<unsigned,5> c{};for(auto const& row:plan.nodes)++c[row.second];
        assert(c[0]==1&&c[1]+c[3]+c[4]==n-1);assert(c[1]>=c[3]&&c[3]>=c[4]&&c[1]-c[4]<=1);
    }
    for(auto& m:opening)m.bot=false;assert(PlanAbOpening(opening,true).home==0);
    // Existing authored waiting positions are inside the physical gate plane.
    assert(InsideStartingGate(1286.054f,1282.500f,1284.6f,1281.17f,1286.054f,1282.500f));
    assert(InsideStartingGate(706.607f,707.211f,708.09f,708.448f,706.607f,707.211f));
    assert(InsideStartingGate(1510.502f,1493.385f,1503.34f,1493.47f,1510.502f,1493.385f));
    assert(InsideStartingGate(944.981f,1423.478f,949.166f,1423.77f,944.981f,1423.478f));
    assert(!InsideStartingGate(1283,1280,1284.6f,1281.17f,1286.054f,1282.500f));
    assert(!InsideStartingGate(950,1423,949.166f,1423.77f,944.981f,1423.478f));
    // Stable per-actor/phase samples: no frame reroll, diverse continuous goals.
    std::vector<float> prepLateral;
    for(unsigned actor=1;actor<=40;++actor){
        auto offset=PreparationOffset(actor,123,1,0,false);
        auto repeated=PreparationOffset(actor,123,1,0,false);
        assert(offset.lateral==repeated.lateral&&offset.depth==repeated.depth);
        assert(std::abs(offset.lateral)<=7&&offset.depth>=4&&offset.depth<=14);
        auto final=PreparationOffset(actor,123,2,0,true);
        assert(std::abs(final.lateral)<=3.5f&&final.depth>=2&&final.depth<=3);
        auto interval=PreparationIdleInterval(actor,123,1);
        assert(interval>=8000&&interval<=20000);
        prepLateral.push_back(offset.lateral);
    }
    std::sort(prepLateral.begin(),prepLateral.end());
    assert(std::unique(prepLateral.begin(),prepLateral.end())==prepLateral.end());
    auto old=PreparationOffset(1,123,1,0,false),next=PreparationOffset(1,123,2,0,false);
    assert(old.lateral!=next.lateral||old.depth!=next.depth);
    assert(PreparationDetour(10,5));assert(!PreparationDetour(30,5));
    assert(!PreparationDetour(NAN,10));assert(!PreparationDetour(10,INFINITY));
    AbCaptureRisk cap;assert(AbCaptureOpportunity(cap));
    cap.combat=true;cap.enemies=1;cap.friends=2;assert(!AbCaptureOpportunity(cap));
    cap.friends=3;assert(AbCaptureOpportunity(cap));
    cap.threatened=true;assert(!AbCaptureOpportunity(cap));cap.threatened=false;
    cap.periodic=true;assert(!AbCaptureOpportunity(cap));cap.periodic=false;
    cap.recentDamage=true;assert(!AbCaptureOpportunity(cap));cap.recentDamage=false;
    cap.healer=true;assert(!AbCaptureOpportunity(cap));cap.enemies=0;assert(AbCaptureOpportunity(cap));
    AbCaptureLease lease;
    assert(AbClaimCapture(lease,10,100,1000));assert(!AbClaimCapture(lease,11,100,1001));
    assert(AbClaimCapture(lease,11,100,11000)); // expired owner cannot monopolize the flag
    assert(AbClaimCapture(lease,12,101,11001)); // banner replacement invalidates old lease
    assert(!AbClaimCapture(lease,0,101,12000));
    assert(!AbCaptureCandidateReady(2000,3000,0));assert(AbCaptureCandidateReady(3000,3000,0));
    assert(!AbCaptureCandidateReady(5000,0,3000));assert(AbCaptureCandidateReady(6000,0,3000));
    // A failed low-GUID opener yields to a different eligible candidate during backoff.
    std::vector<std::pair<uint64_t,uint64_t>> candidates{{10,6000},{11,0},{12,0}};uint64_t chosen=0;
    for(auto c:candidates)if(AbCaptureCandidateReady(5000,c.second,0)&&(!chosen||c.first<chosen))chosen=c.first;
    assert(chosen==11);
    // Actual AB events contain the aura/decorations as well as a banner; load order is not semantic.
    std::vector<uint32_t> eventObjects{180102,180218,180219,180059};unsigned banners=0;uint32_t actualBanner=0;
    for(auto entry:eventObjects)if(AbBannerMatches(0,1,entry,true,true,true)){++banners;actualBanner=entry;}
    assert(banners==1&&actualBanner==180059);
    assert(AbBannerMatches(4,0,180091,true,true,true));assert(!AbBannerMatches(0,0,180091,true,true,true));
    assert(!AbBannerMatches(0,1,180059,false,true,true));assert(!AbBannerMatches(0,1,180059,true,false,true));
    assert(!AbBannerMatches(0,1,180059,true,true,false));assert(!AbBannerMatches(0,2,180059,true,true,true));
    assert(!AbBannerMatches(5,1,180059,true,true,true));assert(!AbBannerMatches(0,5,180059,true,true,true));
    using BattleGroundCapturePolicy::RejectDirectArathiUse;
    assert(RejectDirectArathiUse(true,0,1,180059,1,true,true));
    assert(RejectDirectArathiUse(true,0,4,180060,1,true,true));
    assert(!RejectDirectArathiUse(false,0,1,180059,1,true,true)); // other instance/event
    assert(!RejectDirectArathiUse(true,0,0,180087,10,true,true)); // preserve neutral 23932
    assert(!RejectDirectArathiUse(true,0,1,180102,6,true,true)); // aura decoration
    assert(!RejectDirectArathiUse(true,0,1,12345,1,true,true)); // ordinary button
    assert(!RejectDirectArathiUse(true,0,1,180059,0,true,true)); // door
    assert(!RejectDirectArathiUse(true,0,1,180059,3,true,true)); // chest
    assert(!RejectDirectArathiUse(true,0,1,180059,1,false,true));
    assert(!RejectDirectArathiUse(true,0,1,180059,1,true,false));
    cap.combat=false;cap.enemies=4;cap.friends=1;cap.healer=true;assert(AbCaptureOpportunity(cap));
    cap.threatened=true;assert(!AbCaptureOpportunity(cap));cap.threatened=false;
    cap.recentDamage=true;assert(!AbCaptureOpportunity(cap));cap.recentDamage=false;
    cap.periodic=true;assert(!AbCaptureOpportunity(cap));
    assert(AbAdvanceBeforeRecovery(false,false,50,50,true)); // resurrection resources suffice for movement
    assert(!AbAdvanceBeforeRecovery(true,false,100,100,true)); // last assigned guard stays
    assert(!AbAdvanceBeforeRecovery(false,true,100,100,true));
    assert(!AbAdvanceBeforeRecovery(false,false,34,100,true));assert(!AbAdvanceBeforeRecovery(false,false,100,14,true));
    assert(AbAdvanceBeforeRecovery(false,false,100,0,false));assert(!AbAdvanceBeforeRecovery(false,false,NAN,100,true));
    bool seenDry=true;float wetPrefix=0;assert(AbLandSample(false,seenDry,wetPrefix,3));assert(!AbLandSample(true,seenDry,wetPrefix,3));
    seenDry=false;wetPrefix=0;assert(AbLandSample(true,seenDry,wetPrefix,10));assert(AbLandSample(false,seenDry,wetPrefix,3));assert(!AbLandSample(true,seenDry,wetPrefix,1));
    seenDry=false;wetPrefix=0;assert(!AbLandSample(true,seenDry,wetPrefix,21));assert(!AbLandSample(false,seenDry,wetPrefix,NAN));
    seenDry=false;wetPrefix=0;assert(AbLandSample(true,seenDry,wetPrefix,30,40));assert(AbLandSample(false,seenDry,wetPrefix,3,40));assert(!AbLandSample(true,seenDry,wetPrefix,1,40));
    assert(AbAdvanceBeforeRecovery(false,false,100,0,true,true)); // opening travel is zero-mana legal
    assert(!AbAdvanceBeforeRecovery(false,false,100,0,true,false));
    assert(!AbAdvanceBeforeRecovery(true,false,100,0,true,true));
    assert(!AbAdvanceBeforeRecovery(false,false,34,0,true,true));
    RollingDamage window;
    window.Add(1,10,100,1000);window.Add(1,10,200,2000);window.Add(2,10,999,2000);
    assert(window.Get(1,10,15999)==300);assert(window.Get(1,10,16000)==200);
    assert(window.Get(1,10,17000)==0);assert(window.Get(2,10,17000)==0);
    window.Add(1,10,0,20000);window.Add(1,0,999,20000);assert(window.Get(1,10,20000)==0);
    window.Add(1,10,0xffffffffu,20001);window.Add(1,10,0xffffffffu,20002);
    assert(window.Get(1,10,20003)==8589934590ull);assert(window.Get(1,11,20003)==0);
    window.Prune(35002);assert(window.Get(1,10,35002)==0);
    std::cout<<"PASS: production priority/stable-target/hysteresis, guard assignment/leash, 15-second effective-damage window\n";
}
