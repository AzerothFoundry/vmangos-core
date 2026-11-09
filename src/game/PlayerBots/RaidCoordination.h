#ifndef RAID_COORDINATION_H
#define RAID_COORDINATION_H
#include <string>
class Player;
class Unit;
class PartyBotAI;
class SpellEntry;
namespace RaidCoordination
{
bool UpdatePull(PartyBotAI &);
void ReadyResponse(Player *, unsigned);
std::string AutoPull(Player *, bool);
std::string BeginPull(Player *, Unit *);
bool PullPending(Player *);
bool TryMassTaunt(PartyBotAI &);
bool Enabled(Player *);
void SetEnabled(Player *, bool);
bool Reset(Player *);
void CancelPull(Player *);
Player *AssignedTank(Player *, Unit const *);
Unit *TankTarget(PartyBotAI const &);
Player *HealingTank(PartyBotAI const &);
Unit *HealTarget(PartyBotAI const &, float, float);
bool AllowDamage(PartyBotAI const &, Unit const *, SpellEntry const *);
bool AllowAreaDamage(PartyBotAI const &, SpellEntry const *,float,float,float);
std::string Status(Player *);
} // namespace RaidCoordination
#endif
