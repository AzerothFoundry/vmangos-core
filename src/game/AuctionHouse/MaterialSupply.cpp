#include "MaterialSupply.h"
#include "AuctionHouseMgr.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "World.h"
#include "Database/DatabaseEnv.h"
#include "Config/Config.h"
#include "Log.h"
#include <fstream>
#include <sstream>
#include <set>
#include <vector>
#include <chrono>
#include <limits>
#include <memory>
#include <algorithm>
#include <cctype>

namespace MaterialSupply
{
namespace
{
struct Row { uint32 item,stack,target,price; };
// Explicit vanilla material whitelist. IDs alone do not bypass active-patch prototypes.
std::set<uint32> const allowed = {
2589,2592,4306,4338,14047,14256, // cloth
2318,2319,4234,4304,8170,4231,4232,4235,8169,15410,15412,15414,15415, // leather/hides/scales
2770,2771,2772,2775,2776,3858,7911,10620, // ores
2840,2841,2842,3575,3576,3577,3859,3860,6037,12359,12360, // bars
2835,2836,2838,7912,12365, // stones
765,785,2447,2449,2450,2452,2453,3355,3356,3357,3358,3369,3818,3819,3820,3821,4625,8153,8831,8836,8838,8839,8845,8846,13463,13464,13465,13466,13467,13468,
10938,10939,10940,10978,10998,11082,11083,11084,11134,11135,11137,11138,11139,11174,11175,11176,11177,11178,14343,14344,16202,16203,16204,20725,
7067,7068,7069,7070,7075,7076,7077,7078,7079,7080,7081,7082,7972,12803,12808 // elemental/essences
};
bool Number(std::string const& text,uint32& result)
{
    std::string s=text;
    s.erase(std::remove_if(s.begin(),s.end(),[](unsigned char c){return std::isspace(c);}),s.end());
    if(s.empty() || s.size()>10 || s.find_first_not_of("0123456789")!=std::string::npos)return false;
    unsigned long long n=std::stoull(s);
    if(n>std::numeric_limits<uint32>::max())return false;
    result=uint32(n);return true;
}
bool ReadRows(std::string const& path,std::vector<Row>& rows)
{
    std::ifstream file(path);if(!file)return false;
    std::string line;std::set<uint32> seen;
    while(std::getline(file,line))
    {
        if(line.size()>512)return false;
        line=line.substr(0,line.find('#'));
        if(line.find_first_not_of(" \r\t")==std::string::npos)continue;
        if(std::count(line.begin(),line.end(),',')!=3)return false;
        std::replace(line.begin(),line.end(),',',' ');
        std::istringstream input(line);std::string a,b,c,d,extra;
        Row r{};
        if(!(input>>a>>b>>c>>d) || (input>>extra) || !Number(a,r.item) || !Number(b,r.stack) || !Number(c,r.target) || !Number(d,r.price))return false;
        ItemPrototype const* proto=sObjectMgr.GetItemPrototype(r.item);
        if(!allowed.count(r.item) || !seen.insert(r.item).second || !proto || (proto->Class!=ITEM_CLASS_TRADE_GOODS && proto->Class!=ITEM_CLASS_REAGENT) || proto->HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE) || !r.stack || r.stack>proto->GetMaxStackSize() || !r.target || r.target>20 || !r.price || r.price>100000000 || rows.size()>=128)return false;
        rows.push_back(r);
    }
    return file.eof() && !rows.empty();
}
void Error(char const* message)
{
    sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"MaterialSupply: %s; no new listings this cycle.",message);
}
}
void Update()
{
    if(!sConfig.GetBoolDefault("MaterialSupply.Enable",false))return;
    static auto next=std::chrono::steady_clock::time_point::min();
    auto now=std::chrono::steady_clock::now();if(now<next)return;
    int interval=sConfig.GetIntDefault("MaterialSupply.IntervalSeconds",60);
    int limit=sConfig.GetIntDefault("MaterialSupply.MaxAddsPerTick",30);
    next=now+std::chrono::seconds(60);
    if(interval<60 || interval>86400 || limit<1 || limit>50){Error("invalid interval or addition limit");return;}
    next=now+std::chrono::seconds(interval);
    int sellerConfig=sConfig.GetIntDefault("MaterialSupply.SellerGuid",0);
    if(sellerConfig<=0){Error("missing dedicated seller GUID");return;}
    uint32 seller=uint32(sellerConfig);
    std::unique_ptr<QueryResult> sellerRow(CharacterDatabase.PQuery("SELECT account,name FROM characters WHERE guid=%u AND deleted_time IS NULL",seller));
    if(!sellerRow){Error("seller character does not exist or is deleted");return;}
    uint32 account=sellerRow->Fetch()[0].GetUInt32();
    std::string characterName=sellerRow->Fetch()[1].GetCppString();
    std::unique_ptr<QueryResult> accountRow(LoginDatabase.PQuery("SELECT a.username, COALESCE((SELECT MAX(aa.gmlevel) FROM account_access aa WHERE aa.id=a.id AND aa.RealmID IN (-1,%u)),0) FROM account a WHERE a.id=%u",realmID,account));
    if(!accountRow || accountRow->Fetch()[1].GetUInt32()!=SEC_PLAYER){Error("seller must belong to an ordinary player account");return;}
    std::string username=accountRow->Fetch()[0].GetCppString();
    std::transform(username.begin(),username.end(),username.begin(),[](unsigned char c){return std::toupper(c);});
    std::transform(characterName.begin(),characterName.end(),characterName.begin(),[](unsigned char c){return std::toupper(c);});
    if(username=="ROOT" || username=="WEB_SERVICE" || characterName=="ROOT"){Error("protected account or character cannot be seller");return;}
    std::vector<Row> rows;
    if(!ReadRows(sConfig.GetStringDefault("MaterialSupply.ConfigFile","material-supply.csv"),rows)){Error("invalid or missing material CSV (item,stack,target,buyoutCopper)");return;}
    struct Pool { AuctionHouseEntry const* entry;AuctionHouseObject* house; };
    std::vector<Pool> pools;std::set<AuctionHouseObject*> unique;
    for(uint32 faction:{55u,29u,120u})
    {
        auto entry=sAuctionMgr.GetAuctionHouseEntry(faction);
        auto house=entry ? sAuctionMgr.GetAuctionsMap(entry) : nullptr;
        if(!house){Error("auction house unavailable");return;}
        if(unique.insert(house).second)pools.push_back({entry,house});
    }
    if(CharacterDatabase.InTransaction()){Error("refusing nested character transaction");return;}
    struct AuctionCleanup
    {
        void operator()(AuctionEntry* auction) const
        {
            if(auction){sObjectMgr.FreeAuctionID(auction->Id);delete auction;}
        }
    };
    std::vector<std::unique_ptr<Item>> preparedItems;
    std::vector<std::unique_ptr<AuctionEntry,AuctionCleanup>> preparedAuctions;
    std::vector<AuctionHouseObject*> preparedHouses;
    unsigned added=0;time_t wall=time(nullptr);
    // Rotate traversal: a small cap must not starve later items/houses forever.
    static size_t cursor=0;size_t combinations=pools.size()*rows.size();
    for(size_t walked=0;walked<combinations && added<unsigned(limit);++walked)
    {
        size_t index=cursor++%combinations;Pool const& pool=pools[index/rows.size()];Row const& row=rows[index%rows.size()];
        unsigned count=0;
        for(auto const& pair:*pool.house->GetAuctions())
            if(pair.second->owner==seller && pair.second->itemTemplate==row.item && pair.second->expireTime>wall)++count;
        while(count<row.target && added<unsigned(limit))
        {
            std::unique_ptr<Item> item(Item::CreateItem(row.item,row.stack));
            if(!item){Error("item creation failed");return;}
            item->SetOwnerGuid(ObjectGuid(HIGHGUID_PLAYER,seller));
            std::unique_ptr<AuctionEntry,AuctionCleanup> auction(new AuctionEntry{});
            auction->Id=sObjectMgr.GenerateAuctionID();auction->auctionHouseEntry=pool.entry;
            auction->itemGuidLow=item->GetGUIDLow();auction->itemTemplate=row.item;
            auction->owner=seller;auction->ownerAccount=account;
            auction->startbid=row.price;auction->buyout=row.price;
            // System stock has no player-funded deposit; do not mint refunded deposits.
            auction->deposit=0;auction->depositTime=wall;auction->expireTime=wall+48*HOUR;
            preparedItems.push_back(std::move(item));
            preparedAuctions.push_back(std::move(auction));
            preparedHouses.push_back(pool.house);
            ++count;++added;
        }
    }
    if(!added)return;
    if(!CharacterDatabase.BeginTransaction()){Error("cannot begin character transaction");return;}
    for(size_t i=0;i<preparedItems.size();++i)
    {
        preparedItems[i]->SaveToDB();preparedAuctions[i]->SaveToDB();
    }
    // Direct returns actual SQL execution success; never expose uncommitted stock.
    if(!CharacterDatabase.CommitTransactionDirect())
    {
        if(CharacterDatabase.InTransaction())CharacterDatabase.RollbackTransaction();
        Error("material listing transaction failed");return;
    }
    for(size_t i=0;i<preparedItems.size();++i)
    {
        sAuctionMgr.AddAItem(preparedItems[i].release());
        preparedHouses[i]->AddAuction(preparedAuctions[i].release());
    }
    if(added)sLog.Out(LOG_BASIC,LOG_LVL_MINIMAL,"MaterialSupply: seller %u added %u listings across %u auction pools.",seller,added,unsigned(pools.size()));
}
}
