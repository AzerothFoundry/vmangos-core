#ifndef PARTY_BOT_SERVICES_H
#define PARTY_BOT_SERVICES_H
#include "Common.h"
#include <vector>
#include <string>
class Player;
class PartyBotAI;
class WorldPacket;
struct PartyBotServiceState {
    uint64 trader=0,item=0;
    uint32 choiceTime=0,selected=0;
    bool menu=false,food=false,water=false,ready=false,informed=false;
    std::vector<uint32> choices;
};
namespace PartyBotServices {
void Initialize(PartyBotAI& ai);
bool Update(PartyBotAI& ai);
bool Packet(PartyBotAI& ai,WorldPacket const& packet);
bool Whisper(Player* sender,std::string const& target,std::string const& message);
}
#endif
