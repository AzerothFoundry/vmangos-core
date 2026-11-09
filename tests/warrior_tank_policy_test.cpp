#include "../src/game/PlayerBots/WarriorTankPolicy.h"
#include <cassert>
int main(){using namespace WarriorTankPolicy;
Candidate safe{true,true,true,true,true,true,false,true,true};assert(Eligible(safe));
// Adjacent idle mobs, CC, another tank's target and unreachable/illegal NPCs
// must never enter the threat-balancing candidate set.
auto c=safe;c.engaged=false;assert(!Eligible(c));c=safe;c.controlled=true;assert(!Eligible(c));c=safe;c.held=false;assert(!Eligible(c));c=safe;c.assigned=false;assert(!Eligible(c));c=safe;c.reachable=false;assert(!Eligible(c));c=safe;c.legal=false;assert(!Eligible(c));c=safe;c.npc=false;assert(!Eligible(c));
// Initial pack coverage, then maintain the smallest threat margin first.
assert(Rotate(0,0));assert(Rotate(99,0));assert(!Rotate(100,0));assert(Margin(40,0)<Margin(80,0));assert(Margin(110,100)<Margin(180,100));
assert(MaintainSunder(1,20000));assert(!MaintainSunder(5,10000));assert(MaintainSunder(5,4000));
using namespace WarriorTankPolicy;assert(Rotate(110,100));assert(!Rotate(140,100));assert(Rotate(0,0));assert(!Rotate(100,0));assert(Margin(100,100)<Margin(200,100));assert(ShieldBlock(30,1,100));assert(!ShieldBlock(90,1,100));assert(!ShieldBlock(30,0,100));assert(DumpRage(500)&&!DumpRage(499));assert(MassTaunt(3,8000)&&!MassTaunt(2,9000)&&!MassTaunt(3,7999));}
