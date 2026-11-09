#ifndef WORLD_BOT_ROUTE_RECOVERY_H
#define WORLD_BOT_ROUTE_RECOVERY_H
#include <array>
#include <cstdint>
#include <cmath>
namespace WorldBotRouteRecovery {
using Point=std::array<float,3>;
inline bool Changed(Point const& a,Point const& b,float distance){float sum=0;for(unsigned i=0;i<3;++i){if(!std::isfinite(a[i])||!std::isfinite(b[i]))return true;float d=a[i]-b[i];sum+=d*d;}return sum>distance*distance;}
struct Stall {
 bool tracking=false,attempted=false;std::uint32_t map=0,since=0,lastFailure=0,lastAttempt=0;Point goal{},position{};
 void Clear(){tracking=false;since=lastFailure=0;}
 void Progress(std::uint32_t currentMap,Point const& here){if(tracking&&(map!=currentMap||Changed(position,here,3)))Clear();}
 void Failed(std::uint32_t now,std::uint32_t currentMap,Point const& target,Point const& here){
  if(!tracking||map!=currentMap||Changed(goal,target,1)||Changed(position,here,3)){tracking=true;map=currentMap;since=now;goal=target;position=here;}
  lastFailure=now;
 }
 bool Due(std::uint32_t now)const{return tracking&&std::uint32_t(now-since)>=60000&&std::uint32_t(now-lastFailure)<=5000&&(!attempted||std::uint32_t(now-lastAttempt)>=60000);}
 void Attempt(std::uint32_t now,bool accepted){attempted=true;lastAttempt=now;if(accepted)Clear();}
};
struct Safety {bool freeScene=true,freeGroup=true,alive=true,combat=false,duel=false,bg=false,queue=false,instance=false,control=false,casting=false,water=false,teleport=false,taxi=false,transport=false,trade=false,victim=false;};
inline bool Safe(Safety const& s){return s.freeScene&&s.freeGroup&&s.alive&&!s.combat&&!s.duel&&!s.bg&&!s.queue&&!s.instance&&!s.control&&!s.casting&&!s.water&&!s.teleport&&!s.taxi&&!s.transport&&!s.trade&&!s.victim;}
}
#endif
