#include "WorldPvpFlavorPolicy.h"
#include <cassert>
#include <iostream>
#include <limits>
using namespace WorldPvpFlavor;
int main(){Config c;Readiness r;r.world=true;r.alive=true;r.level=10;r.health=80;r.mana=60;r.usesMana=true;
 assert(Ready(c,r));r.level=9;assert(!Ready(c,r));r.level=10;
 r.health=79;assert(!Ready(c,r));r.health=80;r.mana=59;assert(!Ready(c,r));r.usesMana=false;assert(Ready(c,r));
 r.combat=true;assert(!Ready(c,r));r.combat=false;r.duel=true;assert(!Ready(c,r));r.duel=false;r.group=true;assert(!Ready(c,r));r.group=false;
 r.service=true;assert(!Ready(c,r));r.service=false;r.protectedArea=true;assert(!Ready(c,r));r.protectedArea=false;r.gm=true;assert(!Ready(c,r));r.gm=false;
 assert(Target(c,20,23,80,40,true,false));assert(!Target(c,20,24,80,40,true,false));assert(!Target(c,10,9,100,10,true,false));
 assert(!Target(c,20,20,79,10,true,false));assert(!Target(c,20,20,100,40.1f,true,false));assert(!Target(c,20,20,100,10,false,false));assert(!Target(c,20,20,100,10,true,true));
 Clock clock;assert(!clock.Due(100,45000));for(unsigned i=101;i<45100;++i)assert(!clock.Due(i,45000));assert(clock.Due(45100,45000));assert(!clock.Due(45101,45000));assert(clock.Due(200000,45000));assert(!clock.Due(200001,45000));assert(clock.checks==2);
 Clock changed;assert(!changed.Due(100,45000));assert(!changed.Due(200,10000));assert(!changed.Due(10199,10000));assert(changed.Due(10200,10000));assert(!changed.Due(10201,10000));
 bool valid=true;assert(Setting(std::numeric_limits<float>::quiet_NaN(),.15f,0,1,valid)==.15f&&!valid);valid=true;assert(Setting(2,.15f,0,1,valid)==1&&valid);
 Clock wrap;assert(!wrap.Due(0xfffffff0,45000));assert(!wrap.Due(2,45000));assert(wrap.Due(uint32_t(0xfffffff0u+45000u),45000));
 assert(!Chance(0,0));assert(!Chance(.16,.15));assert(Chance(.14,.15));assert(Chance(1,1));assert(!Chance(std::numeric_limits<double>::quiet_NaN(),1));
 assert(WithinLeash(true,60,60,34999,c));assert(!WithinLeash(true,60.1f,10,1,c));assert(!WithinLeash(true,10,60.1f,1,c));assert(!WithinLeash(true,10,10,35000,c));assert(!WithinLeash(false,10,10,1,c));
 for(unsigned i=1;i<100;++i){float d=PatrolRadius(i,40);assert(d>=0&&d<=32);assert(d==PatrolRadius(i,40));}assert(PatrolRadius(1,120)!=PatrolRadius(2,120));
 // Transition ownership is independent of native combat: an approach can still
 // be OOC when a human invites it, but its old target must be cancelled.
 assert(CancelOldAction(100,100,false,0));
 assert(!CancelOldAction(100,200,false,0)); // preserve new retaliation B
 assert(!CancelOldAction(100,100,true,100)); // preserve already-started new duel A
 assert(CancelOldAction(100,100,true,200)); // cancel old A, preserve new duel B
 assert(!CancelOldAction(0,100,false,0));
 std::cout<<"PASS production free-world eligibility, protected targets, one-roll interval/wrap, bounded chase, legal patrol radius diversity\n";
}
