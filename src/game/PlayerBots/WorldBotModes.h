#ifndef WORLD_BOT_MODES_H
#define WORLD_BOT_MODES_H
#include "Common.h"
class Player;class WorldBotAI;class PartyBotAI;class WorldPacket;
namespace WorldBotModes {
enum class Mode { World,Party,Battleground,Instance,City,Duel,Dead };
void SetDuelArena(uint32 guid,uint32 map,float x,float y,float z,float radius);
Mode Detect(Player* player);
char const* Name(Mode mode);
bool Update(WorldBotAI& ai,uint32 diff);
void MovementInform(PartyBotAI& ai,uint32 type,uint32 point);
bool Packet(PartyBotAI& ai,WorldPacket const& packet);
}
#endif
