#ifndef MANGOS_MC_BOT_TACTICS_H
#define MANGOS_MC_BOT_TACTICS_H
#include <string>
class PartyBotAI;
class Unit;
struct SpellEntry;
namespace McBotTactics
{
bool Update(PartyBotAI &ai);
bool AllowSpell(PartyBotAI const &ai, Unit const *target, SpellEntry const *spell);
bool AllowAttack(PartyBotAI const &ai, Unit *target);
bool WantsAoe(PartyBotAI const &ai, Unit const *target);
std::string Status(PartyBotAI const &ai);
} // namespace McBotTactics
#endif
