#pragma once
#include <cmath>
namespace PartyBotFormationPolicy {
struct Point{float x,y;};
inline Point Around(float x,float y,float angle,float radius){return {x+std::cos(angle)*radius,y+std::sin(angle)*radius};}
inline float Rear(float facing,unsigned guid){return facing+3.14159265f+(int(guid%7)-3)*.12f;}
inline float CastRadius(float minimum,float maximum){return maximum>minimum+4?maximum-2.f:0;}
inline bool Scene(bool dungeon,bool alive,bool group,bool bg,bool duel,bool cast,bool controlled,bool transport){return dungeon&&alive&&group&&!bg&&!duel&&!cast&&!controlled&&!transport;}
inline bool SafePath(unsigned type,float length,float endpointError,bool idleAggro,bool los){return type==1&&length<=65&&endpointError<=2&&!idleAggro&&los;}
inline bool Spaced(float ax,float ay,float az,float bx,float by,float bz,float minimum){float dx=ax-bx,dy=ay-by,dz=az-bz;return dx*dx+dy*dy+dz*dz>=minimum*minimum;}
inline float SegmentDistance(float px,float py,float pz,float ax,float ay,float az,float bx,float by,float bz){float dx=bx-ax,dy=by-ay,dz=bz-az,length=dx*dx+dy*dy+dz*dz;float t=length>0?((px-ax)*dx+(py-ay)*dy+(pz-az)*dz)/length:0;t=t<0?0:t>1?1:t;dx=px-(ax+t*dx);dy=py-(ay+t*dy);dz=pz-(az+t*dz);return std::sqrt(dx*dx+dy*dy+dz*dz);}
inline bool Move(bool tooClose,bool tooFar,bool los,bool crowded){return tooClose||tooFar||!los||crowded;}
}
