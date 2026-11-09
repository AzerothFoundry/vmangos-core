#ifndef GM_BOSS_PATH_SAFETY_H
#define GM_BOSS_PATH_SAFETY_H
#include <cmath>
#include <cstddef>
namespace GmInstanceBoss {
template<class Points,class CollisionCast>
inline bool PathSegmentsClear(Points const& points,float collisionHeight,CollisionCast cast,std::size_t& failedSegment)
{
    failedSegment=0;
    if(points.size()<2 || !std::isfinite(collisionHeight) || collisionHeight<=0)return false;
    // Trace the actual moving actor body center, not a fixed Boss pivot offset.
    float bodyCenter=collisionHeight*.5f;
    for(std::size_t i=1;i<points.size();++i) {
        auto const& a=points[i-1];auto const& b=points[i];
        if(!cast(a.x,a.y,a.z+bodyCenter,b.x,b.y,b.z+bodyCenter)){failedSegment=i;return false;}
    }
    return true;
}
}
#endif
