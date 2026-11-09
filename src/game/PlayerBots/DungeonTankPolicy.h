#ifndef DUNGEON_TANK_POLICY_H
#define DUNGEON_TANK_POLICY_H
#include <cstdint>
#include <cmath>
namespace DungeonTankPolicy {
inline bool AoeReady(unsigned held,bool unsafe,bool threatSafe){return held>=3&&!unsafe&&threatSafe;}
inline bool Assistable(bool alive,bool engaged,bool groupVictim,bool legal,bool near,bool los){return alive&&engaged&&groupVictim&&legal&&near&&los;}
struct Candidate {
 std::uint64_t guid=0;
 bool alive=false,engaged=false,groupVictim=false,heldByTank=false,controlled=false,legal=false,reachable=false,healerVictim=false;
 float distance=0,victimHealth=100;
 std::uint32_t servedAt=0;
};
inline bool Eligible(Candidate const& c){return c.guid&&c.alive&&c.engaged&&c.groupVictim&&!c.heldByTank&&!c.controlled&&c.legal&&c.reachable&&std::isfinite(c.distance)&&std::isfinite(c.victimHealth)&&c.distance>=0&&c.distance<=40;}
inline unsigned Tier(Candidate const& c){return c.victimHealth<35?0:c.healerVictim?1:2;}
inline bool KeepCommitment(Candidate const& current,Candidate const& next,bool active){return active&&Eligible(current)&&Tier(current)<=Tier(next);}
inline bool Better(Candidate const& a,Candidate const& b,std::uint32_t now){
 if(Tier(a)!=Tier(b))return Tier(a)<Tier(b);
 if(bool(a.servedAt)!=bool(b.servedAt))return !a.servedAt;
 if(a.servedAt&&b.servedAt&&a.servedAt!=b.servedAt)return std::uint32_t(now-a.servedAt)>std::uint32_t(now-b.servedAt);
 if(a.distance!=b.distance)return a.distance<b.distance;
 return a.guid<b.guid;
}
}
#endif
