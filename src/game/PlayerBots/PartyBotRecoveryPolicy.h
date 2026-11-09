#ifndef PARTY_BOT_RECOVERY_POLICY_H
#define PARTY_BOT_RECOVERY_POLICY_H
#include <cstdint>
#include <cmath>
#include <algorithm>
namespace PartyBotRecoveryPolicy
{
constexpr std::uint32_t ApproachLimitMs = 20000;
constexpr std::uint32_t RetryDelayMs = 15000;
constexpr float SearchDistance = 100.0f;
constexpr float MaxPathLength = 120.0f;
struct Point { float x, y, z; };
// Check the entire segment: checking only navigation vertices can miss a pack in the middle.
inline bool ClearSegment(Point a, Point b, Point enemy, float radius)
{
    auto finite = [](Point p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    if (!finite(a) || !finite(b) || !finite(enemy) || !std::isfinite(radius) || radius < 0)
        return false;
    float dx = b.x-a.x, dy = b.y-a.y, dz = b.z-a.z;
    float length = dx*dx+dy*dy+dz*dz;
    float t = length > 0 ? ((enemy.x-a.x)*dx+(enemy.y-a.y)*dy+(enemy.z-a.z)*dz)/length : 0;
    t = std::max(0.0f, std::min(1.0f, t));
    float x = a.x+t*dx-enemy.x, y = a.y+t*dy-enemy.y, z = a.z+t*dz-enemy.z;
    return x*x+y*y+z*z > radius*radius;
}
// The reservation protects a cast, not a timer left behind by an interrupted cast.
inline bool KeepReservation(bool sameContext, bool castingThisResurrection, bool requestPending,
                            std::uint32_t age, std::uint32_t castTime)
{
    return sameContext && castingThisResurrection && !requestPending && age <= castTime + 2000;
}
inline bool CanAfford(std::uint32_t available, std::uint32_t cost, bool healthCost)
{
    return healthCost ? available > cost : available >= cost;
}
// Unsigned elapsed times stay correct across the server millisecond clock wrapping.
inline bool ApproachExpired(std::uint32_t now, std::uint32_t started)
{
    return std::uint32_t(now - started) >= ApproachLimitMs;
}
inline bool RetryBlocked(std::uint32_t now, std::uint32_t failedAt)
{
    return std::uint32_t(now - failedAt) < RetryDelayMs;
}
}
#endif
