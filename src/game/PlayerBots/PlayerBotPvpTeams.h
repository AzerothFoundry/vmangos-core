// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MANGOS_PLAYERBOT_PVP_TEAMS_H
#define MANGOS_PLAYERBOT_PVP_TEAMS_H
#include <set>
#include "ItemPrototype.h"
#include "SharedDefines.h"
namespace BotMatchDetail
{
    // Audited 1.12 vendor history, not item-name/faction-35 inference:
    // A armor 1277701/2, weapons 1278201/2; H armor 1279201/2,
    // weapons 1458101/2. Non-set armor: NPC12805(A),12799(H),12781/12793.
    inline uint8 PvpTeamMask(ItemPrototype const* item)
    {
        if (!item)
            return 0;
        static std::set<uint32> const allianceSets = {282, 343, 344, 346, 348, 362, 381, 384, 388, 389, 392, 394, 395, 397, 401, 402, 544, 545, 546, 547, 548, 549, 550, 551};
        static std::set<uint32> const hordeSets = {281, 301, 341, 342, 345, 347, 361, 382, 383, 386, 387, 390, 391, 393, 396, 398, 522, 537, 538, 539, 540, 541, 542, 543};
        static std::set<uint32> const allianceItems = {12584, 18825, 18827, 18830, 18833, 18836, 18838, 18843, 18847, 18855, 18865, 18867, 18869, 18873, 18876, 23451, 23452, 23453, 23454, 23455, 23456, 15198, 15196, 16342, 18440, 18441, 18442, 18443, 18444, 18445, 18447, 18448, 18449, 18452, 18453, 18454, 18455, 18456, 18457, 18854, 18856, 18857, 18858, 18859, 18862, 18863, 18864};
        static std::set<uint32> const hordeItems = {16345, 18826, 18828, 18831, 18835, 18837, 18840, 18844, 18848, 18860, 18866, 18868, 18871, 18874, 18877, 23464, 23465, 23466, 23467, 23468, 23469, 15199, 15197, 15200, 16335, 16341, 16486, 16497, 16532, 18427, 18428, 18429, 18430, 18432, 18434, 18435, 18436, 18437, 18461, 18834, 18845, 18846, 18849, 18850, 18851, 18852, 18853};
        uint8 mask = 0;
        if (allianceSets.count(item->ItemSet) || allianceItems.count(item->ItemId))
            mask |= 1;
        if (hordeSets.count(item->ItemSet) || hordeItems.count(item->ItemId))
            mask |= 2;
        return mask;
    }
    inline bool PvpTeamAllowed(ItemPrototype const* item, uint32 team)
    {
        if (!item)
            return false;
        uint8 mask = PvpTeamMask(item), own = team == ALLIANCE ? 1 : team == HORDE ? 2 : 0;
        return mask ? (mask & own) != 0 : item->RequiredHonorRank == 0;
    }
} // namespace BotMatchDetail
#endif
