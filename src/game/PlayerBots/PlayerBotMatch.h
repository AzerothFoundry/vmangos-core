// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MANGOS_PLAYERBOT_MATCH_H
#define MANGOS_PLAYERBOT_MATCH_H
#include <memory>
#include <string>
#include <vector>
#include "BattleGroundDefines.h"
#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
class Player;
namespace BotMatch
{
    struct Options
    {
        float minFactor = 0.85f, maxFactor = 1.15f;
        int minPvpPieces = -1, maxPvpPieces = -1;
        bool allowSameFactionCohort = false;
    };
    struct Profile
    {
        ObjectGuid owner;
        uint64 requestId = 0;
        BattleGroundQueueTypeId queue = BATTLEGROUND_QUEUE_NONE;
        BattleGroundBracketId bracket = BG_BRACKET_ID_NONE;
        uint32 level = 0;
        uint32 targetPerTeam = 0, healersPerTeam = 0;
        float score = 0;
        uint32 pvpPieces = 0;
        uint8 honorRank = 0;
        Options options;
    };
    float GearScore(Player const* player);
    uint32 CountPvpSetPieces(Player const* player);
    bool Begin(Player* owner, BattleGroundQueueTypeId queue, Options const& options, std::string& result);
    bool Cancel(ObjectGuid owner, std::string& result);
    std::string Status(ObjectGuid owner);
    std::vector<std::string> TeamStatus(ObjectGuid owner);
    bool EquipWeapons(Player* player, std::string& result);
    bool EquipPvpTestPieces(Player* player, uint32 count, std::string& result, uint32 quality = 3);
    uint32 AutoMatchAfterSeconds();
    std::string AutoStatus(ObjectGuid owner);
    // Called only after native invitation validation and before consuming its queue entry.
    void ObserveHumanPort(Player* player, uint32 instanceId);
    bool HumansPendingArrival(uint64 requestId, uint32 instanceId);
    void Tick(uint32 diff);
    bool OwnsQueue(BattleGroundQueueTypeId queue, BattleGroundBracketId bracket);
    bool Active(uint64 requestId);
    uint32 DesiredInstance(uint64 requestId);
    bool CanAcceptInvite(uint64 requestId, Player* bot);
    void EquipMatched(Player* bot, CombatBotRoles role, Profile const& profile, std::string& result);
    void Report(uint64 requestId, std::string const& result);
} // namespace BotMatch
#endif
