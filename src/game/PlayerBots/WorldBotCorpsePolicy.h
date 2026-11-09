#ifndef WORLD_BOT_CORPSE_POLICY_H
#define WORLD_BOT_CORPSE_POLICY_H
#include <cmath>
namespace WorldBotCorpsePolicy {
inline bool LegalApproach(unsigned type,unsigned points,float endpointDistance,float nativeRadius){return (type==1||type==4)&&points>=2&&std::isfinite(endpointDistance)&&endpointDistance>=0&&endpointDistance<=nativeRadius-2;}
}
#endif
