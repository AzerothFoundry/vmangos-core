#ifndef MANGOS_PARTY_BOT_PRESET_H
#define MANGOS_PARTY_BOT_PRESET_H
#include "Common.h"
#include <string>
#include <vector>
class Player;
class PartyBotAI;
namespace PartyPreset
{
struct Mapping { uint32 spec, preraid, phase6; };
std::vector<Mapping> const& Mappings();
Mapping const* Find(uint32 spec);
bool Supported(Player const* owner, Mapping const& mapping, std::string& reason);
bool Queue(Player* owner, uint32 spec, std::string& result, bool quickRaid = false);
bool RaidPreview(Player* owner, unsigned role, std::string& result, uint32 profileMap = 0);
bool RaidApply(Player* owner, uint32 token, std::string& result);
void ArrangeRaid(Player* owner);
unsigned PreferredRole(Player const* player);
bool Initialize(PartyBotAI& ai, std::string& result);
void SendStatus(Player* owner);
uint32 RemoveOwned(Player* owner);
uint32 VerifiedHealers(Player const* owner);
}
#endif
