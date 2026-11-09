// Trade-only conveniences for temporary, owned party companions. No real-player writes.
#include "PartyBotServices.h"
#include "PartyBotServicePolicy.h"
#include "PartyBotAI.h"
#include "PartyBotPreset.h"
#include "PlayerBotMgr.h"
#include "TradeData.h"
#include "Player.h"
#include "Item.h"
#include "Bag.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Packets/Trade.h"
#include "Timer.h"
#include "Config/Config.h"
#include <algorithm>
#include <cstdlib>
#include <string>
namespace PartyBotServices {
namespace {
struct Enchant {uint32 id;char const* text;};
Enchant const enchants[]={
 {22749,"武器：法术伤害 +30"},{22750,"武器：治疗效果 +55"},{20034,"武器：十字军（触发治疗75–125、力量+100/15秒）"},
 {23800,"武器：敏捷 +15"},{27837,"双手武器：敏捷 +25"},{23799,"武器：力量 +15"},{23804,"武器：智力 +22"},{23803,"武器：精神 +20"},{20031,"武器：伤害 +5"},{20030,"双手武器：伤害 +9"},{20029,"武器：冰寒（触发减速）"},{20032,"武器：生命偷取"},{20033,"武器：邪恶（触发降低近战伤害）"},{21931,"武器：冰霜伤害 +7"},
 {20025,"胸甲：所有属性 +4"},{20026,"胸甲：生命值 +100"},{20028,"胸甲：法力值 +100"},
 {23802,"护腕：治疗效果 +24"},{23801,"护腕：每5秒回蓝 +4"},{20008,"护腕：智力 +7"},{20009,"护腕：精神 +9"},{20010,"护腕：力量 +9"},{20011,"护腕：耐力 +9"},
 {25072,"手套：威胁 +2%"},{25073,"手套：暗影伤害 +20"},{25074,"手套：冰霜伤害 +20"},{25078,"手套：火焰伤害 +20"},{25079,"手套：治疗效果 +30"},{25080,"手套：敏捷 +15"},{20013,"手套：力量 +7"},{13948,"手套：攻击速度 +1%"},{13841,"手套：采矿 +5"},{13868,"手套：草药 +5"},
 {13890,"靴子：初级移动速度"},{20020,"靴子：耐力 +7"},{20023,"靴子：敏捷 +7"},{20024,"靴子：精神 +5"},
 {25086,"披风：躲闪 +1%"},{25084,"披风：威胁 -2%"},{25083,"披风：潜行"},{25081,"披风：火抗 +15"},{25082,"披风：自然抗 +15"},{20014,"披风：所有魔法抗性 +5"},{20015,"披风：护甲 +70"},{13882,"披风：敏捷 +3"},
 {20016,"盾牌：精神 +9"},{20017,"盾牌：耐力 +7"},{13933,"盾牌：冰抗 +8"}
};
uint32 const foodSpells[]={587,597,990,6129,10144,10145,28612};
uint32 const waterSpells[]={5504,5505,5506,6127,10138,10139,10140};
void Tell(PartyBotAI& ai,Player* player,std::string const& text){
    if(!player || player->IsBot() || !player->GetSession())return;
    WorldPacket packet;ChatHandler::BuildChatPacket(packet,CHAT_MSG_WHISPER,text.c_str(),LANG_UNIVERSAL,CHAT_TAG_NONE,ai.me->GetObjectGuid(),ai.me->GetName());player->GetSession()->SendPacket(&packet);
}
bool Eligible(PartyBotAI const& ai){return ai.me && ai.m_initialized && ai.m_presetSpecId && ai.me->GetLevel()==60 && ai.m_race && ai.m_class && ai.me->IsBot() && (ai.me->GetClass()==CLASS_MAGE || ai.me->GetClass()==CLASS_ROGUE);}
Item* Stack(Player* p,uint32 id,uint32 count){
    for(uint8 slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)if(Item* item=p->GetItemByPos(INVENTORY_SLOT_BAG_0,slot))if(item->GetEntry()==id&&item->GetCount()>=count)return item;
    for(uint8 bag=INVENTORY_SLOT_BAG_START;bag<INVENTORY_SLOT_BAG_END;++bag)if(Bag* b=dynamic_cast<Bag*>(p->GetItemByPos(INVENTORY_SLOT_BAG_0,bag)))for(uint32 slot=0;slot<b->GetBagSize();++slot)if(Item* item=b->GetItemByPos(slot))if(item->GetEntry()==id&&item->GetCount()>=count)return item;
    return nullptr;
}
bool WaterClass(uint8 cls){return PartyBotServicePolicy::NeedsWater(cls);}
bool Gift(PartyBotAI& ai,Player* customer,bool water){
    TradeData* trade=ai.me->GetTradeData();uint8 slot=water?1:0;
    if(trade->GetItem(TradeSlots(slot)))return true;
    uint32 sid=0,itemId=0,required=0;
    auto inspect=[&](uint32 candidate){
        SpellEntry const* s=sSpellMgr.GetSpellEntry(candidate);if(!s||!ai.me->HasSpell(candidate))return;
        for(unsigned i=0;i<MAX_SPELL_EFFECTS;++i)if(s->Effect[i]==SPELL_EFFECT_CREATE_ITEM){ItemPrototype const* item=sObjectMgr.GetItemPrototype(s->EffectItemType[i]);if(item&&item->RequiredLevel<=customer->GetLevel()&&(!sid||item->RequiredLevel>=required)){sid=candidate;itemId=item->ItemId;required=item->RequiredLevel;}}
    };
    if(water)for(auto id:waterSpells)inspect(id);else for(auto id:foodSpells)inspect(id);
    if(!sid)return true;
    if(Item* item=Stack(ai.me,itemId,20)){trade->SetItem(TradeSlots(slot),item);return true;}
    SpellEntry const* spell=sSpellMgr.GetSpellEntry(sid);
    if(ai.CanTryToCastSpell(ai.me,spell)){
        ai.me->RemoveAurasDueToSpell(1131);ai.me->RemoveAurasDueToSpell(1137);
        ai.me->SetStandState(UNIT_STAND_STATE_STAND);ai.DoCastSpell(ai.me,spell);
    }else{
        if(!ai.me->HasAura(1137) && ai.me->GetPower(POWER_MANA)<Spell::CalculatePowerCost(spell,ai.me))Tell(ai,customer,"我正在恢复法力制作食水，请稍候；准备好后会放进交易窗口。");
        ai.DrinkAndEat();
    }
    return false;
}
bool Fits(Item* item,uint32 id){
    SpellEntry const* s=sSpellMgr.GetSpellEntry(id);
    if(!s || !item || !item->IsFitToSpellRequirements(s) || item->GetProto()->ItemLevel<s->baseLevel || s->HasAttribute(SPELL_ATTR_EX2_ENCHANT_OWN_ITEM_ONLY))return false;
    for(unsigned i=0;i<MAX_SPELL_EFFECTS;++i)if(s->Effect[i]==SPELL_EFFECT_ENCHANT_ITEM && s->EffectMiscValue[i])return true;
    return false;
}
std::string Label(uint32 id){for(auto const& e:enchants)if(e.id==id)return e.text;return "开锁";}
void Menu(PartyBotAI& ai,Player* player){
    auto& state=ai.m_service;state.menu=true;state.selected=0;state.ready=false;state.choiceTime=WorldTimer::getMSTime();
    ai.me->GetTradeData()->SetSpell(0);
    Tell(ai,player,"此部位有多种用途，请私聊回复编号（60秒有效；换物品需重选）。装备留在非交易栏。回复“取消”清除选择。");
    unsigned n=0;for(auto id:state.choices)Tell(ai,player,std::to_string(++n)+". "+Label(id));
}
bool Offer(PartyBotAI& ai,Player* player,uint32 id){
    auto& state=ai.m_service;TradeData* trade=ai.me->GetTradeData();Item* item=trade?trade->GetTraderData()->GetItem(TRADE_SLOT_NONTRADED):nullptr;
    if(!item || item->GetGUID()!=state.item || (id!=1804&&!Fits(item,id)))return false;
    SpellEntry const* s=sSpellMgr.GetSpellEntry(id);if(!s || !ai.me->HasSpell(id))return false;
    if(sConfig.GetBoolDefault("PartyBot.ServiceFreeMaterials",true))for(unsigned i=0;i<MAX_SPELL_REAGENTS;++i)if(s->Reagent[i]>0 && s->ReagentCount[i]){
        uint32 have=ai.me->GetItemCount(uint32(s->Reagent[i]),false);if(have<s->ReagentCount[i])ai.AddItemToInventory(uint32(s->Reagent[i]),s->ReagentCount[i]-have);
    }
    // Crafting tools are checked independently from consumed reagents by CheckItems.
    for(uint32 tool:s->Totem)if(tool && !ai.me->HasItemCount(tool,1))ai.AddItemToInventory(tool,1);
    // Match the native trade handler's spell validation; it queues the spell in TradeData.
    Spell* check=new Spell(ai.me,s,false);SpellCastTargets targets;targets.setTradeItemTarget(ai.me);check->m_targets=targets;
    SpellCastResult result=check->CheckCast(true);check->Delete();
    if((result!=SPELL_CAST_OK && result!=SPELL_FAILED_DONT_REPORT)||trade->GetSpell()!=id){
        trade->SetSpell(0);Tell(ai,player,"无法提供该服务：原生施法检查未通过（"+std::to_string(unsigned(result))+"）。请核对物品部位、材料、熟练度及等级。");return false;
    }
    state.selected=id;state.menu=false;state.ready=true;
    Tell(ai,player,"已在交易窗口提供："+Label(id)+"。请核对后点击交易；装备不转交。私聊“附魔”可重新选择。");return true;
}
uint32 Recommend(Player* p,Item* item,std::vector<uint32> const& choices){
    auto has=[&](uint32 id){return std::find(choices.begin(),choices.end(),id)!=choices.end();};
    if(item->GetProto()->Class!=ITEM_CLASS_WEAPON || item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT))return 0;
    unsigned role=PartyPreset::PreferredRole(p);
    uint32 best=PartyBotServicePolicy::WeaponEnchant(p->GetClass(),role,has(27837),false);
    return has(best)?best:0;
}
}
void Initialize(PartyBotAI& ai){
    if(!ai.me || !ai.m_presetSpecId || ai.me->GetLevel()!=60 || !ai.m_race || !ai.m_class || !ai.me->IsBot())return;
    if(ai.me->GetClass()==CLASS_MAGE){
        ai.me->SetSkill(SKILL_ENCHANTING,300,300,4);
        for(auto const& e:enchants)if(sSpellMgr.GetSpellEntry(e.id))ai.me->LearnSpell(e.id,false);
        for(auto id:foodSpells)if(SpellEntry const* s=sSpellMgr.GetSpellEntry(id))if(s->spellLevel<=ai.me->GetLevel())ai.me->LearnSpell(id,false);
        for(auto id:waterSpells)if(SpellEntry const* s=sSpellMgr.GetSpellEntry(id))if(s->spellLevel<=ai.me->GetLevel())ai.me->LearnSpell(id,false);
    }else if(ai.me->GetClass()==CLASS_ROGUE){ai.me->LearnSpell(1804,false);ai.me->SetSkill(SKILL_LOCKPICKING,ai.me->GetLevel()*5,ai.me->GetLevel()*5);ai.AddItemToInventory(5060);}
}
bool Update(PartyBotAI& ai){
    if(!Eligible(ai))return false;
    auto& state=ai.m_service;TradeData* trade=ai.me->GetTradeData();
    if(!trade){state={};return false;}
    Player* customer=trade->GetTrader();
    if(!customer || customer->IsBot() || customer->GetGroup()!=ai.me->GetGroup() || customer->GetMap()!=ai.me->GetMap())return false;
    if(state.trader!=customer->GetGUID()){state={};state.trader=customer->GetGUID();}
    state.ready=false;
    if(!ai.me->IsAlive()||ai.me->IsInCombat()||customer->IsInCombat())return true;
    if(ai.me->IsNonMeleeSpellCasted())return true;
    ai.me->StopMoving();
    if(ai.me->GetClass()==CLASS_MAGE){
        if(!state.food){if(!Gift(ai,customer,false))return true;state.food=true;}
        if(WaterClass(customer->GetClass())&&!state.water){if(!Gift(ai,customer,true))return true;state.water=true;}
        if(!state.informed){Tell(ai,customer,WaterClass(customer->GetClass())?"已准备适合你等级的面包和水。装备放非交易栏可附魔。":"已准备适合你等级的面包；你的职业不用法力，不给水。装备放非交易栏可附魔。");state.informed=true;}
    }
    Item* item=trade->GetTraderData()->GetItem(TRADE_SLOT_NONTRADED);
    if(!item){state.item=0;state.selected=0;state.menu=false;state.choices.clear();state.ready=true;}
    else if(item->GetGUID()!=state.item){
        state.item=item->GetGUID();state.selected=0;state.menu=false;state.choices.clear();
        if(ai.me->GetClass()==CLASS_ROGUE){
            if(!item->GetProto()->LockID || item->HasFlag(ITEM_FIELD_FLAGS,ITEM_DYNFLAG_UNLOCKED)){Tell(ai,customer,"这个物品没有需要开启的锁。请把锁箱放在非交易栏。");return true;}
            if(!Offer(ai,customer,1804))return true;
        }else{
            for(auto const& e:enchants)if(Fits(item,e.id))state.choices.push_back(e.id);
            if(Fits(item,27837))state.choices.erase(std::remove(state.choices.begin(),state.choices.end(),23800),state.choices.end());
            if(state.choices.empty()){Tell(ai,customer,"此部位没有本服务支持的1.12.1常规可交易附魔。头腿/肩膀任务或声望附魔、戒指腰带等不会伪造处理。");return true;}
            uint32 recommended=Recommend(customer,item,state.choices);
            if(!recommended || !Offer(ai,customer,recommended))Menu(ai,customer);
        }
    }else if(state.selected && trade->GetSpell()==state.selected)state.ready=true;
    if(state.menu && uint32(WorldTimer::getMSTime()-state.choiceTime)>60000){state.menu=false;Tell(ai,customer,"附魔编号已过期；私聊“附魔”重新查看。换物品也会重新生成选项。");}
    if(state.ready && trade->GetTraderData()->IsAccepted()&&!trade->IsAccepted()){
        auto packet=std::make_unique<WorldPackets::Trade::AcceptTrade>();ai.me->GetSession()->QueuePacket(std::move(packet));
    }
    return true;
}
bool Packet(PartyBotAI& ai,WorldPacket const& packet){
    if(!Eligible(ai))return false;
    if(packet.GetOpcode()==SMSG_TRADE_STATUS && packet.size()>=4){
        WorldPacket copy(packet);uint32 status;copy>>status;
        if(status==TRADE_STATUS_BEGIN_TRADE)ai.m_service={};
        if(status==TRADE_STATUS_TRADE_ACCEPT)return Update(ai);
        return false;
    }
    if(packet.GetOpcode()!=SMSG_MESSAGECHAT || packet.size()<18 || !ai.me->GetTradeData())return false;
    WorldPacket copy(packet);uint8 type;uint32 language,length;ObjectGuid sender;copy>>type>>language>>sender>>length;
    if(type!=CHAT_MSG_WHISPER || language==LANG_ADDON || !length || length>256 || copy.rpos()+length>copy.size())return false;
    Player* customer=ai.me->GetTradeData()->GetTrader();if(!customer || customer->GetObjectGuid()!=sender || customer->IsBot() || customer->GetGroup()!=ai.me->GetGroup())return false;
    std::string text;copy>>text;
    auto& state=ai.m_service;
    if(text=="附魔" && !state.choices.empty()){Menu(ai,customer);return true;}
    if(text=="取消"){ai.me->GetTradeData()->SetSpell(0);state.selected=0;state.menu=false;state.ready=false;Tell(ai,customer,"已取消附魔选择；可关闭交易或重新私聊“附魔”。");return true;}
    if(!state.menu || uint32(WorldTimer::getMSTime()-state.choiceTime)>60000)return false;
    if(text.empty()||text.find_first_not_of("0123456789")!=std::string::npos)return false;
    unsigned long selected=std::strtoul(text.c_str(),nullptr,10);
    if(!selected || selected>state.choices.size()){Tell(ai,customer,"编号无效，请回复列表中的数字。");return true;}
    Offer(ai,customer,state.choices[selected-1]);return true;
}
bool Whisper(Player* sender,std::string const& target,std::string const& message){
    if(!sender || !sender->GetTradeData() || !sender->GetGroup() || sender->IsBot())return false;
    Player* partner=sender->GetTradeData()->GetTrader();
    if(!partner || !partner->IsBot() || target!=partner->GetName() || partner->GetGroup()!=sender->GetGroup() || partner->GetMap()!=sender->GetMap() || partner->GetTeam()!=sender->GetTeam())return false;
    for(auto const* entry:sPlayerBotMgr.GetOwnedPartyBots(sender->GetGroup()->GetLeaderGuid())){
        if(!entry->m_initialized || entry->me!=partner || !Eligible(*entry))continue;
        PartyBotAI& ai=*const_cast<PartyBotAI*>(entry);
        WorldPacket input;ChatHandler::BuildChatPacket(input,CHAT_MSG_WHISPER,message.c_str(),LANG_UNIVERSAL,CHAT_TAG_NONE,sender->GetObjectGuid(),sender->GetName());
        if(!Packet(ai,input))Tell(ai,sender,"请回复当前附魔列表的数字；可回复“附魔”查看选项，或“取消”清除选择。");
        WorldPacket echo;ChatHandler::BuildChatPacket(echo,CHAT_MSG_WHISPER_INFORM,message.c_str(),LANG_UNIVERSAL,CHAT_TAG_NONE,partner->GetObjectGuid(),partner->GetName());sender->GetSession()->SendPacket(&echo);
        return true;
    }
    return false;
}

}
