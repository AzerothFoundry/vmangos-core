#ifndef WORLD_BOT_DUEL_POLICY_H
#define WORLD_BOT_DUEL_POLICY_H
#include <cmath>
namespace WorldBotDuelPolicy {
inline bool InsideReturnEnvelope(float pointDistance,float currentDistance){return std::isfinite(pointDistance)&&std::isfinite(currentDistance)&&pointDistance<=std::fmax(70.0f,currentDistance);}
inline bool ReturnToCenter(float distance,bool returning){return std::isfinite(distance)&&(distance>60.0f||(returning&&distance>10.0f));}
}
#endif
