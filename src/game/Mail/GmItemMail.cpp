#include "PlayerBotPvpTeams.h"
#include "GmItemMail.h"
#include "GmBulkItemPolicy.h"
#include "Player.h"
#include "WorldSession.h"
#include "World.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Item.h"
#include "Mail.h"
#include "Database/DatabaseEnv.h"
#include "Util.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <vector>

namespace GmItemMail
{
namespace
{
struct Recipient { ObjectGuid guid; uint32 classId=0, level=0, team=0; std::string name; };
bool Resolve(Player* gm, std::string name, Recipient& out, std::string& error)
{
    if (!gm || !gm->GetSession() || gm->GetSession()->GetSecurity() < SEC_ADMINISTRATOR)
    { error="Administrator session required"; return false; }
    if (name.empty() || name.size()>48 || !normalizePlayerName(name))
    { error="Invalid recipient name"; return false; }
    PlayerCacheData const* data=sObjectMgr.GetPlayerDataByName(name);
    if (!data) { error="Recipient character not found"; return false; }
    // Authority is account_access, never the legacy account.gmlevel column.
    std::unique_ptr<QueryResult> authority(LoginDatabase.PQuery(
        "SELECT COALESCE(MAX(`gmlevel`),0) FROM `account_access` WHERE `id`=%u AND `RealmID` IN (-1,%u)", data->uiAccount, realmID));
    if (!authority) { error="Recipient authority lookup failed"; return false; }
    if (authority->Fetch()[0].GetUInt32()>uint32(gm->GetSession()->GetSecurity()))
    { error="Recipient has higher security"; return false; }
    out.guid=ObjectGuid(HIGHGUID_PLAYER,data->uiGuid);out.classId=data->uiClass;out.level=data->uiLevel;out.team=Player::TeamForRace(data->uiRace);out.name=data->sName;
    if (Player* online=ObjectAccessor::FindPlayer(out.guid))
    {
        if (online->IsBot()) { error="Temporary bots cannot receive GM mail"; return false; }
        out.classId=online->GetClass();out.level=online->GetLevel();out.team=online->GetTeam();
        if (online->GetSession() && online->GetSession()->GetSecurity()>gm->GetSession()->GetSecurity())
        { error="Recipient has higher live security"; return false; }
    }
    return true;
}
struct TierSets { uint32 classId; uint32 sets[4]; };
// Verified against the server's patch-10 item prototypes: 8/8/5/9 pieces.
TierSets const tiers[]={
    {CLASS_WARRIOR,{209,218,496,523}}, {CLASS_PALADIN,{208,217,505,528}},
    {CLASS_HUNTER,{206,215,509,530}}, {CLASS_ROGUE,{204,213,497,524}},
    {CLASS_PRIEST,{202,211,507,525}}, {CLASS_SHAMAN,{207,216,501,527}},
    {CLASS_MAGE,{201,210,503,526}}, {CLASS_WARLOCK,{203,212,499,529}},
    {CLASS_DRUID,{205,214,493,521}}
};
using Items=std::vector<std::unique_ptr<Item>>;
bool Prepare(Recipient const& target,uint32 id,uint32 count,Items& items,std::string& error)
{
    ItemPrototype const* proto=sObjectMgr.GetItemPrototype(id);
    if (!proto || (proto->ExtraFlags & ITEM_EXTRA_NOT_OBTAINABLE) || !GmBulkItemPolicy::Count(count) || !proto->GetMaxStackSize())
    { error="Unavailable item or invalid count"; return false; }
    if (!BotMatchDetail::PvpTeamAllowed(proto,target.team))
    { error="PvP item does not belong to recipient faction"; return false; }
    if (proto->MaxCount && count>proto->MaxCount)
    { error="Count exceeds unique-item limit"; return false; }
    uint32 const stack=proto->GetMaxStackSize();
    if (!GmBulkItemPolicy::MailAllowed(count,stack,proto->MaxCount,uint32(items.size())))
    { error="At most 1000 mail attachments per request"; return false; }
    while (count)
    {
        uint32 n=std::min(count,stack);
        std::unique_ptr<Item> item(Item::CreateItem(id,n,target.guid));
        if (!item) { error="Item creation failed; nothing sent"; return false; }
        items.push_back(std::move(item));count-=n;
    }
    return true;
}
bool Deliver(Recipient const& target,Items& items,std::string const& subject,std::string& result)
{
    if (items.empty() || CharacterDatabase.InTransaction())
    { result="Invalid batch transaction state"; return false; }
    if (!CharacterDatabase.BeginTransaction(target.guid.GetCounter()))
    { result="Could not start mail transaction"; return false; }
    std::vector<std::function<void(bool)>> completion;
    for (auto& item:items)
    {
        item->SaveToDB();
        MailDraft draft(subject);
        draft.AddItem(item.release());
        draft.SendMailTo(MailReceiver(target.guid),MailSender(MAIL_NORMAL,uint32(0),MAIL_STATIONERY_GM),
                         MAIL_CHECK_MASK_NONE,0,0,false,&completion);
    }
    bool const committed=CharacterDatabase.CommitTransactionDirect();
    for (auto& complete:completion) complete(committed);
    if (!committed) { result="Mail transaction failed; nothing delivered";return false; }
    result="Mail batch committed for "+target.name+": "+std::to_string(items.size())+" envelopes; claim at a mailbox";
    return true;
}
}
bool SendTier(Player* gm,std::string target,std::string tier,std::string& result)
{
    Recipient recipient;if (!Resolve(gm,target,recipient,result))return false;
    std::transform(tier.begin(),tier.end(),tier.begin(),[](unsigned char c){return char(std::tolower(c));});
    unsigned index=tier=="t1"?0:tier=="t2"?1:(tier=="t2.5"||tier=="t25")?2:tier=="t3"?3:4;
    if (index==4) { result="Tier must be T1, T2, T2.5 or T3";return false; }
    // Receiving is allowed below 60; native RequiredLevel still governs equipping.
    uint32 set=0;for (auto const& row:tiers)if(row.classId==recipient.classId)set=row.sets[index];
    if (!set) { result="Unsupported recipient class";return false; }
    Items items;unsigned const expected=index==2?5:index==3?9:8;unsigned rings=0;
    for (auto const& entry:sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const& proto=entry.second;if (proto.ItemSet!=set)continue;
        if (proto.Class!=ITEM_CLASS_ARMOR || proto.RequiredLevel!=60 || proto.AllowableClass!=(1u<<(recipient.classId-1)))
        { result="Tier prototype class/level validation failed";return false; }
        if (proto.InventoryType==INVTYPE_FINGER)++rings;
        if (!Prepare(recipient,proto.ItemId,1,items,result))return false;
    }
    if (items.size()!=expected || rings!=(index==3?1u:0u))
    { result="Tier is incomplete in the active content phase; nothing sent";return false; }
    return Deliver(recipient,items,"GM职业套装："+tier,result);
}
bool SendItem(Player* gm,std::string target,std::uint32_t itemId,std::uint32_t count,std::string& result)
{
    Recipient recipient;if (!Resolve(gm,target,recipient,result))return false;
    Items items;if (!Prepare(recipient,itemId,count,items,result))return false;
    return Deliver(recipient,items,"GM物品补给",result);
}
}
