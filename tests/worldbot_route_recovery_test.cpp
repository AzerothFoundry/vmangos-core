#include "WorldBotRouteRecovery.h"
#include <cassert>
#include <iostream>
#include <limits>
using namespace WorldBotRouteRecovery;
int main(){
 Point here{{0,0,0}},goal{{100,10,0}};Stall s;
 s.Failed(100,1,goal,here);s.Failed(60099,1,goal,here);assert(!s.Due(60099));s.Failed(60100,1,goal,here);assert(s.Due(60100));
 assert(!s.Due(66101)); // Old failures are not evidence of current failure.
 s.Attempt(60100,true);assert(!s.tracking&&!s.Due(60100));
 s.Failed(60101,1,goal,here);s.Failed(120101,1,goal,here);assert(s.Due(120101));
 s.Attempt(120101,false);s.Failed(120851,1,goal,here);assert(!s.Due(120851));
 s.Failed(180101,1,goal,here);assert(s.Due(180101));
 Point changedGoal{{103,10,0}};s.Failed(180102,1,changedGoal,here);assert(!s.Due(180102));
 Point moved{{4,0,0}};s.Progress(1,moved);assert(!s.tracking);
 s.Failed(180103,1,changedGoal,moved);s.Progress(0,moved);assert(!s.tracking);
 auto t=std::numeric_limits<std::uint32_t>::max()-20000;Stall wrap;wrap.Failed(t,1,goal,here);wrap.Failed(t+60000u,1,goal,here);assert(wrap.Due(t+60000u));
 Safety safe;assert(Safe(safe));safe.freeScene=false;assert(!Safe(safe));safe=Safety{};safe.freeGroup=false;assert(!Safe(safe));safe=Safety{};safe.alive=false;assert(!Safe(safe));
 bool Safety::* blocks[]={&Safety::combat,&Safety::duel,&Safety::bg,&Safety::queue,&Safety::instance,&Safety::control,&Safety::casting,&Safety::water,&Safety::teleport,&Safety::taxi,&Safety::transport,&Safety::trade,&Safety::victim};
 for(auto block:blocks){safe=Safety{};safe.*block=true;assert(!Safe(safe));}
 std::cout<<"PASS production route recovery: continuous 60s/same goal, freshness, progress/map resets, acceptance/rejection backoff, clock wrap, protected states\n";
}
