#ifndef MANGOS_GM_ITEM_MAIL_H
#define MANGOS_GM_ITEM_MAIL_H
#include <string>
#include <cstdint>
class Player;
namespace GmItemMail
{
// Administrator only; existing characters, including offline recipients.
// Success means the native database transaction committed, not yet claimed.
bool SendTier(Player* gm, std::string target, std::string tier, std::string& result);
bool SendItem(Player* gm, std::string target, std::uint32_t itemId, std::uint32_t count, std::string& result);
}
#endif
