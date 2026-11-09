#ifndef WORLD_BOT_SUPPLY_POLICY_H
#define WORLD_BOT_SUPPLY_POLICY_H
namespace WorldBotSupplyPolicy {
inline bool Empty(bool mana,unsigned food,unsigned water){return !food||(mana&&!water);}
inline bool Stocked(bool mana,unsigned food,unsigned water,unsigned foodStack,unsigned waterStack){return foodStack&&food>=foodStack&&(!mana||(waterStack&&water>=waterStack));}
inline bool Active(bool pending,bool mana,unsigned food,unsigned water,unsigned foodStack,unsigned waterStack){return pending?!Stocked(mana,food,water,foodStack,waterStack):Empty(mana,food,water);}
}
#endif
