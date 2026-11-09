#ifndef MANGOS_CHARACTER_OVERRIDES_H
#define MANGOS_CHARACTER_OVERRIDES_H
#include <cstdint>
#include <string>
class Player;
namespace CharacterOverrides {
struct Limits { int talents=-1;int professions=-1; };
Limits Get(Player const* player);
std::uint32_t TalentPoints(Player const* player,std::uint32_t normal);
std::uint32_t ProfessionLimit(Player const* player);
bool Set(Player const* player,Limits value,std::string& error);
unsigned ProfessionCount(Player const* player);
void RefreshProfessionPoints(Player* player);
}
#endif
