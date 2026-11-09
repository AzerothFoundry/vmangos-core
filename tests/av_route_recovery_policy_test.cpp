#include "../src/game/PlayerBots/AvRouteRecoveryPolicy.h"
#include <cassert>
int main(){using AvRouteRecoveryPolicy::Observe;unsigned since=0;
// Existing route, cancelled native movement: pointer must become reclaimable.
assert(!Observe(true,true,false,1000,since));assert(!Observe(true,true,false,3999,since));assert(Observe(true,true,false,4000,since));assert(since==0);
// Healthy motion never expires, even if a legacy route remains attached.
assert(!Observe(false,true,false,5000,since));assert(!Observe(false,true,false,20000,since));assert(since==0);
// Recovery, casting/control and foreign movement cannot be cancelled.
assert(!Observe(true,true,false,30000,since));assert(!Observe(true,true,true,32000,since));assert(since==0);assert(!Observe(true,false,false,50000,since));assert(since==0);
// Observation must restart following protection; no stale wall-clock shortcut.
assert(!Observe(true,true,false,60000,since));assert(!Observe(true,true,false,62000,since));assert(Observe(true,true,false,63000,since));
}
