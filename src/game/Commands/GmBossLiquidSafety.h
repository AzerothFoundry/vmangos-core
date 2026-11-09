#ifndef GM_BOSS_LIQUID_SAFETY_H
#define GM_BOSS_LIQUID_SAFETY_H
#include "GridMapDefines.h"
#include <cmath>
namespace GmInstanceBoss {
inline bool SafeLiquidLanding(GridMapLiquidStatus status,GridMapLiquidData const& liquid)
{
    // A dry floor/bridge above liquid is not liquid contact.
    if(status==LIQUID_MAP_NO_WATER || status==LIQUID_MAP_ABOVE_WATER)return true;
    if(!std::isfinite(liquid.level) || !std::isfinite(liquid.depth_level))return false;
    if(liquid.type_flags & (MAP_LIQUID_TYPE_MAGMA|MAP_LIQUID_TYPE_SLIME|MAP_LIQUID_TYPE_DEEP_WATER))return false;
    if(status==LIQUID_MAP_UNDER_WATER)return false;
    if(status!=LIQUID_MAP_IN_WATER && status!=LIQUID_MAP_WATER_WALK)return false;
    // Same depth criterion as TerrainInfo::IsSwimmable (default radius 1.5).
    return liquid.level-liquid.depth_level<=1.5f;
}
}
#endif
