#include "PartyBotRecoveryPolicy.h"
#include "RaidBotPlanner.h"
#include <cassert>
#include <cstdint>
#include <limits>
int main()
{
    using namespace PartyBotRecoveryPolicy;
    // Mid-segment enemies are caught even when both navigation vertices are safe.
    assert(!ClearSegment({0,0,0}, {100,0,0}, {50,0,0}, 20));
    assert(ClearSegment({0,0,0}, {100,0,0}, {50,21,0}, 20));
    assert(!ClearSegment({0,0,0}, {100,0,0}, {50,20,0}, 20));
    assert(!ClearSegment({0,0,0}, {0,0,0}, {0,0,0}, 5));
    assert(ClearSegment({0,0,0}, {100,0,0}, {50,0,30}, 20));
    assert(!ClearSegment({0,0,0}, {100,0,0}, {50,0,0}, std::numeric_limits<float>::quiet_NaN()));
    // Casting protects a corpse only while that exact caster/spell/target is still active.
    assert(KeepReservation(true, true, false, 5000, 10000));
    assert(!KeepReservation(true, false, false, 1000, 10000)); // interruption, even at 1s
    assert(!KeepReservation(false, true, false, 1000, 10000)); // death, group/map/logout change
    assert(!KeepReservation(true, true, true, 10000, 10000)); // request now owns the flow
    assert(!KeepReservation(true, true, false, 12001, 10000)); // broken/stuck cast timeout
    // One resurrection is enough mana; full pools are irrelevant. A health cost must not kill.
    assert(CanAfford(100, 100, false));
    assert(!CanAfford(99, 100, false));
    assert(!CanAfford(100, 100, true));
    assert(CanAfford(101, 100, true));
    assert(!ApproachExpired(19999, 0));
    assert(ApproachExpired(20000, 0));
    assert(RetryBlocked(14999, 0));
    assert(!RetryBlocked(15000, 0));
    auto const beforeWrap = std::numeric_limits<std::uint32_t>::max() - 999;
    assert(!ApproachExpired(5000, beforeWrap));
    assert(ApproachExpired(20000, beforeWrap));
    assert(RetryBlocked(5000, beforeWrap));
    assert(!RetryBlocked(15000, beforeWrap));
    // Real players, tanks, dedicated healers, other heal-capable classes, everyone else.
    using RaidBotPlanner::ResurrectionPriority;
    assert(ResurrectionPriority(false, 0, 9) < ResurrectionPriority(true, 3, 1));
    assert(ResurrectionPriority(true, 3, 1) < ResurrectionPriority(true, 4, 5));
    assert(ResurrectionPriority(true, 4, 5) < ResurrectionPriority(true, 1, 2));
    assert(ResurrectionPriority(true, 1, 2) < ResurrectionPriority(true, 1, 9));
}
