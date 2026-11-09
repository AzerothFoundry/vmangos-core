#ifndef WORLD_BOT_AMBIENT_POLICY_H
#define WORLD_BOT_AMBIENT_POLICY_H
#include <cmath>
#include <cstdint>
namespace WorldBotAmbient {
struct Patient {bool alive=true,inWorld=true,sameMap=true,friendly=true,combat=false,duel=false,ffa=false,resting=false,casting=false;float health=100;};
inline bool MayHelp(Patient const& p){return p.alive&&p.inWorld&&p.sameMap&&p.friendly&&!p.combat&&!p.duel&&!p.ffa&&!p.resting&&!p.casting&&std::isfinite(p.health)&&p.health>0&&p.health<70;}
inline bool StageProgress(float distance,float remaining){return std::isfinite(distance)&&std::isfinite(remaining)&&distance>=5&&remaining>=2;}
inline bool DuelBuffBudget(std::uint32_t now,std::uint32_t since){return std::uint32_t(now-since)<8000;}
}
#endif
