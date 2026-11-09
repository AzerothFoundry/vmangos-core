#include "McBotPolicy.h"
#include <cassert>
#include <limits>
using namespace McBotPolicy;
int main()
{
    std::vector<Enemy> mixed = {{1, 11671, 90}, {2, 11671, 80}, {3, 113, 100}};
    assert(!Aoe(mixed, false)); // Two dogs and one other entry remain a focus pack.
    mixed.push_back({4, 11671, 75});
    assert(Aoe(mixed, false));
    assert(!Aoe(mixed, true)); // Encounter veto beats count.
    mixed[0].controlled = true;
    assert(!Aoe(mixed, false));
    mixed[0].controlled = false;
    mixed[2].engaged = false;
    assert(!Aoe(mixed, false));
    mixed[2].engaged = true;
    mixed[0].health = 1;
    mixed[0].fakeDead = true;
    assert(HoundFocus(mixed) == 2); // Fake death is not highest-health focus.
    mixed[1].fakeDead = true;
    mixed[3].fakeDead = true;
    assert(HoundFocus(mixed) == 0);
    assert(Aoe(mixed,
               false)); // But fake-dead hounds stay in the active pack until real death.
    mixed.erase(mixed.begin() + 3);
    assert(!Aoe(mixed, false));
    assert(!GolemaggDamageAllowed(11672, true, false));
    assert(GolemaggDamageAllowed(11672, true, true));
    assert(GolemaggDamageAllowed(11672, false, false));
    assert(GolemaggDamageAllowed(11988, true, false));
    std::vector<Point> raid = {{0, 0, 0}, {5, 0, 0}};
    assert(!BombSafe({10, 0, 0}, raid));
    assert(BombSafe({30, 0, 0}, raid));
    assert(!BombSafe({std::numeric_limits<float>::quiet_NaN(), 0, 0}, raid));
    // Every nearby member must be clear, not just an average group centre.
    raid.push_back({29, 1, 0});
    assert(!BombSafe({30, 0, 0}, raid));
}
