#ifndef BOT_LOOT33_POLICY_H
#define BOT_LOOT33_POLICY_H
namespace BotLoot33Policy {
enum class Vote { Pass,Need,Greed };
struct Input {unsigned cls=0,itemClass=0,subclass=0,inventory=0,itemLevel=0,dominantArmor=0;bool usable=false,intellect=false,spellPower=false,physicalStats=false,capacity=false,retained=false;};
struct Decision {Vote vote;char const* reason;};
inline bool ArmorSlot(unsigned slot){return slot==1||slot==3||slot==5||slot==6||slot==7||slot==8||slot==9||slot==10||slot==20;}
inline unsigned DominantArmor(unsigned cls){switch(cls){case 1:case 2:return 4;case 3:case 7:return 3;case 4:case 11:return 2;case 5:case 8:case 9:return 1;default:return 0;}}
inline Decision Decide(Input const& i){
 if(!i.retained)return {Vote::Pass,"temporary_inventory_not_retained"};
 if(!i.capacity)return {Vote::Pass,"inventory_full_or_unique_limit"};
 if(!i.usable)return {Vote::Greed,"native_item_use_restriction"};
 if(!DominantArmor(i.cls))return {Vote::Greed,"unsupported_class"};
 if(i.itemClass==4&&ArmorSlot(i.inventory)&&i.itemLevel>70&&i.subclass!=i.dominantArmor)return {Vote::Greed,"dominant_armor_above_ilvl70"};
 bool equipment=i.itemClass==2||i.itemClass==4;
 if(equipment&&(i.cls==1||i.cls==4)&&(i.intellect||i.spellPower))return {Vote::Greed,"pure_physical_caster_stats"};
 if(equipment&&(i.cls==5||i.cls==8||i.cls==9)&&i.physicalStats&&!i.intellect&&!i.spellPower)return {Vote::Greed,"pure_caster_physical_stats"};
 if(equipment&&i.cls==3&&i.spellPower)return {Vote::Greed,"hunter_spell_power"};
 return {Vote::Need,"class_eligible"};
}
}
#endif
