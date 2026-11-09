#ifndef WORLD_BOT_PK_TASK_STATE_H
#define WORLD_BOT_PK_TASK_STATE_H
#include "Common.h"
#include <string>
struct WorldBotPKTaskState {
 bool loaded=false,valid=true,arrived=false,entryUsed=false,prepared=false,duelSeen=false,transferPending=false,failed=false,standChosen=false,supplyPending=false;
 uint32 elapsed=0,saveElapsed=0,remaining=3600000,duration=3600000,rest=0,sequence=1;
 uint32 target=0,approachAt=0,searchAt=0,blockSpell=0,completed=0,attempts=0,duels=0,routeFailures=0;
 uint32 transferAttempts=0,transferRetryAt=0,failures=0,routeRetryAt=0,progressAt=0,lastMoveAt=0;float progressX=0,progressY=0,progressZ=0;float standX=0,standY=0,standZ=0;
 uint32 supplyCastAt=0,supplyItem=0,supplyBefore=0,supplyAttempts=0;
 uint32 mountSpell=0,mountRetryAt=0,mountAttempts=0,mountCasts=0;std::string mountReason="not_checked";
 std::string phase="selecting",lastEvent,error;
};
#endif
