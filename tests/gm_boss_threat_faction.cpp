#include "Common.h"
#include "GmBossThreatFaction.h"
#include <cassert>
using namespace GmInstanceBoss;
int main() {
 assert(AggroStagingClearance(21.f)==24.f); // Native center distance + margin, no repeated hitbox.
 assert(AggroStagingClearance(5.f)==8.f);
 assert(AggroStagingClearance(0.f)==0.f);
 FactionTemplateEntry racial{}, gm35{}, enemy{}, friendly{};
 racial.ID=1; racial.faction=1; racial.ourMask=1;
 gm35.ID=35; gm35.faction=0;
 enemy.hostileMask=1; friendly.friendlyMask=1;
 auto lookup=[&](uint32 id) {return id==35?&gm35:&racial;};
 assert(!ThreatFactionHostile(&enemy,&gm35)); // The GM-mode bypass being fixed.
 assert(ThreatFactionHostile(&enemy,lookup(ThreatFactionId(true,35,1))));
 assert(ThreatFactionHostile(&enemy,lookup(ThreatFactionId(false,1,1))));
 assert(!ThreatFactionHostile(&friendly,lookup(ThreatFactionId(true,35,1))));
 assert(!ThreatFactionHostile(&friendly,lookup(ThreatFactionId(false,1,1))));
 assert(ThreatFactionId(false,35,1)==35); // GM-off current relation is preserved.
 assert(ThreatFactionHostile(nullptr,&racial));
}
