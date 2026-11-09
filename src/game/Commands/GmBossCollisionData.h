#ifndef GM_BOSS_COLLISION_DATA_H
#define GM_BOSS_COLLISION_DATA_H
#include "GridDefines.h"
#include "GridMap.h"
namespace GmInstanceBoss {
inline bool CollisionDataPresent(uint32 map,float x,float y,
    bool (*existsVMap)(uint32,int,int)=GridMap::ExistVMap)
{
    if(!MaNGOS::IsValidMapCoord(x,y))return false;
    auto grid=MaNGOS::ComputeGridPair(x,y);
    // Same grid mapping as MapManager::ExistMapAndVMap, without demanding a
    // terrain .map file. Native WMO-only instances legitimately have none;
    // TerrainInfo::GetHeightStatic explicitly supports their vmap-only floors.
    return existsVMap(map,63-grid.x_coord,63-grid.y_coord);
}
}
#endif
