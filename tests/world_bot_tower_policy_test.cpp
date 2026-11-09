#include "../src/game/PlayerBots/WorldBotTowerPolicy.h"
#include <cassert>
int main(){using namespace WorldBotTowerPolicy;assert(UsefulStage(200,1000,810));assert(!UsefulStage(2,1000,998));assert(!UsefulStage(100,1000,980));assert(!UsefulStage(100,1000,1100));assert(WithinSphere(15,0,-34,39));assert(!WithinSphere(20,0,-34,39));assert(WithinSphere(10,10,-26,37));assert(!WithinSphere(30,20,-26,37));assert(WithinSphere(60,0,-26,75));assert(!WithinSphere(70,0,-30,75));assert(!Arrived(3.9f));assert(!Arrived(1.01f));assert(Arrived(1.f));assert(Arrived(.3f));
// Persistent squad objective lifecycle: keep unfinished tower, advance only at
// raw endpoint, hold guard interval, then patrol; loss invalidates idle guards.
assert(Decide(false,false,false,false)==Action::SelectCapture);
assert(Decide(true,false,false,false)==Action::KeepCapture);
assert(Decide(true,true,false,false)==Action::SelectCapture);
assert(Decide(true,true,true,false)==Action::KeepGuard);
assert(Decide(true,true,true,true)==Action::SelectGuardOrPatrol);
assert(Decide(true,true,false,false)==Action::SelectCapture);
// Missing/offline corpse members cannot satisfy a five-member wipe barrier;
// bounded timeout still releases ready ghosts individually.
assert(!BarrierReady(false,true,4));assert(BarrierReady(false,true,5));assert(BarrierReady(true,false,3));
assert(GatherWait(false,44000));assert(!GatherWait(false,45000));assert(!GatherWait(true,0));
assert(!Full(250,1200,true));assert(Full(1200,1200,true));assert(!Full(1199,1200,true));assert(Full(-1200,1200,false));assert(!Full(-400,1200,false));assert(!Full(0,0,true));assert(!Reclaim(true,false,true,999999));assert(!Reclaim(false,true,true,999999));assert(!Reclaim(true,true,false,89999));assert(Reclaim(true,true,false,90000));assert(Reclaim(true,true,true,0));assert(!Engage(true,true,false,true,20));assert(!Engage(true,true,true,false,20));assert(!Engage(true,false,true,true,20));assert(!Engage(true,true,true,true,46));assert(Engage(true,true,true,true,40));assert(ObjectiveCost(300,0,0)<ObjectiveCost(100,1,0));assert(ObjectiveCost(100,0,0)<ObjectiveCost(300,0,0));}
