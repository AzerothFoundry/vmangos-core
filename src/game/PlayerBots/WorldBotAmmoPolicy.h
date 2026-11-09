#ifndef WORLD_BOT_AMMO_POLICY_H
#define WORLD_BOT_AMMO_POLICY_H
#include <cstdint>
#include <vector>
namespace WorldBotAmmoPolicy {
struct Candidate {unsigned id,subclass,level,itemLevel,stack;bool usable;};
// Vanilla weapon subclasses: bow2, gun3, crossbow18; arrow2, bullet3.
inline bool Compatible(unsigned weapon,unsigned ammo){return (weapon==2||weapon==18)?ammo==2:weapon==3&&ammo==3;}
inline unsigned Select(bool fixed,unsigned declared,unsigned weapon,unsigned level,std::vector<Candidate> const& pool){
 unsigned wanted=declared?declared:19316;Candidate const* best=nullptr;
 for(auto const& c:pool){if(!c.usable||!c.stack||c.level>level||!Compatible(weapon,c.subclass)||(fixed&&c.id!=wanted))continue;
  if(!best||c.itemLevel>best->itemLevel||(c.itemLevel==best->itemLevel&&c.id<best->id))best=&c;
 }
 return best?best->id:0;
}
inline unsigned Deficit(unsigned current,unsigned stack){return current<stack?stack-current:0;}
}
#endif
