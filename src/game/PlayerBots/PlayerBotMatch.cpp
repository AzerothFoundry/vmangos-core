// SPDX-License-Identifier: GPL-2.0-or-later
#include "PlayerBotMatch.h"
#include "BgFillLifecycle.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include "Bag.h"
#include "BattleBotAI.h"
#include "BattleGround.h"
#include "BattleGroundMgr.h"
#include "Config/Config.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemPrototype.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerBotMgr.h"
#include "PlayerBotPvpTeams.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "Utilities/Random.h"
#include "World.h"

namespace BotMatch
{
    namespace
    {
        bool IsHealingPlayer(Player const* player)
        {
            uint8 cls = player->GetClass();
            return (cls == CLASS_PRIEST && !player->HasSpell(15473)) || (cls == CLASS_DRUID && !player->HasSpell(24858) && !player->HasSpell(17007)) || (cls == CLASS_PALADIN && !player->HasSpell(20218) && !player->HasSpell(20925)) || (cls == CLASS_SHAMAN && !player->HasSpell(17364) && !player->HasSpell(16166));
        }
        struct Spawn
        {
            uint32 guid;
            Team team;
            uint8 playerClass;
            CombatBotRoles role;
        };
        struct Request
        {
            std::shared_ptr<Profile> profile;
            std::vector<Spawn> bots;
            std::map<uint32,Spawn> retiring;
            std::map<ObjectGuid, uint32> humans;
            uint32 clientInstance = 0, instanceId = 0;
            std::set<ObjectGuid> cancelledHumans;
            std::string report;
            uint32 configured = 0, limited = 0;
        };
        std::map<uint64, Request> requests;
        std::recursive_mutex requestMutex;
        uint64 nextRequest = 0;
        uint32 timer = 0;
        std::map<ObjectGuid, std::string> lastResults;
        typedef std::pair<ObjectGuid, uint32> AutoKey;
        struct AutoWait
        {
            uint32 joinTime = 0;
            bool blocked = false;
            std::string reason = "none";
        };
        std::map<AutoKey, AutoWait> autoWaits;
        Request* HumanRequest(ObjectGuid guid)
        {
            for (auto& pair : requests)
                if (pair.second.humans.count(guid))
                    return &pair.second;
            return nullptr;
        }
        bool ScoredSlot(uint8 slot) { return slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD; }
        bool Military(ItemPrototype const* item)
        {
            if (!item || item->Class != ITEM_CLASS_ARMOR || !item->ItemSet || !item->RequiredHonorRank)
                return false;
            switch (item->InventoryType)
            {
            case INVTYPE_HEAD:
            case INVTYPE_SHOULDERS:
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:
            case INVTYPE_HANDS:
            case INVTYPE_LEGS:
            case INVTYPE_FEET:
                return true;
            default:
                return false;
            }
        }
        uint32 Primary(uint8 cls, CombatBotRoles role)
        {
            if (cls == CLASS_WARRIOR || (cls == CLASS_PALADIN && role != ROLE_HEALER))
                return ITEM_MOD_STRENGTH;
            if (cls == CLASS_HUNTER || cls == CLASS_ROGUE || ((cls == CLASS_DRUID || cls == CLASS_SHAMAN) && role == ROLE_MELEE_DPS))
                return ITEM_MOD_AGILITY;
            return ITEM_MOD_INTELLECT;
        }
        bool Eligible(Player* player, ItemPrototype const* item, bool ignoreHonor = false)
        {
            if (!item || item->HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE) || (item->Class != ITEM_CLASS_ARMOR && item->Class != ITEM_CLASS_WEAPON))
                return false;
            if (item->InventoryType == INVTYPE_TABARD || item->InventoryType == INVTYPE_BODY)
                return false;
            // Race/class masks on many 1.12 PvP items are unrestricted. Use the
            // audited vendor/set ownership, never item names or GM exemptions.
            if (!BotMatchDetail::PvpTeamAllowed(item, player->GetTeam()))
                return false;
            if (item->SourceQuestRaces && !(item->SourceQuestRaces & player->GetRaceMask()))
                return false;
            if (item->SourceQuestClasses && !(item->SourceQuestClasses & player->GetClassMask()))
                return false;
            if (item->SourceQuestLevel > int32(player->GetLevel()))
                return false;
            if (item->SourceQuestLevel <= 0 && !item->RequiredLevel && item->ItemLevel > player->GetLevel())
                return false;
            if (item->RequiredReputationFaction && uint32(player->GetReputationRank(item->RequiredReputationFaction)) < item->RequiredReputationRank)
                return false;
            if (uint32 skill = item->GetProficiencySkill())
            {
                SkillRaceClassInfoEntry const* allowed = GetSkillRaceClassInfo(skill, player->GetRace(), player->GetClass());
                if (!allowed || allowed->reqLevel > player->GetLevel())
                    return false;
            }
            if (item->Class == ITEM_CLASS_WEAPON && item->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                return false;
            if (item->Class == ITEM_CLASS_WEAPON && player->GetClass() == CLASS_SHAMAN && (item->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 || item->SubClass == ITEM_SUBCLASS_WEAPON_MACE2) && !player->HasSpell(16269))
                return false;
            return player->CanUseItem(item, !ignoreHonor) == EQUIP_ERR_OK;
        }
        uint32 OrdinaryFreeSlots(Player const* player)
        {
            uint32 count = 0;
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    ++count;
            for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (item->GetProto()->Class == ITEM_CLASS_CONTAINER && item->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER)
                        count += static_cast<Bag*>(item)->GetFreeSlots();
            return count;
        }
        void HonorForItem(Player* player, ItemPrototype const* item)
        {
            if (player->GetHonorMgr().GetHighestRank().rank < item->RequiredHonorRank)
                player->GetHonorMgr().SetHighestRank(uint8(item->RequiredHonorRank));
            if (player->GetHonorMgr().GetRank().rank < item->RequiredHonorRank)
            {
                HonorRankInfo rank;
                rank.rank = uint8(item->RequiredHonorRank);
                player->GetHonorMgr().CalculateRankInfo(rank);
                player->GetHonorMgr().SetRank(rank);
            }
            player->SetByteValue(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTES_OFFSET_HIGHEST_HONOR_RANK, player->GetHonorMgr().GetHighestRank().rank);
            player->SetByteValue(PLAYER_BYTES_3, PLAYER_BYTES_3_OFFSET_HONOR_RANK, player->GetHonorMgr().GetRank().rank);
        }
        void Remove(Request& request)
        {
            for (auto const& bot : request.bots)
                sPlayerBotMgr.RequestBotRemoval(bot.guid);
            request.bots.clear();
            for(auto const& bot:request.retiring)sPlayerBotMgr.RequestBotRemoval(bot.first);
            request.retiring.clear();
        }
        Request* Find(uint64 id)
        {
            for (auto& pair : requests)
                if (pair.second.profile->requestId == id)
                    return &pair.second;
            return nullptr;
        }
        bool ActiveBot(Request const& r,ObjectGuid guid)
        {
            return std::any_of(r.bots.begin(),r.bots.end(),[&](Spawn const& b){return b.guid==guid.GetCounter();});
        }
        bool OwnBot(Request const& r,ObjectGuid guid)
        {
            return ActiveBot(r,guid) || r.retiring.count(guid.GetCounter());
        }
        uint32 DesiredInternalInstance(BattleGroundTypeId type,BattleGroundBracketId bracket,uint32 clientId)
        {
            if(!clientId)return 0;
            std::vector<BgFillLifecycle::InstanceIdentity> instances;
            for(auto i=sBattleGroundMgr.GetBattleGroundsBegin(type);i!=sBattleGroundMgr.GetBattleGroundsEnd(type);++i){
                BattleGround* bg=i->second;
                instances.push_back({uint32(type),uint32(bg->GetBracketId()),bg->GetClientInstanceID(),bg->GetInstanceID(),
                    bg->GetStatus()==STATUS_WAIT_JOIN || bg->GetStatus()==STATUS_IN_PROGRESS});
            }
            return BgFillLifecycle::ResolveDesiredInstance(uint32(type),uint32(bracket),clientId,instances);
        }
        BattleGround* DesiredBattleGround(BattleGroundTypeId type,BattleGroundBracketId bracket,uint32 clientId)
        {
            uint32 internal=DesiredInternalInstance(type,bracket,clientId);
            return internal?sBattleGroundMgr.GetBattleGround(internal,type):nullptr;
        }
        BattleGround* BoundInstance(Request& r)
        {
            auto type=BattleGroundMgr::BgTemplateId(r.profile->queue);
            if(r.instanceId)return sBattleGroundMgr.GetBattleGround(r.instanceId,type);
            BattleGround* bg=r.clientInstance?DesiredBattleGround(type,r.profile->bracket,r.clientInstance):nullptr;
            auto const& queue=sBattleGroundMgr.m_battleGroundQueues[r.profile->queue].m_queuedPlayers;
            for(auto const& human:r.humans) {
                if(bg)break;
                Player* p=sObjectAccessor.FindPlayerNotInWorld(human.first);
                BattleGround* current=p && p->InBattleGround()?p->GetBattleGround():nullptr;
                if(current && current->GetTypeID()==type && current->GetBracketId()==r.profile->bracket)bg=current;
                auto q=queue.find(human.first);
                if(!bg && q!=queue.end() && q->second.groupInfo && q->second.groupInfo->isInvitedToBgInstanceGuid)
                    bg=sBattleGroundMgr.GetBattleGround(q->second.groupInfo->isInvitedToBgInstanceGuid,type);
                if(bg && bg->GetBracketId()!=r.profile->bracket)bg=nullptr;
                if(!bg && q!=queue.end() && q->second.groupInfo && !q->second.groupInfo->isInvitedToBgInstanceGuid && q->second.groupInfo->desiredInstanceId)
                    bg=DesiredBattleGround(type,r.profile->bracket,q->second.groupInfo->desiredInstanceId);
            }
            if(bg && bg->GetBracketId()==r.profile->bracket) {
                r.instanceId=bg->GetInstanceID();r.clientInstance=bg->GetClientInstanceID();return bg;
            }
            return nullptr;
        }
        BgFillLifecycle::Population Population(Request& r)
        {
            BgFillLifecycle::Population pop;pop.target=r.profile->targetPerTeam;
            auto type=BattleGroundMgr::BgTemplateId(r.profile->queue);
            BattleGround* bg=BoundInstance(r);
            pop.ended=r.instanceId && (!bg || bg->GetStatus()==STATUS_WAIT_LEAVE || bg->GetStatus()==STATUS_NONE);
            std::map<ObjectGuid,uint32> humans;
            std::set<ObjectGuid> present;
            if(bg)for(auto const& member:bg->GetPlayers()) {
                present.insert(member.first);if(OwnBot(r,member.first))continue;
                unsigned t=member.second.playerTeam==ALLIANCE?0:1;
                Player* p=sObjectAccessor.FindPlayer(member.first);
                if((p && p->IsBot()) || sPlayerBotMgr.HasBot(member.first.GetCounter())){++pop.foreignBots[t];continue;}
                ++pop.humans[t];humans[member.first]=p?p->GetLevel():r.profile->level;
            }
            if(r.instanceId)for(auto const& old:r.humans)if(!present.count(old.first))if(Player* p=sObjectAccessor.FindPlayerNotInWorld(old.first))
                if(!p->IsBot() && BgFillLifecycle::InFlightArrival(r.instanceId,p->GetBattleGroundId(),p->IsBeingTeleported())){++pop.pending[p->GetBGTeam()==ALLIANCE?0:1];humans[old.first]=p->GetLevel();present.insert(old.first);}
            auto const& queue=sBattleGroundMgr.m_battleGroundQueues[r.profile->queue].m_queuedPlayers;
            for(auto const& member:queue) {
                if(present.count(member.first) || OwnBot(r,member.first) || !member.second.online || !member.second.groupInfo)continue;
                Player* p=sObjectAccessor.FindPlayer(member.first);if(!p)continue;
                if(p->IsBot()){if(r.instanceId && member.second.groupInfo->isInvitedToBgInstanceGuid==r.instanceId)++pop.foreignBots[p->GetTeam()==ALLIANCE?0:1];continue;}
                BgFillLifecycle::Location location;
                location.requestMember=r.humans.count(member.first);location.sameQueue=true;location.sameBracket=p->GetBattleGroundBracketIdFromLevel(type)==r.profile->bracket;
                location.cancelled=r.cancelledHumans.count(member.first);location.currentInstance=p->GetBattleGroundId();
                location.invitedInstance=member.second.groupInfo->isInvitedToBgInstanceGuid;
                location.desiredInstance=location.invitedInstance?0:DesiredInternalInstance(type,r.profile->bracket,member.second.groupInfo->desiredInstanceId);
                // An invalid explicit destination is not an unspecified/generic queue.
                if(!location.invitedInstance && member.second.groupInfo->desiredInstanceId && !location.desiredInstance)continue;
                if(BgFillLifecycle::PendingFor(r.instanceId,location)) {
                    ++pop.pending[p->GetTeam()==ALLIANCE?0:1];humans[member.first]=p->GetLevel();
                }
            }
            for(auto i=r.cancelledHumans.begin();i!=r.cancelledHumans.end();)if(!queue.count(*i))i=r.cancelledHumans.erase(i);else ++i;
            r.humans=std::move(humans);
            for(auto const& bot:r.bots)++pop.ownBots[bot.team==ALLIANCE?0:1];
            for(auto const& bot:r.retiring)++pop.retiringBots[bot.second.team==ALLIANCE?0:1];
            return pop;
        }
        bool AddOne(Request& request,Team team)
        {
            auto const& profile=*request.profile;uint32 healers=0;
            for(auto const& h:request.humans)if(auto p=sObjectAccessor.FindPlayer(h.first))if(p->GetTeam()==team && IsHealingPlayer(p))++healers;
            for(auto const& b:request.bots)if(b.team==team && b.role==ROLE_HEALER)++healers;
            CombatBotRoles role;uint8 cls;
            if(healers<profile.healersPerTeam) {
                std::vector<uint8> choices{CLASS_PRIEST,CLASS_DRUID,uint8(team==ALLIANCE?CLASS_PALADIN:CLASS_SHAMAN)};
                cls=SelectRandomContainerElement(choices);role=ROLE_HEALER;
            } else {
                std::vector<uint8> choices{CLASS_WARRIOR,CLASS_ROGUE,CLASS_HUNTER,CLASS_MAGE,CLASS_WARLOCK,CLASS_PRIEST,CLASS_DRUID,uint8(team==ALLIANCE?CLASS_PALADIN:CLASS_SHAMAN)};
                cls=SelectRandomContainerElement(choices);role=cls==CLASS_WARRIOR||cls==CLASS_ROGUE||cls==CLASS_PALADIN?ROLE_MELEE_DPS:ROLE_RANGE_DPS;
                if(cls==CLASS_DRUID||cls==CLASS_SHAMAN)role=urand(0,1)?ROLE_MELEE_DPS:ROLE_RANGE_DPS;
            }
            uint8 race=SelectRandomRaceForClass(cls,team);if(!race)return false;
            uint32 instance=sMapMgr.GetContinentInstanceId(1,16224.356f,16284.763f);
            auto ai=new BattleBotAI(race,cls,profile.level,1,instance,16224.356f,16284.763f,13.175f,4.56f,profile.queue,true);
            ai->m_role=role;ai->m_matchProfile=request.profile;
            if(!sPlayerBotMgr.AddBot(ai)) {
                if(ai->botEntry)sPlayerBotMgr.RequestBotRemoval(uint32(ai->botEntry->playerGUID));
                request.report="A bot spawn failed; remaining players and bots preserved; fill retries later.";return false;
            }
            request.bots.push_back({uint32(ai->botEntry->playerGUID),team,cls,role});return true;
        }
        bool RetireOne(Request& request,Team team)
        {
            auto chosen=request.bots.end();int score=100;
            for(auto it=request.bots.begin();it!=request.bots.end();++it) {
                if(it->team!=team)continue;
                Player* p=sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER,it->guid));
                if(p && (p->HasAura(AURA_WARSONG_FLAG) || p->HasAura(AURA_SILVERWING_FLAG)))continue;
                int candidate=(!p||!p->InBattleGround()?0:!p->IsAlive()?10:!p->IsInCombat()?20:30)+(it->role==ROLE_HEALER?1:0);
                if(candidate<score){chosen=it;score=candidate;}
            }
            if(chosen==request.bots.end())return false;
            uint32 guid=chosen->guid;
            if(!BgFillLifecycle::RetireOwned(request.bots,request.retiring,guid))return false;
            sPlayerBotMgr.RequestBotRemoval(guid);return true;
        }
    } // namespace
    float GearScore(Player const* player)
    {
        if (!player)
            return 0;
        float total = 0, weights = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (!ScoredSlot(slot))
                continue;
            float weight = (slot == EQUIPMENT_SLOT_MAINHAND || (slot == EQUIPMENT_SLOT_RANGED && player->GetClass() == CLASS_HUNTER)) ? 2.0f : 1.0f;
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item && slot == EQUIPMENT_SLOT_OFFHAND)
            {
                Item* main = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                if (main && main->GetProto()->InventoryType == INVTYPE_2HWEAPON)
                    item = main;
            }
            weights += weight;
            if (item)
                total += item->GetProto()->ItemLevel * weight;
        }
        return weights ? total / weights : 0;
    }
    uint32 CountPvpSetPieces(Player const* player)
    {
        uint32 count = 0;
        if (player)
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (Military(item->GetProto()))
                        ++count;
        return count;
    }
    bool OwnsQueue(BattleGroundQueueTypeId queue, BattleGroundBracketId bracket)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        for (auto const& pair : requests)
            if (pair.second.profile->queue == queue && pair.second.profile->bracket == bracket)
                return true;
        return false;
    }
    bool Active(uint64 id)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        return Find(id) != nullptr;
    }
    uint32 DesiredInstance(uint64 id)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        Request* request = Find(id);
        if(request)BoundInstance(*request);
        return request ? request->clientInstance : 0;
    }
    bool CanAcceptInvite(uint64 id, Player* bot)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        Request* r=Find(id);if(!r || !bot || !bot->IsBot() || !ActiveBot(*r,bot->GetObjectGuid()))return false;
        auto pop=Population(*r);auto plan=BgFillLifecycle::Decide(pop);
        if(plan.cleanup || !r->instanceId)return false;
        auto type=BattleGroundMgr::BgTemplateId(r->profile->queue);
        auto const& queue=sBattleGroundMgr.m_battleGroundQueues[r->profile->queue].m_queuedPlayers;
        auto entry=queue.find(bot->GetObjectGuid());if(entry==queue.end() || !entry->second.groupInfo)return false;
        if(entry->second.groupInfo->isInvitedToBgInstanceGuid!=r->instanceId) {
            sPlayerBotMgr.RequestBotRemoval(bot->GetGUIDLow());
            r->bots.erase(std::remove_if(r->bots.begin(),r->bots.end(),[&](Spawn const& b){return b.guid==bot->GetGUIDLow();}),r->bots.end());
            r->report="One bot received a different instance invitation; only that bot retired.";return false;
        }
        BattleGround* bg=sBattleGroundMgr.GetBattleGround(r->instanceId,type);if(!bg)return false;
        Team team=bot->GetTeam();unsigned index=team==ALLIANCE?0:1,rank=0;
        for(auto const& owned:r->bots)if(owned.team==team){if(owned.guid==bot->GetGUIDLow())break;++rank;}
        return rank<plan.budget[index] && bg->GetPlayersCountByTeam(team)<pop.target;
    }
    void Report(uint64 id, std::string const& result)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        if (Request* request = Find(id))
        {
            ++request->configured;
            if (result.find("limited:") != std::string::npos)
            {
                ++request->limited;
                request->report = result;
            }
            else if (request->report.empty())
                request->report = result;
        }
    }

    bool Begin(Player* owner, BattleGroundQueueTypeId queue, Options const& options, std::string& result)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        if (!owner || owner->IsBot())
        {
            result = "A real player is required.";
            return false;
        }
        if (queue != BATTLEGROUND_QUEUE_WS && queue != BATTLEGROUND_QUEUE_AB && queue != BATTLEGROUND_QUEUE_AV)
        {
            result = "Unsupported battleground.";
            return false;
        }
        if (!(options.minFactor > 0 && options.minFactor <= options.maxFactor && options.maxFactor <= 3) || options.minPvpPieces < -1 || options.maxPvpPieces < -1 || options.minPvpPieces > 6 || options.maxPvpPieces > 6)
        {
            result = "Invalid match limits.";
            return false;
        }
        BattleGroundTypeId type = BattleGroundMgr::BgTemplateId(queue);
        BattleGroundBracketId bracket = owner->GetBattleGroundBracketIdFromLevel(type);
        if (bracket == BG_BRACKET_ID_NONE || (!owner->InBattleGroundQueueForBattleGroundQueueType(queue) && !(owner->InBattleGround() && owner->GetBattleGround()->GetTypeID() == type)))
        {
            result = "Join the matching battleground queue first; your level is never changed automatically.";
            return false;
        }
        if (HumanRequest(owner->GetObjectGuid()) || (!owner->InBattleGround() && OwnsQueue(queue, bracket)))
        {
            result = "A test owner already controls this queue/bracket. Cancel that request first.";
            return false;
        }
        if(owner->InBattleGround())for(auto const& pair:requests)if(pair.second.instanceId==owner->GetBattleGroundId()){result="This battleground instance already has a fill request.";return false;}
        auto const& queued = sBattleGroundMgr.m_battleGroundQueues[queue].m_queuedPlayers;
        std::map<ObjectGuid, uint32> humans;
        humans[owner->GetObjectGuid()] = owner->GetLevel();
        uint32 initialInstance=owner->GetBattleGroundId();
        auto ownQueue=queued.find(owner->GetObjectGuid());
        if(!initialInstance && ownQueue!=queued.end() && ownQueue->second.groupInfo)initialInstance=ownQueue->second.groupInfo->isInvitedToBgInstanceGuid;
        for(auto const& pair:queued) {
            Player* other=sObjectAccessor.FindPlayer(pair.first);
            if(!other || HumanRequest(other->GetObjectGuid()) || other->IsBot() || other->InBattleGround() || !pair.second.online || !pair.second.groupInfo || other->GetBattleGroundBracketIdFromLevel(type)!=bracket)continue;
            auto const* q=pair.second.groupInfo;
            if(!initialInstance && (q->isInvitedToBgInstanceGuid || q->desiredInstanceId))continue;
            if(initialInstance && (q->isInvitedToBgInstanceGuid ? q->isInvitedToBgInstanceGuid!=initialInstance : (q->desiredInstanceId && DesiredInternalInstance(type,bracket,q->desiredInstanceId)!=initialInstance)))continue;
            humans[other->GetObjectGuid()]=other->GetLevel();
        }
        BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(type);
        if (!bg)
        {
            result = "Battleground template is unavailable.";
            return false;
        }
        if(owner->InBattleGround())for(auto const& member:owner->GetBattleGround()->GetPlayers())
            if(Player* p=sObjectAccessor.FindPlayer(member.first))if(!p->IsBot())humans[member.first]=p->GetLevel();
        Request request;
        request.humans = humans;
        if (owner->InBattleGround())
            request.clientInstance = owner->GetBattleGround()->GetClientInstanceID();
        else
        {
            auto own = queued.find(owner->GetObjectGuid());
            if (own != queued.end() && own->second.groupInfo->isInvitedToBgInstanceGuid)
                if (BattleGround* invited = sBattleGroundMgr.GetBattleGround(own->second.groupInfo->isInvitedToBgInstanceGuid, type))
                    request.clientInstance = invited->GetClientInstanceID();
        }
        request.profile = std::make_shared<Profile>();
        Profile& profile = *request.profile;
        profile.owner = owner->GetObjectGuid();
        profile.requestId = ++nextRequest;
        profile.queue = queue;
        profile.bracket = bracket;
        profile.level = owner->GetLevel();
        profile.score = GearScore(owner);
        profile.pvpPieces = CountPvpSetPieces(owner);
        profile.honorRank = owner->GetHonorMgr().GetHighestRank().rank;
        profile.options = options;
        if (profile.options.minPvpPieces < 0)
            profile.options.minPvpPieces = profile.pvpPieces ? int(profile.pvpPieces) - 1 : 0;
        if (profile.options.maxPvpPieces < 0)
            profile.options.maxPvpPieces = std::min<int>(6, profile.pvpPieces + 2);
        if (profile.options.minPvpPieces > profile.options.maxPvpPieces)
        {
            result = "Minimum PvP pieces exceeds maximum.";
            return false;
        }
        uint32 const target = bg->GetMaxPlayersPerTeam();
        // A full match includes the real cohort; minimum start size is not the fill target.
        if (!target)
        {
            result = "Real-player cohort exceeds battleground team capacity.";
            return false;
        }
        uint32 const healers = std::max<uint32>(1, (target + 4) / 5);
        profile.targetPerTeam = target;
        profile.healersPerTeam = healers;
        // New bots get a per-request immutable snapshot; no global mode is changed.
        requests.emplace(profile.requestId, request);
        Request& stored = requests.at(profile.requestId);
        auto population=Population(stored);
        for(auto const& other:requests)if(other.first!=profile.requestId && stored.instanceId && other.second.instanceId==stored.instanceId){requests.erase(profile.requestId);result="This battleground already has a fill request.";return false;}
        auto plan=BgFillLifecycle::Decide(population);
        for(Team team:{ALLIANCE,HORDE})for(uint32 count=0;count<plan.budget[team==ALLIANCE?0:1];++count)
            if(!AddOne(stored,team))break;
        lastResults.erase(owner->GetObjectGuid());
        result = Status(owner->GetObjectGuid());
        return true;
    }
    uint32 AutoMatchAfterSeconds() { return uint32(std::max<int32>(0, std::min<int32>(300, sConfig.GetIntDefault("BattleBot.AutoMatchAfterSeconds", 0)))); }
    std::string AutoStatus(ObjectGuid owner)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        std::ostringstream out;
        uint32 waiting = 0, queueId = 0;
        std::string blocked = "none";
        bool queued = false;
        for (auto const& pair : autoWaits)
            if (pair.first.first == owner)
            {
                uint32 elapsed = WorldTimer::getMSTimeDiff(pair.second.joinTime, WorldTimer::getMSTime()) / 1000;
                if (!queued || elapsed > waiting)
                {
                    queued = true;
                    waiting = elapsed;
                    queueId = pair.first.second;
                    blocked = pair.second.reason;
                }
            }
        out << "autoSeconds=" << AutoMatchAfterSeconds() << "|waiting=" << waiting << "|queued=" << (queued ? 1 : 0) << "|blocked=" << blocked << "|queue=" << queueId;
        return out.str();
    }
    bool Cancel(ObjectGuid owner, std::string& result)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        auto it = std::find_if(requests.begin(), requests.end(), [&](auto const& pair) { return pair.second.humans.count(owner) != 0; });
        if (it == requests.end())
        {
            bool blocked = false;
            for (auto queueType : {BATTLEGROUND_QUEUE_WS, BATTLEGROUND_QUEUE_AB, BATTLEGROUND_QUEUE_AV})
            {
                auto const& queue = sBattleGroundMgr.m_battleGroundQueues[queueType].m_queuedPlayers;
                auto entry = queue.find(owner);
                if (entry != queue.end() && entry->second.groupInfo)
                {
                    auto& state = autoWaits[{owner, uint32(queueType)}];
                    state.joinTime = entry->second.groupInfo->joinTime;
                    state.blocked = true;
                    state.reason = "manual_cancel";
                    blocked = true;
                }
            }
            result = blocked ? "Automatic fill paused for this queue entry; leave and requeue to resume." : "No active test request.";
            lastResults[owner] = result;
            return blocked;
        }
        Request& request=it->second;request.cancelledHumans.insert(owner);
        auto const& queue=sBattleGroundMgr.m_battleGroundQueues[request.profile->queue].m_queuedPlayers;
        auto q=queue.find(owner);if(q!=queue.end() && q->second.groupInfo){auto& state=autoWaits[{owner,uint32(request.profile->queue)}];state.joinTime=q->second.groupInfo->joinTime;state.blocked=true;state.reason="manual_cancel";}
        auto population=Population(request);
        if(Player* caller=sObjectAccessor.FindPlayer(owner))if(request.instanceId && caller->GetBattleGroundId()==request.instanceId && !caller->IsBeingTeleported()){unsigned t=caller->GetBGTeam()==ALLIANCE?0:1;if(population.humans[t])--population.humans[t];}
        auto plan=BgFillLifecycle::Decide(population);
        if(plan.cleanup){Remove(request);requests.erase(it);result="Personal pending fill cancelled; no remaining instance humans or pending arrivals.";}
        else result="Personal pending fill cancelled; the occupied battleground continues for its real players.";
        lastResults[owner]=result;
        return true;
    }
    std::string Status(ObjectGuid owner)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        Request* found = HumanRequest(owner);
        if (!found)
            return lastResults.count(owner) ? lastResults.at(owner) : "No active battleground match request.";
        Request const& r = *found;
        std::ostringstream out;
        out << "request=" << r.profile->requestId << " queue=" << uint32(r.profile->queue) << " bracket=" << uint32(r.profile->bracket) << " level=" << r.profile->level << " humans=" << r.humans.size() << " snapshotWeightedItemLevel=" << r.profile->score << " pvpPieces=" << r.profile->pvpPieces << " range=" << r.profile->options.minFactor << "-" << r.profile->options.maxFactor << " pvpTarget=" << r.profile->options.minPvpPieces << "-" << r.profile->options.maxPvpPieces << " bots=" << r.bots.size() << " retiring=" << r.retiring.size() << " ready=" << r.configured << " limited=" << r.limited << " instance=" << r.clientInstance;
        if (r.limited)
            out << " unmet=legal gear pool constraints";
        return out.str();
    }
    std::vector<std::string> TeamStatus(ObjectGuid owner)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        Request* r = HumanRequest(owner);
        if (!r)
            return {};
        std::vector<std::string> lines;
        for (Team team : {ALLIANCE, HORDE})
        {
            uint32 humans = 0, humanHealers = 0, bots = 0, healers = 0, ready = 0, readyHealers = 0;
            unsigned flagBots=0,midBots=0,flagHealers=0,midHealers=0;
            for (auto const& human : r->humans)
                if (Player* player = sObjectAccessor.FindPlayer(human.first))
                    if (player->GetTeam() == team)
                    {
                        ++humans;
                        if (IsHealingPlayer(player))
                            ++humanHealers;
                    }
            for (auto const& bot : r->bots)
                if (bot.team == team)
                {
                    ++bots;
                    if (bot.role == ROLE_HEALER)
                        ++healers;
                    // FindPlayer only returns a loaded avatar; never inspect pending AI state.
                    if (Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, bot.guid)))
                        if (player->IsBot() && player->AI())
                        {
                            if (auto ai = dynamic_cast<BattleBotAI*>(player->AI()))
                                if (ai->m_initialized && ai->m_matchProfile && ai->m_matchProfile->requestId == r->profile->requestId)
                                {
                                    ++ready;
                                    if(player->GetBattleGround() && player->GetBattleGround()->GetTypeID()==BATTLEGROUND_WS)
                                    {
                                        bool flag=ai->m_wsgSquad==BattleBotTactics::WsgSquad::Flag;
                                        if(flag){++flagBots;if(ai->GetRole()==ROLE_HEALER)++flagHealers;}
                                        else {++midBots;if(ai->GetRole()==ROLE_HEALER)++midHealers;}
                                    }
                                    if (ai->GetRole() == ROLE_HEALER)
                                        ++readyHealers;
                                }
                        }
                }
            uint32 retiring=0;for(auto const& bot:r->retiring)if(bot.second.team==team)++retiring;
            std::ostringstream out;
            out << "GMUI|BGTEAM|team=" << uint32(team) << "|target=" << r->profile->targetPerTeam << "|humans=" << humans << "|humanHealers=" << humanHealers << "|bots=" << bots << "|retiring=" << retiring << "|healerTarget=" << r->profile->healersPerTeam << "|botHealers=" << healers << "|ready=" << ready << "|readyHealers=" << readyHealers << "|flagBots=" << flagBots << "|midBots=" << midBots << "|flagHealers=" << flagHealers << "|midHealers=" << midHealers;
            lines.push_back(out.str());
        }
        return lines;
    }
    void ObserveHumanPort(Player* player,uint32 instanceId)
    {
        if(!player || player->IsBot() || !instanceId)return;
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        for(auto& pair:requests) {
            Request& r=pair.second;if(r.instanceId!=instanceId)continue;
            auto type=BattleGroundMgr::BgTemplateId(r.profile->queue);
            BattleGround* bg=sBattleGroundMgr.GetBattleGround(instanceId,type);if(!bg)continue;
            bool bracket=bg->GetBracketId()==r.profile->bracket && player->GetBattleGroundBracketIdFromLevel(type)==r.profile->bracket;
            bool joinable=bg->GetStatus()==STATUS_WAIT_JOIN || bg->GetStatus()==STATUS_IN_PROGRESS;
            if(BgFillLifecycle::ObserveHumanPort(r.humans,player->GetObjectGuid(),player->GetLevel(),r.instanceId,instanceId,false,bracket,joinable))
                r.cancelledHumans.erase(player->GetObjectGuid());
        }
    }
    bool HumansPendingArrival(uint64 id, uint32 instanceId)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        Request* r = Find(id);
        if (!r)
            return false;
        auto population=Population(*r);
        if(r->instanceId!=instanceId)return false;
        if(population.pending[0] || population.pending[1])return true;
        for(auto const& human:r->humans)if(Player* p=sObjectAccessor.FindPlayerNotInWorld(human.first))
            if(p->GetBattleGroundId()==instanceId && p->IsBeingTeleported())return true;
        return false;
    }
    static void ScanAutomaticQueues()
    {
        uint32 seconds = AutoMatchAfterSeconds(), now = WorldTimer::getMSTime();
        std::set<AutoKey> seen;
        struct WaitingHuman
        {
            Player* player;
            uint32 joinTime;
        };
        for (auto queueType : {BATTLEGROUND_QUEUE_WS, BATTLEGROUND_QUEUE_AB, BATTLEGROUND_QUEUE_AV})
        {
            auto type = BattleGroundMgr::BgTemplateId(queueType);
            auto const& queued = sBattleGroundMgr.m_battleGroundQueues[queueType].m_queuedPlayers;
            std::map<BattleGroundBracketId, std::vector<WaitingHuman>> brackets;
            for (auto const& pair : queued)
            {
                AutoKey key{pair.first, uint32(queueType)};
                seen.insert(key);
                Player* player = sObjectAccessor.FindPlayer(pair.first);
                if (!player || player->IsBot() || !pair.second.groupInfo || !pair.second.online)
                    continue;
                auto bracket = player->GetBattleGroundBracketIdFromLevel(type);
                if (bracket == BG_BRACKET_ID_NONE)
                    continue;
                auto& state = autoWaits[key];
                if (state.joinTime != pair.second.groupInfo->joinTime)
                {
                    state = AutoWait{};
                    state.joinTime = pair.second.groupInfo->joinTime;
                }
                if (pair.second.groupInfo->isInvitedToBgInstanceGuid)
                {
                    if (!state.blocked)
                        state.reason = "invited";
                    continue;
                }
                brackets[bracket].push_back({player, pair.second.groupInfo->joinTime});
            }
            for (auto const& bracket : brackets)
            {
                auto const& humans = bracket.second;
                if (humans.empty() || OwnsQueue(queueType, bracket.first))
                    continue;
                bool alliance = false, horde = false, blocked = false, activeElsewhere = false;
                WaitingHuman anchor = humans.front();
                for (auto const& human : humans)
                {
                    if (human.player->GetTeam() == ALLIANCE)
                        alliance = true;
                    else
                        horde = true;
                    auto& state = autoWaits[{human.player->GetObjectGuid(), uint32(queueType)}];
                    blocked = blocked || state.blocked;
                    activeElsewhere = activeElsewhere || HumanRequest(human.player->GetObjectGuid());
                    if (WorldTimer::getMSTimeDiff(human.joinTime, now) > WorldTimer::getMSTimeDiff(anchor.joinTime, now))
                        anchor = human;
                }
                std::string reason = alliance && horde ? "both_sides_real" : blocked ? "cohort_cancelled" : activeElsewhere ? "other_request" : seconds ? "waiting" : "disabled";
                for (auto const& human : humans)
                {
                    auto& state = autoWaits[{human.player->GetObjectGuid(), uint32(queueType)}];
                    if (!state.blocked)
                        state.reason = reason;
                }
                if (!seconds || (alliance && horde) || blocked || activeElsewhere || WorldTimer::getMSTimeDiff(anchor.joinTime, now) < seconds * 1000)
                    continue;
                Options options;
                options.allowSameFactionCohort = true;
                std::string result;
                if (!Begin(anchor.player, queueType, options, result))
                {
                    for (auto const& human : humans)
                    {
                        auto& state = autoWaits[{human.player->GetObjectGuid(), uint32(queueType)}];
                        state.blocked = true;
                        state.reason = "begin_failed";
                        lastResults[human.player->GetObjectGuid()] = result;
                    }
                }
                else
                    for (auto const& human : humans)
                        autoWaits[{human.player->GetObjectGuid(), uint32(queueType)}].reason = "active";
            }
        }
        for (auto it = autoWaits.begin(); it != autoWaits.end();)
            if (!seen.count(it->first))
                it = autoWaits.erase(it);
            else
                ++it;
    }

    void Tick(uint32 diff)
    {
        std::lock_guard<std::recursive_mutex> guard(requestMutex);
        timer += diff;
        if (timer < 1000)
            return;
        timer = 0;
        for(auto it=requests.begin();it!=requests.end();) {
            Request& r=it->second;
            for(auto b=r.bots.begin();b!=r.bots.end();) {
                if(sPlayerBotMgr.HasBot(b->guid)){++b;continue;}
                if(sObjectAccessor.FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER,b->guid)))r.retiring.emplace(b->guid,*b);
                b=r.bots.erase(b);
            }
            BgFillLifecycle::PruneRetired(r.retiring,[](uint32 guid){return sPlayerBotMgr.HasBot(guid);},
                [](uint32 guid){return sObjectAccessor.FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER,guid))!=nullptr;});
            auto population=Population(r);auto plan=BgFillLifecycle::Decide(population);
            if(plan.cleanup) {
                for(auto const& human:r.humans)lastResults[human.first]=population.ended?"Battleground ended; its fill bots cleaned up.":"No real participants or pending arrivals remain; fill cleaned up.";
                Remove(r);it=requests.erase(it);continue;
            }
            for(Team team:{ALLIANCE,HORDE}) {
                unsigned t=team==ALLIANCE?0:1;
                if(plan.retire[t])RetireOne(r,team);
                else if(plan.add[t])AddOne(r,team);
            }
            ++it;
        }
        ScanAutomaticQueues();
    }
    void EquipMatched(Player* bot, CombatBotRoles role, Profile const& profile, std::string& result)
    {
        if (!bot || !bot->IsBot())
        {
            result = "Refused to alter a real player's gear.";
            return;
        }
        uint32 level = bot->GetLevel();
        bot->GetHonorMgr().SetHighestRank(profile.honorRank);
        HonorRankInfo rank;
        rank.rank = profile.honorRank;
        bot->GetHonorMgr().CalculateRankInfo(rank);
        bot->GetHonorMgr().SetRank(rank);
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (ScoredSlot(slot) && bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        std::map<uint8, std::vector<ItemPrototype const*>> ordinary, pvp;
        uint32 primary = Primary(bot->GetClass(), role);
        for (auto const& pair : sObjectMgr.GetItemPrototypeMap())
        {
            ItemPrototype const* item = &pair.second;
            if (!Eligible(bot, item, true))
                continue;
            uint8 slots[4];
            item->GetAllowedEquipSlots(slots, bot->GetClass(), bot->CanDualWield());
            for (uint8 slot : slots)
                if (slot < EQUIPMENT_SLOT_END && ScoredSlot(slot))
                {
                    uint32 skill = item->GetProficiencySkill();
                    if (item->Class == ITEM_CLASS_ARMOR && skill && item->InventoryType != INVTYPE_CLOAK && item->InventoryType != INVTYPE_SHIELD && skill != bot->GetHighestKnownArmorProficiency() && role != ROLE_HEALER)
                        continue;
                    if (slot == EQUIPMENT_SLOT_OFFHAND && item->InventoryType == INVTYPE_HOLDABLE && role == ROLE_MELEE_DPS)
                        continue;
                    (Military(item) ? pvp : ordinary)[slot].push_back(item);
                }
        }
        uint32 possible = pvp.size();
        uint32 requested = urand(profile.options.minPvpPieces, profile.options.maxPvpPieces);
        uint32 targetPvp = std::min(requested, possible);
        std::set<uint8> militarySlots;
        std::vector<uint8> available;
        for (auto const& pair : pvp)
            available.push_back(pair.first);
        while (militarySlots.size() < targetPvp)
        {
            size_t i = urand(0, available.size() - 1);
            militarySlots.insert(available[i]);
            available.erase(available.begin() + i);
        }
        float targetScore = profile.score * frand(profile.options.minFactor, profile.options.maxFactor);
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            auto const& pool = militarySlots.count(slot) ? pvp[slot] : ordinary[slot];
            ItemPrototype const* chosen = nullptr;
            float best = 1e30f;
            for (auto const* item : pool)
            {
                uint16 dest;
                InventoryResult equip = bot->CanEquipItem(slot, dest, item, nullptr, false);
                if (equip != EQUIP_ERR_OK && equip != EQUIP_ERR_CANT_EQUIP_RANK)
                    continue;
                float error = std::abs(float(item->ItemLevel) - targetScore);
                bool hasPrimary = false;
                for (auto const& stat : item->ItemStat)
                    if (stat.ItemStatType == primary && stat.ItemStatValue > 0)
                        hasPrimary = true;
                if (!hasPrimary)
                    error += 5;
                if (error < best)
                {
                    best = error;
                    chosen = item;
                }
            }
            if (chosen)
            {
                HonorForItem(bot, chosen);
                uint16 dest;
                if (bot->CanEquipItem(slot, dest, chosen, nullptr, false) == EQUIP_ERR_OK)
                    bot->StoreNewItemInBestSlots(chosen->ItemId, 1);
            }
        }
        // Empty source slots are part of the snapshot score. Do not overgear a fresh
        // character merely because the bot has legal candidates in every armor slot.
        for (uint32 step = 0; step < EQUIPMENT_SLOT_END && GearScore(bot) > profile.score * profile.options.maxFactor; ++step)
        {
            float totalWeight = 18.0f + (bot->GetClass() == CLASS_HUNTER ? 1.0f : 0.0f);
            float current = GearScore(bot), bestError = std::abs(current - targetScore);
            uint8 removeSlot = EQUIPMENT_SLOT_END;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                if (!ScoredSlot(slot) || slot == EQUIPMENT_SLOT_MAINHAND || (slot == EQUIPMENT_SLOT_RANGED && bot->GetClass() == CLASS_HUNTER))
                    continue;
                Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!item || Military(item->GetProto()))
                    continue;
                float reduced = current - float(item->GetProto()->ItemLevel) / totalWeight;
                if (reduced >= profile.score * profile.options.minFactor && std::abs(reduced - targetScore) < bestError)
                {
                    bestError = std::abs(reduced - targetScore);
                    removeSlot = slot;
                }
            }
            if (removeSlot == EQUIPMENT_SLOT_END)
                break;
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, removeSlot, true);
        }
        std::ostringstream out;
        float actual = GearScore(bot);
        uint32 pieces = CountPvpSetPieces(bot);
        out << "class=" << uint32(bot->GetClass()) << " role=" << uint32(role) << " actualWeightedItemLevel=" << actual << " requested=" << targetScore << " pvpPieces=" << pieces << " requestedPieces=" << requested;
        if (possible < requested)
            out << " limited: no level/class/skill/rank eligible military sets for requested count";
        if (actual < profile.score * profile.options.minFactor || actual > profile.score * profile.options.maxFactor)
            out << " limited: legal slot pool cannot meet score band";
        if (pieces < targetPvp)
            out << " limited: unique/slot requirements prevented requested pieces";
        if (bot->GetLevel() != level)
            out << " ERROR: level invariant violated";
        result = out.str();
    }
    bool EquipWeapons(Player* player, std::string& result)
    {
        if (!player || player->IsBot() || !player->IsAlive() || player->IsInCombat() || player->InBattleGround())
        {
            result = "Use weapon configuration outside combat and battlegrounds on a living real player.";
            return false;
        }
        enum Kind
        {
            CastMain,
            MeleeMain,
            MeleeOff,
            TwoHand,
            Shield,
            Holdable,
            Wand,
            HunterRanged,
            PhysicalRanged,
            Relic
        };
        uint8 cls = player->GetClass();
        bool healer = IsHealingPlayer(player);
        bool tank = (cls == CLASS_WARRIOR && player->HasSpell(23922)) || (cls == CLASS_PALADIN && player->HasSpell(20925));
        if (cls == CLASS_WARRIOR && player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            tank = tank || player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND)->GetProto()->InventoryType == INVTYPE_SHIELD;
        bool feral = cls == CLASS_DRUID && (player->HasSpell(17007) || player->GetShapeshiftForm() == FORM_BEAR || player->GetShapeshiftForm() == FORM_DIREBEAR || player->GetShapeshiftForm() == FORM_CAT);
        bool caster = cls == CLASS_MAGE || cls == CLASS_PRIEST || cls == CLASS_WARLOCK || (cls == CLASS_PALADIN && (healer || tank)) || (cls == CLASS_SHAMAN && !player->HasSpell(17364)) || (cls == CLASS_DRUID && !feral);
        std::map<uint8, ItemPrototype const*> plan;
        std::set<uint32> preferredRelics;
        uint32 relicTemplate = cls == CLASS_PALADIN ? (tank ? 31 : healer ? 26 : 18) : cls == CLASS_SHAMAN ? (healer ? 28 : player->HasSpell(17364) ? 0 : 22) : cls == CLASS_DRUID ? (feral ? 32 : healer ? 29 : 25) : 0;
        auto preset = sObjectMgr.GetPlayerPremadeGearTemplates().find(relicTemplate);
        if (preset != sObjectMgr.GetPlayerPremadeGearTemplates().end())
            for (auto const& entry : preset->second.items)
                if (!entry.requiredTeam || entry.requiredTeam == player->GetTeam())
                    if (ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry.itemId))
                        if (proto->InventoryType == INVTYPE_RELIC)
                            preferredRelics.insert(proto->ItemId);
        auto kindMatches = [&](ItemPrototype const* item, Kind kind)
        {
            switch (kind)
            {
            case CastMain:
            case MeleeMain:
                return item->Class == ITEM_CLASS_WEAPON && (item->InventoryType == INVTYPE_WEAPON || item->InventoryType == INVTYPE_WEAPONMAINHAND);
            case MeleeOff:
                return item->Class == ITEM_CLASS_WEAPON && player->CanDualWield() && (item->InventoryType == INVTYPE_WEAPON || item->InventoryType == INVTYPE_WEAPONOFFHAND);
            case TwoHand:
                return item->Class == ITEM_CLASS_WEAPON && item->InventoryType == INVTYPE_2HWEAPON;
            case Shield:
                return item->InventoryType == INVTYPE_SHIELD;
            case Holdable:
                return item->InventoryType == INVTYPE_HOLDABLE;
            case Wand:
                return item->Class == ITEM_CLASS_WEAPON && item->SubClass == ITEM_SUBCLASS_WEAPON_WAND;
            case HunterRanged:
                return item->Class == ITEM_CLASS_WEAPON && (item->SubClass == ITEM_SUBCLASS_WEAPON_BOW || item->SubClass == ITEM_SUBCLASS_WEAPON_GUN || item->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW);
            case PhysicalRanged:
                return item->Class == ITEM_CLASS_WEAPON && (item->SubClass == ITEM_SUBCLASS_WEAPON_BOW || item->SubClass == ITEM_SUBCLASS_WEAPON_GUN || item->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW || item->SubClass == ITEM_SUBCLASS_WEAPON_THROWN);
            case Relic:
                return item->InventoryType == INVTYPE_RELIC;
            }
            return false;
        };
        auto score = [&](ItemPrototype const* item, Kind kind)
        {
            float intellect = 0, strength = 0, agility = 0, stamina = 0, spellDamage = 0, healing = 0;
            for (auto const& stat : item->ItemStat)
            {
                if (stat.ItemStatType == ITEM_MOD_INTELLECT)
                    intellect += stat.ItemStatValue;
                if (stat.ItemStatType == ITEM_MOD_STRENGTH)
                    strength += stat.ItemStatValue;
                if (stat.ItemStatType == ITEM_MOD_AGILITY)
                    agility += stat.ItemStatValue;
                if (stat.ItemStatType == ITEM_MOD_STAMINA)
                    stamina += stat.ItemStatValue;
            }
            for (auto const& effect : item->Spells)
                if (effect.SpellId && effect.SpellTrigger == ITEM_SPELLTRIGGER_ON_EQUIP)
                    if (SpellEntry const* spell = sSpellMgr.GetSpellEntry(effect.SpellId))
                        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
                        {
                            if (spell->EffectApplyAuraName[i] == SPELL_AURA_MOD_HEALING_DONE)
                                healing += spell->EffectBasePoints[i] + 1;
                            if (spell->EffectApplyAuraName[i] == SPELL_AURA_MOD_DAMAGE_DONE && (spell->EffectMiscValue[i] & ~1))
                                spellDamage += spell->EffectBasePoints[i] + 1;
                        }
            bool casterSlot = kind == CastMain || kind == Holdable || kind == Wand;
            if (casterSlot && kind != Wand && intellect <= 0 && spellDamage <= 0 && healing <= 0)
                return -1.0f;
            if (casterSlot && !healer && healing > 0 && spellDamage <= 0)
                return -1.0f;
            if ((kind == TwoHand || kind == MeleeMain || kind == MeleeOff) && (spellDamage > 0 || healing > 0) && strength <= 0 && agility <= 0)
                return -1.0f;
            float value = item->ItemLevel;
            if (casterSlot || (caster && kind == Shield))
                value += intellect * 2 + (healer ? healing * 8 + spellDamage : spellDamage * 8);
            else
            {
                value += strength * 2 + agility * (cls == CLASS_HUNTER || cls == CLASS_ROGUE || feral ? 4 : 1) + stamina;
                if (item->Class == ITEM_CLASS_WEAPON && !feral)
                {
                    float damage = 0;
                    for (auto const& d : item->Damage)
                        damage += (d.DamageMin + d.DamageMax) * 0.5f;
                    if (item->Delay)
                        value += damage * 1000 / item->Delay * (cls == CLASS_HUNTER && kind != HunterRanged ? 1 : 10);
                    if (cls == CLASS_ROGUE)
                        value += (kind == MeleeOff ? -1 : 1) * float(item->Delay) / 100;
                }
            }
            if (item->Class == ITEM_CLASS_WEAPON)
                for (auto const& known : player->GetSpellMap())
                    if (player->HasSpell(known.first))
                        if (SpellEntry const* spell = sSpellMgr.GetSpellEntry(known.first))
                            if (spell->IsPassiveSpell() && spell->EquippedItemClass == ITEM_CLASS_WEAPON && spell->EquippedItemSubClassMask && (uint32(spell->EquippedItemSubClassMask) & (1u << item->SubClass)) && (!spell->EquippedItemInventoryTypeMask || (uint32(spell->EquippedItemInventoryTypeMask) & (1u << item->InventoryType))))
                                value += 500;
            if (kind == Relic && preferredRelics.count(item->ItemId))
                value += 5000;
            if (BotMatchDetail::PvpTeamMask(item) && item->RequiredHonorRank)
                value += 10000;
            return value;
        };
        auto pick = [&](uint8 slot, Kind kind)
        {
            ItemPrototype const* best = nullptr;
            float bestScore = -1;
            for (auto const& pair : sObjectMgr.GetItemPrototypeMap())
            {
                auto const* item = &pair.second;
                if (!kindMatches(item, kind) || !Eligible(player, item, true) || item->Quality < 2 || item->Quality > 4)
                    continue;
                uint8 slots[4];
                item->GetAllowedEquipSlots(slots, cls, player->CanDualWield());
                if (std::find(slots, slots + 4, slot) == slots + 4)
                    continue;
                bool duplicate = false;
                for (auto const& chosen : plan)
                    if (chosen.second->ItemId == item->ItemId && item->MaxCount == 1)
                        duplicate = true;
                if (duplicate)
                    continue;
                Item* existing = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!existing || existing->GetEntry() != item->ItemId)
                {
                    uint16 dest;
                    InventoryResult equip = player->CanEquipItem(slot, dest, item, nullptr, true);
                    bool offAfterTwoHand = slot == EQUIPMENT_SLOT_OFFHAND && plan.count(EQUIPMENT_SLOT_MAINHAND) && plan.at(EQUIPMENT_SLOT_MAINHAND)->InventoryType != INVTYPE_2HWEAPON;
                    if (equip != EQUIP_ERR_OK && equip != EQUIP_ERR_CANT_EQUIP_RANK && !(equip == EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED && offAfterTwoHand))
                        continue;
                }
                float value = score(item, kind);
                if (value > bestScore)
                {
                    best = item;
                    bestScore = value;
                }
            }
            if (best)
                plan[slot] = best;
            return best != nullptr;
        };
        bool complete = false;
        if (caster)
        {
            complete = pick(EQUIPMENT_SLOT_MAINHAND, CastMain);
            bool shield = (cls == CLASS_PALADIN || cls == CLASS_SHAMAN);
            complete = (shield ? pick(EQUIPMENT_SLOT_OFFHAND, Shield) : pick(EQUIPMENT_SLOT_OFFHAND, Holdable)) && complete;
            if (cls == CLASS_MAGE || cls == CLASS_PRIEST || cls == CLASS_WARLOCK)
                complete = pick(EQUIPMENT_SLOT_RANGED, Wand) && complete;
            else
                complete = pick(EQUIPMENT_SLOT_RANGED, Relic) && complete;
        }
        else if (tank)
        {
            complete = pick(EQUIPMENT_SLOT_MAINHAND, MeleeMain) && pick(EQUIPMENT_SLOT_OFFHAND, Shield);
            if (cls == CLASS_WARRIOR)
                pick(EQUIPMENT_SLOT_RANGED, PhysicalRanged);
            else
                pick(EQUIPMENT_SLOT_RANGED, Relic);
        }
        else if (cls == CLASS_ROGUE || (cls == CLASS_WARRIOR && player->HasSpell(23881) && player->CanDualWield()))
        {
            complete = pick(EQUIPMENT_SLOT_MAINHAND, MeleeMain) && pick(EQUIPMENT_SLOT_OFFHAND, MeleeOff);
            pick(EQUIPMENT_SLOT_RANGED, PhysicalRanged);
        }
        else
        {
            complete = pick(EQUIPMENT_SLOT_MAINHAND, TwoHand);
            if (!complete && cls == CLASS_HUNTER)
            {
                complete = pick(EQUIPMENT_SLOT_MAINHAND, MeleeMain);
                if (player->CanDualWield())
                    complete = pick(EQUIPMENT_SLOT_OFFHAND, MeleeOff) && complete;
            }
            if (cls == CLASS_HUNTER)
                complete = pick(EQUIPMENT_SLOT_RANGED, HunterRanged) && complete;
            else if (cls == CLASS_WARRIOR)
                pick(EQUIPMENT_SLOT_RANGED, PhysicalRanged);
            else
                pick(EQUIPMENT_SLOT_RANGED, Relic);
        }
        if (!complete)
        {
            result = "No legal complete weapon plan for current class/level/learned skills; nothing changed.";
            return false;
        }
        std::set<uint8> clear;
        if (!plan.count(EQUIPMENT_SLOT_OFFHAND) && player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            clear.insert(EQUIPMENT_SLOT_OFFHAND);
        for (uint8 slot : {uint8(EQUIPMENT_SLOT_MAINHAND), uint8(EQUIPMENT_SLOT_OFFHAND), uint8(EQUIPMENT_SLOT_RANGED)})
            if (!plan.count(slot))
                if (Item* old = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (!Eligible(player, old->GetProto(), true))
                        clear.insert(slot);
        uint32 newItems = 0;
        for (auto const& pair : plan)
        {
            Item* old = player->GetItemByPos(INVENTORY_SLOT_BAG_0, pair.first);
            if (!old || old->GetEntry() != pair.second->ItemId)
            {
                ++newItems;
                if (old && player->CanUnequipItem(old->GetPos(), true) != EQUIP_ERR_OK)
                {
                    result = "Current weapon cannot be safely moved; nothing changed.";
                    return false;
                }
            }
        }
        for (uint8 slot : clear)
            if (player->CanUnequipItem(uint16(INVENTORY_SLOT_BAG_0) << 8 | slot, true) != EQUIP_ERR_OK)
            {
                result = "Current weapon slot cannot be safely moved; nothing changed.";
                return false;
            }
        if (OrdinaryFreeSlots(player) < newItems + clear.size())
        {
            result = "Free ordinary bag slots for replacement weapons and old offhand first; nothing changed.";
            return false;
        }
        uint32 level = player->GetLevel();
        uint8 rank = player->GetHonorMgr().GetHighestRank().visualRank;
        for (uint8 slot : clear)
        {
            Item* old = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            ItemPosCountVec destinations;
            uint8 bagSlot = 0;
            if (player->CanStoreItem(NULL_BAG, NULL_SLOT, destinations, old, bagSlot, false) != EQUIP_ERR_OK || destinations.empty())
            {
                result = "Could not preserve old weapon; it remains equipped.";
                return false;
            }
            player->SwapItem(old->GetPos(), destinations.front().pos);
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                result = "Old weapon was retained; configuration stopped.";
                return false;
            }
        }
        for (auto const& pair : plan)
            HonorForItem(player, pair.second);
        for (auto const& pair : plan)
        {
            Item* old = player->GetItemByPos(INVENTORY_SLOT_BAG_0, pair.first);
            if (old && old->GetEntry() == pair.second->ItemId)
                continue;
            Item* item = player->StoreNewItemInInventorySlot(pair.second->ItemId, 1);
            if (!item)
            {
                result = "Weapon creation stopped; original items were preserved.";
                return false;
            }
            player->SwapItem(item->GetPos(), uint16(INVENTORY_SLOT_BAG_0) << 8 | pair.first);
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, pair.first) != item)
            {
                result = "Weapon equip rejected; original gear preserved and new item remains in inventory.";
                return false;
            }
        }
        if (player->GetLevel() != level)
        {
            result = "Weapon configuration level invariant failed.";
            return false;
        }
        auto equippedId = [&](uint8 slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            return item ? item->GetEntry() : 0;
        };
        result = "Weapons configured; main=" + std::to_string(equippedId(EQUIPMENT_SLOT_MAINHAND)) + " off=" + std::to_string(equippedId(EQUIPMENT_SLOT_OFFHAND)) + " ranged=" + std::to_string(equippedId(EQUIPMENT_SLOT_RANGED)) + "; old equipment in inventory; level unchanged; highest rank " + std::to_string(rank) + "->" + std::to_string(player->GetHonorMgr().GetHighestRank().visualRank);
        return true;
    }

    bool EquipPvpTestPieces(Player* player, uint32 count, std::string& result, uint32 quality)
    {
        if (!player || player->IsBot() || !player->IsAlive() || player->IsInCombat() || player->InBattleGround())
        {
            result = "Use this outside combat and battlegrounds on a living real player.";
            return false;
        }
        if (count > 6 || (count && quality != 3 && quality != 4))
        {
            result = "Choose a final total of 0-6 military pieces and blue(3)/epic(4) quality.";
            return false;
        }

        // Keep eligible existing pieces first. The requested count is an absolute
        // final equipped total, not the number of items to add on this click.
        std::map<uint8, ItemPrototype const*> selected;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END && selected.size() < count; ++slot)
        {
            Item* existing = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (existing && Military(existing->GetProto()) && existing->GetProto()->Quality == quality && Eligible(player, existing->GetProto(), true))
                selected[slot] = existing->GetProto();
        }
        std::map<uint8, ItemPrototype const*> candidates;
        if (selected.size() < count)
        {
            for (auto const& pair : sObjectMgr.GetItemPrototypeMap())
            {
                auto const* item = &pair.second;
                if (!Military(item) || item->Quality != quality || !Eligible(player, item, true))
                    continue;
                uint8 slots[4];
                item->GetAllowedEquipSlots(slots, player->GetClass(), player->CanDualWield());
                for (uint8 slot : slots)
                {
                    if (slot >= EQUIPMENT_SLOT_END || !ScoredSlot(slot) || selected.count(slot))
                        continue;
                    uint16 dest;
                    InventoryResult equip = player->CanEquipItem(slot, dest, item, nullptr, true);
                    if (equip != EQUIP_ERR_OK && equip != EQUIP_ERR_CANT_EQUIP_RANK)
                        continue;
                    if (!candidates.count(slot) || candidates[slot]->ItemLevel < item->ItemLevel)
                        candidates[slot] = item;
                }
            }
            for (auto const& pair : candidates)
            {
                if (selected.size() == count)
                    break;
                selected.emplace(pair.first, pair.second);
            }
        }
        if (selected.size() != count)
        {
            result = "Not enough genuine military set pieces meet this character's level/class/race/skill requirements; nothing changed.";
            return false;
        }

        std::vector<uint8> removeSlots;
        uint32 newItems = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* existing = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            auto pick = selected.find(slot);
            bool replacing = pick != selected.end() && (!existing || existing->GetEntry() != pick->second->ItemId);
            bool removing = existing && Military(existing->GetProto()) && pick == selected.end();
            if (replacing)
                ++newItems;
            if (removing)
                removeSlots.push_back(slot);
            if (existing && (replacing || removing) && player->CanUnequipItem(existing->GetPos(), true) != EQUIP_ERR_OK)
            {
                result = "An equipped item cannot be moved to the inventory; nothing changed.";
                return false;
            }
        }
        uint32 requiredSpace = newItems + uint32(removeSlots.size());
        if (OrdinaryFreeSlots(player) < requiredSpace)
        {
            result = "Free " + std::to_string(requiredSpace) + " ordinary bag slots for new/replaced and extra military gear first; nothing changed.";
            return false;
        }

        // Every candidate, affected equipped slot and required bag space has
        // passed preflight before the explicitly authorized honor-rank change.
        uint32 level = player->GetLevel();
        uint8 oldRank = player->GetHonorMgr().GetHighestRank().visualRank;
        for (auto const& pair : selected)
            HonorForItem(player, pair.second);
        for (auto const& pair : selected)
        {
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, pair.first);
            if (current && current->GetEntry() == pair.second->ItemId)
                continue;
            Item* item = player->StoreNewItemInInventorySlot(pair.second->ItemId, 1);
            if (!item)
            {
                result = "Inventory changed after preflight; existing gear was retained. Final military count=" + std::to_string(CountPvpSetPieces(player));
                return false;
            }
            player->SwapItem(item->GetPos(), uint16(INVENTORY_SLOT_BAG_0) << 8 | pair.first);
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, pair.first) != item)
            {
                result = "Equip rejected; original equipment retained and new item remains in inventory. Final military count=" + std::to_string(CountPvpSetPieces(player));
                return false;
            }
        }
        for (uint8 slot : removeSlots)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item || !Military(item->GetProto()))
                continue;
            ItemPosCountVec destinations;
            uint8 bagSlot = 0;
            if (player->CanStoreItem(NULL_BAG, NULL_SLOT, destinations, item, bagSlot, false) != EQUIP_ERR_OK || destinations.empty())
            {
                result = "Inventory changed after preflight; extra military gear remains equipped. Final military count=" + std::to_string(CountPvpSetPieces(player));
                return false;
            }
            player->SwapItem(item->GetPos(), destinations.front().pos);
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) == item)
            {
                result = "Could not move extra military gear; it was retained, not deleted. Final military count=" + std::to_string(CountPvpSetPieces(player));
                return false;
            }
        }
        uint32 actual = CountPvpSetPieces(player);
        if (player->GetLevel() != level || actual != count)
        {
            result = "Final military count verification failed: requested=" + std::to_string(count) + " actual=" + std::to_string(actual) + "; items were retained.";
            return false;
        }
        result = "Final military set pieces=" + std::to_string(actual) + "; replaced/extra gear preserved in inventory; level unchanged; highest honor rank " + std::to_string(oldRank) + "->" + std::to_string(player->GetHonorMgr().GetHighestRank().visualRank);
        return true;
    }

} // namespace BotMatch
