#ifndef RAID_COORDINATION_POLICY_H
#define RAID_COORDINATION_POLICY_H
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>
namespace RaidCoordinationPolicy
{
struct HoundPoint
{
    std::uint64_t guid;
    float x, y, z;
};
inline float DistanceSquared(HoundPoint const &a, HoundPoint const &b)
{
    float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x * x + y * y + z * z;
}
// Connected component under 15m adjacency, clipped to the selected hound's 25m envelope.
// Clipping happens before traversal: a chain cannot expand its own authorized region.
inline std::set<std::uint64_t> ConnectedHounds(std::vector<HoundPoint> const &points, std::uint64_t selected)
{
    HoundPoint const *seed = nullptr;
    for (auto const &point : points)
        if (point.guid == selected)
        {
            seed = &point;
            break;
        }
    if (!seed || !std::isfinite(seed->x) || !std::isfinite(seed->y) || !std::isfinite(seed->z))
        return {};
    std::set<std::uint64_t> result{selected};
    std::vector<HoundPoint const *> pending{seed};
    for (std::size_t n = 0; n < pending.size(); ++n)
        for (auto const &point : points)
        {
            if (result.count(point.guid))
                continue;
            float envelope = DistanceSquared(*seed, point), adjacent = DistanceSquared(*pending[n], point);
            if (!std::isfinite(envelope) || !std::isfinite(adjacent) || envelope > 625.0f || adjacent > 225.0f)
                continue;
            result.insert(point.guid);
            pending.push_back(&point);
        }
    return result;
}

inline bool KeepPlannedOwner(bool existsAliveOnMap, bool inCombat, bool planned, std::uint32_t elapsed)
{
    return existsAliveOnMap && (inCombat || (planned && elapsed < 20000));
}
inline bool PreserveCurrentTank(bool human, unsigned currentOtherLoad, bool reachableIdleTank)
{
    return human || currentOtherLoad == 0 || !reachableIdleTank;
}

inline bool InsideAutoRegion(float anchorDistance, float tankDistance)
{
    return std::isfinite(anchorDistance) && std::isfinite(tankDistance) && anchorDistance >= 0 && tankDistance >= 0 &&
           anchorDistance <= 45 && tankDistance <= 30;
}

inline bool AuthorizedPack(unsigned target, unsigned other, float distance)
{
    if (!std::isfinite(distance) || distance < 0)
        return false;
    return (target == 11671 && other == 11671 && distance <= 15) ||
           (target == 11988 && other == 11672 && distance <= 80);
}

// Hysteresis is relative to the responsible tank, not the highest unrelated tank.
inline bool HoldDamage(float damageThreat, float tankThreat, bool melee, bool held)
{
    if (!std::isfinite(damageThreat) || !std::isfinite(tankThreat) || tankThreat <= 0)
        return true;
    float stop = melee ? 0.95f : 1.10f;
    if (damageThreat >= tankThreat * stop)
        return true;
    if (damageThreat < tankThreat * (stop - 0.15f))
        return false;
    return held;
}
} // namespace RaidCoordinationPolicy
#endif
