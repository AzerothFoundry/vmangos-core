#include "GmItemBrowser.h"
#include "Player.h"
#include "WorldSession.h"
#include "ObjectMgr.h"
#include "ItemPrototype.h"
#include "PlayerBotPvpTeams.h"
#include "Util.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <sstream>
namespace {
bool Number(std::string const& text, long min, long max, long& value) {
    if(text.empty())return false; char* end=nullptr;errno=0;value=std::strtol(text.c_str(),&end,10);
    return !errno && end && !*end && value>=min && value<=max;
}
std::string Name(ItemPrototype const* item,int locale) {
    if(locale>=0)if(auto data=sObjectMgr.GetItemLocale(item->ItemId))
        if(size_t(locale)<data->Name.size()&&!data->Name[locale].empty())return data->Name[locale];
    return item->Name1;
}
std::string Safe(std::string text) {
    for(char& c:text)if(c=='|'||c=='\r'||c=='\n')c=' ';
    if(text.size()>90){text.resize(90);while(!text.empty()&&(uint8(text.back())&0xc0)==0x80)text.pop_back();if(!text.empty()&&uint8(text.back())>=0xc0)text.pop_back();}
    return text;
}
bool Kind(ItemPrototype const* item,std::string const& kind) {
    if(kind=="all")return true;
    if(kind=="gear")return item->Class==ITEM_CLASS_ARMOR||item->Class==ITEM_CLASS_WEAPON;
    if(kind=="waist")return item->InventoryType==INVTYPE_WAIST;
    if(kind=="wrist")return item->InventoryType==INVTYPE_WRISTS;
    if(kind=="feet")return item->InventoryType==INVTYPE_FEET;
    if(kind=="head")return item->InventoryType==INVTYPE_HEAD;
    if(kind=="shoulder")return item->InventoryType==INVTYPE_SHOULDERS;
    if(kind=="chest")return item->InventoryType==INVTYPE_CHEST||item->InventoryType==INVTYPE_ROBE;
    if(kind=="hands")return item->InventoryType==INVTYPE_HANDS;
    if(kind=="legs")return item->InventoryType==INVTYPE_LEGS;
    if(kind=="neck")return item->InventoryType==INVTYPE_NECK;
    if(kind=="rings")return item->InventoryType==INVTYPE_FINGER;
    if(kind=="cloak")return item->InventoryType==INVTYPE_CLOAK;
    if(kind=="trinkets")return item->InventoryType==INVTYPE_TRINKET;
    if(kind=="weapons")return item->Class==ITEM_CLASS_WEAPON;
    if(kind=="offhand")return item->InventoryType==INVTYPE_SHIELD||item->InventoryType==INVTYPE_HOLDABLE||item->InventoryType==INVTYPE_WEAPONOFFHAND;
    if(kind=="wand")return item->Class==ITEM_CLASS_WEAPON&&item->SubClass==ITEM_SUBCLASS_WEAPON_WAND;
    if(kind=="bags")return item->Class==ITEM_CLASS_CONTAINER&&item->SubClass==ITEM_SUBCLASS_CONTAINER;
    if(kind=="materials")return item->Class==ITEM_CLASS_TRADE_GOODS||item->Class==ITEM_CLASS_REAGENT;
    return false;
}
}
bool GmItemBrowser::Search(Player* gm,std::vector<std::string> const& words,std::vector<std::string>& lines,std::string& error) {
    if(!gm||!gm->GetSession()||words.size()<9){error="Expected items search min max quality kind class page req|ilvl [name]";return false;}
    long min,max,quality,cls,page;
    if(!Number(words[2],0,100,min)||!Number(words[3],0,100,max)||min>max||!Number(words[4],-1,6,quality)||!Number(words[6],0,11,cls)||cls==6||cls==10||!Number(words[7],1,10000,page)||(words[8]!="req"&&words[8]!="ilvl")) {error="Invalid item browse limits";return false;}
    auto const& kind=words[5];if(kind!="all"&&kind!="gear"&&kind!="bags"&&kind!="materials"&&kind!="neck"&&kind!="rings"&&kind!="cloak"&&kind!="trinkets"&&kind!="weapons"&&kind!="offhand"&&kind!="wand"&&kind!="waist"&&kind!="wrist"&&kind!="feet"&&kind!="head"&&kind!="shoulder"&&kind!="chest"&&kind!="hands"&&kind!="legs"){error="Invalid item category";return false;}
    std::string query;for(size_t i=9;i<words.size();++i){if(!query.empty())query+=' ';query+=words[i];}
    std::wstring needle;if(!Utf8toWStr(query,needle)){error="Invalid item search text";return false;}wstrToLower(needle);
    int locale=gm->GetSession()->GetSessionDbLocaleIndex();
    struct Row{ItemPrototype const* item;std::string name;};std::vector<Row> rows;
    for(auto const& pair:sObjectMgr.GetItemPrototypeMap()) {
        auto item=&pair.second;uint32 level=words[8]=="ilvl"?item->ItemLevel:item->RequiredLevel;
        if(item->HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE)||level<uint32(min)||level>uint32(max)||(quality>=0&&item->Quality!=uint32(quality))||!Kind(item,kind))continue;
        if(cls&&item->AllowableClass!=-1&&!(item->AllowableClass&(1u<<(cls-1))))continue;
        std::string name=Name(item,locale);
        if(!query.empty()&&!Utf8FitTo(name,needle)&&!Utf8FitTo(item->Name1,needle))continue;
        rows.push_back({item,name});
    }
    bool ilvl=words[8]=="ilvl";
    std::sort(rows.begin(),rows.end(),[ilvl](Row const& a,Row const& b){
        uint32 x=ilvl?a.item->ItemLevel:a.item->RequiredLevel,y=ilvl?b.item->ItemLevel:b.item->RequiredLevel;
        if(x!=y)return x>y;if(a.item->Quality!=b.item->Quality)return a.item->Quality>b.item->Quality;
        if(a.item->ItemLevel!=b.item->ItemLevel)return a.item->ItemLevel>b.item->ItemLevel;return a.item->ItemId<b.item->ItemId;
    });
    size_t begin=size_t(page-1)*8,end=std::min(rows.size(),begin+8);
    for(size_t i=begin;i<end;++i) {
        auto const& r=rows[i];std::ostringstream out;
        out<<"GMUI|ITEM|id="<<r.item->ItemId<<"|name="<<Safe(r.name)<<"|level="<<r.item->RequiredLevel<<"|ilvl="<<r.item->ItemLevel<<"|quality="<<r.item->Quality<<"|class="<<r.item->Class<<"|slot="<<r.item->InventoryType<<"|stack="<<r.item->GetMaxStackSize()<<"|team="<<(BotMatchDetail::PvpTeamMask(r.item)==1 ? 469 : BotMatchDetail::PvpTeamMask(r.item)==2 ? 67 : 0);lines.push_back(out.str());
    }
    std::ostringstream meta;meta<<"GMUI|ITEMMETA|page="<<page<<"|total="<<rows.size()<<"|more="<<(end<rows.size());lines.push_back(meta.str());lines.push_back("GMUI|DONE|items");return true;
}
