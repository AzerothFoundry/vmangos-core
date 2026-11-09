#ifndef MANGOS_GM_SKILL_TOOLS_H
#define MANGOS_GM_SKILL_TOOLS_H
#include <string>
#include <vector>
class Player;
namespace GmSkillTools {
bool Execute(Player* self,std::vector<std::string> const& words,std::vector<std::string>& lines,std::string& error);
unsigned LearnClass(Player* self);
void Inspect(Player* self,std::vector<std::string>& lines);
}
#endif
