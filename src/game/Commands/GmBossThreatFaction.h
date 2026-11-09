#ifndef GM_BOSS_THREAT_FACTION_H
#define GM_BOSS_THREAT_FACTION_H
#include "DBCStructure.h"
namespace GmInstanceBoss {
// BasicAI uses SizeFactor::None: detection radius is center-to-center.
inline float AggroStagingClearance(float attackDistance)
{
    return attackDistance>0 ? attackDistance+3.f : 0.f;
}
inline uint32 ThreatFactionId(bool gameMaster,uint32 current,uint32 racial)
{
    return gameMaster ? racial : current;
}
inline bool ThreatFactionHostile(FactionTemplateEntry const* creature,FactionTemplateEntry const* player)
{
    // Unknown data cannot establish a harmless creature.
    return !creature || !player || creature->IsHostileTo(*player);
}
}
#endif
