#ifndef WORLD_BOT_POPULATION_H
#define WORLD_BOT_POPULATION_H
#include <cstdint>
#include <array>
#include <cmath>
#include <random>
#include <algorithm>
namespace WorldBotPopulation {
enum class CityRole {Gate,Ambient,Healer,Other};
inline unsigned CityQuota(CityRole,unsigned configured){return configured;}
inline bool CapacityLimits(unsigned overall,unsigned level60){return overall>=1&&overall<=2500&&level60<=2000&&level60<=overall;}
inline unsigned AdmissionPriority(bool missingCityRole,bool missingCityClass,bool full){return missingCityRole?0u:missingCityClass?1u:full?2u:3u;}
inline unsigned Priority(bool missingCityClass,bool full){return missingCityClass?0:full?1:2;}
inline unsigned Deficit(unsigned online,unsigned target){return online<target?target-online:0;}
template<class Rng> unsigned SessionSeconds(unsigned low,unsigned high,Rng& rng){return std::uniform_int_distribution<unsigned>(low,high)(rng);}
inline bool SeedSnapshot(unsigned level,unsigned seed,unsigned money,unsigned xp){return seed>=1&&seed<=60&&level==seed&&money==20000000&&xp==0;}
inline bool CampPoint(float distance,float radius){return std::isfinite(distance)&&std::isfinite(radius)&&distance>=0&&radius>0&&distance<=radius;}
inline bool StarterQuiver(bool hunter,unsigned level,unsigned xp,unsigned money,unsigned nativeMoney,std::array<unsigned,4> const& bags){return hunter&&level==1&&xp==0&&money==nativeMoney&&bags[0]==2101&&bags[1]==0&&bags[2]==0&&bags[3]==0;}
enum class Presence {Initializing,Transferring,Ready};
inline Presence PresenceOf(bool initialized,bool inWorld){return !initialized?Presence::Initializing:!inWorld?Presence::Transferring:Presence::Ready;}
inline bool LoginTimedOut(bool playerExists,bool hasStart,std::uint32_t now,std::uint32_t start){return !playerExists&&hasStart&&std::uint32_t(now-start)>60000;}
struct Safety {bool alive=true,humanParty=false,combat=false,duel=false,bg=false,instance=false,teleport=false,casting=false;};
inline bool Safe(Safety const& s){return s.alive&&!s.humanParty&&!s.combat&&!s.duel&&!s.bg&&!s.instance&&!s.teleport&&!s.casting;}
inline bool Expired(std::uint32_t now,std::uint32_t start,unsigned seconds){return std::uint32_t(now-start)/1000>=seconds;}
inline bool Retire(bool fullLevel,bool overdue,Safety const& s){return fullLevel&&overdue&&Safe(s);}
inline bool Admit(bool fullLevel,unsigned online60,unsigned cap60,bool cooldown){return !cooldown&&(!fullLevel||online60<cap60);}
}
#endif
