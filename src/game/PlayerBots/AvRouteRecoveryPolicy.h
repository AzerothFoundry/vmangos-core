#pragma once
#include <cstdint>
namespace AvRouteRecoveryPolicy {
// Tick only for an existing AV route. External movement and protected actions
// reset observation; three seconds of lost native movement are required.
inline bool Observe(bool stopped,bool owned,bool protectedAction,std::uint32_t now,std::uint32_t& since){
 if(!stopped||!owned||protectedAction){since=0;return false;}
 if(!since){since=now;return false;}
 if(std::uint32_t(now-since)<3000)return false;
 since=0;return true;
}
}
