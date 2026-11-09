#ifndef BOT_LOOT33_H
#define BOT_LOOT33_H
#include "BotLoot33Policy.h"
#include "Player.h"
#include "PlayerBotAI.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Item.h"
#include "Config/Config.h"
#include "DBCStores.h"
#include "DBCEnums.h"
namespace BotLoot33 {
inline bool Enabled(){return sConfig.GetBoolDefault("BotLoot33.Enabled",false);}
inline bool Retained(Player* p){auto ai=p&&p->IsBot()?dynamic_cast<PlayerBotAI*>(p->AI()):nullptr;return ai&&ai->AllowsPersistentSave();}
inline bool Participant(Player* p){return Enabled()&&Retained(p);}
inline BotLoot33Policy::Decision Decide(Player* player,ItemPrototype const* item,unsigned count=1,int32 randomProperty=0){
 if(!player||!item)return {BotLoot33Policy::Vote::Pass,"missing_player_or_item"};
 BotLoot33Policy::Input in;in.cls=player->GetClass();in.itemClass=item->Class;in.subclass=item->SubClass;in.inventory=item->InventoryType;in.itemLevel=item->ItemLevel;
 in.dominantArmor=BotLoot33Policy::DominantArmor(in.cls);in.retained=Retained(player);in.usable=player->CanUseItem(item)==EQUIP_ERR_OK;
 ItemPosCountVec destinations;in.capacity=player->CanStoreNewItem(NULL_BAG,NULL_SLOT,destinations,item->ItemId,count)==EQUIP_ERR_OK;
 if(!in.retained||!in.capacity)return BotLoot33Policy::Decide(in);
 for(auto const& stat:item->ItemStat)if(stat.ItemStatValue>0){
  if(stat.ItemStatType==ITEM_MOD_INTELLECT)in.intellect=true;
  if(stat.ItemStatType==ITEM_MOD_STRENGTH||stat.ItemStatType==ITEM_MOD_AGILITY)in.physicalStats=true;
 }
 auto inspectSpell=[&](uint32 id){
  if(auto spell=sSpellMgr.GetSpellEntry(id))for(unsigned i=0;i<3;++i){
   if(spell->Effect[i]!=SPELL_EFFECT_APPLY_AURA||spell->EffectBasePoints[i]<0)continue;
   if(spell->EffectApplyAuraName[i]==SPELL_AURA_MOD_ATTACK_POWER||spell->EffectApplyAuraName[i]==SPELL_AURA_MOD_RANGED_ATTACK_POWER)in.physicalStats=true;
   if(spell->EffectApplyAuraName[i]==SPELL_AURA_MOD_HEALING_DONE)in.spellPower=true;
   if(spell->EffectApplyAuraName[i]==SPELL_AURA_MOD_DAMAGE_DONE&&(spell->EffectMiscValue[i]&0x7e))in.spellPower=true;
  }
 };
 for(auto const& effect:item->Spells)if(effect.SpellTrigger==ITEM_SPELLTRIGGER_ON_EQUIP)inspectSpell(effect.SpellId);
 if(randomProperty){
  auto prop=randomProperty>0?sItemRandomPropertiesStore.LookupEntry(randomProperty):nullptr;
  if(!prop)return {BotLoot33Policy::Vote::Greed,"unknown_random_property"};
  for(uint32 id:prop->enchant_id)if(auto enchant=sSpellItemEnchantmentStore.LookupEntry(id))for(unsigned i=0;i<3;++i){
   if(enchant->type[i]==ITEM_ENCHANTMENT_TYPE_STAT&&enchant->amount[i]>0){
    if(enchant->spellid[i]==ITEM_MOD_INTELLECT)in.intellect=true;
    if(enchant->spellid[i]==ITEM_MOD_STRENGTH||enchant->spellid[i]==ITEM_MOD_AGILITY)in.physicalStats=true;
   }
   if(enchant->type[i]==ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL)inspectSpell(enchant->spellid[i]);
  }
 }
 return BotLoot33Policy::Decide(in);
}
}
#endif
