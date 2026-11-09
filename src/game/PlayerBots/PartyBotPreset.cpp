// Explicit PvE presets for new, temporary level-60 party bots. No real-player mutations.
#include "PartyBotPreset.h"
#include "RaidBotPlanner.h"
#include "Timer.h"
#include "PartyBotAI.h"
#include "PlayerBotMgr.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "Map.h"
#include "Chat.h"
#include "Log.h"
#include "SpellMgr.h"
#include "DBCStores.h"
#include "World.h"
#include "Utilities/Random.h"
#include <map>
#include <set>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <sstream>
namespace PartyPreset
{
namespace
{
using Slots = std::map<uint8,PlayerPremadeItem>;
bool Initialized(PartyBotAI const& ai)
{
    // Registry state and the AI-completion flag precede any Player dereference.
    return ai.botEntry && ai.botEntry->state==PB_STATE_ONLINE && ai.m_initialized && ai.me;
}
bool Ready(PartyBotAI const& ai)
{
    return Initialized(ai) && !ai.botEntry->requestRemoval && ai.me->IsInWorld();
}

bool Scored(uint8 slot) { return slot < EQUIPMENT_SLOT_END && slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD; }
float Metric(ItemPrototype const* item) { return item ? float(item->ItemLevel)*(0.7f+0.1f*std::min<uint32>(5,item->Quality)) : 0; }
float Weight(uint8 slot,uint8 cls) { return slot == EQUIPMENT_SLOT_MAINHAND || (cls==CLASS_HUNTER && slot==EQUIPMENT_SLOT_RANGED) ? 2.0f : 1.0f; }
bool ClassAndRace(Player const* player,ItemPrototype const* item)
{
    return item && (item->AllowableClass & player->GetClassMask()) && (item->AllowableRace & player->GetRaceMask()) && item->RequiredLevel<=60;
}
Slots Canonical(Player const* context,PlayerPremadeGearTemplate const& gear)
{
    Slots result; bool twoHand=false;
    bool dual=context->GetClass()==CLASS_WARRIOR || context->GetClass()==CLASS_ROGUE || context->GetClass()==CLASS_HUNTER;
    for (auto const& row : gear.items)
    {
        if (row.requiredTeam && row.requiredTeam!=uint32(context->GetTeam())) continue;
        ItemPrototype const* item=sObjectMgr.GetItemPrototype(row.itemId);
        if (!ClassAndRace(context,item)) continue;
        uint8 slots[4];item->GetAllowedEquipSlots(slots,context->GetClass(),dual);
        for (uint8 slot : slots)
        {
            if (!Scored(slot) || result.count(slot) || (slot==EQUIPMENT_SLOT_OFFHAND && twoHand)) continue;
            if (item->InventoryType==INVTYPE_2HWEAPON && result.count(EQUIPMENT_SLOT_OFFHAND)) continue;
            result.emplace(slot,row);
            if (slot==EQUIPMENT_SLOT_MAINHAND && item->InventoryType==INVTYPE_2HWEAPON) twoHand=true;
            break;
        }
    }
    return result;
}
float Mean(Player const* context,Slots const& slots)
{
    float total=0,weights=0;
    for (auto const& pair:slots) { float w=Weight(pair.first,context->GetClass());total+=Metric(sObjectMgr.GetItemPrototype(pair.second.itemId))*w;weights+=w; }
    return weights ? total/weights : 0;
}
float OwnerRatio(Player const* owner,uint32& reference)
{
    float best=-1;Slots chosen;
    auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();auto const& gears=sObjectMgr.GetPlayerPremadeGearTemplates();
    for (auto const& mapping:Mappings())
    {
        auto spec=specs.find(mapping.spec);auto gear=gears.find(mapping.phase6);
        if (!mapping.phase6 || spec==specs.end() || gear==gears.end() || spec->second.requiredClass!=owner->GetClass()) continue;
        Slots slots=Canonical(owner,gear->second);if (slots.empty()) continue;
        uint32 talents=0,known=0,worn=0;
        for (uint32 spell:spec->second.spells)
        {
            uint32 first=sSpellMgr.GetFirstSpellInChain(spell);
            if (GetTalentSpellPos(first ? first : spell)) { ++talents;if(owner->HasSpell(spell))++known; }
        }
        for (auto const& pair:slots)
            if (Item* actual=owner->GetItemByPos(INVENTORY_SLOT_BAG_0,pair.first))
                if (actual->GetEntry()==pair.second.itemId) ++worn;
        float fit=(talents ? float(known)/talents : 0)*1000 + worn*10 + Mean(owner,slots)*0.001f;
        if (fit>best) { best=fit;chosen=slots;reference=mapping.spec; }
    }
    if (chosen.empty()) return 0;
    float total=0,weights=0;
    for (auto const& pair:chosen)
    {
        float baseline=Metric(sObjectMgr.GetItemPrototype(pair.second.itemId));if(baseline<=0)continue;
        Item* actual=owner->GetItemByPos(INVENTORY_SLOT_BAG_0,pair.first);
        if(!actual && pair.first==EQUIPMENT_SLOT_OFFHAND)
        {
            Item* main=owner->GetItemByPos(INVENTORY_SLOT_BAG_0,EQUIPMENT_SLOT_MAINHAND);
            if(main && main->GetProto()->InventoryType==INVTYPE_2HWEAPON)actual=main;
        }
        float w=Weight(pair.first,owner->GetClass());weights+=w;
        total+=w*std::min(1.0f,actual ? Metric(actual->GetProto())/baseline : 0);
    }
    return weights ? total/weights : 0;
}
bool OwnGroup(Player const* owner,std::string& result,bool mixed = false)
{
    if (Group const* group=owner->GetGroup())
    {
        if (group->GetLeaderGuid()!=owner->GetObjectGuid()) { result="Only the group leader may add a preset companion.";return false; }
        std::set<uint64> owned;
        for(auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid(),true))
            if(ai->botEntry)owned.insert(ai->botEntry->playerGUID);
        for (auto const& member:group->GetMemberSlots())
        {
            if (member.guid==owner->GetObjectGuid()) continue;
            if(!mixed && !owned.count(member.guid.GetRawValue()))
            { result="Preset companions require an owner-only group; another real/offline player or foreign bot needs separate authorization.";return false; }
        }
    }
    return true;
}
uint32 PendingGroup(Player const* owner)
{
    uint32 count=0;for (auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid()))
        if (!owner->GetGroup() || !Initialized(*ai) || ai->me->GetGroup()!=owner->GetGroup()) ++count;
    return count;
}
bool SpellCovered(Player const* player,uint32 spell)
{
    if(player->HasSpell(spell))return true;
    uint32 first=sSpellMgr.GetFirstSpellInChain(spell);
    if(!first)return false;
    uint8 rank=sSpellMgr.GetSpellRank(spell);
    for(auto const& known:player->GetSpellMap())
        if(known.second.state!=PLAYERSPELL_REMOVED && !known.second.disabled && sSpellMgr.GetFirstSpellInChain(known.first)==first && sSpellMgr.GetSpellRank(known.first)>=rank)return true;
    return false;
}
bool FullSpecKnown(PartyBotAI const& ai)
{
    auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();auto spec=specs.find(ai.m_presetSpecId);
    if(!Ready(ai) || spec==specs.end() || spec->second.spells.empty() || spec->second.level!=60 || ai.me->GetLevel()!=60 || spec->second.requiredClass!=ai.me->GetClass() || spec->second.role!=ai.GetRole())return false;
    for(uint32 spell:spec->second.spells)if(!SpellCovered(ai.me,spell))return false;
    return true;
}
bool Graduated(PartyBotAI const& ai)
{
    Mapping const* mapping=Find(ai.m_presetSpecId);
    auto const& gears=sObjectMgr.GetPlayerPremadeGearTemplates();
    if(!Ready(ai) || !mapping || !mapping->phase6 || !gears.count(mapping->phase6))return false;
    Slots expected=Canonical(ai.me,gears.at(mapping->phase6));
    if(expected.empty())return false;
    for(auto const& pair:expected)
    {
        Item* actual=ai.me->GetItemByPos(INVENTORY_SLOT_BAG_0,pair.first);
        if(!actual || actual->GetEntry()!=pair.second.itemId || actual->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)!=pair.second.enchantId)return false;
    }
    return true;
}
bool ChosenEquipmentMatches(Player const* player,PartyBotAI const& ai)
{
    // Initialization calls this with its validated, fully spawned local Player.
    // Read-only status must instead pass through Ready below.
    if(!player || !ai.m_presetExpectedItems || ai.m_presetChosenItems.size()!=ai.m_presetExpectedItems)return false;
    for(auto const& pair:ai.m_presetChosenItems)
    {
        Item* item=player->GetItemByPos(INVENTORY_SLOT_BAG_0,pair.first);
        if(!item || item->GetEntry()!=pair.second.first || item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)!=pair.second.second)return false;
    }
    return true;
}
bool LoadoutMatches(PartyBotAI const& ai) { return Ready(ai) && ChosenEquipmentMatches(ai.me,ai); }
float TankYellowMiss63(Player const* bot)
{
    uint32 skill=bot->GetClass()==CLASS_DRUID ? bot->GetSkillMaxForLevel() : bot->GetWeaponSkillValue(BASE_ATTACK);
    int32 difference=int32(skill)-315;
    float hit=bot->GetWeaponBasedAuraModifier(BASE_ATTACK,SPELL_AURA_MOD_HIT_CHANCE);
    float effective=hit-(difference < -10 && hit > 0 ? 1.0f : 0.0f);
    return std::max(0.0f,std::min(60.0f,5.0f-difference*(difference < -10 ? 0.2f : 0.1f)-effective));
}
uint32 Healers(Player const* owner,bool includePending=true)
{
    uint32 count=0;auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();
    for (auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid()))
    {
        auto spec=specs.find(ai->m_presetSpecId);
        if(spec==specs.end() || spec->second.level!=60 || spec->second.role!=ROLE_HEALER || spec->second.requiredClass!=ai->m_class || ai->m_role!=ROLE_HEALER || !Find(ai->m_presetSpecId) || !Find(ai->m_presetSpecId)->phase6)continue;
        // Pending healers are plans only: GetRole may inspect duel/player state.
        if(!Initialized(*ai)) { if(includePending)++count;continue; }
        if(Ready(*ai) && ai->GetRole()==ROLE_HEALER && Graduated(*ai) && FullSpecKnown(*ai))++count;
    }
    return count;
}
bool SameFunction(ItemPrototype const* item,ItemPrototype const* anchor)
{
    return item->Class==anchor->Class && item->SubClass==anchor->SubClass && item->InventoryType==anchor->InventoryType;
}
}
uint32 VerifiedHealers(Player const* owner) { return Healers(owner,false); }
std::vector<Mapping> const& Mappings()
{
    static std::vector<Mapping> const rows={{1,1,17},{14,14,30},{2,2,18},{10,10,26},{15,15,31},{3,3,19},{4,4,20},{5,5,21},{11,11,27},{6,6,22},{12,12,28},{41,0,0},{7,7,23},{8,8,24},{9,9,25},{13,13,29},{16,16,32}};
    return rows;
}
Mapping const* Find(uint32 id) { for(auto const& row:Mappings())if(row.spec==id)return &row;return nullptr; }
bool Supported(Player const* owner,Mapping const& mapping,std::string& reason)
{
    auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();auto const& gears=sObjectMgr.GetPlayerPremadeGearTemplates();
    auto spec=specs.find(mapping.spec);auto gear=gears.find(mapping.phase6);
    if (!mapping.phase6) { reason="no_phase6";return false; }
    auto pre=gears.find(mapping.preraid);
    if (spec==specs.end() || gear==gears.end() || pre==gears.end() || spec->second.level!=60 || gear->second.level!=60 || pre->second.level!=60 || spec->second.requiredClass!=gear->second.requiredClass || spec->second.role!=gear->second.role || pre->second.requiredClass!=spec->second.requiredClass || pre->second.role!=spec->second.role)
    { reason="template_mismatch";return false; }
    if ((owner->GetTeam()==ALLIANCE && spec->second.requiredClass==CLASS_SHAMAN) || (owner->GetTeam()==HORDE && spec->second.requiredClass==CLASS_PALADIN))
    { reason="faction";return false; }
    if(owner->GetLevel()!=60) {reason="level60";return false;}
    reason="none";return true;
}
bool Queue(Player* owner,uint32 id,std::string& result,bool quickRaid)
{
    if(!owner || owner->IsBot() || !owner->GetSession() || owner->GetSession()->GetSecurity()<SEC_ADMINISTRATOR) {result="Administrator player required.";return false;}
    Mapping const* mapping=Find(id);if(!mapping){result="Unknown trusted party spec.";return false;}
    if(!Supported(owner,*mapping,result))
    {
        if(result=="no_phase6")result="This spec has no verified phase-6 gear template.";
        else if(result=="faction")result="This class is not available for your faction.";
        else if(result=="level60")result="Graduation companions require a level-60 player; your level is unchanged.";
        else result="Verified spec and gear templates are missing or inconsistent.";
        return false;
    }
    if(!owner->IsAlive() || owner->IsInCombat() || owner->InBattleGround() || owner->InBattleGroundQueue() || owner->IsTaxiFlying() || owner->IsBeingTeleported())
    {result="Leave combat, battleground, queue and taxi flight before summoning preset companions.";return false;}
    if(!OwnGroup(owner,result,quickRaid))return false;
    Group const* group=owner->GetGroup();uint32 members=group ? group->GetMembersCount() : 1,pending=PendingGroup(owner);
    uint32 cap=group && group->isRaidGroup() ? MAX_RAID_SIZE : MAX_GROUP_SIZE;
    if (owner->GetMap()->IsDungeon())cap=std::min<uint32>(cap,owner->GetMap()->GetMapEntry()->maxPlayers);
    if(members+pending>=cap){result="Party capacity is full, including companions still loading.";return false;}
    uint32 configured=sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_MAX_BOTS);
    if(configured && members-1+pending>=configured){result="Party bot limit reached, including loading companions.";return false;}
    auto const& spec=sObjectMgr.GetPlayerPremadeSpecTemplates().at(id);
    if(spec.role!=ROLE_HEALER && members+pending+1>=cap && !Healers(owner))
    {result="Reserve the last party slot for a phase-6 healer.";return false;}
    if(owner->GetMap()->IsDungeon())
    {
        uint32 pendingMap=0;for(auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid()))
        {
            if(Initialized(*ai)) { if(!ai->me->IsInWorld() || ai->me->GetMap()!=owner->GetMap())++pendingMap; }
            else
            {
                // Successful new-session spawning adds the player to the planned
                // map before the manager changes LOADING to ONLINE. No pending
                // Player pointer is needed to reserve the not-yet-spawned cases.
                bool spawnedHere=ai->botEntry && ai->botEntry->state==PB_STATE_ONLINE && ai->m_race && ai->m_class && ai->m_mapId==owner->GetMapId() && ai->m_instanceId==owner->GetMap()->GetInstanceId();
                if(!spawnedHere)++pendingMap;
            }
        }
        if(owner->GetMap()->GetPlayers().getSize()+pendingMap>=owner->GetMap()->GetMapEntry()->maxPlayers)
        {result="Instance capacity is full, including loading companions.";return false;}
    }
    uint8 race=SelectRandomRaceForClass(spec.requiredClass,owner->GetTeam());if(!race){result="Spec class is unavailable for this faction.";return false;}
    float x,y,z;owner->GetNearPoint(owner,x,y,z,0,5.0f,frand(0.0f,6.0f));
    std::unique_ptr<PartyBotAI> ai(new PartyBotAI(owner,nullptr,spec.role,race,spec.requiredClass,60,owner->GetMapId(),owner->GetMap()->GetInstanceId(),x,y,z,owner->GetOrientation()));
    ai->m_presetSpecId=id;ai->m_presetGearId=mapping->phase6;ai->m_quickRaid=quickRaid;
    ai->m_ownerGearRatio=OwnerRatio(owner,ai->m_ownerReferenceSpec);
    if(!sPlayerBotMgr.AddOwnedBot(std::move(ai))){result="Unable to queue preset companion.";return false;}
    result="Preset companion queued; inspect party status for actual talents and equipment.";return true;
}
bool Initialize(PartyBotAI& ai,std::string& result)
{
    Player* bot=ai.me;Player* owner=ObjectAccessor::FindPlayer(ai.m_leaderGuid);
    if(!bot || !bot->IsBot() || !owner || bot->GetLevel()!=60 || !ai.m_race || !ai.m_class) {result="Preset initialization requires a new temporary level-60 bot.";return false;}
    Mapping const* mapping=Find(ai.m_presetSpecId);if(!mapping || !Supported(owner,*mapping,result) || !OwnGroup(owner,result,ai.m_quickRaid))return false;
    if(bot->GetGroup()!=owner->GetGroup() || !bot->GetGroup()){result="Companion could not join the owner group.";return false;}
    auto const& spec=sObjectMgr.GetPlayerPremadeSpecTemplates().at(mapping->spec);
    auto const& gears=sObjectMgr.GetPlayerPremadeGearTemplates();auto phase=gears.find(mapping->phase6),pre=gears.find(mapping->preraid);
    if(phase==gears.end() || pre==gears.end()){result="Missing verified PvE gear anchors.";return false;}
    if(ai.m_quickRaid && (owner->GetMapId()!=ai.m_mapId || owner->GetMap()->GetInstanceId()!=ai.m_instanceId || owner->IsInCombat())){result="Raid location or combat state changed while companions were loading.";return false;}
    ai.m_role=spec.role;
    sObjectMgr.ApplyPremadeSpecTemplateToPlayer(mapping->spec,bot);
    uint32 known=0;for(uint32 spell:spec.spells)if(SpellCovered(bot,spell))++known;
    ai.m_presetKnownSpells=known;ai.m_presetTotalSpells=uint32(spec.spells.size());
    if(!known || known!=spec.spells.size()){result="Preset spell application was incomplete; companion removed.";return false;}
    Slots originalGraduation=Canonical(bot,phase->second),graduation=originalGraduation,chosen;
    if(graduation.empty()){result="Graduation template has no legal faction equipment.";return false;}
    bool tankHit=mapping->spec==15;
    if(tankHit)
    {
        // Explicit hit-first protection-paladin reference revision: keep Styleen,
        // replace only the Onyxia trinket with a genuine legal P6 candidate.
        // Do not edit the database template or grant an artificial hit aura.
        ItemPrototype const* drake=sObjectMgr.GetItemPrototype(19406);
        if(!ClassAndRace(bot,drake) || drake->InventoryType!=INVTYPE_TRINKET)
        {result="Paladin hit-reference trinket is unavailable.";return false;}
        bool replaced=false;
        for(auto& pair:graduation)
        {
            if(pair.second.itemId==19406){result="Paladin reference already contains a unique Drake Fang Talisman.";return false;}
            if(pair.second.itemId==18406){pair.second=PlayerPremadeItem(19406,0,0);replaced=true;}
        }
        if(!replaced){result="Paladin reference no longer contains the reviewed Onyxia trinket.";return false;}
    }
    ai.m_presetExpectedItems=uint32(graduation.size());ai.m_presetChosenItems.clear();
    // Starting equipment must not block planning the offhand of a one-hand
    // caster set (e.g. a newly created druid begins with a two-handed staff).
    for(uint8 slot=EQUIPMENT_SLOT_START;slot<EQUIPMENT_SLOT_END;++slot)bot->AutoUnequipItemFromSlot(slot);
    bool fixed=spec.role==ROLE_TANK || spec.role==ROLE_HEALER || ai.m_ownerGearRatio>=0.97f;
    float target=fixed ? 1.0f : std::min(1.0f,ai.m_ownerGearRatio*frand(0.9f,1.1f));
    ai.m_presetTargetRatio=target;
    bool complete=true;
    uint32 filteredDiagnostics=0;
    for(auto const& pair:graduation)
    {
        ItemPrototype const* anchor=sObjectMgr.GetItemPrototype(pair.second.itemId);
        std::vector<PlayerPremadeItem> pool={pair.second};
        if(!fixed)
        {
            auto add=[&](PlayerPremadeGearTemplate const& gear){
                for(auto const& item:gear.items)
                {
                    if(item.requiredTeam && item.requiredTeam!=uint32(bot->GetTeam()))continue;
                    ItemPrototype const* proto=sObjectMgr.GetItemPrototype(item.itemId);
                    if(!ClassAndRace(bot,proto) || Metric(proto)>Metric(anchor)+0.001f)continue;
                    bool anchored=SameFunction(proto,anchor);
                    if(!anchored)continue;
                    uint8 slots[4];proto->GetAllowedEquipSlots(slots,bot->GetClass(),bot->CanDualWield());
                    if(std::find(slots,slots+4,pair.first)!=slots+4)pool.push_back(item);
                }
            };
            add(pre->second);add(phase->second);
            for(auto const& gear:gears)
                if(gear.second.requiredClass==spec.requiredClass && gear.second.role==spec.role && (gear.second.level==19 || gear.second.level==29 || gear.second.level==39 || gear.second.level==49))add(gear.second);
        }
        PlayerPremadeItem pick=pair.second;float distance=1e30f,desired=Metric(anchor)*target;
        for(auto const& item:pool)
        {
            ItemPrototype const* proto=sObjectMgr.GetItemPrototype(item.itemId);
            // Explicitly authorized provisioning only on this newly spawned bot.
            bot->SatisfyItemRequirements(proto);
            uint16 dest;
            // Earlier choices are already equipped, so unique-count, unique-
            // equipped and hand/slot restrictions see the actual combination.
            InventoryResult equip=bot->CanEquipItem(pair.first,dest,proto,nullptr,false,true);
            if(equip!=EQUIP_ERR_OK)
            {
                if(filteredDiagnostics<4)
                {
                    sLog.Out(LOG_BASIC,LOG_LVL_DETAIL,"[PartyPreset] filtered bot=%u spec=%u slot=%u item=%u equipError=%u",bot->GetGUIDLow(),mapping->spec,uint32(pair.first),item.itemId,uint32(equip));
                    ++filteredDiagnostics;
                }
                continue;
            }
            float error=std::abs(Metric(proto)-desired);
            if(error<distance){distance=error;pick=item;}
        }
        if(distance==1e30f)
        {
            sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[PartyPreset] no candidate bot=%u spec=%u slot=%u anchor=%u",bot->GetGUIDLow(),mapping->spec,uint32(pair.first),pair.second.itemId);
            result="No legal role-appropriate candidate for slot="+std::to_string(pair.first)+" anchor="+std::to_string(pair.second.itemId);return false;
        }
        ItemPrototype const* proto=sObjectMgr.GetItemPrototype(pick.itemId);
        bot->SatisfyItemRequirements(proto);bot->UpdateSkillsToMaxSkillsForLevel();
        uint16 dest;
        InventoryResult equip=bot->CanEquipItem(pair.first,dest,proto,nullptr,false,true);
        if(equip!=EQUIP_ERR_OK)
        {
            sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[PartyPreset] final check failed bot=%u spec=%u slot=%u item=%u equipError=%u",bot->GetGUIDLow(),mapping->spec,uint32(pair.first),pick.itemId,uint32(equip));
            result="Companion equip rejected slot="+std::to_string(pair.first)+" item="+std::to_string(pick.itemId)+" code="+std::to_string(uint32(equip));return false;
        }
        Item* item=bot->EquipNewItem(dest,pick.itemId,true);
        if(!item){result="Companion item creation failed slot="+std::to_string(pair.first)+" item="+std::to_string(pick.itemId);return false;}
        if(pick.enchantId){bot->ApplyEnchantment(item,PERM_ENCHANTMENT_SLOT,false);item->SetEnchantment(PERM_ENCHANTMENT_SLOT,pick.enchantId,0,0);bot->ApplyEnchantment(item,PERM_ENCHANTMENT_SLOT,true);}
        chosen.emplace(pair.first,pick);
        ai.m_presetChosenItems.emplace(pair.first,std::make_pair(pick.itemId,pick.enchantId));
        auto original=originalGraduation.find(pair.first);
        if(original==originalGraduation.end() || original->second.itemId!=item->GetEntry() || original->second.enchantId!=item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT))complete=false;
    }
    ai.m_presetEquipped=uint32(chosen.size());ai.m_presetPhase6=complete;
    ai.m_appliedGearRatio=Mean(bot,chosen)/std::max(1.0f,Mean(bot,originalGraduation));
    ai.m_presetLimited=!fixed && std::abs(ai.m_appliedGearRatio-target)>0.1f*std::max(target,0.01f);
    if(!ChosenEquipmentMatches(bot,ai)){result="Chosen equipment identity verification failed.";return false;}
    if(fixed && !complete && !tankHit){result="Graduation equipment verification failed.";return false;}
    if(spec.role==ROLE_TANK)
    {
        float miss=TankYellowMiss63(bot);
        if(miss>0.001f)
        {
            sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[PartyPreset] tank reference insufficient bot=%u spec=%u skill=%u hit=%.2f yellowMiss63=%.2f",bot->GetGUIDLow(),mapping->spec,bot->GetClass()==CLASS_DRUID ? bot->GetSkillMaxForLevel() : bot->GetWeaponSkillValue(BASE_ATTACK),bot->GetWeaponBasedAuraModifier(BASE_ATTACK,SPELL_AURA_MOD_HIT_CHANCE),miss);
            result="Tank reference remains below the level-63 yellow-hit requirement; companion not ready.";return false;
        }
    }
    std::ostringstream out;out<<"spec="<<mapping->spec<<" gear="<<mapping->phase6<<" p6="<<complete<<" loadout="<<ChosenEquipmentMatches(bot,ai)<<" ownerRef="<<ai.m_ownerReferenceSpec<<" ownerRatio="<<std::fixed<<std::setprecision(2)<<ai.m_ownerGearRatio<<" actualRatio="<<ai.m_appliedGearRatio<<" limited="<<ai.m_presetLimited;
    result=out.str();ai.m_presetReport=result;return true;
}
uint32 RemoveOwned(Player* owner)
{
    if(!owner || owner->IsBot() || !owner->GetSession() || owner->GetSession()->GetSecurity()<SEC_ADMINISTRATOR)return 0;
    uint32 requested=0;
    for(auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid()))
    {
        // The manager catalog already narrows this to PartyBotAI and the exact
        // owner. Do not select a world target or touch battlebots/human players.
        if(!ai->botEntry || ai->m_leaderGuid!=owner->GetObjectGuid() || (Initialized(*ai) && !ai->me->IsBot()))continue;
        ai->botEntry->requestRemoval=true;
        ++requested;
    }
    return requested;
}
void SendStatus(Player* owner)
{
    auto bots=sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid(),true);uint32 ready=0,removing=0;
    auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();auto const& gears=sObjectMgr.GetPlayerPremadeGearTemplates();
    for(auto const* ai:bots)
    {
        bool closing=ai->botEntry && ai->botEntry->requestRemoval;
        bool live=Ready(*ai);
        CombatBotRoles role=live ? ai->GetRole() : ai->m_role;
        if(closing)++removing;else if(live)++ready;
        uint32 known=0,total=0,equipped=0;float currentRatio=0;
        auto spec=specs.find(ai->m_presetSpecId);
        if(spec!=specs.end()) { total=uint32(spec->second.spells.size());if(live)for(uint32 spell:spec->second.spells)if(SpellCovered(ai->me,spell))++known; }
        Mapping const* mapping=Find(ai->m_presetSpecId);
        if(live && mapping && gears.count(mapping->phase6))
        {
            Slots expected=Canonical(ai->me,gears.at(mapping->phase6));float actual=0,base=0;
            for(auto const& pair:expected)
            {
                float w=Weight(pair.first,ai->me->GetClass());base+=Metric(sObjectMgr.GetItemPrototype(pair.second.itemId))*w;
                if(Item* item=ai->me->GetItemByPos(INVENTORY_SLOT_BAG_0,pair.first)){++equipped;actual+=Metric(item->GetProto())*w;}
            }
            if(base>0)currentRatio=actual/base;
        }
        bool limited=ai->m_presetLimited || (live && std::abs(currentRatio-ai->m_presetTargetRatio)>0.1f*std::max(ai->m_presetTargetRatio,0.01f));
        char const* strategy=!ai->m_presetSpecId ? "native" : ai->m_presetSpecId==15 ? "tank_hit" : role==ROLE_HEALER || role==ROLE_TANK ? "phase6" : "adaptive";
        std::ostringstream out;out<<"GMUI|PARTYBOT|guid="<<(ai->botEntry ? ai->botEntry->playerGUID : 0)<<"|name="<<(live?ai->me->GetName():"loading")<<"|class="<<(live?unsigned(ai->me->GetClass()):unsigned(ai->m_class))<<"|group="<<(live?unsigned(ai->me->GetSubGroup()):99)<<"|state="<<(closing ? "removing" : live ? "ready" : "loading")<<"|specId="<<ai->m_presetSpecId<<"|role="<<uint32(role)<<"|level="<<(live ? uint32(ai->me->GetLevel()) : 0)<<"|strategy="<<strategy<<"|p6="<<(live && Graduated(*ai))<<"|loadout="<<(live && LoadoutMatches(*ai))<<"|verified="<<(live && FullSpecKnown(*ai) ? 1 : 0)<<"|equipped="<<equipped<<"|ownerRef="<<ai->m_ownerReferenceSpec<<"|ratio="<<std::fixed<<std::setprecision(2)<<currentRatio<<"|limited="<<limited<<"|spells="<<known<<"/"<<total;
        ChatHandler(owner).SendSysMessage(out.str().c_str());
        if(live)
        {
            Player const* bot=ai->me;
            // Attribute inspection only. This is the server's yellow-attack
            // miss formula against a standing level-63 NPC with defense 315,
            // without target auras or spell-specific hit modifiers. It is not
            // an encounter simulation or a promise of tanking optimality.
            uint32 weaponSkill=bot->GetWeaponSkillValue(BASE_ATTACK);
            float hit=bot->GetWeaponBasedAuraModifier(BASE_ATTACK,SPELL_AURA_MOD_HIT_CHANCE);
            int32 difference=int32(weaponSkill)-315;
            float baseMiss=5.0f-difference*(difference < -10 ? 0.2f : 0.1f);
            float effectiveHit=hit;
            if(difference < -10 && hit > 0)effectiveHit-=1.0f;
            float need=std::max(0.0f,baseMiss+(difference < -10 ? 1.0f : 0.0f));
            float yellowMiss=std::max(0.0f,std::min(60.0f,baseMiss-effectiveHit));
            std::ostringstream stats;
            stats<<"GMUI|PARTYSTATS|guid="<<(ai->botEntry ? ai->botEntry->playerGUID : 0)
                 <<"|hitPct="<<std::fixed<<std::setprecision(2)<<hit
                 <<"|weaponSkill="<<weaponSkill<<"|defense="<<bot->GetDefenseSkillValue()
                 <<"|armor="<<bot->GetArmor()<<"|health="<<bot->GetMaxHealth()
                 <<"|form="<<uint32(bot->GetShapeshiftForm())<<"|targetLevel=63|hitNeed="<<need
                 <<"|yellowMiss="<<yellowMiss<<"|whiteDwPenalty="<<(bot->HaveOffhandWeapon() ? 19 : 0)
                 <<"|currentLevel="<<uint32(bot->GetLevel())<<"|currentHealth="<<bot->GetHealth();
            ChatHandler(owner).SendSysMessage(stats.str().c_str());
            float defense=float(bot->GetDefenseSkillValue());
            float armor=std::max(0.0f,float(bot->GetArmor()));
            float crit=std::max(0.0f,5.0f+(315.0f-defense)*0.04f);
            float reduction=std::min(75.0f,100.0f*armor/(armor+5755.0f));
            std::ostringstream survival;
            survival<<"GMUI|PARTYDEFENSE|guid="<<(ai->botEntry ? ai->botEntry->playerGUID : 0)
                    <<"|targetLevel=63|critRef="<<std::fixed<<std::setprecision(2)<<crit
                    <<"|armorDr="<<reduction<<"|dodgePct="<<bot->GetUnitDodgeChance()
                    <<"|parryPct="<<bot->GetUnitParryChance()<<"|blockPct="<<bot->GetUnitBlockChance()
                    <<"|blockValue="<<bot->GetShieldBlockValue()<<"|basis=current";
            ChatHandler(owner).SendSysMessage(survival.str().c_str());
        }
    }
    uint32 actualHealers=Healers(owner,false);
    std::ostringstream out;out<<"GMUI|PARTYMETA|owned="<<bots.size()<<"|pending="<<PendingGroup(owner)<<"|ready="<<ready<<"|removing="<<removing<<"|p6Healers="<<actualHealers<<"|pendingP6Healers="<<(Healers(owner,true)-actualHealers);
    ChatHandler(owner).SendSysMessage(out.str().c_str());ChatHandler(owner).SendSysMessage("GMUI|DONE|partyStatus");
}
namespace {
struct CachedRaid {
    uint32 token=0,map=0,instance=0,created=0,profileMap=0;
    unsigned ownerRole=0;
    std::string signature;
    std::vector<RaidBotPlanner::Spec> add;
};
std::map<uint64,CachedRaid> raidPlans;
struct RaidOwnerRole {unsigned role;uint32 map,instance,profileMap,token;};
std::map<uint64,RaidOwnerRole> raidOwnerRoles;
bool RaidContext(Player* owner,std::string& result,bool allowWorld = false) {
    if(!owner || owner->IsBot() || !owner->GetSession() || owner->GetSession()->GetSecurity()<SEC_ADMINISTRATOR){result="Administrator player required.";return false;}
    if(owner->GetLevel()!=60 || !owner->IsAlive() || owner->IsInCombat() || owner->InBattleGround() || owner->InBattleGroundQueue() || owner->IsBeingTeleported() || owner->IsTaxiFlying()){result="Raid setup requires a living level-60 player outside combat, battleground, queue and taxi.";return false;}
    bool supported=owner->GetMap()->IsDungeon() && RaidBotPlanner::Find(owner->GetMapId());
    if(!supported && !(allowWorld && (owner->GetMapId()==0 || owner->GetMapId()==1))){result="Enter a supported dungeon or select a raid preset while on a continent.";return false;}
    if(!OwnGroup(owner,result,true))return false;
    if(PendingGroup(owner)){result="Wait for existing companions to finish loading.";return false;}
    for(auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid(),true))if(ai->botEntry && ai->botEntry->requestRemoval){result="Wait for companion removal to finish.";return false;}
    return true;
}
bool RaidRoster(Player* owner,unsigned ownerRole,std::vector<RaidBotPlanner::Spec>& roster,std::string& signature,std::string& result) {
    std::map<uint64,unsigned> roles;
    for(auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid()))if(Ready(*ai))roles[ai->me->GetGUID()]=unsigned(ai->GetRole());
    std::vector<Player*> members;
    if(Group* group=owner->GetGroup())for(auto const& m:group->GetMemberSlots()){
        Player* p=ObjectAccessor::FindPlayer(m.guid);
        if(!p || !p->IsInWorld() || p->GetMap()!=owner->GetMap() || p->IsInCombat() || p->GetLevel()!=60 || p->GetTeam()!=owner->GetTeam()){result="All existing members must be online, level 60, same faction and instance, and outside combat.";return false;}
        members.push_back(p);
    }else members.push_back(owner);
    std::sort(members.begin(),members.end(),[](Player* a,Player* b){return a->GetGUID()<b->GetGUID();});
    std::ostringstream key;key<<owner->GetMapId()<<':'<<owner->GetMap()->GetInstanceId();
    for(Player* p:members){
        unsigned role=p==owner?ownerRole:roles[p->GetGUID()];
        if(!role){
            // Infer other real players only from an actually learned talent in a trusted spec.
            uint32 ref=0;OwnerRatio(p,ref);
            auto it=sObjectMgr.GetPlayerPremadeSpecTemplates().find(ref);
            bool talent=false;
            if(it!=sObjectMgr.GetPlayerPremadeSpecTemplates().end())for(uint32 sid:it->second.spells)if(GetTalentSpellPos(sid)&&p->HasSpell(sid)){talent=true;break;}
            if(!talent){result="Cannot infer an existing member role from learned talents; set that role before requesting a full preset.";return false;}
            role=it->second.role;
        }
        roster.push_back({0,unsigned(p->GetClass()),role});key<<'/'<<p->GetGUID()<<':'<<unsigned(p->GetClass())<<':'<<role;
        std::ostringstream line;line<<"GMUI|RAIDMEMBER|name="<<p->GetName()<<"|class="<<unsigned(p->GetClass())<<"|role="<<role;
        ChatHandler(owner).SendSysMessage(line.str().c_str());
    }
    signature=key.str();return true;
}
}
bool RaidPreview(Player* owner,unsigned role,std::string& result,uint32 profileMap) {
    if(role<1||role>4){result="Select your own tank, healer, melee or ranged role.";return false;}
    if(owner)raidPlans.erase(owner->GetGUID());
    if(!RaidContext(owner,result,profileMap!=0))return false;
    auto selected=RaidBotPlanner::Find(profileMap?profileMap:owner->GetMapId());
    if(!selected){result="Unknown raid preset.";return false;}
    if(owner->GetMap()->IsDungeon() && selected->map!=owner->GetMapId()){result="Current dungeon differs from selected preset.";return false;}
    auto const& profile=*selected;
    if(owner->GetMap()->IsDungeon() && profile.size>owner->GetMap()->GetMapEntry()->maxPlayers){result="Current map capacity is smaller than the preset.";return false;}
    std::vector<RaidBotPlanner::Spec> pool,roster;std::string signature;
    if(!RaidRoster(owner,role,roster,signature,result))return false;
    auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();
    for(auto const& mapping:Mappings()){
        std::string reason;auto spec=specs.find(mapping.spec);
        if(spec!=specs.end()&&Supported(owner,mapping,reason))pool.push_back({mapping.spec,unsigned(spec->second.requiredClass),unsigned(spec->second.role)});
    }
    uint32 token=urand(1,0x7FFFFFFF);
    auto plan=RaidBotPlanner::Build(profile,owner->GetTeam()==HORDE,pool,roster,token);
    if(!plan.error.empty()){result=plan.error;return false;}
    uint32 limit=sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_MAX_BOTS);
    if(limit && roster.size()-1+plan.add.size()>limit){result="PartyBot.MaxBots is below the requested full-team size.";return false;}
    if(owner->GetMap()->IsDungeon() && owner->GetMap()->GetPlayers().getSize()+plan.add.size()>profile.size){result="Other occupants leave insufficient instance capacity.";return false;}
    uint32 now=WorldTimer::getMSTime();
    for(auto it=raidPlans.begin();it!=raidPlans.end();)if(uint32(now-it->second.created)>60000)it=raidPlans.erase(it);else ++it;
    raidPlans[owner->GetGUID()]={token,owner->GetMapId(),owner->GetMap()->GetInstanceId(),now,profile.map,role,signature,plan.add};
    std::map<std::pair<unsigned,unsigned>,unsigned> counts;
    for(auto s:plan.add)++counts[{s.cls,s.role}];
    for(auto const& item:counts){std::ostringstream line;line<<"GMUI|RAIDADD|class="<<item.first.first<<"|role="<<item.first.second<<"|count="<<item.second;ChatHandler(owner).SendSysMessage(line.str().c_str());}
    std::ostringstream line;line<<"GMUI|RAIDPLAN|token="<<token<<"|name="<<profile.name<<"|size="<<profile.size<<"|tanks="<<profile.tanks<<"|heals="<<profile.heals<<"|dps="<<(profile.size-profile.tanks-profile.heals)<<"|existing="<<roster.size()<<"|add="<<plan.add.size();
    ChatHandler(owner).SendSysMessage(line.str().c_str());
    result="Raid preview ready for 60 seconds; review roles before confirming.";return true;
}
bool RaidApply(Player* owner,uint32 token,std::string& result) {
    if(!RaidContext(owner,result,true))return false;
    auto it=raidPlans.find(owner->GetGUID());
    if(it==raidPlans.end() || it->second.token!=token || uint32(WorldTimer::getMSTime()-it->second.created)>60000){result="Raid preview expired; request a new preview.";return false;}
    CachedRaid plan=it->second;raidPlans.erase(it);
    std::vector<RaidBotPlanner::Spec> roster;std::string signature;
    if(!RaidRoster(owner,plan.ownerRole,roster,signature,result))return false;
    if(signature!=plan.signature){result="Roster or instance changed; request a new preview.";return false;}
    if(!owner->GetGroup()){
        Group* group=new Group;
        if(!group->Create(owner->GetObjectGuid(),owner->GetName())){delete group;result="Unable to create player group.";return false;}
        sObjectMgr.AddGroup(group);
    }
    if(RaidBotPlanner::Find(plan.profileMap)->size>5 && !owner->GetGroup()->isRaidGroup())owner->GetGroup()->ConvertToRaid();
    std::set<uint64> before;for(auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid(),true))if(ai->botEntry)before.insert(ai->botEntry->playerGUID);
    unsigned queued=0;
    // Healers first, so the existing last-slot safety gate is never bypassed.
    std::stable_sort(plan.add.begin(),plan.add.end(),[](RaidBotPlanner::Spec a,RaidBotPlanner::Spec b){return (a.role==4)>(b.role==4);});
    for(auto spec:plan.add){
        if(!Queue(owner,spec.id,result,true)){
            for(auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid(),true))if(ai->botEntry && !before.count(ai->botEntry->playerGUID))ai->botEntry->requestRemoval=true;
            result="Raid batch cancelled; only this batch is being removed. "+result;return false;
        }
        ++queued;
    }
    raidOwnerRoles[owner->GetGUID()]={plan.ownerRole,plan.map,plan.instance,plan.profileMap,plan.token};
    if(!queued)ArrangeRaid(owner);
    result="Raid batch queued: "+std::to_string(queued)+" companions; inspect ready/loading counts. Boss mechanics are not implied.";return true;
}
void ArrangeRaid(Player* owner) {
    Group* group=owner?owner->GetGroup():nullptr;if(!group||!group->isRaidGroup()||PendingGroup(owner)||owner->IsInCombat())return;
    auto profile=RaidBotPlanner::Find(owner->GetMapId());
    auto saved=raidOwnerRoles.find(owner->GetGUID());if(!profile&&saved!=raidOwnerRoles.end())profile=RaidBotPlanner::Find(saved->second.profileMap);
    if(!profile||group->GetMembersCount()!=profile->size)return;
    static std::map<uint64,std::string> arranged;
    std::ostringstream arrangement;arrangement<<owner->GetMapId()<<':'<<owner->GetMap()->GetInstanceId();if(saved!=raidOwnerRoles.end())arrangement<<':'<<saved->second.token;
    for(auto const& slot:group->GetMemberSlots())arrangement<<'/'<<slot.guid.GetRawValue();
    if(arranged[owner->GetGUID()]==arrangement.str())return;
    struct Member {Player* p;unsigned role;};std::vector<Member> members;
    std::map<uint64,unsigned> roles;for(auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(owner->GetObjectGuid()))if(Ready(*ai))roles[ai->me->GetGUID()]=unsigned(ai->GetRole());
    for(auto const& slot:group->GetMemberSlots()){Player* p=ObjectAccessor::FindPlayer(slot.guid);if(!p||p->GetMap()!=owner->GetMap())return;members.push_back({p,roles[p->GetGUID()]});}
    std::stable_sort(members.begin(),members.end(),[](Member a,Member b){auto rank=[](Member m){return m.p->GetClass()==CLASS_SHAMAN?0:m.role==ROLE_HEALER?1:m.role==ROLE_TANK?2:3;};return rank(a)<rank(b);});
    unsigned groups=(profile->size+4)/5;std::array<unsigned,8> counts{};std::map<uint64,uint8> desired;
    for(auto member:members){unsigned best=0;for(unsigned i=1;i<groups;++i)if(counts[i]<counts[best])best=i;desired[member.p->GetGUID()]=uint8(best);++counts[best];}
    for(auto member:members){uint8 wanted=desired[member.p->GetGUID()];if(member.p->GetSubGroup()==wanted)continue;
        if(group->HasFreeSlotSubGroup(wanted))group->ChangeMembersGroup(member.p,uint8(wanted));
        else for(auto other:members)if(other.p->GetSubGroup()==wanted && desired[other.p->GetGUID()]!=wanted){group->SwapMembersGroup(member.p,other.p);break;}
    }
    arranged[owner->GetGUID()]=arrangement.str();
    ChatHandler(owner).SendSysMessage("GMUI|OK|Raid roster ready; five-player subgroups balanced with shamans spread between groups. Verify special encounter duties before pulling.");
}

unsigned PreferredRole(Player const* player) {
    if(!player)return 0;
    auto chosen=raidOwnerRoles.find(player->GetGUID());
    if(chosen!=raidOwnerRoles.end() && ((player->GetMapId()==chosen->second.map && player->GetMap()->GetInstanceId()==chosen->second.instance)||player->GetMapId()==chosen->second.profileMap))return chosen->second.role;
    uint32 ref=0;OwnerRatio(player,ref);
    auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();auto it=specs.find(ref);
    if(it==specs.end())return 0;
    for(uint32 sid:it->second.spells)if(GetTalentSpellPos(sid)&&player->HasSpell(sid))return unsigned(it->second.role);
    return 0;
}

}
