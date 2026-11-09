#include "Common.h"
#include "GmBossLiquidSafety.h"
#include <cassert>
#include <limits>
using namespace GmInstanceBoss;
int main() {
 GridMapLiquidData l{};l.type_flags=MAP_LIQUID_TYPE_WATER;l.level=1;l.depth_level=0;
 assert(SafeLiquidLanding(LIQUID_MAP_NO_WATER,l));
 assert(SafeLiquidLanding(LIQUID_MAP_IN_WATER,l)); // Shallow wading is not swimming.
 assert(SafeLiquidLanding(LIQUID_MAP_WATER_WALK,l));
 assert(!SafeLiquidLanding(LIQUID_MAP_UNDER_WATER,l));
 l.level=2;assert(!SafeLiquidLanding(LIQUID_MAP_IN_WATER,l));
 assert(!SafeLiquidLanding(LIQUID_MAP_WATER_WALK,l));
 for(uint32 hazard:{MAP_LIQUID_TYPE_MAGMA,MAP_LIQUID_TYPE_SLIME,MAP_LIQUID_TYPE_DEEP_WATER}) {
  l.type_flags=hazard;l.level=.1;
  assert(!SafeLiquidLanding(LIQUID_MAP_IN_WATER,l));
  assert(!SafeLiquidLanding(LIQUID_MAP_WATER_WALK,l));
  assert(SafeLiquidLanding(LIQUID_MAP_ABOVE_WATER,l)); // Dry bridge/floor retained.
 }
 l.type_flags=MAP_LIQUID_TYPE_WATER;l.level=std::numeric_limits<float>::quiet_NaN();
 assert(!SafeLiquidLanding(LIQUID_MAP_IN_WATER,l));
}
