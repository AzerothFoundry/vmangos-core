#ifndef MANGOS_GM_ITEM_BROWSER_H
#define MANGOS_GM_ITEM_BROWSER_H
#include <string>
#include <vector>
class Player;
namespace GmItemBrowser {
// items search min max quality kind class page req|ilvl [localized name]
bool Search(Player* gm, std::vector<std::string> const& words,
            std::vector<std::string>& lines, std::string& error);
}
#endif
