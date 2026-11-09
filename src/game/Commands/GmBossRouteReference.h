#ifndef GM_BOSS_ROUTE_REFERENCE_H
#define GM_BOSS_ROUTE_REFERENCE_H
#include <cmath>
namespace GmInstanceBoss {
inline unsigned NativeRouteReferenceId(unsigned map,unsigned boss)
{
    // Native game_tele 467 (ragna3) is the player platform; Rag event pivot is lava.
    return map==409 && boss==11502 ? 467 : 0;
}
inline bool RouteReferenceInBounds(unsigned map,unsigned boss,unsigned routeMap,
    float bx,float by,float bz,float x,float y,float z)
{
    return NativeRouteReferenceId(map,boss) && routeMap==map &&
        std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
        std::hypot(x-bx,y-by)<=75 && std::abs(z-bz)<=6;
}
}
#endif
