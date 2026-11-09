#ifndef MANGOS_MC_BOT_POLICY_H
#define MANGOS_MC_BOT_POLICY_H
#include <cmath>
#include <cstdint>
#include <vector>
namespace McBotPolicy
{
struct Point
{
    float x = 0, y = 0, z = 0;
};
inline float Distance(Point const &a, Point const &b) { return std::hypot(a.x - b.x, a.y - b.y); }
inline bool Finite(Point const &p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
inline bool BombSafe(Point const &p, std::vector<Point> const &party, float radius = 22.0f)
{
    if (!Finite(p))
        return false;
    for (auto const &other : party)
        if (!Finite(other) || Distance(p, other) < radius)
            return false;
    return true;
}
struct Enemy
{
    std::uint64_t guid = 0;
    unsigned entry = 0;
    float health = 0;
    bool controlled = false;
    bool engaged = true;
    bool fakeDead = false;
};
inline bool Aoe(std::vector<Enemy> const &enemies, bool forbidden)
{
    if (forbidden)
        return false;
    for (auto const &e : enemies)
        if (e.controlled || !e.engaged)
            return false;
    // Core hounds stay alive at 1 HP during their ten-second fake death.
    // They remain in the pack until the server's own death/despawn completes.
    for (auto const &seed : enemies)
    {
        unsigned count = 0;
        for (auto const &e : enemies)
            if (e.entry == seed.entry)
                ++count;
        if (count >= 3)
            return true;
    }
    return false;
}
inline std::uint64_t HoundFocus(std::vector<Enemy> const &enemies)
{
    Enemy const *best = nullptr;
    for (auto const &e : enemies)
        if (e.entry == 11671 && e.engaged && !e.controlled && !e.fakeDead && e.health > 0 &&
            (!best || e.health > best->health || (e.health == best->health && e.guid < best->guid)))
            best = &e;
    return best ? best->guid : 0;
}
inline bool GolemaggDamageAllowed(unsigned entry, bool bossAlive, bool tank)
{
    return tank || !bossAlive || entry != 11672;
}
} // namespace McBotPolicy
#endif
