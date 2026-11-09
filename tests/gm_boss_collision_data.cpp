#include "Common.h"
#include "GmBossCollisionData.h"
#include <cassert>
#include <limits>
static int calls=0;
static bool wmoOnly(uint32 map,int gx,int gy) {
 ++calls;assert(map==389 || map==409);assert(gx>=0 && gx<64 && gy>=0 && gy<64);
 return true; // VMap exists; no terrain callback/file is involved.
}
static bool missingVMap(uint32,int,int) {++calls;return false;}
int main() {
 using GmInstanceBoss::CollisionDataPresent;
 assert(CollisionDataPresent(389,0,0,wmoOnly));
 assert(CollisionDataPresent(409,100,-100,wmoOnly));
 assert(!CollisionDataPresent(409,100,-100,missingVMap));
 assert(calls==3);
 assert(!CollisionDataPresent(409,std::numeric_limits<float>::quiet_NaN(),0,wmoOnly));
 assert(!CollisionDataPresent(409,1e9f,0,wmoOnly));
 assert(calls==3);
}
