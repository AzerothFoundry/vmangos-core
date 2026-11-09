#include "WorldBotPopulation.h"
#include "WorldBotDuelPolicy.h"
#include <cassert>
#include <iostream>
#include <limits>
using namespace WorldBotPopulation;
int main(){
 assert(CapacityLimits(2500,2000));assert(!CapacityLimits(2500,2001));assert(!CapacityLimits(2501,2000));assert(!CapacityLimits(100,101));assert(!CapacityLimits(0,0));assert(CapacityLimits(100,100));

 using CR=WorldBotPopulation::CityRole;
 assert(CityQuota(CR::Gate,80)==80);assert(CityQuota(CR::Gate,8)==8);assert(CityQuota(CR::Ambient,6)==6);assert(CityQuota(CR::Healer,4)==4);assert(CityQuota(CR::Healer,0)==0);
 assert(2*(CityQuota(CR::Gate,80)+CityQuota(CR::Ambient,6)+CityQuota(CR::Healer,4))==180);
 assert(AdmissionPriority(true,false,true)<AdmissionPriority(false,true,true));assert(AdmissionPriority(false,true,true)<AdmissionPriority(false,false,true));

 assert(!WorldBotDuelPolicy::ReturnToCenter(59,false));assert(WorldBotDuelPolicy::ReturnToCenter(61,false));assert(WorldBotDuelPolicy::ReturnToCenter(30,true));assert(!WorldBotDuelPolicy::ReturnToCenter(10,true));assert(!WorldBotDuelPolicy::ReturnToCenter(NAN,true));
 assert(WorldBotDuelPolicy::InsideReturnEnvelope(69,61));assert(!WorldBotDuelPolicy::InsideReturnEnvelope(75,61));assert(WorldBotDuelPolicy::InsideReturnEnvelope(79,80));assert(!WorldBotDuelPolicy::InsideReturnEnvelope(81,80));
 assert(SeedSnapshot(60,60,20000000,0));assert(SeedSnapshot(1,1,20000000,0));assert(!SeedSnapshot(60,60,1000000,0));assert(!SeedSnapshot(60,60,20000000,1));assert(!SeedSnapshot(60,60,20000000,0));assert(!SeedSnapshot(1,60,20000000,0));assert(!SeedSnapshot(0,0,20000000,0));
 assert(CampPoint(0,60));assert(CampPoint(60,60));assert(!CampPoint(60.1f,60));assert(!CampPoint(NAN,60));assert(!CampPoint(1,0));
 assert(StarterQuiver(true,1,0,1000000,1000000,{{2101,0,0,0}}));assert(!StarterQuiver(false,1,0,1000000,1000000,{{2101,0,0,0}}));assert(!StarterQuiver(true,1,1,1000000,1000000,{{2101,0,0,0}}));assert(!StarterQuiver(true,1,0,20000000,1000000,{{2101,0,0,0}}));assert(!StarterQuiver(true,60,0,1000000,1000000,{{2101,0,0,0}}));assert(!StarterQuiver(true,1,0,1000000,1000000,{{2101,4500,0,0}}));assert(!StarterQuiver(true,1,0,1000000,1000000,{{2102,0,0,0}}));
 assert(PresenceOf(false,false)==Presence::Initializing);assert(PresenceOf(false,true)==Presence::Initializing);assert(PresenceOf(true,false)==Presence::Transferring);assert(PresenceOf(true,true)==Presence::Ready);
 assert(!LoginTimedOut(true,false,86400000,0));assert(!LoginTimedOut(true,true,86400000,1));assert(!LoginTimedOut(false,false,86400000,0));assert(!LoginTimedOut(false,true,60000,0));assert(LoginTimedOut(false,true,60001,0));
 Safety s;assert(Safe(s));assert(Retire(true,true,s));assert(!Retire(false,true,s));assert(!Retire(true,false,s));
 for(unsigned mask=1;mask<128;++mask){Safety busy;busy.humanParty=mask&1;busy.combat=mask&2;busy.duel=mask&4;busy.bg=mask&8;busy.instance=mask&16;busy.teleport=mask&32;busy.casting=mask&64;assert(!Retire(true,true,busy));}
 s.alive=false;assert(!Retire(true,true,s));
 assert(!Expired(10799999,0,10800));assert(Expired(10800000,0,10800));
 assert(Expired(999,std::numeric_limits<unsigned>::max()-1000,2));
 assert(Admit(true,49,50,false));assert(!Admit(true,50,50,false));assert(!Admit(true,51,50,false));
 assert(Admit(false,50,50,false));assert(!Admit(true,0,50,true));
 assert(Priority(true,true)<Priority(false,true));assert(Priority(false,true)<Priority(false,false));
 assert(Deficit(16,50)==34);assert(Deficit(51,50)==0);
 std::mt19937 rng(123);bool differs=false;unsigned first=SessionSeconds(10800,18000,rng);
 for(unsigned i=0;i<10000;++i){unsigned duration=SessionSeconds(10800,18000,rng);assert(duration>=10800&&duration<=18000);if(duration!=first)differs=true;}assert(differs);
 std::cout<<"PASS production population policy: protected activity, expiry/wrap, cap vs leveling, city-class priority, individual 3-5h sessions, truthful deficits\n";
}
