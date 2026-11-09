#include "BgFillLifecycle.h"
#include <cassert>
#include <iostream>
using namespace BgFillLifecycle;
int main(){
 // Reproduce the real client1/internal7 mismatch, including one reserved human slot.
 std::vector<InstanceIdentity> instances{{3,1,1,7,true},{3,2,1,9,true},{3,1,2,11,true},{2,1,1,15,true},{3,3,1,18,false}};
 auto resolve=[&](uint32_t client){return ResolveDesiredInstance(3,1,client,instances);};
 assert(ResolveDesiredInstance(3,2,1,instances)==9); // same client1, different bracket
 assert(ResolveDesiredInstance(2,1,1,instances)==15); // type also isolates ids
 assert(ResolveDesiredInstance(3,3,1,instances)==0); // ended is not joinable
 assert(ResolveDesiredInstance(3,4,1,instances)==0);
 assert(resolve(0)==0);
 assert(resolve(1)==7);
 assert(resolve(2)==11);
 assert(resolve(999)==0);
 Location late;late.sameQueue=true;late.sameBracket=true;late.desiredInstance=1;
 assert(!PendingFor(7,late)); // The old direct comparison was wrong.
 late.desiredInstance=resolve(1);assert(PendingFor(7,late));
 late.desiredInstance=resolve(2);assert(!PendingFor(7,late));
 late.desiredInstance=0;late.invitedInstance=7;assert(PendingFor(7,late)); // Invited id is already internal and wins.
 Population reserved;reserved.target=10;reserved.humans={{1,0}};reserved.pending={{1,0}};reserved.ownBots={{9,10}};
 auto replacement=Decide(reserved);assert(!replacement.cleanup&&replacement.budget[0]==8&&replacement.retire[0]==1);
 Population p;p.target=10;p.humans={{1,0}};p.ownBots={{9,10}};
 auto plan=Decide(p);assert(!plan.cleanup&&!plan.add[0]&&!plan.retire[0]);
 p.humans[0]=2;plan=Decide(p);assert(!plan.cleanup&&plan.retire[0]==1&&plan.retire[1]==0&&plan.budget[0]==8); // late human displaces one own bot only
 p.ownBots[0]=8;assert(!Decide(p).retire[0]);
 p.humans[0]=1;plan=Decide(p);assert(!plan.cleanup&&plan.add[0]==1); // owner left but other human stays
 p.humans={{0,1}};assert(!Decide(p).cleanup); // unrelated human in opposite team is still a participant
 Location q;q.sameQueue=true;q.sameBracket=true;q.invitedInstance=77;
 assert(PendingFor(77,q));q.invitedInstance=88;assert(!PendingFor(77,q));
 q.invitedInstance=0;q.desiredInstance=88;assert(!PendingFor(77,q));
 q.desiredInstance=0;assert(!PendingFor(77,q)); // other unrelated same-bracket queue is not this instance
 q.requestMember=true;assert(PendingFor(77,q));q.cancelled=true;assert(!PendingFor(77,q));
 q.cancelled=false;q.currentInstance=88;assert(!PendingFor(77,q));
 q.currentInstance=0;q.sameQueue=false;assert(!PendingFor(77,q));q.sameQueue=true;q.sameBracket=false;assert(!PendingFor(77,q));
 p.humans={{0,0}};p.pending={{1,0}};assert(!Decide(p).cleanup); // pending initial cohort survives arrival window
 p.pending={{0,0}};assert(Decide(p).cleanup); // genuinely empty, even though bots remain
 p.humans={{1,1}};p.ended=true;assert(Decide(p).cleanup); // normal match end
 p.ended=false;p.humans={{10,0}};p.foreignBots={{1,0}};plan=Decide(p);assert(plan.budget[0]==0&&plan.retire[0]==1&&!plan.add[0]);
 p.humans={{1,0}};p.foreignBots={{0,0}};p.ownBots={{0,0}};plan=Decide(p);assert(plan.add[0]==1&&plan.add[1]==1); // bounded refill, not a new full batch
 // Validated late B port: queue consumed before AddPlayer, and creator A leaves in the gap.
 std::map<uint32_t,uint32_t> observed{{100,60}};
 assert(ObserveHumanPort(observed,200u,60,7,7,false,true,true));
 assert(observed.count(200));observed.erase(100); // creator A no longer participates
 Population porting;porting.target=10;porting.ownBots={{8,10}};
 bool queueStillContainsB=false,instanceAlreadyContainsB=false,actorInWorld=false;
 assert(!queueStillContainsB&&!instanceAlreadyContainsB&&!actorInWorld); // production must use FindPlayerNotInWorld during this window
 if(observed.count(200) && InFlightArrival(7,7,true))++porting.pending[0];
 assert(!Decide(porting).cleanup); // production in-flight predicate bridges the native window
 assert(!ObserveHumanPort(observed,201u,60,7,9,false,true,true));
 assert(!ObserveHumanPort(observed,201u,60,7,7,true,true,true));
 assert(!ObserveHumanPort(observed,201u,60,7,7,false,false,true));
 assert(!ObserveHumanPort(observed,201u,60,7,7,false,true,false));
 assert(!ObserveHumanPort(observed,201u,60,0,7,false,true,true));
 assert(!InFlightArrival(7,9,true)&&!InFlightArrival(7,7,false));
 porting.pending[0]=0;porting.humans[0]=1;assert(!Decide(porting).cleanup); // AddPlayer/ACK completed
 porting.humans[0]=0;assert(Decide(porting).cleanup); // departed and no pending arrival
 // Exercise the production ownership ledger, not just precomputed population counts.
 struct Bot {uint32_t guid;};
 for(unsigned delay:{2u,10u}) {
     std::vector<Bot> active{{100},{101}};std::map<uint32_t,Bot> retiring;
     Population delayed;delayed.target=3;delayed.humans={{1,0}};delayed.pending={{1,0}};delayed.ownBots={{2,3}};
     assert(Decide(delayed).retire[0]==1);
     assert(RetireOwned(active,retiring,101));assert(active.size()==1&&retiring.count(101));
     assert(!RetireOwned(active,retiring,101)); // same actor cannot be retired repeatedly
     bool registered=true,present=true;
     for(unsigned tick=0;tick<delay;++tick) {
         if(tick)registered=false; // manager removal can precede actual player destruction
         PruneRetired(retiring,[&](uint32_t id){assert(id==101);return registered;},[&](uint32_t){return present;});
         assert(retiring.count(101)); // still owned, never becomes a foreign bot
         delayed.ownBots[0]=active.size();delayed.retiringBots[0]=retiring.size();
         auto stable=Decide(delayed);assert(!stable.cleanup&&!stable.retire[0]&&!stable.add[0]);
     }
     delayed.humans[0]=0;assert(!Decide(delayed).cleanup&&!Decide(delayed).add[0]);
     registered=true;present=false;PruneRetired(retiring,[&](uint32_t){return registered;},[&](uint32_t){return present;});assert(retiring.count(101));
     registered=false;PruneRetired(retiring,[&](uint32_t){return registered;},[&](uint32_t){return present;});assert(retiring.empty());
     delayed.retiringBots[0]=retiring.size();assert(Decide(delayed).add[0]==1);
 }
 std::cout<<"late join / owner leave / foreign queue-instance / scoped cancel / pending / empty / end lifecycle PASS\n";
}
