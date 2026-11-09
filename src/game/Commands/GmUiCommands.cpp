#include "PlayerBots/BotLoot33.h"
#include "RaidCoordination.h"
#include "PvPCombatPolicy.h"
#include "BattleBotPolicyRuntime.h"
#include "BattleBotAI.h"
#include "BattleGround.h"
#include "PartyBotAI.h"
#include "McBotTactics.h"
#include "TemporarySummon.h"
// Local administrator panel: typed self-only actions, never an arbitrary command proxy.
#include "Chat.h"
#include "Player.h"
#include "Bag.h"
#include "WorldSession.h"
#include "World.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "SpellAuras.h"
#include "PlayerBotMatch.h"
#include "PartyBotPreset.h"
#include "GmItemBrowser.h"
#include "GmItemMail.h"
#include "GmBulkItemPolicy.h"
#include "GmMassRevive.h"
#include "Item.h"
#include "GmSkillTools.h"
#include "HonorMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Util.h"
#include "MapManager.h"
#include "Config/Config.h"
#include <fstream>
#include <map>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <vector>
#include <algorithm>

#include "GmInstanceBossTeleport.h"
#include "GmBossEncounterOrder.h"
#include "InstanceData.h"

extern bool RegisterPlayerToBG(WorldSession*, BattleGroundTypeId);
namespace {
bool Integer(std::string const& text, long lo, long hi, long& out)
{
    if (text.empty()) return false;
    char* end = nullptr; errno = 0;
    out = std::strtol(text.c_str(), &end, 10);
    return !errno && end && !*end && out >= lo && out <= hi;
}
bool Factor(std::string const& text, float& out)
{
    char* end = nullptr; errno = 0;
    out = std::strtof(text.c_str(), &end);
    return !errno && end && end != text.c_str() && !*end && std::isfinite(out) && out >= 0.25f && out <= 3.0f;
}
bool Coordinate(std::string const& text, float& value)
{
    char* end = nullptr; errno = 0;
    value = std::strtof(text.c_str(), &end);
    return !errno && end && end != text.c_str() && !*end && std::isfinite(value);
}
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}
std::string LocalTele(char const* kind,uint32 id,std::string const& fallback) {
    static std::map<std::pair<std::string,uint32>,std::string> names=[] {
        std::map<std::pair<std::string,uint32>,std::string> values;
        std::ifstream file(sConfig.GetStringDefault("GmTeleportNames.File","/workspace/vmangos/_install/etc/gm-teleport-zhCN.tsv"));
        std::string line;
        while(std::getline(file,line)){
            std::istringstream input(line);std::string type,number,name;
            if(!std::getline(input,type,'\t')||!std::getline(input,number,'\t')||!std::getline(input,name)||name.empty())continue;
            char* end=nullptr;unsigned long key=std::strtoul(number.c_str(),&end,10);if(!end||*end||key>0xFFFFFFFFUL)continue;
            values[{type,uint32(key)}]=name;
        }
        return values;
    }();
    auto found=names.find({kind,id});return found==names.end()?fallback:found->second;
}
struct SelfSelection {
    Player* player; ObjectGuid previous;
    explicit SelfSelection(Player* p) : player(p), previous(p->GetSelectionGuid()) { p->SetSelectionGuid(p->GetObjectGuid()); }
    ~SelfSelection() { player->SetSelectionGuid(previous); }
};
uint32 FreeGeneralSlots(Player* p)
{
    uint32 slots = 0;
    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (!p->GetItemByPos(INVENTORY_SLOT_BAG_0, i)) ++slots;
    for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        if (Bag* bag = dynamic_cast<Bag*>(p->GetItemByPos(INVENTORY_SLOT_BAG_0, i)))
            if (bag->GetProto()->BagFamily == 0) slots += bag->GetFreeSlots();
    return slots;
}
std::string SafeField(std::string s)
{
    for (char& c : s) if (c == '|' || c == '\n' || c == '\r') c = ' ';
    if (s.size() > 90) s.resize(90);
    return s;
}
}

bool ChatHandler::HandleGmUiCommand(char* args)
{
    auto error = [this](char const* message) { SendSysMessage((std::string("GMUI|ERROR|") + message).c_str()); SetSentErrorMessage(true); return false; };
    auto ok = [this](char const* message) {
        // Persist through the normal character save pipeline; status always reads
        // the online Player object rather than its potentially older DB snapshot.
        if (m_session && m_session->GetPlayer()) m_session->GetPlayer()->SaveToDB();
        SendSysMessage((std::string("GMUI|OK|") + message).c_str()); return true;
    };
    std::vector<std::string> words; std::istringstream input(args ? args : ""); std::string word;
    while (input >> word) words.push_back(word);
    if (words.empty()) return error("Action required");
    if(words.size()==4&&words[0]=="loot33"&&words[1]=="inspect"){
        if(GetAccessLevel()<SEC_ADMINISTRATOR)return error("Administrator access required");
        std::string name=words[2];if(!normalizePlayerName(name))return error("Invalid bot name");
        Player* actor=ObjectAccessor::FindPlayerByName(name.c_str());if(!actor||!actor->IsInWorld()||!actor->IsBot())return error("Online bot required");
        unsigned long id=0;try{size_t used=0;id=std::stoul(words[3],&used);if(used!=words[3].size()||id>1000000)throw std::runtime_error("id");}catch(...){return error("Invalid item id");}
        auto item=sObjectMgr.GetItemPrototype(uint32(id));if(!item)return error("Unknown item");
        auto result=BotLoot33::Decide(actor,item);
        char const* vote=result.vote==BotLoot33Policy::Vote::Need?"need":result.vote==BotLoot33Policy::Vote::Greed?"greed":"pass";
        std::ostringstream out;out<<"GMUI|BOTLOOT33|bot="<<SafeField(actor->GetName())<<"|class="<<uint32(actor->GetClass())<<"|item="<<id<<"|ilvl="<<item->ItemLevel<<"|inventory="<<item->InventoryType<<"|subclass="<<item->SubClass<<"|enabled="<<BotLoot33::Enabled()<<"|persistent="<<BotLoot33::Retained(actor)<<"|vote="<<vote<<"|reason="<<result.reason;
        SendSysMessage(out.str().c_str());SendSysMessage("GMUI|DONE|bot_loot33_inspect");return true;
    }
    // Global policy management is fixed-path, administrator-only and never selects or saves a character.
    if(words.size()>=2 && words[0]=="pvp" && words[1]=="policy")
    {
        if(GetAccessLevel()<SEC_ADMINISTRATOR) return error("Administrator policy access required");
        BattleBotPolicyRuntime::EnsureConfigured();
        if(words.size()<3) return error("pvp policy status/reload/save/enable/show/set/explain");
        std::string const& op=words[2];
        auto result=[&](PvPCombatPolicy::Result const& r) {
            if(!r.ok)return error(r.message.c_str());
            SendSysMessage((std::string("GMUI|OK|PVP policy: ")+r.message).c_str());
            SendSysMessage((std::string("GMUI|PVPPOLICY|")+PvPCombatPolicy::Status()).c_str());
            SendSysMessage("GMUI|DONE|pvp_policy");return true;
        };
        if(op=="status" && words.size()==3) {
            PvPCombatPolicy::Poll();
            SendSysMessage((std::string("GMUI|PVPPOLICY|")+PvPCombatPolicy::Status()).c_str());
            SendSysMessage("GMUI|DONE|pvp_policy");return true;
        }
        if(op=="reload" && words.size()==3)return result(PvPCombatPolicy::Reload());
        if(op=="save" && words.size()==3)return result(PvPCombatPolicy::Save());
        if(op=="enable" && words.size()==4) {
            if(words[3]!="on" && words[3]!="off")return error("Policy enable requires on/off");
            return result(PvPCombatPolicy::SetEnabled(words[3]=="on"));
        }
        if(op=="show" && words.size()==4) {
            auto json=PvPCombatPolicy::RuleJson(words[3]);
            if(json.empty())return error("Unknown policy rule id");
            if(json.size()>8192)return error("Rule too large for chat; inspect fixed policy file");
            size_t count=(json.size()+159)/160;
            for(size_t part=0;part<count;++part)
                SendSysMessage((std::string("GMUI|PVPRULE|")+words[3]+"|part="+std::to_string(part+1)+"/"+std::to_string(count)+"|"+json.substr(part*160,160)).c_str());
            SendSysMessage("GMUI|DONE|pvp_policy");return true;
        }
        if(op=="set" && words.size()>=6) {
            std::string value=words[5];for(size_t i=6;i<words.size();++i)value+=' '+words[i];
            return result(PvPCombatPolicy::Patch(words[3],words[4],value));
        }
        if(op=="explain" && words.size()==4) {
            std::string name=words[3];if(!normalizePlayerName(name))return error("Invalid policy actor name");
            Player* target=ObjectAccessor::FindPlayerByName(name.c_str());
            if(!target || !target->IsInWorld())return error("Policy actor must be online");
            SendSysMessage((std::string("GMUI|PVPDECISION|")+BattleBotPolicyRuntime::Explain(target)).c_str());
            SendSysMessage("GMUI|DONE|pvp_policy");return true;
        }
        return error("Invalid fixed-path policy operation or argument count");
    }
    if(words.size()==3 && words[0]=="bg" && (words[1]=="actor" || words[1]=="flags" || words[1]=="route" || words[1]=="wsgroute"))
    {
        if(GetAccessLevel()<SEC_ADMINISTRATOR)return error("Administrator access required");
        std::string name=words[2];if(!normalizePlayerName(name))return error("Invalid actor name");
        Player* actor=ObjectAccessor::FindPlayerByName(name.c_str());
        auto* ai=actor && actor->AI()?dynamic_cast<BattleBotAI*>(actor->AI()):nullptr;
        if(words[1]=="route"){
            if(!actor||!actor->IsInWorld()||!actor->GetBattleGround()||actor->GetBattleGround()->GetTypeID()!=BATTLEGROUND_AB)return error("Online Arathi player required");
            for(auto const& row:BattleBotAI::ArathiRouteStatus(actor))SendSysMessage(row.c_str());
            SendSysMessage("GMUI|DONE|bg_route");return true;
        }
        if(words[1]=="wsgroute"){
            if(!actor||!actor->IsInWorld()||!actor->GetBattleGround()||actor->GetBattleGround()->GetTypeID()!=BATTLEGROUND_WS)return error("Online Warsong player required");
            for(auto const& row:BattleBotAI::WarsongRouteStatus(actor))SendSysMessage(row.c_str());
            SendSysMessage("GMUI|DONE|wsg_route");return true;
        }
        if(words[1]=="flags"){
            if(!actor||!actor->IsInWorld()||!actor->GetBattleGround()||actor->GetBattleGround()->GetTypeID()!=BATTLEGROUND_AB)return error("Online Arathi player required");
            for(auto const& row:BattleBotAI::ArathiFlagStatus(actor))SendSysMessage(row.c_str());
            SendSysMessage("GMUI|DONE|bg_flags");return true;
        }
        if(!actor || !actor->IsInWorld() || !ai)return error("Online BattleBot required");
        std::ostringstream out;out<<"GMUI|BGACTOR|name="<<SafeField(actor->GetName())
           <<"|map="<<actor->GetMapId()<<"|x="<<actor->GetPositionX()<<"|y="<<actor->GetPositionY()<<"|z="<<actor->GetPositionZ()
           <<"|speed="<<actor->GetSpeed(MOVE_RUN)<<"|mounted="<<actor->IsMounted()<<"|health="<<actor->GetHealthPercent()
           <<"|moving="<<actor->IsMoving()<<"|combat="<<actor->IsInCombat()<<"|"<<ai->OpeningStatus();
        SendSysMessage(out.str().c_str());
        SendSysMessage((std::string("GMUI|BGPREP|name=")+SafeField(actor->GetName())+"|"+ai->PreparationStatus()).c_str());
        SendSysMessage((std::string("GMUI|BGMOUNT|name=")+SafeField(actor->GetName())+"|"+ai->MountStatus()).c_str());
        if(actor->GetBattleGround()&&actor->GetBattleGround()->GetTypeID()==BATTLEGROUND_WS)SendSysMessage((std::string("GMUI|WSGSCENE|name=")+SafeField(actor->GetName())+"|"+ai->WarsongStatus()).c_str());
        if(actor->GetBattleGround()&&actor->GetBattleGround()->GetTypeID()==BATTLEGROUND_AV)SendSysMessage((std::string("GMUI|AVROUTE|name=")+SafeField(actor->GetName())+"|"+ai->AvRouteStatus()).c_str());
        SendSysMessage("GMUI|DONE|bg_actor");return true;
    }
    // Console/SOAP gets one read-only inspection action; all mutations below still
    // require an in-game administrator and are fixed to that session's own player.
    if(words[0]=="inspect" && words.size()==2 && GetAccessLevel()>=SEC_ADMINISTRATOR)
    {
        std::string name=words[1];if(!normalizePlayerName(name))return error("Invalid character name");
        Player* target=ObjectAccessor::FindPlayer(sObjectMgr.GetPlayerGuidByName(name));
        if(!target || !target->IsInWorld())return error("Character must be online for live inspection");
        std::vector<std::string> lines;GmSkillTools::Inspect(target,lines);
        for(auto const& line:lines)SendSysMessage(line.c_str());
        SendSysMessage(RaidCoordination::Status(target).c_str());
        if(target->GetGroup())for(GroupReference* ref=target->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* m=ref->getSource())if(auto* ai=dynamic_cast<PartyBotAI*>(m->AI()))SendSysMessage((std::string(m->GetName())+" "+McBotTactics::Status(*ai)).c_str());
        return true;
    }
    if (!m_session || m_session->GetSecurity() < SEC_ADMINISTRATOR || !m_session->GetPlayer())
        return error("Administrator session required");
    Player* self = m_session->GetPlayer();

    auto invoke = [this, self, &error, &ok](bool (ChatHandler::*method)(char*), std::string value) {
        SelfSelection selection(self);
        std::vector<char> text(value.begin(), value.end()); text.push_back(0);
        if (!(this->*method)(text.data())) return error("Action rejected; see server response");
        return ok("Action completed");
    };
    if(words.size()==3 && words[0]=="raidai" && words[1]=="autopull"){
        if(words[2]!="on"&&words[2]!="off")return error("raidai autopull on/off");
        SendSysMessage(RaidCoordination::AutoPull(self,words[2]=="on").c_str());return true;
    }
    if(words.size()==2 && words[0]=="raidai"){
        if(words[1]=="nearby" || words[1]=="target")
        {
            if(self->GetMapId()!=409)return error("该目标诊断仅在熔火之心可用");
            auto emit=[&](Creature* creature,uint64 guid)
            {
                std::ostringstream row;
                row<<"GMUI|RAIDNPC|guid="<<guid<<"|exists="<<(creature?1:0);
                if(creature)
                {
                    auto* temporary=dynamic_cast<TemporarySummon*>(creature);
                    row<<"|entry="<<creature->GetEntry()<<"|name="<<SafeField(creature->GetName())
                       <<"|temporary="<<(temporary?1:0)
                       <<"|summoner="<<(temporary?temporary->GetSummonerGuid().GetRawValue():0)
                       <<"|alive="<<creature->IsAlive()<<"|combat="<<creature->IsInCombat()
                       <<"|hp="<<creature->GetHealth()<<"|maxHp="<<creature->GetMaxHealth()
                       <<"|x="<<creature->GetPositionX()<<"|y="<<creature->GetPositionY()<<"|z="<<creature->GetPositionZ();
                }
                SendSysMessage(row.str().c_str());
            };
            if(words[1]=="target")emit(GetSelectedCreature(),self->GetSelectionGuid().GetRawValue());
            else
            {
                unsigned count=0;
                for(uint32 entry:{11671u,11672u,11988u,12056u})
                {
                    std::list<Creature*> nearby;
                    self->GetCreatureListWithEntryInGrid(nearby,entry,80.0f);
                    for(Creature* creature:nearby)
                    {
                        if(count>=32)break;
                        emit(creature,creature->GetGUID());++count;
                    }
                }
            }
            SendSysMessage("GMUI|DONE|raidNearby");return true;
        }
        if(words[1]=="pull"){SendSysMessage(RaidCoordination::BeginPull(self,GetSelectedUnit()).c_str());return true;}
        if(words[1]=="cancel"){RaidCoordination::CancelPull(self);SendSysMessage(RaidCoordination::Status(self).c_str());return true;}
        if(words[1]=="on")RaidCoordination::SetEnabled(self,true);
        else if(words[1]=="off")RaidCoordination::SetEnabled(self,false);
        else if(words[1]=="reset"){if(!RaidCoordination::Reset(self))return error("团队职责重置需要全员与宠物脱战");}
        else if(words[1]!="status")return error("raidai status/on/off/reset");
        SendSysMessage(RaidCoordination::Status(self).c_str());
        if(self->GetGroup())for(GroupReference* ref=self->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* member=ref->getSource())if(auto* ai=dynamic_cast<PartyBotAI*>(member->AI()))SendSysMessage((std::string(member->GetName())+" "+McBotTactics::Status(*ai)).c_str());return true;
    }
    long n = 0, count = 0;
    if(words.size()==2 && words[0]=="auras" && words[1]=="status"){
        Unit* target=GetSelectedUnit();if(!target)target=self;
        unsigned negative=0,limited=0,visible=0;
        for(auto const& pair:target->GetSpellAuraHolderMap())if(!pair.second->IsPositive()){
            ++negative;if(pair.second->IsAffectedByVisibleSlotLimit())++limited;
            if(pair.second->GetAuraSlot()<MAX_AURAS)++visible;
        }
        std::ostringstream out;out<<"GMUI|AURACAP|limit="<<sWorld.getConfig(CONFIG_UINT32_DEBUFF_LIMIT)<<"|negative="<<negative<<"|limited="<<limited<<"|visible="<<visible;
        SendSysMessage(out.str().c_str());return true;
    }
    if(words[0]=="skills")
    {
        std::vector<std::string> lines;std::string message;
        if(!GmSkillTools::Execute(self,words,lines,message))return error(message.c_str());
        for(auto const& line:lines)SendSysMessage(line.c_str());
        return true;
    }
    if(words[0]=="supplies" && words.size()==2 && (words[1]=="food"||words[1]=="water"))
    {
        std::vector<uint32> candidates=words[1]=="food" ? std::vector<uint32>{5349,1113,1114,1487,8075,8076,22895} : std::vector<uint32>{5350,2288,2136,3772,8077,8078,8079};
        ItemPrototype const* best=nullptr;
        for(auto id:candidates)if(auto item=sObjectMgr.GetItemPrototype(id))
            if(item->RequiredLevel<=self->GetLevel()&&(!best||item->RequiredLevel>best->RequiredLevel))best=item;
        if(!best)return error("No conjured food or water for current level");
        return invoke(&ChatHandler::HandleAddItemCommand,std::to_string(best->ItemId)+" 20");
    }
    if (words[0] == "status" && words.size() == 1)
    {
        std::ostringstream out;
        out << "GMUI|STATUS|level=" << unsigned(self->GetLevel()) << "|class=" << unsigned(self->GetClass())
            << "|team=" << uint32(self->GetTeam()) << "|gm=" << (self->IsGameMaster() ? 1 : 0)
            << "|security=" << unsigned(m_session->GetSecurity()) << "|maxLevel=" << std::min<uint32>(60, sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
            << "|inBG=" << (self->InBattleGround() ? 1 : 0)
            << "|visible=" << (self->IsGMVisible() ? 1 : 0) << "|god=" << (self->GetInvincibilityHpThreshold() > 0 ? 1 : 0) << "|fly=" << (self->HasCheatOption(PLAYER_CHEAT_FLY) ? 1 : 0)
            << "|score=" << BotMatch::GearScore(self) << "|pvpPieces=" << BotMatch::CountPvpSetPieces(self)
            << "|money=" << self->GetMoney() << "|rank=" << int(self->GetHonorMgr().GetRank().visualRank)
            << "|highestRank=" << int(self->GetHonorMgr().GetHighestRank().visualRank)
            << "|rankPoints=" << self->GetHonorMgr().GetRankPoints() << "|p6Healers=" << PartyPreset::VerifiedHealers(self);
        SendSysMessage(out.str().c_str());
        std::string match = "GMUI|MATCH|" + BotMatch::Status(self->GetObjectGuid());
        SendSysMessage(match.substr(0, 230).c_str());
        for (auto const& line : BotMatch::TeamStatus(self->GetObjectGuid())) SendSysMessage(line.c_str());
        SendSysMessage(("GMUI|AUTO|" + BotMatch::AutoStatus(self->GetObjectGuid())).substr(0,230).c_str());
        return true;
    }
    if (words[0] == "honor" && words.size() == 3 && words[1] == "rank")
    {
        if (!Integer(words[2],0,14,n)) return error("Honor rank must be R0 to R14");
        if (self->IsInCombat() || self->InBattleGround() || self->InBattleGroundQueue()) return error("Leave combat and battleground queue before changing honor rank");
        float rp = n == 0 ? 0.0f : n == 1 ? 1.0f : n == 2 ? 2000.0f : float((n-2)*5000);
        HonorMgr& honor = self->GetHonorMgr();
        HonorRankInfo rank = HonorMgr::CalculateRank(rp,honor.GetTotalHK());
        if (rank.visualRank != n) return error("Honor rank calculation failed; nothing changed");
        honor.SetRankPoints(rp);
        // Update raises historical highest only when appropriate; lowering the
        // current rank must not erase the player's historical achievement.
        honor.Update();
        std::string result = "Current honor rank set to R" + std::to_string(n) + "; historical highest retained, normal save requested. Weekly ranking rules still apply.";
        return ok(result.c_str());
    }
    if (words[0] == "level" && words.size() == 2)
    {
        if (!Integer(words[1], 1, std::min<uint32>(60, sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL)), n)) return error("Level must be between 1 and server maximum (at most 60)");
        if (self->IsInCombat() || self->InBattleGround() || self->InBattleGroundQueue()) return error("Leave combat, battleground and battleground queue before changing level");
        return invoke(&ChatHandler::HandleCharacterLevelCommand, self->GetName() + std::string(" ") + std::to_string(n));
    }
    if(words[0]=="raidtools"&&words.size()>=3&&words[1]=="revive"){
        bool preview=words.size()==3&&words[2]=="preview";
        bool immediate=words.size()==3&&words[2]=="now";
        bool confirm=words.size()==5&&words[2]=="confirm";long expectedMap=0,expectedInstance=0;
        if(!preview&&!confirm&&!immediate)return error("Expected raidtools revive now, preview or confirm map instance");
        if(!self->IsAlive()||!self->GetGroup()||!self->GetMap()->IsDungeon()||self->InBattleGround()||self->m_duel||self->IsBeingTeleported()||self->IsTaxiFlying()||self->GetTransport())return error("Mass revive requires a living administrator in the current dungeon group");
        if(confirm&&(!Integer(words[3],0,16777215,expectedMap)||!Integer(words[4],0,2147483647,expectedInstance)||!GmBossEncounterOrder::SameContext(self->GetMapId(),self->GetInstanceId(),uint32(expectedMap),uint32(expectedInstance))))return error("Mass revive context changed; preview the current instance again");
        auto counts=GmMassRevive::Run(self,confirm||immediate);std::ostringstream row;row<<"GMUI|MASSREVIVE|eligible="<<counts.eligible<<"|bots="<<counts.bots<<"|players="<<counts.players<<"|skipped="<<counts.skipped<<"|revived="<<counts.revived<<"|offered="<<counts.offered<<"|map="<<self->GetMapId()<<"|instance="<<self->GetInstanceId();SendSysMessage(row.str().c_str());
        if(preview){SendSysMessage("GMUI|DONE|massrevive_preview");return true;}
        std::ostringstream result;result<<"GMUI|MASSREVIVERESULT|bots="<<counts.revived<<"|players="<<counts.offered<<"|skipped="<<counts.skipped<<"|map="<<self->GetMapId()<<"|instance="<<self->GetInstanceId();SendSysMessage(result.str().c_str());
        return ok("Group revival applied at 100 percent health and mana; human players receive a confirmation offer");
    }
    if (words[0] == "money" && words.size() == 2)
    {
        if (!Integer(words[1], -1000, 1000, n)) return error("Gold delta must be -1000 to 1000");
        return invoke(&ChatHandler::HandleModifyMoneyCommand, std::to_string(n * 10000));
    }
    if (words[0] == "item" && words.size() == 3)
    {
        if (!Integer(words[1], 1, 1000000, n) || !Integer(words[2], 1, 100000, count)) return error("Expected positive item id and count 1 to 100000");
        if (!sObjectMgr.GetItemPrototype(uint32(n))) return error("Item unavailable in current content patch");
        uint32 requested=uint32(count),noSpace=0;ItemPosCountVec destinations;
        auto result=self->CanStoreNewItem(NULL_BAG,NULL_SLOT,destinations,uint32(n),requested,&noSpace);
        if(result!=EQUIP_ERR_OK||noSpace||destinations.empty())return error("Not enough bag capacity or unique-item limit; nothing granted. Use mail for large quantities");
        uint32 before=self->GetItemCount(uint32(n),true);
        Item* item=self->StoreNewItem(destinations,uint32(n),true,Item::GenerateItemRandomPropertyId(uint32(n)));
        uint32 after=self->GetItemCount(uint32(n),true),received=after>=before?after-before:0;
        if(!item||received!=requested){self->SaveToDB();std::ostringstream row;row<<"GMUI|ITEMGRANT|id="<<n<<"|requested="<<requested<<"|received="<<received;SendSysMessage(row.str().c_str());return error("Native item grant did not complete; inspect received count before retrying");}
        // Preserve native GM add-item behavior: newly granted self items are unbound.
        for(auto const& destination:destinations)if(Item* stored=self->GetItemByPos(destination.pos))stored->SetBinding(false);
        self->SendNewItem(item,requested,false,true);self->SaveToDB();
        std::ostringstream row;row<<"GMUI|ITEMGRANT|id="<<n<<"|requested="<<requested<<"|received="<<received;SendSysMessage(row.str().c_str());
        return ok("Item grant saved");
    }
    if (words[0] == "spell" && (words.size() == 2 || (words.size() == 3 && words[2] == "all")))
    {
        if (!Integer(words[1], 1, 1000000, n) || !sSpellMgr.GetSpellEntry(uint32(n))) return error("Unknown spell");
        return invoke(&ChatHandler::HandleLearnCommand, words[1] + (words.size() == 3 ? " all" : ""));
    }
    if (words[0] == "classspells" && words.size() == 1)
    {
        GmSkillTools::LearnClass(self);
        // Equipment proficiency is an explicit part of the skill action, never gear equip.
        if (self->GetLevel() >= 40)
        {
            uint32 armor = (self->GetClass() == CLASS_WARRIOR || self->GetClass() == CLASS_PALADIN) ? 750 :
                (self->GetClass() == CLASS_HUNTER || self->GetClass() == CLASS_SHAMAN) ? 8737 : 0;
            if (armor && !self->HasSpell(armor)) self->LearnSpell(armor, false, false);
        }
        // An explicit training action; equipment issuance never teaches proficiencies.
        if (self->GetClass() == CLASS_MAGE || self->GetClass() == CLASS_WARLOCK || self->GetClass() == CLASS_PRIEST)
            for (uint32 spell : {uint32(1180),uint32(5009)}) if (!self->HasSpell(spell)) self->LearnSpell(spell,false,false);
        if ((self->GetClass() == CLASS_MAGE || self->GetClass() == CLASS_WARLOCK) && !self->HasSpell(201)) self->LearnSpell(201,false,false);
        if (self->GetLevel() >= 20 && (self->GetClass() == CLASS_WARRIOR || self->GetClass() == CLASS_ROGUE || self->GetClass() == CLASS_HUNTER))
            if (!self->HasSpell(674)) self->LearnSpell(674,false,false);
        self->UpdateSkillsToMaxSkillsForLevel();
        self->SaveToDB(); // Include explicit proficiency changes.
        return ok("Player class spells learned; pet spells are separate");
    }
    if (words[0] == "tele" && words.size() >= 2)
    {
        if(words[1]=="bosses" || words[1]=="boss" || words[1]=="bosscheck") {
            MapEntry const* instance=sMapStorage.LookupEntry<MapEntry>(self->GetMapId());
            if(!instance || !instance->IsDungeon() || self->InBattleGround()) return error("Boss teleport requires the current dungeon instance");
            auto spawns=GmInstanceBoss::Spawns(self->GetMapId());
            auto loaded=GmInstanceBoss::LoadedBosses(self);
            if(words[1]=="bosses") {
                long page=1;
                if(words.size()>3 || (words.size()==3 && !Integer(words[2],1,10000,page))) return error("Expected tele bosses [page]");
                auto entries=GmBossEncounterOrder::Entries(self->GetMapId());
                size_t begin=size_t(page-1)*8,finish=std::min(entries.size(),begin+8);
                for(size_t i=begin;i<finish;++i) {
                    uint32 id=entries[i].id;auto info=sObjectMgr.GetCreatureTemplate(id);
                    std::string name=info?info->name:std::to_string(id);
                    int locale=m_session->GetSessionDbLocaleIndex();
                    if(auto loc=sObjectMgr.GetCreatureLocale(id))
                        if(locale>=0 && size_t(locale)<loc->Name.size() && !loc->Name[locale].empty()) name=loc->Name[locale];
                    CreatureData fallback;bool scripted=GmInstanceBoss::Fallback(spawns,self->GetMapId(),id,fallback);
                    bool available=loaded.count(id) || GmInstanceBoss::Anchor(spawns,id)!=nullptr || scripted;
                    std::ostringstream row;row<<"GMUI|BOSS|id="<<id<<"|name="<<SafeField(name)<<"|map="<<self->GetMapId()<<"|instance="<<self->GetInstanceId()<<"|available="<<available<<"|order="<<entries[i].order<<"|fallback="<<available<<"|reason="<<(loaded.count(id)?"loaded_boss":GmInstanceBoss::Anchor(spawns,id)?"static_spawn":scripted?"native_anchor":"no_static_spawn");
                    SendSysMessage(row.str().c_str());
                }
                std::ostringstream meta;meta<<"GMUI|BOSSMETA|page="<<page<<"|total="<<entries.size()<<"|more="<<(finish<entries.size()?1:0)<<"|map="<<self->GetMapId()<<"|instance="<<self->GetInstanceId();
                SendSysMessage(meta.str().c_str());SendSysMessage("GMUI|DONE|bosses");return true;
            }
            bool confirmedFallback=false;
            if(words[1]=="boss"&&words.size()==6&&words[3]=="fallback"){
                long expectedMap=0,expectedInstance=0;
                if(!Integer(words[4],0,16777215,expectedMap)||!Integer(words[5],0,2147483647,expectedInstance)||!GmBossEncounterOrder::SameContext(self->GetMapId(),self->GetInstanceId(),uint32(expectedMap),uint32(expectedInstance)))return error("Boss fallback context changed; refresh the current instance list");
                confirmedFallback=true;
            }
            if((words.size()!=3&&!confirmedFallback)||!Integer(words[2],1,16777215,n))return error("Expected tele boss entry or tele boss entry fallback map instance");
            if(!self->IsAlive() || self->IsInCombat() || self->IsBeingTeleported() || self->IsTaxiFlying() || self->GetTransport() || self->InBattleGroundQueue()) return error("Boss teleport requires alive, out of combat and off transport");
            if(auto data=self->GetMap()->GetInstanceData())
                if(data->IsEncounterInProgress())return error("Boss teleport refused during an active encounter");
            if(!GmInstanceBoss::Registered(self->GetMapId(),uint32(n)))return error("Boss is not registered in the current map");
            auto anchor=GmInstanceBoss::Anchor(spawns,uint32(n));
            CreatureData actual; GmInstanceBoss::Spawn live{0,&actual};
            if(loaded.count(uint32(n))) {
                auto c=loaded[uint32(n)];
                if(c->IsInCombat())return error("Boss teleport refused during an active encounter");
                actual.creature_id[0]=uint32(n);actual.position.mapId=self->GetMapId();
                actual.position.x=c->GetPositionX();actual.position.y=c->GetPositionY();actual.position.z=c->GetPositionZ();actual.position.o=c->GetOrientation();
                anchor=&live;
            }
            if(!anchor && GmInstanceBoss::Fallback(spawns,self->GetMapId(),uint32(n),actual))anchor=&live;
            if(!anchor)return error("Boss has no static spawn; teleport unsupported");
            Movement::Vector3 safe;
            if(words[1]=="bosscheck") {
                GmInstanceBoss::Diagnostic d;bool valid=GmInstanceBoss::SafePoint(self,*anchor,spawns,safe,&d);
                std::ostringstream a,b,c,e,route;
                a<<"GMUI|BOSSCHECK|id="<<n<<"|map="<<self->GetMapId()<<"|instance="<<self->GetInstanceId()<<"|safe="<<valid<<"|preflight="<<d.preflight<<"|candidates="<<d.candidates;
                b<<"GMUI|BOSSCHECKREJECT|map="<<d.mapMissing<<"|height="<<d.height<<"|liquid="<<d.liquid<<"|boss="<<d.bossAggro<<"|spawn="<<d.spawnAggro<<"|actor="<<d.actorAggro<<"|los="<<d.los<<"|path="<<d.path;
                auto const& p=anchor->data->position;
                c<<"GMUI|BOSSCHECKANCHOR|x="<<p.x<<"|y="<<p.y<<"|z="<<p.z<<"|firstHeight="<<d.firstHeight<<"|heightRadius="<<d.firstRadius<<"|blockEntry="<<d.firstBlock<<"|aggro="<<d.firstAggro<<"|bossRange="<<GmInstanceBoss::StaticAggro(self,*anchor->data);
                e<<"GMUI|BOSSCHECKPATH|type="<<d.firstPathType<<"|length="<<d.firstPathLength<<"|endpointError="<<d.firstPathError<<"|failedSegment="<<d.failedSegment<<"|corner="<<d.corner;
                route<<"GMUI|BOSSCHECKROUTE|source="<<(d.routeTele?"game_tele":"boss_anchor")<<"|id="<<d.routeTele<<"|x="<<d.route.x<<"|y="<<d.route.y<<"|z="<<d.route.z;
                SendSysMessage(route.str().c_str());
                SendSysMessage(a.str().c_str());SendSysMessage(b.str().c_str());SendSysMessage(c.str().c_str());SendSysMessage(e.str().c_str());SendSysMessage("GMUI|DONE|bosscheck");return true;
            }
            bool verifiedSafe=GmInstanceBoss::SafePoint(self,*anchor,spawns,safe);
            if(!verifiedSafe&&!confirmedFallback){
                std::ostringstream warning;warning<<"GMUI|BOSSUNSAFE|id="<<n<<"|map="<<self->GetMapId()<<"|instance="<<self->GetInstanceId()<<"|reason=no_safe_point";
                SendSysMessage(warning.str().c_str());SendSysMessage("GMUI|DONE|boss_unsafe");return true;
            }
            if(!verifiedSafe){auto const& position=anchor->data->position;
                if(position.mapId!=self->GetMapId()||!MaNGOS::IsValidMapCoord(position.x,position.y,position.z,position.o))return error("Boss anchor coordinates are invalid");
                safe=Movement::Vector3{position.x,position.y,position.z};
            }
            self->SaveRecallPosition();
            if(!self->NearTeleportTo(safe.x,safe.y,safe.z,self->GetOrientation()))return error("Current instance teleport rejected");
            return ok(verifiedSafe?"Teleported to verified current-instance Boss staging point":"Teleported to unverified Boss anchor");
        }
        auto category = [](uint32 id, uint32 map) -> std::string {
            if (id == 67 || id == 118 || id == 225 || id == 246 || id == 309 || id == 404) return "city";
            MapEntry const* entry = sMapStorage.LookupEntry<MapEntry>(map);
            if (!entry) return "unknown";
            return entry->IsBattleGround() ? "battleground" : entry->IsRaid() ? "raid" : entry->IsDungeon() ? "dungeon" : "world";
        };
        if (words[1] == "search" || words[1] == "entrances")
        {
            struct Row { uint32 id, map; std::string name, kind, category; };
            std::vector<Row> rows;
            size_t last = words.size(); long page = 1;
            if (last >= 4 && Integer(words.back(),1,10000,page)) --last;
            else page = 1;
            // `entrances 2` is the shorter page form; search accepts a query.
            if (words[1] == "entrances" && words.size() == 3)
            { if (Integer(words[2],1,10000,page)) last = 2; else page = 1; }
            std::string query;
            for (size_t i=2;i<last;++i) { if (!query.empty()) query += ' '; query += words[i]; }
            query=Lower(query);
            if (query == "all" || query == "*" || query == "\"\"") query.clear();
            if (words[1] == "search")
            {
                bool chinese=sObjectMgr.GetLocaleForIndex(m_session->GetSessionDbLocaleIndex())==LOCALE_zhCN;
                for (auto const& pair : sObjectMgr.GetGameTeleMap()) {
                    std::string translated=LocalTele("point",pair.first,pair.second.name);
                    if(query.empty() || Lower(pair.second.name).find(query)!=std::string::npos || Lower(translated).find(query)!=std::string::npos || std::to_string(pair.first)==query)
                        rows.push_back({pair.first,pair.second.mapId,chinese?translated:pair.second.name,"point",category(pair.first,pair.second.mapId)});
                }
            }
            else
            {
                for (auto const& pair : sObjectMgr.GetAreaTriggersMap())
                {
                    AreaTriggerTeleport const* target=sObjectMgr.GetAreaTriggerTeleport(pair.first);
                    if (!target) continue;
                    MapEntry const* entry=sMapStorage.LookupEntry<MapEntry>(target->destination.mapId);
                    if (!entry || !entry->IsDungeon()) continue;
                    std::string name=entry->name ? entry->name : "Instance";
                    std::string translated=LocalTele("map",target->destination.mapId,name);
                    if(!query.empty() && Lower(name).find(query)==std::string::npos && Lower(translated).find(query)==std::string::npos)continue;
                    if(sObjectMgr.GetLocaleForIndex(m_session->GetSessionDbLocaleIndex())==LOCALE_zhCN)name=translated;
                    rows.push_back({pair.first,target->destination.mapId,name,"entrance",entry->IsRaid() ? "raid" : "dungeon"});
                }
            }
            std::sort(rows.begin(),rows.end(),[](Row const& a,Row const& b){ return a.id < b.id; });
            size_t begin=size_t(page-1)*8, finish=std::min(rows.size(),begin+8), shown=0;
            for (size_t i=begin;i<finish;++i)
            {
                std::ostringstream out;
                out << "GMUI|TELE|id=" << rows[i].id << "|name=" << SafeField(rows[i].name) << "|map=" << rows[i].map << "|kind=" << rows[i].kind << "|category=" << rows[i].category;
                SendSysMessage(out.str().c_str()); ++shown;
            }
            std::ostringstream meta; meta << "GMUI|TELEMETA|shown=" << shown << "|more=" << (finish < rows.size() ? 1 : 0) << "|page=" << page << "|total=" << rows.size();
            SendSysMessage(meta.str().c_str()); SendSysMessage("GMUI|DONE|tele"); return true;
        }
        if (!self->IsAlive() || self->IsInCombat() || self->InBattleGround() || self->InBattleGroundQueue() || self->IsBeingTeleported() || self->IsTaxiFlying())
            return error("Teleport requires a living player outside combat, taxi flight, battleground and queue");
        uint32 map=0; float x=0,y=0,z=0,o=self->GetOrientation();
        if (words[1] == "xyz" && (words.size() == 6 || words.size() == 7))
        {
            if (!Coordinate(words[2],x) || !Coordinate(words[3],y) || !Coordinate(words[4],z) || !Integer(words[5],0,65535,n) || (words.size()==7 && !Coordinate(words[6],o)))
                return error("Expected finite x y z map [orientation]");
            map=uint32(n);
        }
        else if (words[1] == "entrance" && words.size() == 3)
        {
            if (!Integer(words[2],1,16777215,n)) return error("Invalid entrance id");
            AreaTriggerTeleport const* target=sObjectMgr.GetAreaTriggerTeleport(uint32(n));
            if (!target) return error("No existing area-trigger teleport for this id");
            MapEntry const* entry=sMapStorage.LookupEntry<MapEntry>(target->destination.mapId);
            if (!entry || !entry->IsDungeon()) return error("Target is not an instance entrance");
            map=target->destination.mapId;x=target->destination.x;y=target->destination.y;z=target->destination.z;o=target->destination.o;
        }
        else
        {
            uint32 id=0;
            if (words[1] == "id" && words.size() == 3)
            { if (!Integer(words[2],1,16777215,n)) return error("Invalid teleport id"); id=uint32(n); }
            else if (words.size()==2)
                id=words[1]=="stormwind" ? 246 : words[1]=="orgrimmar" ? 118 : words[1]=="ironforge" ? 67 : words[1]=="undercity" ? 225 : words[1]=="thunderbluff" ? 309 : words[1]=="darnassus" ? 404 : words[1]=="naxx" ? 270 : 0;
            GameTele const* point=id ? sObjectMgr.GetGameTele(id) : nullptr;
            if (!point) return error("Unknown existing teleport point");
            map=point->mapId;x=point->x;y=point->y;z=point->z;o=point->o;
        }
        MapEntry const* entry=sMapStorage.LookupEntry<MapEntry>(map);
        if (!entry || entry->IsBattleGround()) return error("Invalid destination or battleground map; use the battleground queue");
        if (!MapManager::IsValidMapCoord(map,x,y,z,o)) return error("Destination coordinates are invalid");
        self->SaveRecallPosition();
        if (!self->TeleportTo(map,x,y,z,o)) return error("Native map entry/teleport checks rejected this destination");
        return ok("Teleport accepted to existing map coordinates; instance entry rules still apply");
    }
    if (words[0] == "party" && words.size() >= 2)
    {
        if(words[1]=="raid" && (words.size()==4 || words.size()==5)) {
            if(!Integer(words[3],1,0x7FFFFFFF,n))return error("Invalid raid role or preview token");
            std::string result;bool done=false;
            if(words[2]=="preview"){
                long map=0;if(words.size()==5 && !Integer(words[4],1,65535,map))return error("Invalid raid preset map");
                done=PartyPreset::RaidPreview(self,unsigned(n),result,uint32(map));
            }
            else if(words[2]=="apply")done=PartyPreset::RaidApply(self,uint32(n),result);
            else return error("Expected party raid preview role or party raid apply token");
            if(!done)return error(result.c_str());
            SendSysMessage(("GMUI|OK|"+result).c_str());return true;
        }
        if (words[1] == "specs" && words.size() == 2)
        {
            auto const& specs = sObjectMgr.GetPlayerPremadeSpecTemplates();
            auto const& gears = sObjectMgr.GetPlayerPremadeGearTemplates();
            for (auto const& mapping : PartyPreset::Mappings())
            {
                auto spec = specs.find(mapping.spec); if (spec == specs.end()) continue;
                auto gear = gears.find(mapping.phase6);
                std::string reason; bool supported = PartyPreset::Supported(self, mapping, reason);
                std::ostringstream out;
                out << "GMUI|PARTYSPEC|id=" << mapping.spec << "|class=" << unsigned(spec->second.requiredClass)
                    << "|role=" << unsigned(spec->second.role) << "|level=" << unsigned(spec->second.level)
                    << "|name=" << SafeField(spec->second.name) << "|gear=" << mapping.phase6
                    << "|gearName=" << (gear == gears.end() ? "none" : SafeField(gear->second.name))
                    << "|supported=" << (supported ? 1 : 0) << "|reason=" << reason;
                SendSysMessage(out.str().c_str());
            }
            SendSysMessage("GMUI|DONE|party"); return true;
        }
        if (words[1] == "status" && words.size() == 2)
        { PartyPreset::SendStatus(self); return true; }
        if (words[1] == "remove" && words.size() == 3 && words[2] == "all")
        {
            uint32 requested=PartyPreset::RemoveOwned(self);
            std::string result="Removal requested for "+std::to_string(requested)+" owned party companions; waiting for cleanup.";
            return ok(result.c_str());
        }
        if (words[1] == "add" && words.size() == 3)
        {
            if (!Integer(words[2],1,1000000,n)) return error("Invalid party spec id");
            std::string result;
            if (!PartyPreset::Queue(self,uint32(n),result)) return error(SafeField(result).c_str());
            return ok(SafeField(result).c_str());
        }
        return error("Expected party specs, status, add specId or remove all");
    }
    if (words[0] == "items" && words.size() >= 2 && words[1] == "search")
    {
        std::vector<std::string> lines; std::string message;
        if (!GmItemBrowser::Search(self, words, lines, message)) return error(message.c_str());
        for (auto const& line : lines) SendSysMessage(line.c_str());
        return true;
    }
    if (words[0] == "mail" && words.size() >= 2)
    {
        std::string message; bool sent=false;
        if (words[1] == "set" && words.size() == 4)
            sent=GmItemMail::SendTier(self,words[3],words[2],message);
        else if (words[1] == "item" && words.size() == 5)
        {
            long item=0,amount=0;
            if (!Integer(words[3],1,10000000,item) || !Integer(words[4],1,100000,amount)) return error("Invalid mailed item or quantity");
            sent=GmItemMail::SendItem(self,words[2],uint32(item),uint32(amount),message);
        }
        else return error("Expected mail set tier player or mail item player id count");
        return sent ? ok(message.c_str()) : error(message.c_str());
    }
    if (words[0] == "gear" && words.size() >= 2)
    {
        if (words[1] == "list" && words.size() == 2)
        {
            std::vector<uint32> entries;
            for (auto const& entry : sObjectMgr.GetPlayerPremadeGearTemplates()) if (entry.second.requiredClass == self->GetClass()) entries.push_back(entry.first);
            std::sort(entries.begin(), entries.end());
            for (uint32 id : entries)
            {
                auto const& t = sObjectMgr.GetPlayerPremadeGearTemplates().at(id);
                std::ostringstream out; out << "GMUI|GEAR|id=" << id << "|level=" << unsigned(t.level) << "|role=" << unsigned(t.role) << "|name=" << SafeField(t.name);
                SendSysMessage(out.str().c_str());
            }
            SendSysMessage("GMUI|DONE|gear"); return true;
        }
        if (words[1] == "weapons" && words.size() == 2)
        {
            std::string message;
            if (!BotMatch::EquipWeapons(self, message)) return error(SafeField(message).c_str());
            return ok(SafeField(message).c_str());
        }
        if (words[1] == "pvp" && (words.size() == 3 || words.size() == 4))
        {
            if (!Integer(words[2], 0, 6, count)) return error("PvP set pieces must be 0 to 6");
            uint32 quality = 3;
            if (words.size() == 4) { if (words[3] == "epic") quality = 4; else if (words[3] != "blue") return error("Quality must be blue or epic"); }
            std::string message;
            if (!BotMatch::EquipPvpTestPieces(self, uint32(count), message, quality)) return error(SafeField(message).c_str());
            return ok(SafeField(message).c_str());
        }
        if ((words[1] == "give" || words[1] == "equip") && words.size() == 3)
        {
            if (self->IsInCombat() || self->InBattleGround()) return error("Leave combat/battleground before changing gear");
            if (!Integer(words[2], 1, 1000000, n)) return error("Invalid template id");
            auto const& templates = sObjectMgr.GetPlayerPremadeGearTemplates(); auto it = templates.find(uint32(n));
            if (it == templates.end() || it->second.requiredClass != self->GetClass()) return error("Template not available for this class");
            auto const& t = it->second;
            if (self->GetLevel() < t.level) return error("Player level is below template level; use level action explicitly");
            std::vector<PlayerPremadeItem> items;
            for (auto const& item : t.items) if (!item.requiredTeam || item.requiredTeam == uint32(self->GetTeam())) items.push_back(item);
            if (items.empty()) return error("Template has no items for this faction");
            uint32 requiredSlots = uint32(items.size());
            if (words[1] == "equip")
            {
                uint32 equipped = 0;
                for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i) if (self->GetItemByPos(INVENTORY_SLOT_BAG_0, i)) ++equipped;
                requiredSlots = std::max(requiredSlots, equipped);
            }
            if (FreeGeneralSlots(self) < requiredSlots) return error("Free more general bag slots before applying template");
            for (auto const& item : items)
            {
                ItemPrototype const* proto = sObjectMgr.GetItemPrototype(item.itemId);
                if (!proto) return error("Template references unavailable item");
                if (words[1] == "equip" && self->CanUseItem(proto, true) != EQUIP_ERR_OK) return error("Item requirements not met; level, rank, reputation and skills are not raised automatically");
                ItemPosCountVec dest;
                if (self->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item.itemId, 1) != EQUIP_ERR_OK) return error("Template item cannot be stored (unique item or inventory restriction)");
            }
            // Choose slots through ordinary eligibility, never through the premade
            // applicator (which grants Dual Wield and satisfies item requirements).
            std::vector<uint8> plannedSlots(items.size(), NULL_SLOT);
            std::vector<uint8> occupiedSlots;
            bool plannedTwoHand = false;
            if (words[1] == "equip")
            {
                if (FreeGeneralSlots(self) < items.size() + 1)
                    return error("Free one additional general bag slot for safe weapon swaps");
                for (size_t i = 0; i < items.size(); ++i)
                {
                    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(items[i].itemId);
                    uint8 slots[4];
                    proto->GetAllowedEquipSlots(slots, self->GetClass(), self->CanDualWield());
                    for (uint8 slot : slots)
                    {
                        if (slot >= EQUIPMENT_SLOT_END || std::find(occupiedSlots.begin(), occupiedSlots.end(), slot) != occupiedSlots.end()) continue;
                        if (slot == EQUIPMENT_SLOT_OFFHAND && plannedTwoHand) continue;
                        if (proto->InventoryType == INVTYPE_2HWEAPON && std::find(occupiedSlots.begin(), occupiedSlots.end(), EQUIPMENT_SLOT_OFFHAND) != occupiedSlots.end()) continue;
                        uint16 destination;
                        if (self->CanEquipItem(slot, destination, proto, nullptr, true, true) != EQUIP_ERR_OK) continue;
                        if (Item* old = self->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                            if (self->CanUnequipItem(old->GetPos(), true) != EQUIP_ERR_OK) continue;
                        plannedSlots[i] = slot;
                        occupiedSlots.push_back(slot);
                        if (proto->InventoryType == INVTYPE_2HWEAPON) plannedTwoHand = true;
                        break;
                    }
                }
            }
            std::vector<Item*> granted;
            for (auto const& item : items)
            {
                ItemPosCountVec dest;
                if (self->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item.itemId, 1) != EQUIP_ERR_OK) return error("Inventory changed; remaining items not granted");
                Item* created = self->StoreNewItem(dest, item.itemId, true, Item::GenerateItemRandomPropertyId(item.itemId));
                if (!created) return error("Item creation failed; inspect bag before retrying");
                if (item.enchantId) created->SetEnchantment(PERM_ENCHANTMENT_SLOT, item.enchantId, 0, 0);
                granted.push_back(created);
            }
            if (words[1] == "equip")
            {
                for (size_t i = 0; i < granted.size(); ++i)
                {
                    if (plannedSlots[i] == NULL_SLOT) continue;
                    uint16 destination;
                    if (self->CanEquipItem(plannedSlots[i], destination, granted[i], true, true) == EQUIP_ERR_OK)
                        self->SwapItem(granted[i]->GetPos(), destination);
                }
                uint32 equipped = 0;
                for (size_t i = 0; i < granted.size(); ++i)
                    if (plannedSlots[i] != NULL_SLOT && self->GetItemByPos(INVENTORY_SLOT_BAG_0, plannedSlots[i]) == granted[i]) ++equipped;
                std::string result = "Template granted: " + std::to_string(equipped) + " equipped, " + std::to_string(granted.size() - equipped) + " bag-only (alternatives or unmet equip rules). No level, rank, reputation or skills changed.";
                return ok(result.c_str());
            }
            return ok("Template items placed in bags; existing equipment unchanged");
        }
        return error("Unknown gear action");
    }
    if (words[0] == "bg" && words.size() >= 2)
    {
        if (words[1] == "cancel" && words.size() == 2)
        {
            std::string message; if (!BotMatch::Cancel(self->GetObjectGuid(), message)) return error(SafeField(message).c_str()); return ok(SafeField(message).c_str());
        }
        if (words[1] == "status" && words.size() == 2)
        { SendSysMessage(("GMUI|MATCH|" + BotMatch::Status(self->GetObjectGuid())).substr(0,230).c_str()); for (auto const& line : BotMatch::TeamStatus(self->GetObjectGuid())) SendSysMessage(line.c_str()); SendSysMessage(("GMUI|AUTO|" + BotMatch::AutoStatus(self->GetObjectGuid())).substr(0,230).c_str()); return true; }
        if (words.size() < 3) return error("Battleground key required");
        BattleGroundTypeId bg = words[2] == "warsong" ? BATTLEGROUND_WS : words[2] == "arathi" ? BATTLEGROUND_AB : words[2] == "alterac" ? BATTLEGROUND_AV : BATTLEGROUND_TYPE_NONE;
        if (bg == BATTLEGROUND_TYPE_NONE) return error("Unknown battleground");
        if (words[1] == "open" && words.size() == 3)
        { if (!RegisterPlayerToBG(m_session, bg)) return error("Battleground access rejected; check level or current battleground"); return ok("Choose queue in the battleground window, then enable matching"); }
        if (words[1] == "match" && (words.size() == 4 || words.size() == 6 || words.size() == 8))
        {
            if (!Integer(words[3], 10, 60, n) || n != self->GetLevel()) return error("Match level must equal your current level");
            BotMatch::Options options;
            if (words.size() >= 6)
            {
                if (!Factor(words[4], options.minFactor) || !Factor(words[5], options.maxFactor) || options.minFactor > options.maxFactor) return error("Invalid score factor range");
            }
            if (words.size() == 8)
            {
                long minPieces, maxPieces;
                if (!Integer(words[6],0,6,minPieces) || !Integer(words[7],0,6,maxPieces) || minPieces > maxPieces) return error("Invalid PvP-piece range");
                options.minPvpPieces = int(minPieces); options.maxPvpPieces = int(maxPieces);
            }
            BattleGroundQueueTypeId queue = bg == BATTLEGROUND_WS ? BATTLEGROUND_QUEUE_WS : bg == BATTLEGROUND_AB ? BATTLEGROUND_QUEUE_AB : BATTLEGROUND_QUEUE_AV;
            std::string message; if (!BotMatch::Begin(self, queue, options, message)) return error(SafeField(message).c_str()); return ok(SafeField(message).c_str());
        }
        return error("Unknown battleground action");
    }
    if (words[0] == "toggles" && words.size() == 3 && (words[2] == "on" || words[2] == "off"))
    {
        if (words[1] == "gm") return invoke(&ChatHandler::HandleGMCommand, words[2]);
        if (words[1] == "visible") return invoke(&ChatHandler::HandleGMVisibleCommand, words[2]);
        if (words[1] == "god") return invoke(&ChatHandler::HandleCheatGodCommand, words[2]);
        if (words[1] == "fly") return invoke(&ChatHandler::HandleCheatFlyCommand, words[2]);
    }
    return error("Unknown action or invalid arguments");
}
