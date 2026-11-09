#ifndef PARTY_BOT_SERVICE_POLICY_H
#define PARTY_BOT_SERVICE_POLICY_H
#include <cstdint>
namespace PartyBotServicePolicy {
inline bool NeedsWater(unsigned cls){return cls!=1&&cls!=4;}
inline uint32_t WeaponEnchant(unsigned cls,unsigned role,bool twoHand,bool alreadyEnchanted){
    if(alreadyEnchanted)return 0;
    if(role==4)return 22750;
    if(cls==8||cls==9||(role==2&&(cls==5||cls==11||cls==7)))return 22749;
    if(cls==4||cls==3)return twoHand?27837:23800;
    if(cls==1)return 20034;
    return 0;
}
}
#endif
