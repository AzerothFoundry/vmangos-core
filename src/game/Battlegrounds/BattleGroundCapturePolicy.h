#ifndef MANGOS_BATTLEGROUND_CAPTURE_POLICY_H
#define MANGOS_BATTLEGROUND_CAPTURE_POLICY_H
#include <cstdint>
namespace BattleGroundCapturePolicy {
inline bool ArathiBanner(unsigned node,unsigned state,std::uint32_t entry,std::uint32_t type,bool locked,bool spawned)
{
    static std::uint32_t const entries[5]={0,180059,180061,180058,180060};
    return node<5&&state<5&&locked&&spawned&&type==(state?1u:10u)&&entry==(state?entries[state]:180087+node);
}
inline bool RejectDirectArathiUse(bool ownsEvent,unsigned node,unsigned state,std::uint32_t entry,std::uint32_t type,bool locked,bool spawned)
{
    // Neutral GOOBER Use/23932 is a legitimate native compatibility path.
    return ownsEvent&&state>=1&&state<=4&&ArathiBanner(node,state,entry,type,locked,spawned);
}
}
#endif
