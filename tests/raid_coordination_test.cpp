#include "RaidCoordinationPolicy.h"
#include <algorithm>
#include <cassert>
#include <limits>
int main()
{
    using RaidCoordinationPolicy::KeepPlannedOwner;
    using RaidCoordinationPolicy::PreserveCurrentTank;
    assert(KeepPlannedOwner(true, false, true, 0));
    assert(KeepPlannedOwner(true, false, true, 19999));
    assert(!KeepPlannedOwner(true, false, true, 20000));
    assert(!KeepPlannedOwner(true, false, false, 0));
    assert(KeepPlannedOwner(true, true, false, 30000));
    assert(!KeepPlannedOwner(false, false, true, 1));
    assert(!KeepPlannedOwner(false, true, true, 1));
    assert(PreserveCurrentTank(true, 4, true));
    assert(PreserveCurrentTank(false, 0, true));
    assert(!PreserveCurrentTank(false, 1, true));
    assert(PreserveCurrentTank(false, 4, false));
    // Natural five-add assist on the same MT must spread to five reachable idle tanks.
    unsigned loads[5] = {};
    for (unsigned mob = 0; mob < 5; ++mob)
    {
        bool idle = false;
        for (unsigned i = 1; i < 5; ++i)
            if (loads[i] == 0)
                idle = true;
        unsigned chosen = 0;
        if (!PreserveCurrentTank(false, loads[0], idle))
            for (unsigned i = 1; i < 5; ++i)
                if (loads[i] < loads[chosen])
                    chosen = i;
        ++loads[chosen];
    }
    for (unsigned load : loads)
        assert(load == 1);
    // A planned owner survives the MT's walk-in and the later assist-combat transition.
    for (unsigned elapsed : {0u, 1000u, 5000u})
        assert(KeepPlannedOwner(true, false, true, elapsed));
    assert(KeepPlannedOwner(true, true, true, 5001));

    using RaidCoordinationPolicy::ConnectedHounds;
    using RaidCoordinationPolicy::HoundPoint;
    // Actual five-spawn pack: the two outer hounds are >15m apart, but linked via the centre.
    std::vector<HoundPoint> native{{1, 1036, -963, 0},       {2, 1034.7f, -972.4f, 0}, {3, 1025.5f, -961.3f, 0},
                                   {4, 1046.5f, -961.1f, 0}, {5, 1034, -954, 0},       {6, 1001, -966, 0},
                                   {7, 991, -967, 0}};
    for (unsigned seed = 1; seed <= 5; ++seed)
    {
        auto pack = ConnectedHounds(native, seed);
        assert((pack == std::set<std::uint64_t>{1, 2, 3, 4, 5}));
    }
    auto original = ConnectedHounds(native, 3);
    std::reverse(native.begin(), native.end());
    assert(ConnectedHounds(native, 3) == original);
    // Exact envelope boundary is allowed; even a connected candidate just outside is excluded.
    std::vector<HoundPoint> boundary{{1, 0, 0, 0}, {2, 15, 0, 0}, {3, 25, 0, 0}, {4, 25.01f, 0, 0}, {5, 0, 0, 25.01f}};
    assert((ConnectedHounds(boundary, 1) == std::set<std::uint64_t>{1, 2, 3}));
    assert(ConnectedHounds(boundary, 999).empty());
    boundary.push_back({6, std::numeric_limits<float>::quiet_NaN(), 0, 0});
    assert((ConnectedHounds(boundary, 1) == std::set<std::uint64_t>{1, 2, 3}));

    assert(RaidCoordinationPolicy::InsideAutoRegion(45, 30));
    assert(!RaidCoordinationPolicy::InsideAutoRegion(45.1f, 30));
    assert(!RaidCoordinationPolicy::InsideAutoRegion(45, 30.1f));
    assert(!RaidCoordinationPolicy::InsideAutoRegion(-1, 1));
    assert(!RaidCoordinationPolicy::InsideAutoRegion(std::numeric_limits<float>::infinity(), 1));
    using RaidCoordinationPolicy::AuthorizedPack;
    using RaidCoordinationPolicy::HoldDamage;
    assert(AuthorizedPack(11671, 11671, 15));
    assert(!AuthorizedPack(11671, 11671, 15.1f));
    assert(AuthorizedPack(11988, 11672, 80));
    assert(!AuthorizedPack(11988, 11672, 80.1f));
    assert(!AuthorizedPack(12056, 11671, 1));
    assert(!AuthorizedPack(11671, 11672, 1));
    assert(HoldDamage(0, 0, true, false));
    assert(!HoldDamage(94, 100, true, false));
    assert(HoldDamage(96, 100, true, false));
    // A fluctuation below stop does not immediately resume an already throttled actor.
    assert(HoldDamage(90, 100, true, true));
    assert(!HoldDamage(79, 100, true, true));
    assert(!HoldDamage(105, 100, false, false));
    assert(HoldDamage(111, 100, false, false));
    assert(HoldDamage(100, 100, false, true));
    assert(!HoldDamage(94, 100, false, true));
    assert(HoldDamage(0, std::numeric_limits<float>::quiet_NaN(), false, false));
    // Tank dies/resets threat; the actor waits until the successor has built a lead.
    assert(HoldDamage(100, 0, false, false));
    assert(!HoldDamage(100, 150, false, true));
}
