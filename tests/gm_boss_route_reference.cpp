#include "GmBossRouteReference.h"
#include <cassert>
#include <limits>
int main() {
 using namespace GmInstanceBoss;
 assert(NativeRouteReferenceId(409,11502)==467);
 assert(NativeRouteReferenceId(409,11988)==0 && NativeRouteReferenceId(389,11502)==0);
 assert(RouteReferenceInBounds(409,11502,409,838.308f,-831.466f,-232.185f,799.662f,-808.124f,-226.25f));
 assert(!RouteReferenceInBounds(409,11502,533,0,0,0,10,0,0));
 assert(!RouteReferenceInBounds(409,11502,409,0,0,0,76,0,0));
 assert(!RouteReferenceInBounds(409,11502,409,0,0,0,10,0,7));
 assert(!RouteReferenceInBounds(409,11502,409,0,0,0,std::numeric_limits<float>::quiet_NaN(),0,0));
}
