#ifndef MANGOS_BATTLEBOT_WAYPOINT_POLICY_H
#define MANGOS_BATTLEBOT_WAYPOINT_POLICY_H
#include <cstdint>
#include <cstddef>
#include <limits>
namespace BattleBotWaypointPolicy {
struct Ticket {std::uint32_t serial=0,pending=0,index=0;std::uintptr_t path=0;};
inline bool IsTicket(std::uint32_t id){return (id&0xf0000000u)==0xd0000000u;}
inline void Invalidate(Ticket& t){t.pending=0;t.path=0;}
inline std::uint32_t Arm(Ticket& t,std::uintptr_t path,std::uint32_t index){++t.serial;t.pending=0xd0000000u|(t.serial&0x0fffffffu);t.path=path;t.index=index;return t.pending;}
inline bool Accept(Ticket& t,std::uint32_t id,std::uintptr_t path,std::size_t size,std::uint32_t current){
 if(!t.pending||id!=t.pending)return false;
 bool valid=t.path==path&&t.index==current&&t.index<size;
 Invalidate(t);return valid;
}
inline bool Next(std::size_t size,std::uint32_t current,bool reverse,std::uint32_t& next){
 if(!size||size>std::numeric_limits<std::uint32_t>::max())return false;
 if(reverse){if(current==size){next=std::uint32_t(size-1);return true;}if(current>=size||current==0)return false;next=current-1;return true;}
 if(current==std::numeric_limits<std::uint32_t>::max()){next=0;return true;}
 if(current>=size||current+1>=size)return false;next=current+1;return true;
}
}
#endif
