#ifndef WORLD_PVP_FLAVOR_POLICY_H
#define WORLD_PVP_FLAVOR_POLICY_H
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace WorldPvpFlavor {
struct Config {bool enabled=true,valid=true;double chance=.15;uint32_t intervalMs=45000,minLevel=10,maxGap=3;float radius=40,minHealth=80,minMana=60,leash=60;uint32_t fightMs=35000;bool routeVariation=true;};
struct Readiness {bool world=false,alive=false,combat=false,duel=false,group=false,service=false,protectedArea=false,gm=false,movingTransport=false;unsigned level=0;float health=0,mana=0;bool usesMana=false;};
inline bool Ready(Config const& c,Readiness const& s){return c.enabled&&s.world&&s.alive&&!s.combat&&!s.duel&&!s.group&&!s.service&&!s.protectedArea&&!s.gm&&!s.movingTransport&&s.level>=c.minLevel&&std::isfinite(s.health)&&s.health>=c.minHealth&&(!s.usesMana||(std::isfinite(s.mana)&&s.mana>=c.minMana));}
inline bool Target(Config const& c,unsigned ownLevel,unsigned level,float health,float distance,bool lawful,bool protectedTarget){return lawful&&!protectedTarget&&level>=c.minLevel&&std::abs(int(level)-int(ownLevel))<=int(c.maxGap)&&std::isfinite(health)&&health>=c.minHealth&&std::isfinite(distance)&&distance<=c.radius;}
inline bool Chance(double value,double chance){return std::isfinite(value)&&std::isfinite(chance)&&chance>0&&value>=0&&value<=1&&value<=chance;}
inline float Setting(float value,float fallback,float min,float max,bool& valid){if(!std::isfinite(value)){valid=false;return fallback;}return std::max(min,std::min(max,value));}
struct Clock {uint32_t next=0,period=0;bool armed=false;uint64_t checks=0,rolls=0;
 bool Due(uint32_t now,uint32_t interval){if(!armed||period!=interval){armed=true;period=interval;next=now+interval;return false;}if(int32_t(now-next)<0)return false;next=now+interval;++checks;return true;}
};
inline bool WithinLeash(bool sameMap,float own,float enemy,uint32_t elapsed,Config const& c){return sameMap&&std::isfinite(own)&&std::isfinite(enemy)&&own<=c.leash&&enemy<=c.leash&&elapsed<c.fightMs;}
inline bool CancelOldAction(uint64_t tracked,uint64_t owner,bool newDuelStarted,uint64_t duelOpponent){return tracked&&owner==tracked&&!(newDuelStarted&&owner==duelOpponent);}
inline float PatrolRadius(uint64_t guid,float campRadius){uint64_t mixed=guid*2654435761ULL;return std::min(70.0f,campRadius*.8f)*(.65f+.30f*float(mixed%101)/100.0f);}
}
#endif
