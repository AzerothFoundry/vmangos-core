#ifndef PARTY_BOT_LOOT_POLICY_H
#define PARTY_BOT_LOOT_POLICY_H
#include <cmath>
namespace PartyBotLootPolicy {
inline bool Candidate(bool dead,bool tapped,bool permitted,bool unlooted,bool action,float corpseDistance,float leaderDistance){return dead&&tapped&&permitted&&unlooted&&action&&std::isfinite(corpseDistance)&&std::isfinite(leaderDistance)&&corpseDistance<=25&&leaderDistance<=40;}
inline bool TakeMoney(bool opened,unsigned money){return opened&&money>0;}
inline bool TakeItem(bool persistent,bool allowed,bool blocked,bool space){return persistent&&allowed&&!blocked&&space;}
}
#endif
