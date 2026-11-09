#include "DungeonTankPolicy.h"
#include <cassert>
#include <limits>
#include <iostream>
int main(){using namespace DungeonTankPolicy;
 assert(AoeReady(3,false,true));assert(AoeReady(5,false,true));assert(!AoeReady(2,false,true));
 assert(!AoeReady(3,true,true));assert(!AoeReady(3,false,false));
 assert(Assistable(true,true,true,true,true,true));
 assert(!Assistable(true,false,true,true,true,true)); // Never let DPS pull a selected idle mob.
 assert(!Assistable(true,true,false,true,true,true)); // No unrelated fight.
 assert(!Assistable(true,true,true,false,true,true)); // CC/immune target is not a valid assist.
 assert(!Assistable(true,true,true,true,true,false));
 Candidate healer;healer.guid=1;healer.alive=healer.engaged=healer.groupVictim=healer.legal=healer.reachable=true;healer.healerVictim=true;healer.distance=20;
 assert(Eligible(healer));
 Candidate c=healer;c.engaged=false;assert(!Eligible(c)); // No ADD.
 c=healer;c.groupVictim=false;assert(!Eligible(c)); // No unrelated combat.
 c=healer;c.heldByTank=true;assert(!Eligible(c)); // No stealing from another tank.
 c=healer;c.controlled=true;assert(!Eligible(c)); // Preserve sheep/banish.
 c=healer;c.legal=false;assert(!Eligible(c));c=healer;c.reachable=false;assert(!Eligible(c));
 c=healer;c.distance=41;assert(!Eligible(c));c.distance=std::numeric_limits<float>::quiet_NaN();assert(!Eligible(c));
 Candidate dps=healer;dps.guid=2;dps.healerVictim=false;dps.distance=5;
 assert(Better(healer,dps,10000));dps.victimHealth=20;assert(Better(dps,healer,10000));
 Candidate second=healer;second.guid=3;second.distance=18;
 assert(KeepCommitment(healer,second,true)); // Don't oscillate between two ranged casters.
 assert(!KeepCommitment(healer,dps,true)); // A critically injured ally overrides ordinary healer priority.
 assert(!KeepCommitment(healer,second,false));
 healer.servedAt=9900;assert(Better(second,healer,10000));
 second.servedAt=1000;second.distance=35;healer.servedAt=2000;healer.distance=2;assert(Better(second,healer,10000)); // Older unrescued mob beats a closer recently serviced one.
 healer.heldByTank=true;assert(!KeepCommitment(healer,second,true)); // Stop rescuing when another tank acquires it.
 std::cout<<"PASS dungeon tank eligibility, party rescue ranking, no ADD/CC/tank stealing, commitment and emergency override\n";
}
