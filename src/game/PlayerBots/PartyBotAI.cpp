/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "PartyBotAI.h"
#include "PveDecisionRuntime.h"
#include "PartyBotLootPolicy.h"
#include "LootMgr.h"
#include "Packets/Loot.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "DungeonTankPolicy.h"
#include "PartyBotFormation.h"
#include "WarriorTankPolicy.h"
#include <mutex>
#include "BotMountPolicy.h"
#include "PartyBotRecoveryPolicy.h"
#include "PathFinder.h"
#include "PointMovementGenerator.h"
#include "MoveSpline.h"
#include "Creature.h"
#include "RaidCoordination.h"
#include "McBotTactics.h"
#include "BotDamagePolicy.h"
#include "PartyBotPreset.h"
#include "RaidBotPlanner.h"
#include "Timer.h"
#include "Player.h"
#include "CreatureAI.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "World.h"
#include "WorldPacket.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "Utilities/Random.h"

#include <random>

enum PartyBotSpells
{
    PB_SPELL_FOOD = 1131,
    PB_SPELL_DRINK = 1137,
    PB_SPELL_AUTO_SHOT = 75,
    PB_SPELL_SHOOT_WAND = 5019,
    PB_SPELL_HONORLESS_TARGET = 2479,
};

#define PB_UPDATE_INTERVAL 1000
#define PB_MIN_FOLLOW_DIST 3.0f
#define PB_MAX_FOLLOW_DIST 6.0f
#define PB_MIN_FOLLOW_ANGLE 0.0f
#define PB_MAX_FOLLOW_ANGLE 6.0f

bool PartyBotAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    if (!m_race && !m_class)
    {
        sess->LoginPlayer(entry->playerGUID);
        return true;
    }

    return SpawnNewPlayer(sess, m_class, m_race, m_mapId, m_instanceId, m_x, m_y, m_z, m_o, sObjectAccessor.FindPlayer(m_cloneGuid));
}

void PartyBotAI::CloneFromPlayer(Player const* pPlayer)
{
    if (!pPlayer)
        return;

    if (pPlayer->GetLevel() != me->GetLevel())
    {
        me->GiveLevel(pPlayer->GetLevel());
        me->InitTalentForLevel();
        me->SetUInt32Value(PLAYER_XP, 0);
    }

    // Learn all of the target's spells.
    for (const auto& spell : pPlayer->GetSpellMap())
    {
        if (spell.second.disabled)
            continue;

        if (spell.second.state == PLAYERSPELL_REMOVED)
            continue;

        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(spell.first);
        if (!pSpellEntry)
            continue;

        uint32 const firstRankId = sSpellMgr.GetFirstSpellInChain(spell.first);
        if (!me->HasSpell(spell.first))
            me->LearnSpell(spell.first, false, (firstRankId == spell.first && GetTalentSpellPos(firstRankId)));
    }

    me->GetHonorMgr().SetHighestRank(pPlayer->GetHonorMgr().GetHighestRank());
    me->GetHonorMgr().SetRank(pPlayer->GetHonorMgr().GetRank());

    // Unequip current gear
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        me->AutoUnequipItemFromSlot(i);

    // Copy gear from target.
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            me->SatisfyItemRequirements(pItem->GetProto());
            me->StoreNewItemInBestSlots(pItem->GetEntry(), 1, pItem->GetEnchantmentId(EnchantmentSlot(0)));
        }
    }
}

Player* PartyBotAI::GetPartyLeader() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    if (Player* originalLeader = ObjectAccessor::FindPlayerNotInWorld(m_leaderGuid))
    {
        if (me->InBattleGround() == originalLeader->InBattleGround())
        {
            // In case the original spawner is not in the same group as the bots anymore.
            if (pGroup != originalLeader->GetGroup())
                return nullptr;

            // In case the current leader is the bot itself and it's not inside a Battleground.
            ObjectGuid currentLeaderGuid = pGroup->GetLeaderGuid();
            if (currentLeaderGuid == me->GetObjectGuid() && !me->InBattleGround())
                return nullptr;
        }

        return originalLeader;
    }
    return nullptr;
}

bool PartyBotAI::IsValidDistancingTarget(Unit* pTarget, Unit* pEnemy)
{
    if (pTarget->IsInWorld() && pTarget->IsAlive() &&
        pTarget->GetMap() == me->GetMap())
    {
        float const distance = me->GetDistance(pTarget);
        if (distance >= 15.0f && distance <= 30.0f &&
            pTarget->GetDistance(pEnemy) >= 15.0f)
            return true;
    }

    return false;
}

Unit* PartyBotAI::GetDistancingTarget(Unit* pEnemy)
{
    if (Player* pLeader = GetPartyLeader())
        if (IsValidDistancingTarget(pLeader, pEnemy))
            return pLeader;

    Unit* pNonTank = nullptr;
    Group* pGroup = me->GetGroup();
    if(!pGroup)return nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == me)
                continue;

            if (IsValidDistancingTarget(pMember, pEnemy))
            {
                if (IsTankingForm(pMember->GetShapeshiftForm()) || IsWearingShield(pMember))
                    return pMember;
                else
                    pNonTank = pMember;
            }
        }
    }

    return pNonTank;
}

bool PartyBotAI::RunAwayFromTarget(Unit* pEnemy)
{
    if (Unit* pTarget = GetDistancingTarget(pEnemy))
    {
        me->MonsterMove(pTarget->GetPositionX(), pTarget->GetPositionY(), pTarget->GetPositionZ());
        return true;
    }

    return me->GetMotionMaster()->MoveDistance(pEnemy, 15.0f);
}

SpellCastResult PartyBotAI::CastPartySpell(Unit* target, SpellEntry const* spell, bool triggered)
{
    if(!spell || !AllowLegacySpell(spell))return SPELL_FAILED_DONT_REPORT;
    return me->CastSpell(target,spell,triggered);
}
SpellCastResult PartyBotAI::CastPartySpell(Unit* target, uint32 spellId, bool triggered)
{
    return CastPartySpell(target,sSpellMgr.GetSpellEntry(spellId),triggered);
}

bool PartyBotAI::DrinkAndEat()
{
    if (m_isBuffing)
        return false;

    if (me->GetVictim())
        return false;

    bool const needToEat = me->GetHealthPercent() < 100.0f;
    bool const needToDrink = (me->GetPowerType() == POWER_MANA) && (me->GetPowerPercent(POWER_MANA) < 100.0f);

    if (!needToEat && !needToDrink)
        return false;

    bool const isEating = me->HasAura(PB_SPELL_FOOD);
    bool const isDrinking = me->HasAura(PB_SPELL_DRINK);

    if (!isEating && needToEat)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(PB_SPELL_FOOD))
        {
            CastPartySpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(pSpellEntry);
        }
        return true;
    }

    if (!isDrinking && needToDrink)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(PB_SPELL_DRINK))
        {
            CastPartySpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(pSpellEntry);
        }
        return true;
    }

    return needToEat || needToDrink;
}

namespace { bool ReservedCorpse(Player* corpse); }

bool PartyBotAI::ShouldAutoRevive() const
{
    if(me->IsRessurectRequested() || (me->GetDeathState()==CORPSE && ReservedCorpse(me)))return false;
    if (me->GetDeathState() == DEAD)
        return true;

    bool alivePlayerNearby = false;
    Group* pGroup = me->GetGroup();
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == me)
                continue;

            if (pMember->IsInCombat())
                return false;

            if (pMember->IsAlive())
            {
                if (IsHealerClass(pMember->GetClass()))
                    return false;

                if (me->IsWithinDistInMap(pMember, 15.0f))
                    alivePlayerNearby = true;
            }
        }
    }

    return alivePlayerNearby;
}

namespace DungeonTank {
bool Scene(PartyBotAI const& ai);
bool AreaSafe(PartyBotAI const& ai,Unit const* center,SpellEntry const* spell,float forcedRadius=0,float const* ground=nullptr);
bool GuardAoe(PartyBotAI& ai);
bool TryAoe(PartyBotAI& ai);
}

bool PartyBotAI::CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    if (!McBotTactics::AllowSpell(*this,pTarget,pSpellEntry) || !RaidCoordination::AllowDamage(*this,pTarget,pSpellEntry)) return false;
    if (!CombatBotBaseAI::CanTryToCastSpell(pTarget, pSpellEntry))
        return false;

    if(DungeonTank::Scene(*this)&&(m_role==ROLE_MELEE_DPS||m_role==ROLE_RANGE_DPS)){
        // Some multi-target attacks are self buffs, chains or next-swing
        // actions, so the generic hostile-AOE classification is insufficient.
        bool chain=(me->GetClass()==CLASS_SHAMAN&&pSpellEntry==m_spells.shaman.pChainLightning)||(me->GetClass()==CLASS_HUNTER&&pSpellEntry==m_spells.hunter.pMultiShot);
        float radius=chain?30.0f:8.0f;
        bool multi=chain||(me->GetClass()==CLASS_ROGUE&&pSpellEntry==m_spells.rogue.pBladeFlurry)||(me->GetClass()==CLASS_WARRIOR&&pSpellEntry==m_spells.warrior.pCleave)||(me->GetClass()==CLASS_PRIEST&&pSpellEntry==m_spells.priest.pHolyNova);
        if(multi)return DungeonTank::AreaSafe(*this,pTarget,pSpellEntry,radius);
    }

    if (pSpellEntry->IsAreaOfEffectSpell() && !pSpellEntry->IsPositiveSpell() && !IsInDuel())
    {
        if(DungeonTank::Scene(*this)&&(m_role==ROLE_MELEE_DPS||m_role==ROLE_RANGE_DPS))
            return DungeonTank::AreaSafe(*this,pTarget,pSpellEntry);
        for(auto mark:m_marksToCC)if(Unit* controlled=GetMarkedTarget(mark))
            if(controlled->IsAlive() && pTarget && controlled->IsWithinDistInMap(pTarget,30.0f))return false;

        // Coordinated parties use the shared per-target threat budget above.
        // Keep the legacy health-sized AoE margin only for uncoordinated bots.
        if (m_role != ROLE_TANK && !RaidCoordination::Enabled(me))
        {
            float radius;
            if (pSpellEntry->EffectRadiusIndex[0])
                radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(pSpellEntry->EffectRadiusIndex[0]));
            else if (pSpellEntry->EffectRadiusIndex[1])
                radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(pSpellEntry->EffectRadiusIndex[1]));
            else if (pSpellEntry->EffectRadiusIndex[2])
                radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(pSpellEntry->EffectRadiusIndex[2]));
            else
                radius = 10.0f;

            std::list<Unit*> targets;
            me->GetEnemyListInRadiusAround(pTarget, radius, targets);

            for (auto const& pEnemy : targets)
            {
                if (((pEnemy->GetLevel() + 5) > me->GetLevel()) &&
                    ((pEnemy->GetHealth() * 4) > me->GetHealth()) &&
                    pEnemy->GetVictim() && pEnemy->GetVictim() != me &&
                    pEnemy->IsValidAttackTarget(me) &&
                    pEnemy->CanHaveThreatList())
                {
                    float const myThreat = pEnemy->GetThreatManager().getThreat(me);
                    float const victimThreat = pEnemy->GetThreatManager().getThreat(pEnemy->GetVictim());

                    if (victimThreat < (myThreat + me->GetMaxHealth()))
                        return false;
                }
            }
        }
    }

    return true;
}

bool PartyBotAI::CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const
{
    if (IsInDuel() || !me->GetGroup())
        return true;

    if (pSpellEntry->HasAuraInterruptFlag(AURA_INTERRUPT_DAMAGE_CANCELS) &&
        AreOthersOnSameTarget(pTarget->GetObjectGuid()))
        return false;

    if (pSpellEntry->HasSingleTargetAura())
    {
        auto const& singleAuras = me->GetSingleCastSpellTargets();
        if (singleAuras.find(pSpellEntry) != singleAuras.end())
            return false;
    }

    return true;
}

bool PartyBotAI::AttackStart(Unit* pVictim)
{
    if(RaidCoordination::PullPending(me))return false;
    if (!McBotTactics::AllowAttack(*this,pVictim) || !RaidCoordination::AllowDamage(*this,pVictim,nullptr)) return false;
    if(RaidCoordination::Enabled(me) && m_role==ROLE_TANK && pVictim && RaidCoordination::AssignedTank(me,pVictim)!=me)return false;
    m_isBuffing = false;

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    if (me->Attack(pVictim, true))
    {
        BeginChasing(pVictim);
        return true;
    }

    return false;
}

Unit* PartyBotAI::GetMarkedTarget(RaidTargetIcon mark) const
{
    if(!me->GetGroup())return nullptr;
    ObjectGuid targetGuid = me->GetGroup()->GetTargetWithIcon(mark);
    if (targetGuid.IsUnit())
        return me->GetMap()->GetUnit(targetGuid);

    return nullptr;
}

namespace DungeonLoot {
struct State {uint32 touched=0,next=0,started=0;ObjectGuid corpse;std::map<uint64,uint32> tried;};
std::map<uint64,State> states;std::recursive_mutex mutex;
bool GroupSafe(Player* p){
 if(!p||!p->IsAlive()||!p->IsInWorld()||p->IsBeingTeleported()||p->IsInCombat()||p->IsNonMeleeSpellCasted()||p->GetTradeData()||p->GetLootGuid()||p->HasUnitState(UNIT_STATE_NO_FREE_MOVE))return false;
 for(GroupReference* r=p->GetGroup()->GetFirstMember();r;r=r->next())if(Player* q=r->getSource()){
  if(!q->IsInWorld()||q->IsBeingTeleported())continue;
  if(q->GetMap()==p->GetMap()&&(q->IsInCombat()||(q->GetPet()&&q->GetPet()->IsInCombat())))return false;
 }
 return true;
}
bool SlotsForPersistent(PartyBotAI& ai,Creature* corpse){
 if(!ai.AllowsPersistentSave())return false;
 PermissionTypes permission=NONE_PERMISSION;
 Group* group=ai.me->GetGroup();
 if(group&&group==corpse->GetGroupLootRecipient()){
  switch(group->GetLootMethod()){
   case MASTER_LOOT:permission=MASTER_PERMISSION;break;
   case FREE_FOR_ALL:permission=ALL_PERMISSION;break;
   case ROUND_ROBIN:permission=ROUND_ROBIN_PERMISSION;break;
   default:permission=GROUP_PERMISSION;break;
  }
 }
 WorldPackets::Loot::LootResponse view;LootView(corpse->loot,ai.me,permission).WriteLoot(view);
 // Follow the same viewer-specific slots and visibility as the native loot
 // window, including appended quest slots and round-robin ownership.
 for(auto const& shown:view.items){
  if(shown.slotType!=LOOT_SLOT_TYPE_ALLOW_LOOT&&shown.slotType!=LOOT_SLOT_TYPE_OWNER)continue;
  ItemPosCountVec destinations;bool fits=ai.me->CanStoreNewItem(NULL_BAG,NULL_SLOT,destinations,shown.itemId,shown.count)==EQUIP_ERR_OK;
  if(!PartyBotLootPolicy::TakeItem(true,true,false,fits))continue;
  WorldPackets::Loot::AutoStoreLootItem take;take.lootSlot=shown.idx;ai.me->GetSession()->HandleAutostoreLootItemOpcode(take);
 }
 return true;
}
bool Update(PartyBotAI& ai,Player* leader){
 Player* p=ai.me;
 if(!ai.m_initialized||!p||!p->IsInWorld()||!p->GetGroup()||!p->GetMap()->IsDungeon()||p->InBattleGround()||p->m_duel||!leader||!leader->IsInWorld()||leader->IsBeingTeleported()||leader->GetMap()!=p->GetMap())return false;
 std::lock_guard<std::recursive_mutex> lock(mutex);uint32 now=WorldTimer::getMSTime();
 for(auto it=states.begin();it!=states.end();)if(uint32(now-it->second.touched)>300000)it=states.erase(it);else ++it;
 auto& state=states[p->GetGUID()];state.touched=now;
 auto cancel=[&](){
  if(!state.corpse.IsEmpty()){
   if(auto point=dynamic_cast<PointMovementGenerator<Player> const*>(p->GetMotionMaster()->GetCurrent()))if(point->GetId()==0x50424c54){p->StopMoving();p->GetMotionMaster()->Clear();p->GetMotionMaster()->MoveIdle();}
   state.corpse.Clear();
  }
 };
 if(!GroupSafe(p)){cancel();return false;}
 if(int32(now-state.next)<0)return !state.corpse.IsEmpty();
 state.next=now+2000;
 for(auto it=state.tried.begin();it!=state.tried.end();)if(uint32(now-it->second)>120000)it=state.tried.erase(it);else ++it;
 std::list<Creature*> corpses;MaNGOS::AnyUnitInObjectRangeCheck check(p,25.0f);MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> search(corpses,check);Cell::VisitGridObjects(p,search,25.0f);
 Creature* best=nullptr;float nearest=1000;
 for(Creature* corpse:corpses){
  bool actionable=corpse->loot.gold>0||ai.AllowsPersistentSave();
  if(!PartyBotLootPolicy::Candidate(!corpse->IsAlive(),corpse->IsTappedBy(p),p->IsAllowedToLoot(corpse),!corpse->loot.isLooted(),actionable,p->GetDistance(corpse),leader->GetDistance(corpse)))continue;
  if(state.tried.count(corpse->GetGUID()))continue;
  if(!p->IsWithinLOSInMap(corpse))continue;
  std::list<Unit*> nearby; p->GetEnemyListInRadiusAround(corpse,10.0f,nearby);
  bool danger=false;for(Unit* e:nearby)if(e->IsAlive()&&p->IsValidAttackTarget(e)&&e->IsVisibleForOrDetect(p,p,false)&&corpse->IsWithinLOSInMap(e)){danger=true;break;}
  if(danger)continue;
  float distance=p->GetDistance(corpse);if(distance<nearest){best=corpse;nearest=distance;}
 }
 if(!best){cancel();return false;}
 if(state.corpse!=best->GetObjectGuid()){cancel();state.corpse=best->GetObjectGuid();state.started=now;}
 if(uint32(now-state.started)>20000){state.tried[best->GetGUID()]=now;cancel();return false;}
 if(!p->IsWithinDistInMap(best,p->GetMaxLootDistance(best),true,SizeFactor::None)){
  if(auto point=dynamic_cast<PointMovementGenerator<Player> const*>(p->GetMotionMaster()->GetCurrent()))if(point->GetId()==0x50424c54&&p->IsMoving())return true;
  PathInfo path(p);path.ExcludeSteepSlopes();
  if(!path.calculate(best->GetPositionX(),best->GetPositionY(),best->GetPositionZ())||path.getPathType()!=PATHFIND_NORMAL||path.Length()>40||path.getPath().empty()){state.tried[best->GetGUID()]=now;cancel();return false;}
  auto end=path.getPath().back();if(std::hypot(end.x-best->GetPositionX(),end.y-best->GetPositionY())>2||std::fabs(end.z-best->GetPositionZ())>2){state.tried[best->GetGUID()]=now;cancel();return false;}
  p->GetMotionMaster()->MovePoint(0x50424c54,best->GetPositionX(),best->GetPositionY(),best->GetPositionZ(),MOVE_PATHFINDING|MOVE_EXCLUDE_STEEP_SLOPES);return true;
 }
 p->StopMoving();p->SetStandState(UNIT_STAND_STATE_STAND);if(p->IsMounted())p->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
 ObjectGuid guid=best->GetObjectGuid();WorldPackets::Loot::LootUnit open;open.guid=guid;p->GetSession()->HandleLootOpcode(open);
 if(p->GetLootGuid()==guid){
  bool permitted=p->IsAllowedToLoot(best)&&p->GetGroup()==best->GetGroupLootRecipient();
  // Money has no inventory-capacity dependency; native handler shares it.
  if(PartyBotLootPolicy::TakeMoney(permitted,best->loot.gold)){NullClientPacket money(CMSG_LOOT_MONEY);p->GetSession()->HandleLootMoneyOpcode(money);}
  if(permitted&&best->IsInWorld())SlotsForPersistent(ai,best);
  WorldPackets::Loot::LootRelease close;close.guid=guid;p->GetSession()->HandleLootReleaseOpcode(close);
  if(ai.AllowsPersistentSave())p->SaveToDB();
 }
 state.tried[guid.GetRawValue()]=now;cancel();return true;
}
}

namespace DungeonTank {
struct Turn {uint32 touched=0,until=0,rotated=0,demo=0;uint64 rescue=0;std::map<uint64,uint32> served;};
std::map<uint64,Turn> turns;
std::recursive_mutex turnsMutex;
bool Scene(PartyBotAI const& ai){
 Player* p=ai.me;
 return p&&ai.m_initialized&&p->IsInWorld()&&p->IsAlive()&&p->GetGroup()&&p->GetMap()->IsDungeon()&&!p->GetMap()->IsRaid()&&!p->InBattleGround()&&!p->m_duel&&!p->IsBeingTeleported()&&!RaidCoordination::Enabled(p);
}
bool Active(PartyBotAI const& ai){return ai.GetRole()==ROLE_TANK&&Scene(ai);}
bool Tank(Player* p){
 if(!p)return false;
 if(auto* ai=dynamic_cast<PartyBotAI*>(p->AI()))return ai->GetRole()==ROLE_TANK;
 return p->GetGroup()&&(p->GetObjectGuid()==p->GetGroup()->GetMainTankGuid()||PartyPreset::PreferredRole(p)==ROLE_TANK);
}
bool AreaSafe(PartyBotAI const& ai,Unit const* center,SpellEntry const* spell,float forcedRadius,float const* ground){
 if(!Scene(ai)||!center||!spell||center->GetMap()!=ai.me->GetMap())return false;
 float radius=forcedRadius;
 for(unsigned i=0;i<3;++i)if(spell->EffectRadiusIndex[i])radius=std::max(radius,Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i])));
 if(radius<=0)radius=10.0f;
 unsigned held=0;bool unsafe=false,threatSafe=true;std::list<Unit*> nearby;
 float x=ground?ground[0]:center->GetPositionX(),y=ground?ground[1]:center->GetPositionY(),z=ground?ground[2]:center->GetPositionZ();
 float search=ground?std::min(100.0f,ai.me->GetDistance(x,y,z)+radius):radius;
 ai.me->GetEnemyListInRadiusAround(ground?static_cast<Unit const*>(ai.me):center,search,nearby);
 for(Unit* enemy:nearby){
  if(enemy&&!enemy->IsWithinDist3d(x,y,z,radius))continue;
  if(!enemy||!enemy->IsAlive()||enemy->GetTypeId()!=TYPEID_UNIT||enemy->GetCharmerOrOwnerPlayerOrPlayerItself())continue;
  if(!enemy->IsVisibleForOrDetect(ai.me,ai.me,false)||!ai.me->GetMap()->isInLineOfSight(x,y,z+1.0f,enemy->GetPositionX(),enemy->GetPositionY(),enemy->GetPositionZ()+1.0f,true))continue;
  Unit* victim=enemy->GetVictim();Player* tank=victim?victim->ToPlayer():nullptr;
  bool controlled=enemy->HasBreakableByDamageCrowdControlAura();
  for(auto mark:ai.m_marksToCC)if(enemy->GetObjectGuid()==ai.me->GetGroup()->GetTargetWithIcon(mark))controlled=true;
  bool heldByTank=tank&&tank->IsInWorld()&&!tank->IsBeingTeleported()&&tank->GetGroup()==ai.me->GetGroup()&&Tank(tank);
  if(controlled||!enemy->IsInCombat()||!heldByTank){unsafe=true;continue;}
  if(!ai.IsValidHostileTarget(enemy))continue;
  ++held;
  if(enemy->CanHaveThreatList()){
   float tankThreat=enemy->GetThreatManager().getThreat(tank),mine=enemy->GetThreatManager().getThreat(ai.me);
   if(tankThreat<=0||mine>=tankThreat*0.8f)threatSafe=false;
  }
 }
 return DungeonTankPolicy::AoeReady(held,unsafe,threatSafe);
}
bool GuardAoe(PartyBotAI& ai){
 if(!Scene(ai)||(ai.GetRole()!=ROLE_MELEE_DPS&&ai.GetRole()!=ROLE_RANGE_DPS))return false;
 for(auto slot:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL})if(Spell* cast=ai.me->GetCurrentSpell(slot)){
  auto spell=cast->m_spellInfo;
  bool channel=(ai.me->GetClass()==CLASS_MAGE&&spell==ai.m_spells.mage.pBlizzard)||(ai.me->GetClass()==CLASS_WARLOCK&&spell==ai.m_spells.warlock.pRainOfFire)||(ai.me->GetClass()==CLASS_HUNTER&&spell==ai.m_spells.hunter.pVolley)||(ai.me->GetClass()==CLASS_DRUID&&spell==ai.m_spells.druid.pHurricane);
  if(!channel)continue;
  float location[3];float const* ground=nullptr;
  if(cast->m_targets.m_targetMask&TARGET_FLAG_DEST_LOCATION){cast->m_targets.getDestination(location[0],location[1],location[2]);ground=location;}
  Unit* center=cast->m_targets.getUnitTarget();if(!center)center=ai.me->GetVictim();if(!center)center=ai.me;
  if(!AreaSafe(ai,center,spell,0,ground)){ai.me->InterruptSpell(slot,true);return true;}
 }
 return false;
}
bool TryAoe(PartyBotAI& ai){
 if(!Scene(ai)||(ai.GetRole()!=ROLE_MELEE_DPS&&ai.GetRole()!=ROLE_RANGE_DPS)||!ai.me->IsInCombat()||ai.me->IsNonMeleeSpellCasted())return false;
 Unit* victim=ai.me->GetVictim();if(!victim||!ai.IsValidHostileTarget(victim))return false;
 // Let the class's existing interrupt/defensive decisions handle live casts.
 if(victim->IsNonMeleeSpellCasted())return false;
 auto cast=[&](SpellEntry const* spell,bool self,float radius=0.0f){
  Unit* center=self?static_cast<Unit*>(ai.me):victim;
  if(!spell||!AreaSafe(ai,center,spell,radius)||!ai.CanTryToCastSpell(center,spell))return false;
  return ai.DoCastSpell(center,spell)==SPELL_CAST_OK;
 };
 switch(ai.me->GetClass()){
  case CLASS_MAGE:return cast(ai.m_spells.mage.pBlizzard,false)||cast(ai.m_spells.mage.pArcaneExplosion,true);
  case CLASS_WARLOCK:return cast(ai.m_spells.warlock.pRainOfFire,false);
  case CLASS_HUNTER:return cast(ai.m_spells.hunter.pVolley,false);
  case CLASS_DRUID:return cast(ai.m_spells.druid.pHurricane,false);
  case CLASS_PALADIN:return cast(ai.m_spells.paladin.pConsecration,true);
  case CLASS_SHAMAN:return cast(ai.m_spells.shaman.pChainLightning,false,30.0f);
  case CLASS_WARRIOR:return cast(ai.m_spells.warrior.pWhirlwind,true)||(!ai.me->GetCurrentSpell(CURRENT_MELEE_SPELL)&&cast(ai.m_spells.warrior.pCleave,false,8.0f));
  case CLASS_ROGUE:return cast(ai.m_spells.rogue.pBladeFlurry,true,8.0f);
  case CLASS_PRIEST:return !ai.me->HasAura(15473)&&cast(ai.m_spells.priest.pHolyNova,true);
  default:return false;
 }
}
Unit* AssistTarget(PartyBotAI const& ai){
 if(!Scene(ai)||(ai.GetRole()!=ROLE_MELEE_DPS&&ai.GetRole()!=ROLE_RANGE_DPS))return nullptr;
 Player* p=ai.me;Player* best=nullptr;Unit* enemy=nullptr;
 for(GroupReference* ref=p->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* tank=ref->getSource()){
  if(!tank->IsInWorld()||!tank->IsAlive()||tank->IsBeingTeleported()||tank->GetMap()!=p->GetMap()||!Tank(tank))continue;
  Unit* current=tank->GetVictim();if(!current)continue;
  Unit* victim=current->GetVictim();Player* victimOwner=victim?victim->GetCharmerOrOwnerPlayerOrPlayerItself():nullptr;
  bool groupVictim=victimOwner&&victimOwner->IsInWorld()&&!victimOwner->IsBeingTeleported()&&victimOwner->GetGroup()==p->GetGroup();
  if(current->GetTypeId()!=TYPEID_UNIT||current->GetCharmerOrOwnerPlayerOrPlayerItself()||!DungeonTankPolicy::Assistable(current->IsAlive(),current->IsInCombat(),groupVictim,ai.IsValidHostileTarget(current),p->IsWithinDistInMap(current,50.0f),p->IsWithinLOSInMap(current)))continue;
  bool main=tank->GetObjectGuid()==p->GetGroup()->GetMainTankGuid();
  bool oldMain=best&&best->GetObjectGuid()==p->GetGroup()->GetMainTankGuid();
  if(!best||(main&&!oldMain)||(main==oldMain&&tank->GetObjectGuid()<best->GetObjectGuid())){best=tank;enemy=current;}
 }
 return enemy;
}
bool Assist(PartyBotAI& ai){
 Unit* enemy=AssistTarget(ai);if(!enemy||ai.me->GetVictim()==enemy)return false;
 return ai.AttackStart(enemy);
}
Unit* Select(PartyBotAI const& ai){
 if(!Active(ai))return nullptr;
 std::lock_guard<std::recursive_mutex> guard(turnsMutex);
 Player* p=ai.me;uint32 now=WorldTimer::getMSTime();
 for(auto it=turns.begin();it!=turns.end();)if(uint32(now-it->second.touched)>300000)it=turns.erase(it);else ++it;
 auto& turn=turns[p->GetGUID()];turn.touched=now;
 for(auto it=turn.served.begin();it!=turn.served.end();)if(uint32(now-it->second)>30000)it=turn.served.erase(it);else ++it;
 std::list<Unit*> enemies;p->GetEnemyListInRadiusAround(p,40.0f,enemies);
 Unit* best=nullptr;Unit* committed=nullptr;DungeonTankPolicy::Candidate bestRank,currentRank;
 for(Unit* enemy:enemies){
  if(!enemy||enemy->GetTypeId()!=TYPEID_UNIT||enemy->GetCharmerOrOwnerPlayerOrPlayerItself())continue;
  Unit* victim=enemy->GetVictim();if(!victim||victim==p)continue;
  Player* owner=victim->GetCharmerOrOwnerPlayerOrPlayerItself();
  if(!owner||!owner->IsInWorld()||owner->IsBeingTeleported()||owner->GetGroup()!=p->GetGroup()||!owner->IsAlive()||owner->GetMap()!=p->GetMap())continue;
  auto* victimAI=owner->AI()?dynamic_cast<PartyBotAI*>(owner->AI()):nullptr;
  DungeonTankPolicy::Candidate rank;rank.guid=enemy->GetGUID();rank.alive=enemy->IsAlive();rank.engaged=enemy->IsInCombat();rank.groupVictim=true;rank.heldByTank=victim==owner&&Tank(owner);
  rank.controlled=enemy->HasBreakableByDamageCrowdControlAura();rank.distance=p->GetDistance(enemy);rank.victimHealth=victim->GetHealthPercent();rank.healerVictim=victimAI?victimAI->GetRole()==ROLE_HEALER:PartyPreset::PreferredRole(owner)==ROLE_HEALER;
  rank.legal=ai.IsValidHostileTarget(enemy)&&p->IsWithinLOSInMap(enemy);rank.reachable=true;auto served=turn.served.find(rank.guid);if(served!=turn.served.end())rank.servedAt=served->second;
  if(!DungeonTankPolicy::Eligible(rank))continue;
  bool reserved=false;
  for(GroupReference* ref=p->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* other=ref->getSource()){
   if(other==p||!other->IsInWorld()||!other->IsAlive()||other->IsBeingTeleported()||other->GetMap()!=p->GetMap()||other->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)||!Tank(other))continue;
   auto state=turns.find(other->GetGUID());
   if(state!=turns.end()&&state->second.rescue==rank.guid&&int32(now-state->second.until)<0){reserved=true;break;}
  }
  if(reserved)continue;
  // Don't chase caster targets on another floor or behind an unreachable ledge.
  if(!p->CanReachWithMeleeAutoAttack(enemy)){
   PathInfo path(p);path.ExcludeSteepSlopes();if(!path.calculate(enemy->GetPositionX(),enemy->GetPositionY(),enemy->GetPositionZ())||path.getPathType()!=PATHFIND_NORMAL||path.Length()>65||path.getPath().empty())continue;
   auto end=path.getPath().back();if(std::hypot(end.x-enemy->GetPositionX(),end.y-enemy->GetPositionY())>3||std::fabs(end.z-enemy->GetPositionZ())>3)continue;
  }
  if(rank.guid==turn.rescue){committed=enemy;currentRank=rank;}
  if(!best||DungeonTankPolicy::Better(rank,bestRank,now)){best=enemy;bestRank=rank;}
 }
 if(committed&&best&&DungeonTankPolicy::KeepCommitment(currentRank,bestRank,int32(now-turn.until)<0))return committed;
 return best;
}
// Shared party reservation: successful native cast is the only event that commits it.
bool MassTaunt(PartyBotAI& ai){
 if(!Active(ai)||ai.me->IsNonMeleeSpellCasted()||ai.me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))return false;
 std::lock_guard<std::recursive_mutex> guard(turnsMutex);
 static std::map<uint64,uint32> last;
 uint64 key=ai.me->GetGroup()->GetLeaderGuid().GetRawValue();uint32 now=WorldTimer::getMSTime();
 if(last[key]&&uint32(now-last[key])<8000)return false;
 std::vector<Player*> candidates;
 for(GroupReference* ref=ai.me->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* tank=ref->getSource())candidates.push_back(tank);
 auto main=ai.me->GetGroup()->GetMainTankGuid();
 std::sort(candidates.begin(),candidates.end(),[&](Player* a,Player* b){bool am=a->GetObjectGuid()==main,bm=b->GetObjectGuid()==main;return am!=bm?am:a->GetObjectGuid()<b->GetObjectGuid();});
 for(Player* tank:candidates){
  auto* actor=dynamic_cast<PartyBotAI*>(tank->AI());
  if(!actor||!Active(*actor)||tank->GetMap()!=ai.me->GetMap()||tank->IsNonMeleeSpellCasted()||tank->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))continue;
  uint32 id=tank->GetClass()==CLASS_WARRIOR?1161:tank->GetClass()==CLASS_DRUID?5209:0;
  auto* spell=id&&tank->HasSpell(id)?sSpellMgr.GetSpellEntry(id):nullptr;
  if(!spell||!actor->CanTryToCastSpell(tank,spell))continue;
  std::list<Unit*> enemies;tank->GetEnemyListInRadiusAround(tank,10,enemies);unsigned leaks=0;bool unsafe=false;
  for(Unit* e:enemies){
   if(!e||!e->IsAlive()||e->GetTypeId()!=TYPEID_UNIT||!actor->IsValidHostileTarget(e))continue;
   Unit* v=e->GetVictim();Player* owner=v?v->GetCharmerOrOwnerPlayerOrPlayerItself():nullptr;
   bool marked=false;for(auto mark:actor->m_marksToCC)if(e->GetObjectGuid()==tank->GetGroup()->GetTargetWithIcon(mark))marked=true;
   if(marked||!e->IsInCombat()||e->HasBreakableByDamageCrowdControlAura()||!owner||owner->GetGroup()!=tank->GetGroup()){unsafe=true;break;}
   if(v==owner&&Tank(owner))continue;
   ++leaks;
  }
  if(unsafe||!WarriorTankPolicy::MassTaunt(leaks,8000))continue;
  if(tank!=ai.me)return false;
  if(actor->DoCastSpell(tank,spell)==SPELL_CAST_OK){last[key]=now;return true;}
 }
 return false;
}
bool Protect(PartyBotAI& ai){
 if(!Active(ai)||ai.me->IsNonMeleeSpellCasted()||ai.me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))return false;
 std::lock_guard<std::recursive_mutex> guard(turnsMutex);
 Unit* enemy=Select(ai);if(!enemy)return false;
 bool changed=ai.me->GetVictim()!=enemy;
 if(changed&&!ai.AttackStart(enemy))return false;
 auto& turn=turns[ai.me->GetGUID()];
 if(turn.rescue!=enemy->GetGUID()||int32(WorldTimer::getMSTime()-turn.until)>=0){turn.rescue=enemy->GetGUID();turn.until=WorldTimer::getMSTime()+4000;}
 if(changed){turn.served[enemy->GetGUID()]=WorldTimer::getMSTime();
  sLog.Out(LOG_BASIC,LOG_LVL_DEBUG,"[DungeonTank] tank=%u intercept=%u victim=%u",ai.me->GetGUIDLow(),enemy->GetGUIDLow(),enemy->GetVictim()?enemy->GetVictim()->GetGUIDLow():0);}
 for(auto* spell:ai.m_spellListTaunt){
  if(!spell||spell->IsAreaOfEffectSpell()||enemy->IsImmuneToSpell(spell,false))continue;
  if(ai.CanTryToCastSpell(enemy,spell)&&ai.DoCastSpell(enemy,spell)==SPELL_CAST_OK){turns[ai.me->GetGUID()].served[enemy->GetGUID()]=WorldTimer::getMSTime();return true;}
 }
 // If autoattack hasn't entered combat yet, don't let the ordinary OOC
 // drinking/follow/leader-assist loop replace the selected rescue target.
 return !ai.me->IsInCombat();
}
}

Unit* PartyBotAI::SelectAttackTarget(Player* pLeader) const
{
    if(Unit* tankTarget=DungeonTank::AssistTarget(*this))return tankTarget;
    if (IsInDuel())
    {
        if (me->m_duel->opponent && IsValidHostileTarget(me->m_duel->opponent))
            return me->m_duel->opponent;
    }
    else
    {
        // Stick to marked target in combat.
        if (me->IsInCombat() || pLeader->GetVictim())
        {
            for (auto markId : m_marksToFocus)
            {
                ObjectGuid targetGuid = me->GetGroup()->GetTargetWithIcon(markId);
                if (targetGuid.IsUnit())
                    if (Unit* pVictim = me->GetMap()->GetUnit(targetGuid))
                        if (IsValidHostileTarget(pVictim))
                            return pVictim;
            }
        }

        // Who is the leader attacking.
        if (Unit* pVictim = pLeader->GetVictim())
        {
            if (IsValidHostileTarget(pVictim))
                return pVictim;
        }
    }

    // Small-dungeon tanks protect party targets before self attackers.
    if(Unit* endangered=DungeonTank::Select(*this))return endangered;

    // Who is attacking me.
    for (const auto pAttacker : me->GetAttackers())
    {
        if (IsValidHostileTarget(pAttacker))
            return pAttacker;
    }

    if (!IsInDuel())
    {
        // Check if other group members are under attack.
        if (Unit* pPartyAttacker = SelectPartyAttackTarget())
            return pPartyAttacker;
    }

    // Assist pet if its in combat.
    if (Pet* pPet = me->GetPet())
    {
        if (Unit* pPetAttacker = pPet->GetAttackerForHelper())
            if (IsValidHostileTarget(pPetAttacker))
                return pPetAttacker;
    }

    return nullptr;
}

Unit* PartyBotAI::SelectPartyAttackTarget() const
{
    if(RaidCoordination::Enabled(me) && m_role==ROLE_TANK)return RaidCoordination::TankTarget(*this);
    if(DungeonTank::Active(*this))return DungeonTank::Select(*this);
    Group* pGroup = me->GetGroup();
    if(!pGroup)return nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // We already checked self.
            if (pMember == me)
                continue;

            for (const auto pAttacker : pMember->GetAttackers())
            {
                if (IsValidHostileTarget(pAttacker) &&
                    me->IsWithinDist(pAttacker, 50.0f))
                    return pAttacker;
            }
        }
    }

    return nullptr;
}

namespace {
struct ResurrectionReservation
{
    ObjectGuid caster;
    uint32 created;
    uint32 castTime;
    uint32 spellId;
};
std::map<uint64, ResurrectionReservation> resurrectionReservations;
bool ReservedCorpse(Player* corpse)
{
    auto it = resurrectionReservations.find(corpse->GetGUID());
    if (it == resurrectionReservations.end())
        return false;
    Player* caster = ObjectAccessor::FindPlayer(it->second.caster);
    bool const valid = caster && caster->IsAlive() && caster->IsInWorld() &&
        !caster->IsBeingTeleported() && corpse->GetGroup() && caster->GetGroup() == corpse->GetGroup() &&
        caster->GetMap() == corpse->GetMap() && !caster->IsInCombat() && !corpse->IsAlive();
    Spell* cast = valid ? caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
    bool const casting = cast && cast->getState() != SPELL_STATE_FINISHED &&
        cast->m_spellInfo->Id == it->second.spellId && cast->m_targets.getUnitTarget() == corpse;
    if (!PartyBotRecoveryPolicy::KeepReservation(valid, casting, corpse->IsRessurectRequested(),
            WorldTimer::getMSTime() - it->second.created, it->second.castTime))
    {
        resurrectionReservations.erase(it);
        return false;
    }
    return true;
}
}

Player* PartyBotAI::SelectResurrectionTarget() const
{
    if (IsInDuel() || !m_resurrectionSpell || me->GetClass() == CLASS_DRUID || !me->GetGroup())
        return nullptr;
    Player* best = nullptr;
    unsigned bestPriority = 100;
    float bestDistance = 0;
    uint32 const now = WorldTimer::getMSTime();
    for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
    {
        Player* p = itr->getSource();
        if (!p || p == me || !p->IsInWorld() || p->IsBeingTeleported() || p->GetDeathState() != CORPSE ||
            p->IsRessurectRequested() || !me->IsWithinDistInMap(p, PartyBotRecoveryPolicy::SearchDistance) || ReservedCorpse(p))
            continue;
        auto const failure = m_failedResurrectionTargets.find(p->GetGUID());
        if (failure != m_failedResurrectionTargets.end() && PartyBotRecoveryPolicy::RetryBlocked(now, failure->second))
            continue;
        unsigned role = 0;
        if (p->IsBot())
            if (auto const* ai = dynamic_cast<PartyBotAI const*>(p->AI()))
                if (ai->m_initialized)
                    role = unsigned(ai->GetRole());
        unsigned priority = RaidBotPlanner::ResurrectionPriority(p->IsBot(), role, p->GetClass());
        priority = priority * 2 + (!p->IsBot() && p->GetObjectGuid() != m_leaderGuid ? 1 : 0);
        float distance = me->GetDistance(p);
        if (!best || priority < bestPriority || (priority == bestPriority &&
            (distance < bestDistance || (distance == bestDistance && p->GetGUID() < best->GetGUID()))))
        { best = p; bestPriority = priority; bestDistance = distance; }
    }
    return best;
}

void PartyBotAI::PrepareRecoveryCast(bool stopMoving)
{
    me->RemoveAurasDueToSpell(PB_SPELL_FOOD);
    me->RemoveAurasDueToSpell(PB_SPELL_DRINK);
    me->SetStandState(UNIT_STAND_STATE_STAND);
    if (stopMoving)
    {
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
    }
}

bool PartyBotAI::TryEmergencyHeal()
{
    if (m_role != ROLE_HEALER || me->IsNonMeleeSpellCasted(false, false, true))
        return false;
    Unit* target = SelectHealTarget(25.0f, 25.0f);
    if (!target || target->GetTypeId() != TYPEID_PLAYER)
        return false;
    // Check affordability/form before interrupting a drink. The native selector still chooses
    // the efficient direct heal, without redirecting a critical player to a raid assignment.
    for (SpellEntry const* spell : m_spellListDirectHeal)
        if (CanTryToCastSpell(target, spell))
        {
            PrepareRecoveryCast(true);
            return HealInjuredTargetDirect(target);
        }
    return false;
}

bool PartyBotAI::TryRecoveryDispel(Unit* target, SpellEntry const* spell)
{
    // Keep native control, power, cooldown and spell-specific shapeshift checks. Do not leave
    // cat/bear/shadow form just to dispel, or cancel a cast already in progress.
    if (me->IsNonMeleeSpellCasted(false, false, true) || !CanTryToCastSpell(target, spell))
        return false;
    PrepareRecoveryCast(spell->GetCastTime(me) > 0);
    return DoCastSpell(target, spell) == SPELL_CAST_OK;
}

namespace
{
bool SafeResurrectionApproach(Player* actor, Player* corpse, SpellEntry const* spell, float& x, float& y, float& z)
{
    SpellRangeEntry const* range = sSpellRangeStore.LookupEntry(spell->rangeIndex);
    if (!range || Spells::GetSpellMaxRange(range) <= 2)
        return false;
    float const castRange = Spells::GetSpellMaxRange(range) - 2.0f;
    PathFinder path(actor);
    path.ExcludeSteepSlopes();
    path.setPathLengthLimit(PartyBotRecoveryPolicy::MaxPathLength + 1);
    if (!path.calculate(corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ()) ||
        path.getPathType() != PATHFIND_NORMAL || path.Length() > PartyBotRecoveryPolicy::MaxPathLength)
        return false;
    // Stop at the first point on the route with range/LOS, not in the middle of the corpse pack.
    bool found = false;
    auto const& route = path.getPath();
    for (size_t i = 1; i < route.size() && !found; ++i)
    {
        auto const from = route[i-1];
        auto const to = route[i];
        unsigned const samples = std::max(1u, unsigned(std::ceil((to-from).length() / 2.0f)));
        for (unsigned n = 1; n <= samples; ++n)
        {
            auto const point = from + (to-from) * (float(n) / samples);
            if (corpse->IsWithinDist3d(point.x, point.y, point.z, castRange) &&
                corpse->IsWithinLOS(point.x, point.y, point.z))
            { x = point.x; y = point.y; z = point.z; found = true; break; }
        }
    }
    if (!found || !path.calculate(x, y, z) || path.getPathType() != PATHFIND_NORMAL ||
        path.Length() > PartyBotRecoveryPolicy::MaxPathLength)
        return false;
    // Native aggro distance includes level difference, detection auras and server aggro rate.
    // Reject conservatively even if a wall might hide the creature: a healer must not pull.
    std::list<Unit*> enemies;
    actor->GetEnemyListInRadiusAround(actor, PartyBotRecoveryPolicy::MaxPathLength + 100.0f, enemies);
    auto const& points = path.getPath();
    for (Unit* enemy : enemies)
    {
        if (!enemy || !enemy->IsAlive())
            continue;
        float const aggro = enemy->GetTypeId() == TYPEID_UNIT ?
            static_cast<Creature*>(enemy)->GetAttackDistance(actor) : 40.0f;
        float const radius = std::max(5.0f, aggro) + enemy->GetCombatReach() + actor->GetCombatReach() + 3.0f;
        for (size_t i = 1; i < points.size(); ++i)
            if (!PartyBotRecoveryPolicy::ClearSegment(
                {points[i-1].x, points[i-1].y, points[i-1].z}, {points[i].x, points[i].y, points[i].z},
                {enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ()}, radius))
                return false;
    }
    return true;
}
}

namespace
{
bool SafeResurrectionMotion(Player* actor)
{
    if (!actor->movespline->Initialized() || actor->movespline->Finalized())
        return true;
    std::list<Unit*> enemies;
    actor->GetEnemyListInRadiusAround(actor, PartyBotRecoveryPolicy::MaxPathLength + 100.0f, enemies);
    auto from = actor->movespline->ComputePosition();
    for (int32 i = actor->movespline->_currentSplineIdx() + 1; i <= int32(actor->movespline->CountSplinePoints()); ++i)
    {
        auto const to = actor->movespline->GetPoint(i);
        for (Unit* enemy : enemies)
        {
            if (!enemy || !enemy->IsAlive()) continue;
            float const aggro = enemy->GetTypeId() == TYPEID_UNIT ?
                static_cast<Creature*>(enemy)->GetAttackDistance(actor) : 40.0f;
            float const radius = std::max(5.0f, aggro) + enemy->GetCombatReach() + actor->GetCombatReach() + 3.0f;
            if (!PartyBotRecoveryPolicy::ClearSegment({from.x, from.y, from.z}, {to.x, to.y, to.z},
                {enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ()}, radius))
                return false;
        }
        from.x = to.x; from.y = to.y; from.z = to.z;
    }
    return true;
}
}

bool PartyBotAI::OwnsResurrectionApproach() const
{
    if (!me || m_resurrectionApproachTarget.IsEmpty()) return false;
    auto const* point = dynamic_cast<PointMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent());
    return point && point->GetId() == 0x5253;
}

void PartyBotAI::StopResurrectionApproach()
{
    // Pop only our own point. A new MC escape, fear, chase or player-issued point wins.
    if (me && me->IsInWorld() && OwnsResurrectionApproach())
    {
        me->StopMoving();
        me->GetMotionMaster()->MovementExpired(false);
        me->GetMotionMaster()->MoveIdle();
    }
    m_resurrectionApproachTarget.Clear();
}

bool PartyBotAI::TryResurrectParty()
{
    if (!me || !me->IsInWorld() || me->IsBeingTeleported() || m_preventCasting || !me->IsAlive() || me->IsInCombat() || me->InBattleGround() || IsInDuel() ||
        me->GetClass() == CLASS_DRUID || !me->GetGroup() || !m_resurrectionSpell ||
        me->IsNonMeleeSpellCasted(false, false, true))
    {
        StopResurrectionApproach();
        return false;
    }
    if (!m_resurrectionApproachTarget.IsEmpty() && !OwnsResurrectionApproach())
    {
        StopResurrectionApproach(); // metadata only; never cancel a replacement movement
        return false;
    }
    if (OwnsResurrectionApproach() && !SafeResurrectionMotion(me))
    {
        m_failedResurrectionTargets[m_resurrectionApproachTarget.GetRawValue()] = WorldTimer::getMSTime();
        StopResurrectionApproach();
        return false;
    }
    auto const motion = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (!OwnsResurrectionApproach() && motion != IDLE_MOTION_TYPE && motion != FOLLOW_MOTION_TYPE)
        return false;
    for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
        if (Player* p = itr->getSource())
            if (p->GetMap() == me->GetMap() && (p->IsInCombat() || (p->GetPet() && p->GetPet()->IsInCombat())))
            {
                StopResurrectionApproach();
                return false;
            }
    for (auto it = resurrectionReservations.begin(); it != resurrectionReservations.end();)
    {
        auto current = it++;
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid(current->first));
        if (!p)
            resurrectionReservations.erase(current);
        else
            ReservedCorpse(p); // removes interrupted, completed and invalid-context casts now
    }
    uint32 const recoveryNow = WorldTimer::getMSTime();
    for (auto it = m_failedResurrectionTargets.begin(); it != m_failedResurrectionTargets.end();)
        if (!PartyBotRecoveryPolicy::RetryBlocked(recoveryNow, it->second))
            it = m_failedResurrectionTargets.erase(it);
        else
            ++it;
    Player* target = SelectResurrectionTarget();
    if (!target)
    {
        StopResurrectionApproach();
        return false;
    }
    Powers const powerType = Powers(m_resurrectionSpell->powerType);
    bool const healthCost = powerType == POWER_HEALTH;
    uint32 const available = healthCost ? me->GetHealth() : me->GetPower(powerType);
    if (!PartyBotRecoveryPolicy::CanAfford(available, Spell::CalculatePowerCost(m_resurrectionSpell, me), healthCost) ||
        !me->IsSpellReady(m_resurrectionSpell) || me->HasGCD(m_resurrectionSpell))
    {
        StopResurrectionApproach();
        return false; // continue drinking only until one resurrection is affordable
    }

    if (!m_resurrectionSpell->IsTargetInRange(me, target) || !me->IsWithinLOSInMap(target))
    {
        uint32 const now = WorldTimer::getMSTime();
        bool const changed = m_resurrectionApproachTarget != target->GetObjectGuid();
        if (changed)
        {
            m_resurrectionApproachTarget = target->GetObjectGuid();
            m_resurrectionApproachStarted = now;
        }
        if (PartyBotRecoveryPolicy::ApproachExpired(now, m_resurrectionApproachStarted))
        {
            m_failedResurrectionTargets[target->GetGUID()] = now;
            StopResurrectionApproach();
            return true; // next update can choose another corpse
        }
        float x = 0, y = 0, z = 0;
        if (!SafeResurrectionApproach(me, target, m_resurrectionSpell, x, y, z))
        {
            m_failedResurrectionTargets[target->GetGUID()] = now;
            StopResurrectionApproach();
            return false;
        }
        if (changed || me->IsStopped() || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
        {
            PrepareRecoveryCast(false);
            me->GetMotionMaster()->MovePoint(0x5253, x, y, z,
                MOVE_PATHFINDING | MOVE_RUN_MODE | MOVE_EXCLUDE_STEEP_SLOPES);
            // Validate what MovePoint actually issued, not just the earlier candidate route.
            if (!SafeResurrectionMotion(me))
            {
                m_failedResurrectionTargets[target->GetGUID()] = now;
                StopResurrectionApproach();
                return false;
            }
        }
        return true;
    }
    if (!CanTryToCastSpell(target, m_resurrectionSpell))
    {
        StopResurrectionApproach();
        return false;
    }
    StopResurrectionApproach();
    PrepareRecoveryCast(true);
    m_resurrectionApproachTarget.Clear();
    resurrectionReservations[target->GetGUID()] = {me->GetObjectGuid(), WorldTimer::getMSTime(),
        m_resurrectionSpell->GetCastTime(me), m_resurrectionSpell->Id};
    if (DoCastSpell(target, m_resurrectionSpell) == SPELL_CAST_OK)
        return true;
    resurrectionReservations.erase(target->GetGUID());
    return false;
}

Player* PartyBotAI::SelectShieldTarget() const
{
    if (IsInDuel())
        return nullptr;

    Group* pGroup = me->GetGroup();
    if(!pGroup)return nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // We already checked self.
            if (pMember == me)
                continue;

            if ((pMember->GetHealthPercent() < 90.0f) &&
                !pMember->GetAttackers().empty() &&
                !pMember->IsImmuneToMechanic(MECHANIC_SHIELD))
                return pMember;
        }
    }

    return nullptr;
}

bool PartyBotAI::CrowdControlMarkedTargets()
{
    SpellEntry const* pSpellEntry = GetCrowdControlSpell();
    if (!pSpellEntry)
        return false;

    for (auto mark : m_marksToCC)
    {
        if (Unit* pTarget = GetMarkedTarget(mark))
        {
            if (!pTarget->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                IsValidHostileTarget(pTarget) && !AreOthersOnSameTarget(pTarget->GetObjectGuid()))
            {
                if (CanTryToCastSpell(pTarget, pSpellEntry))
                {
                    if (DoCastSpell(pTarget, pSpellEntry) == SPELL_CAST_OK)
                    {
                        me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void PartyBotAI::AddToPlayerGroup()
{
    Player* pPlayer = ObjectAccessor::FindPlayer(m_leaderGuid);
    if (!pPlayer)
        return;

    Group* group = pPlayer->GetGroup();
    if (!group)
    {
        group = new Group;
        // new group: if can't add then delete
        if (!group->Create(pPlayer->GetObjectGuid(), pPlayer->GetName()))
        {
            delete group;
            return;
        }
        sObjectMgr.AddGroup(group);
    }

    if (me->GetGroup() != group)
    {
        if (me->GetGroup())
            me->RemoveFromGroup();

        group->AddMember(me->GetObjectGuid(), me->GetName());
    }
}

void PartyBotAI::OnPacketReceived(WorldPacket const* packet)
{
    if(PartyBotServices::Packet(*this,*packet))return;
    //printf("Bot received %s\n", LookupOpcodeName(packet->GetOpcode()));
    switch (packet->GetOpcode())
    {
        case SMSG_LEARNED_SPELL:
        case SMSG_SUPERCEDED_SPELL:
        case SMSG_REMOVED_SPELL:
        {
            if (m_initialized)
                m_resetSpellData = true;
            return;
        }
        case SMSG_DUEL_REQUESTED:
        {
            auto data = std::make_unique<WorldPackets::Duel::DuelAccepted>();
            data->playerGuid = me->GetObjectGuid();
            me->GetSession()->QueuePacket(std::move(data));
            return;
        }
        case SMSG_PARTYKILLLOG:
        {
            if (!me)
                return;

            if (Group const* pGroup = me->GetGroup())
            {
                if (pGroup->GetLootMethod() == ROUND_ROBIN ||
                    pGroup->GetLootMethod() == GROUP_LOOT ||
                    pGroup->GetLootMethod() == NEED_BEFORE_GREED)
                {
                    ObjectGuid victimGuid = *(((uint64*)(*packet).contents()) + 1);
                    if (Creature* pCreature = me->GetMap()->GetCreature(victimGuid))
                    {
                        pCreature->m_Events.AddLambdaEventAtOffset([pCreature, guid = me->GetGUID()]()
                        {
                            if (pCreature->loot.roundRobinPlayer == guid)
                            {
                                // unassign loot from bot so real players can loot
                                pCreature->loot.roundRobinPlayer = 0;
                                pCreature->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
                            }
                        }, 1);
                    }
                }
            }

            return;
        }
    }

    CombatBotBaseAI::OnPacketReceived(packet);
}

void PartyBotAI::OnPlayerLogin()
{
    if (!m_initialized)
        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
}

void PartyBotAI::UpdateAI(uint32 const diff)
{
    // Removal is a registry request, including not-yet-initialized avatars.
    // Do not configure/join a group after an owner cancelled a pending bot.
    if (botEntry && botEntry->requestRemoval) { StopResurrectionApproach(); return; }
    m_updateTimer.Update(diff);
    if (m_updateTimer.Passed())
        m_updateTimer.Reset(PB_UPDATE_INTERVAL);
    else
        return;

    if (!me->IsInWorld() || me->IsBeingTeleported())
    { StopResurrectionApproach(); return; }

    if (!m_initialized)
    {
        AddToPlayerGroup();

        if (m_race && m_class) // temporary character
        {
            if (m_level && m_level != me->GetLevel())
            {
                me->GiveLevel(m_level);
                me->InitTalentForLevel();
                me->SetUInt32Value(PLAYER_XP, 0);
            }

            if (!m_cloneGuid.IsEmpty())
            {
                CloneFromPlayer(sObjectAccessor.FindPlayer(m_cloneGuid));
                AutoAssignRole();
            }
            else if (m_presetSpecId)
            {
                std::string result;
                if (!PartyPreset::Initialize(*this, result))
                {
                    if (Player* owner = ObjectAccessor::FindPlayer(m_leaderGuid))
                        ChatHandler(owner).SendSysMessage(("GMUI|ERROR|Preset companion initialization failed: " + result).substr(0,230).c_str());
                    botEntry->requestRemoval = true;
                    return;
                }
                if (Player* owner = ObjectAccessor::FindPlayer(m_leaderGuid))
                    ChatHandler(owner).SendSysMessage(("GMUI|OK|Party companion ready: " + result).substr(0,230).c_str());
            }
            else
            {
                LearnPremadeSpecForClass();

                if (m_role == ROLE_INVALID)
                    AutoAssignRole();

                AutoEquipGear(sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_AUTO_EQUIP));

                // fix client bug causing some item slots to not be visible
                if (Player* pLeader = GetPartyLeader())
                {
                    me->SetVisibility(VISIBILITY_OFF);
                    pLeader->UpdateVisibilityOf(pLeader, me);
                    me->SetVisibility(VISIBILITY_ON);
                }
            }
            me->UpdateSkillsToMaxSkillsForLevel();
        }
        else // loaded from db
        {
            if (m_role == ROLE_INVALID)
                AutoAssignRole();

            if (me->IsGameMaster())
                me->SetGameMaster(false);

            me->TeleportTo(m_mapId, m_x, m_y, m_z, m_o);
        }

        PartyBotServices::Initialize(*this);
        ResetSpellData();
        PopulateSpellData();
        AddAllSpellReagents();
        // A ready check must not require the first failed ranged attack to
        // initialize ammo. Only newly generated quick-raid companions use this.
        if(m_quickRaid && m_race && m_class && me->GetClass()==CLASS_HUNTER)AddHunterAmmo();
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
        SummonPetIfNeeded();
        me->SetHealthPercent(100.0f);
        me->SetPowerPercent(me->GetPowerType(), 100.0f);

        uint32 newzone, newarea;
        me->GetZoneAndAreaId(newzone, newarea);
        me->UpdateZone(newzone, newarea);

        m_initialized = true;
        if(m_quickRaid)PartyPreset::ArrangeRaid(GetPartyLeader());
        return;
    }

    if (m_resetSpellData)
    {
        ResetSpellData();
        PopulateSpellData();
        m_resetSpellData = false;
    }

    PveDecisionRuntime::Observe(*this, m_pveDecision);
    Player* pLeader = GetPartyLeader();
    if (!pLeader)
    {
        StopResurrectionApproach();
        botEntry->requestRemoval = true;
        return;
    }

    if (!pLeader->IsInWorld())
    { StopResurrectionApproach(); return; }

    if(m_quickRaid)PartyPreset::ArrangeRaid(pLeader);

    if (pLeader->InBattleGround() &&
        !me->InBattleGround())
    {
        StopResurrectionApproach();
        if (m_receivedBgInvite)
        {
            SendBattlefieldPortPacket();
            m_receivedBgInvite = false;
            return;
        }

        // Remain idle until we can join battleground.
        StopResurrectionApproach();
        return;
    }

    if (pLeader->IsTaxiFlying())
    {
        StopResurrectionApproach();
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        return;
    }

    if (me->HasUnitState(UNIT_STATE_FEIGN_DEATH) && me->HasAuraType(SPELL_AURA_FEIGN_DEATH) &&
       !me->IsInCombat() && (!me->GetPet() || !me->GetPet()->IsInCombat()) &&
       !me->SelectRandomUnfriendlyTarget(nullptr, 20.0f, false, true))
        me->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        StopResurrectionApproach();
        BreakCrowdControlEffects();
        return;
    }

    if (me->IsDead())
    {
        StopResurrectionApproach();
        if (me->InBattleGround())
        {
            if (me->GetDeathState() == CORPSE)
            {
                me->BuildPlayerRepop();
                me->RepopAtGraveyard();
            }
        }
        else
        {
            if (ShouldAutoRevive())
            {
                me->ResurrectPlayer(0.5f);
                me->SpawnCorpseBones();
                CastPartySpell(me, PB_SPELL_HONORLESS_TARGET, true);
            }
        }

        return;
    }

    bool recoveryBlocked = me->IsInCombat() || !me->GetGroup() || m_preventCasting;
    if (Group* group = me->GetGroup())
        for (GroupReference* it = group->GetFirstMember(); it; it = it->next())
            if (Player* member = it->getSource())
                if (member->GetMap() == me->GetMap() && (member->IsInCombat() ||
                    (member->GetPet() && member->GetPet()->IsInCombat())))
                    recoveryBlocked = true;
    if (recoveryBlocked) StopResurrectionApproach();
    if(RaidCoordination::UpdatePull(*this))return;
    if(McBotTactics::Update(*this))return;
    if(RaidCoordination::Enabled(me))
        for(auto slot:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL,CURRENT_AUTOREPEAT_SPELL})
            if(Spell* current=me->GetCurrentSpell(slot))
                if(BotDamagePolicy::IsDamage(current->m_spellInfo))
                {
                    Unit* target=current->m_targets.getUnitTarget();
                    if(!target)target=me->GetVictim();
                    bool areaAllowed=true;
                    if(current->m_spellInfo->IsAreaOfEffectSpell())
                    {
                        if(current->m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
                        {
                            float x,y,z;current->m_targets.getDestination(x,y,z);
                            areaAllowed=RaidCoordination::AllowAreaDamage(*this,current->m_spellInfo,x,y,z);
                        }
                        else if(!target)areaAllowed=false;
                    }
                    if(!areaAllowed || !McBotTactics::AllowSpell(*this,target,current->m_spellInfo) ||
                       !RaidCoordination::AllowDamage(*this,target,current->m_spellInfo))
                        me->InterruptSpell(slot,true);
                }
    if(me->GetVictim() && !RaidCoordination::AllowDamage(*this,me->GetVictim(),nullptr)){
        me->AttackStop();
        if(Pet* pet=me->GetPet()){pet->AttackStop();pet->GetMotionMaster()->MoveFollow(me,2.0f,0.0f);}
    }
    if(me->GetTradeData())m_isBuffing=false;
    if(DungeonTank::GuardAoe(*this))return;
    if(PartyBotServices::Update(*this))return;

    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL)&&PartyBotFormation::Update(*this))return;
    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        // Stop auto shot if no target.
        if (!me->GetVictim())
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
        else if (me->GetClass() == CLASS_HUNTER)
        {
            if (me->GetCombatDistance(me->GetVictim()) < 8.0f)
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
            else
                UpdateInCombatAI_Hunter();
        }

        return;
    }

    if (Spell* pCurrentSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        // Interrupt pre casted heals if target is not injured.
        if (pCurrentSpell->getState() == SPELL_STATE_PREPARING &&
            pCurrentSpell->m_spellInfo->IsHealSpell())
        {
            if (Unit* pTarget = pCurrentSpell->m_targets.getUnitTarget())
            {
                if (pTarget->GetHealth() == pTarget->GetMaxHealth())
                {
                    me->InterruptSpell(CURRENT_GENERIC_SPELL, true);
                }
            }
        }
    }

    if (me->IsNonMeleeSpellCasted(false, false, true))
        return;

    if (me->GetTargetGuid() == me->GetObjectGuid())
        me->ClearTarget();

    if (DungeonTank::MassTaunt(*this))return;
    if (DungeonTank::Protect(*this))return;
    if (DungeonTank::Assist(*this))return;

    if (!me->IsInCombat())
    {
        if (TryEmergencyHeal()) return;
        if (TryResurrectParty()) return;
        if (CheckForDispelTargets()) return;
        if (DrinkAndEat())
        {
            if (!me->IsWithinDistInMap(pLeader, 100.0f))
            {
                me->SetHealth(me->GetMaxHealth());
                if (me->GetPowerType() == POWER_MANA)
                    me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));
            }
            else if (me->IsMounted())
                me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
            return;
        }

        // Teleport to leader if too far away.
        if (!me->IsWithinDistInMap(pLeader, 100.0f) && !IsInDuel())
        {
            if (!me->IsStopped())
                me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
            char name[128] = {};
            snprintf(name, sizeof(name), "%s", pLeader->GetName());
            ChatHandler(me).HandleGonameCommand(name);
            return;
        }
    }

    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);

    if (me->GetSheath() == SHEATH_STATE_UNARMED && !me->IsMounted())
        me->SetSheath(SHEATH_STATE_MELEE);

    if (!me->IsInCombat() && !me->IsMounted())
    {
        UpdateOutOfCombatAI();

        if (m_isBuffing)
            return;

        if (me->IsNonMeleeSpellCasted())
            return;
    }

    if(DungeonLoot::Update(*this,pLeader))return;

    Unit* pVictim = me->GetVictim();

    if (GetRole() != ROLE_HEALER)
    {
        if (!pVictim || !IsValidHostileTarget(pVictim))
        {
            if (pVictim)
                me->AttackStop();

            if (Unit* pVictim = SelectAttackTarget(pLeader))
            {
                AttackStart(pVictim);
                return;
            }
        }
    }

    if (!me->IsInCombat())
    {
        // Mount if leader is mounted and we don't have a target.
        if (pLeader->IsMounted() && !me->GetVictim())
        {
            uint32 ownMount=BotMountPolicy::Select(me->GetGUIDLow(),me->GetRace(),me->GetClass(),me->GetLevel());
            if(me->IsMounted())for(auto* aura:me->GetAurasByType(SPELL_AURA_MOUNTED))
                if(!BotMountPolicy::Allows(aura->GetId(),me->GetRace(),me->GetClass(),me->GetLevel())){me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);break;}
            if (!me->IsMounted() && ownMount)
            {
                // Leave shapeshift before mounting.
                if (me->IsInDisallowedMountForm() &&
                    me->GetDisplayId() != me->GetNativeDisplayId() &&
                    me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
                    me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

                // Copy the leader's mounted state, never their class/race spell.
                if (sSpellMgr.GetSpellEntry(ownMount))
                {
                    bool oldStateCastTime = me->HasCheatOption(PLAYER_CHEAT_NO_CAST_TIME);
                    bool oldStatePower = me->HasCheatOption(PLAYER_CHEAT_NO_POWER);
                    me->SetCheatOption(PLAYER_CHEAT_NO_CAST_TIME, true);
                    me->SetCheatOption(PLAYER_CHEAT_NO_POWER, true);
                    CastPartySpell(me, ownMount, true);
                    me->SetCheatOption(PLAYER_CHEAT_NO_CAST_TIME, oldStateCastTime);
                    me->SetCheatOption(PLAYER_CHEAT_NO_POWER, oldStatePower);
                }
            }
        }
        else if (me->IsMounted())
            me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
    }

    if (me->IsInCombat()) {
        if (TryEmergencyHeal()) return;
        if (CheckForDispelTargets()) return;
    }
    if (PartyBotFormation::Update(*this)) return;
    if (!me->IsMoving()&&!PartyBotFormation::Holding(*this))
    {
        if (!pVictim)
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                me->GetMotionMaster()->MoveFollow(pLeader, urand(PB_MIN_FOLLOW_DIST, PB_MAX_FOLLOW_DIST), frand(PB_MIN_FOLLOW_ANGLE, PB_MAX_FOLLOW_ANGLE));
        }
        else
        {
            if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
               (GetRole() == ROLE_MELEE_DPS || m_role == ROLE_TANK) &&
                IsValidHostileTarget(pVictim) &&
                AttackStart(pVictim))
                return;

            switch (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
            {
                case IDLE_MOTION_TYPE:
                case FOLLOW_MOTION_TYPE:
                    BeginChasing(pVictim);
                    break;
            }
        }
    }

    if (me->IsInCombat())
        UpdateInCombatAI();
}


void PartyBotAI::UpdateOutOfCombatAI()
{
    if (TryEmergencyHeal()) return;
    if (!IsInDuel())
    {
        if(TryResurrectParty())return;

        if (m_role != ROLE_TANK && me->GetVictim() && CrowdControlMarkedTargets())
            return;
    }

    if (CheckForDispelTargets())
        return;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateOutOfCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateOutOfCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateOutOfCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateOutOfCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateOutOfCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateOutOfCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateOutOfCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateOutOfCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateOutOfCombatAI_Druid();
            break;
    }
}

void PartyBotAI::UpdateInCombatAI()
{
    if (TryEmergencyHeal()) return;
    if(RaidCoordination::TryMassTaunt(*this))return;
    if (!IsInDuel())
    {
        if (m_role == ROLE_TANK)
        {
            Unit* pVictim = me->GetVictim();
            if(RaidCoordination::Enabled(me)){
                Unit* assigned=RaidCoordination::TankTarget(*this);
                if(pVictim && RaidCoordination::AssignedTank(me,pVictim)!=me){me->AttackStop();pVictim=nullptr;}
                if(assigned && assigned!=pVictim){AttackStart(assigned);pVictim=me->GetVictim();}
            }

            // Defend party members.
            if (!DungeonTank::Active(*this) && (!pVictim || pVictim->GetVictim() == me))
            {
                if (pVictim = SelectPartyAttackTarget())
                {
                    me->AttackStop(true);
                    AttackStart(pVictim);
                }
            }

            // Taunt target if its attacking someone else.
            if (!DungeonTank::Active(*this) && pVictim && pVictim->GetVictim() != me)
            {
                for (const auto& pSpellEntry : m_spellListTaunt)
                {
                    // Coordinated area taunts are reserved by TryMassTaunt;
                    // do not bypass that policy through the single-target loop.
                    if(RaidCoordination::Enabled(me) && pSpellEntry->IsAreaOfEffectSpell())continue;
                    if (CanTryToCastSpell(pVictim, pSpellEntry))
                    {
                        if (DoCastSpell(pVictim, pSpellEntry) == SPELL_CAST_OK)
                            return;
                    }
                }
            }
        }
        else if (CrowdControlMarkedTargets())
            return;
    }

    if (CheckForDispelTargets())
        return;

    if(DungeonTank::TryAoe(*this))return;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateInCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateInCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateInCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateInCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateInCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateInCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateInCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateInCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateInCombatAI_Druid();
            break;
    }

    if (me->GetVictim())
        UseTrinketEffects();
}

bool PartyBotAI::CheckForDispelTargets()
{
    // Spell-specific native form restrictions decide; Shadowform can dispel magic, while
    // a feral tank must not leave its form just to cast an illegal dispel.

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
        {
            if (m_spells.paladin.pCleanse)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.paladin.pCleanse))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.paladin.pCleanse))
                    {
                        if (TryRecoveryDispel(pFriend, m_spells.paladin.pCleanse))
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_SHAMAN:
        {
            if (m_spells.shaman.pCureDisease)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.shaman.pCureDisease))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.shaman.pCureDisease))
                    {
                        if (TryRecoveryDispel(pFriend, m_spells.shaman.pCureDisease))
                            return true;
                    }
                }
            }

            if (m_spells.shaman.pCurePoison)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.shaman.pCurePoison))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.shaman.pCurePoison))
                    {
                        if (TryRecoveryDispel(pFriend, m_spells.shaman.pCurePoison))
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_MAGE:
        {
            if (m_spells.mage.pRemoveLesserCurse)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.mage.pRemoveLesserCurse))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.mage.pRemoveLesserCurse))
                    {
                        if (TryRecoveryDispel(pFriend, m_spells.mage.pRemoveLesserCurse))
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_PRIEST:
        {
            if (m_spells.priest.pDispelMagic)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pDispelMagic))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.priest.pDispelMagic))
                    {
                        if (TryRecoveryDispel(pFriend, m_spells.priest.pDispelMagic))
                            return true;
                    }
                }
            }
            if (m_spells.priest.pAbolishDisease)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pAbolishDisease))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.priest.pAbolishDisease))
                    {
                        if (TryRecoveryDispel(pFriend, m_spells.priest.pAbolishDisease))
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_DRUID:
        {
            SpellEntry const* pDispelSpell = m_spells.druid.pAbolishPoison ?
                m_spells.druid.pAbolishPoison :
                m_spells.druid.pCurePoison;

            if (pDispelSpell)
            {
                if (Unit* pFriend = SelectDispelTarget(pDispelSpell))
                {
                    if (CanTryToCastSpell(pFriend, pDispelSpell))
                    {
                        if (TryRecoveryDispel(pFriend, pDispelSpell))
                            return true;
                    }
                }
            }

            if (m_spells.druid.pRemoveCurse)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.druid.pRemoveCurse))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.druid.pRemoveCurse))
                    {
                        if (TryRecoveryDispel(pFriend, m_spells.druid.pRemoveCurse))
                            return true;
                    }
                }
            }
            break;
        }
    }

    return false;
}

namespace {
SpellEntry const* KnownSupport(Player* player,char const* name) {
    SpellEntry const* best=nullptr;
    for(auto const& known:player->GetSpellMap()){
        if(known.second.state==PLAYERSPELL_REMOVED || known.second.disabled)continue;
        SpellEntry const* spell=sSpellMgr.GetSpellEntry(known.first);
        if(spell && spell->SpellName[0]==name && (!best || spell->spellLevel>best->spellLevel))best=spell;
    }
    return best;
}
}
void PartyBotAI::ConfigureRaidSupport() {
    if(!m_quickRaid || !me->GetGroup())return;
    bool melee=false,tank=false;
    for(GroupReference* itr=me->GetGroup()->GetFirstMember();itr;itr=itr->next())if(Player* p=itr->getSource())if(p->GetSubGroup()==me->GetSubGroup()){
        unsigned role=PartyPreset::PreferredRole(p);tank=tank||role==ROLE_TANK;melee=melee||role==ROLE_TANK||role==ROLE_MELEE_DPS||p->GetClass()==CLASS_ROGUE||p->GetClass()==CLASS_WARRIOR;
    }
    if(me->GetClass()==CLASS_PALADIN){if(auto spell=KnownSupport(me,tank?"Devotion Aura":"Concentration Aura"))m_spells.paladin.pAura=spell;}
    if(me->GetClass()==CLASS_SHAMAN){
        m_spells.shaman.pAirTotem=KnownSupport(me,melee?"Windfury Totem":"Tranquil Air Totem");
        m_spells.shaman.pEarthTotem=KnownSupport(me,melee?"Strength of Earth Totem":"Stoneskin Totem");
        m_spells.shaman.pWaterTotem=KnownSupport(me,"Mana Spring Totem");
        // Never let a randomly selected offensive fire totem pull idle mobs.
        m_spells.shaman.pFireTotem=nullptr;
    }
}
bool PartyBotAI::BuffRaidBlessings() {
    if(!m_quickRaid || me->GetClass()!=CLASS_PALADIN || !me->GetGroup())return false;
    std::vector<PartyBotAI const*> paladins;
    for(auto const* ai:sPlayerBotMgr.GetOwnedPartyBots(m_leaderGuid))if(ai->m_initialized && ai->me && ai->me->GetGroup()==me->GetGroup() && ai->me->GetClass()==CLASS_PALADIN && ai->me->IsAlive())paladins.push_back(ai);
    std::sort(paladins.begin(),paladins.end(),[](PartyBotAI const* a,PartyBotAI const* b){return a->me->GetGUID()<b->me->GetGUID();});
    unsigned index=0;for(;index<paladins.size()&&paladins[index]!=this;++index){}
    for(GroupReference* itr=me->GetGroup()->GetFirstMember();itr;itr=itr->next())if(Player* target=itr->getSource()){
        if(!target->IsAlive() || target->GetMap()!=me->GetMap() || !me->IsWithinLOSInMap(target))continue;
        unsigned role=PartyPreset::PreferredRole(target);char const* name=nullptr;
        switch(index%4){
            case 0:name="Blessing of Kings";break;
            case 1:if(role!=ROLE_TANK)name="Blessing of Salvation";break;
            case 2:name=(role==ROLE_HEALER || target->GetClass()==CLASS_MAGE || target->GetClass()==CLASS_WARLOCK || target->GetClass()==CLASS_PRIEST)?"Blessing of Wisdom":"Blessing of Might";break;
            case 3:name="Blessing of Light";break;
        }
        if(!name)continue;
        SpellEntry const* spell=KnownSupport(me,name);
        if(spell && !target->HasAura(spell->Id) && CanTryToCastSpell(target,spell) && DoCastSpell(target,spell)==SPELL_CAST_OK){m_isBuffing=true;return true;}
    }
    m_isBuffing=false;return false;
}
void PartyBotAI::UpdateOutOfCombatAI_Paladin()
{
    ConfigureRaidSupport();
    if (m_spells.paladin.pAura &&
        CanTryToCastSpell(me, m_spells.paladin.pAura))
    {
        if (DoCastSpell(me, m_spells.paladin.pAura) == SPELL_CAST_OK)
            return;
    }

    if (m_role == ROLE_TANK &&
        m_spells.paladin.pRighteousFury &&
        CanTryToCastSpell(me, m_spells.paladin.pRighteousFury))
    {
        if (DoCastSpell(me, m_spells.paladin.pRighteousFury) == SPELL_CAST_OK)
            return;
    }

    if(m_quickRaid && BuffRaidBlessings())return;
    if (!m_quickRaid && m_spells.paladin.pBlessingBuff)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.paladin.pBlessingBuff))
        {
            if (CanTryToCastSpell(pTarget, m_spells.paladin.pBlessingBuff))
            {
                if (DoCastSpell(pTarget, m_spells.paladin.pBlessingBuff) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_isBuffing &&
       (!m_spells.paladin.pBlessingBuff ||
        !me->HasGCD(m_spells.paladin.pBlessingBuff)))
    {
        m_isBuffing = false;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;
}

void PartyBotAI::UpdateInCombatAI_Paladin()
{
    if (m_spells.paladin.pDivineShield &&
       (me->GetHealthPercent() < 20.0f) &&
       (m_role != ROLE_TANK) &&
        CanTryToCastSpell(me, m_spells.paladin.pDivineShield))
    {
        if (DoCastSpell(me, m_spells.paladin.pDivineShield) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pFriend = me->FindLowestHpFriendlyUnit(30.0f, 70, true, me))
    {
        if (m_spells.paladin.pBlessingOfProtection &&
           !IsPhysicalDamageClass(pFriend->GetClass()) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pBlessingOfSacrifice &&
           (me->GetHealthPercent() > 80.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pLayOnHands &&
           (pFriend->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pLayOnHands))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;
        }
    }

    if (!me->GetAttackers().empty())
    {
        if (m_spells.paladin.pHolyShield &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShield))
        {
            if (DoCastSpell(me, m_spells.paladin.pHolyShield) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.paladin.pTurnEvil &&
            m_role != ROLE_TANK)
        {
            Unit* pAttacker = SelectAttackerDifferentFrom(me->GetVictim());
            if (pAttacker && pAttacker->GetCreatureType() == CREATURE_TYPE_UNDEAD &&
                CanTryToCastSpell(pAttacker, m_spells.paladin.pTurnEvil))
            {
                if (DoCastSpell(pAttacker, m_spells.paladin.pTurnEvil) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (GetRole() == ROLE_HEALER)
    {
        if (m_spells.paladin.pHolyShock &&
            me->GetHealthPercent() < 50.0f &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShock))
        {
            if (m_spells.paladin.pDivineFavor &&
                CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
            {
                DoCastSpell(me, m_spells.paladin.pDivineFavor);
            }

            if (DoCastSpell(me, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                return;
        }

        if (FindAndHealInjuredAlly(80.0f, 90.0f))
            return;

        if (FindAndPreHealTarget())
            return;
    }
    else
    {
        if (m_spells.paladin.pLayOnHands &&
           (me->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(me, m_spells.paladin.pLayOnHands))
        {
            if (DoCastSpell(me, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;
        }

        bool const hasSeal = m_spells.paladin.pSeal && me->HasAura(m_spells.paladin.pSeal->Id);

        if (!hasSeal &&
            m_spells.paladin.pSeal &&
            CanTryToCastSpell(me, m_spells.paladin.pSeal))
        {
            CastPartySpell(me, m_spells.paladin.pSeal, false);
        }

        if (Unit* pVictim = me->GetVictim())
        {
            if (hasSeal && m_spells.paladin.pJudgement &&
               (me->GetPowerPercent(POWER_MANA) > 30.0f) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pJudgement))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pJudgement) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHammerOfJustice &&
               (pVictim->IsNonMeleeSpellCasted() ||
               (me->GetHealthPercent() < 20.0f && !me->GetAttackers().empty())) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfJustice))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfJustice) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHammerOfWrath &&
                pVictim->GetHealthPercent() < 20.0f &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfWrath))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfWrath) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pConsecration &&
               (GetAttackersInRangeCount(10.0f) > 2) &&
                CanTryToCastSpell(me, m_spells.paladin.pConsecration))
            {
                if (DoCastSpell(me, m_spells.paladin.pConsecration) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHolyShock &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHolyShock))
            {
                if (m_spells.paladin.pDivineFavor &&
                    CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
                {
                    DoCastSpell(me, m_spells.paladin.pDivineFavor);
                }

                if (DoCastSpell(pVictim, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pExorcism &&
                pVictim->IsCreature() &&
                (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pExorcism))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pExorcism) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHolyWrath &&
                pVictim->IsCreature() &&
               (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD ||
                pVictim->GetCreatureType() == CREATURE_TYPE_DEMON) &&
               (me->GetAttackers().size() < 3) && // too much pushback
                CanTryToCastSpell(pVictim, m_spells.paladin.pHolyWrath))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHolyWrath) == SPELL_CAST_OK)
                    return;
            }
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }
        }
    }

    if (m_spells.paladin.pBlessingOfFreedom &&
       (me->HasUnitState(UNIT_STATE_ROOT) || me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
        CanTryToCastSpell(me, m_spells.paladin.pBlessingOfFreedom))
    {
        if (DoCastSpell(me, m_spells.paladin.pBlessingOfFreedom) == SPELL_CAST_OK)
            return;
    }

    if (GetRole() != ROLE_HEALER &&
        me->GetHealthPercent() < 30.0f)
        HealInjuredTarget(me);
}

void PartyBotAI::UpdateOutOfCombatAI_Shaman()
{
    ConfigureRaidSupport();
    if (m_spells.shaman.pWeaponBuff &&
        CanTryToCastSpell(me, m_spells.shaman.pWeaponBuff))
    {
        if (CastWeaponBuff(m_spells.shaman.pWeaponBuff, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.shaman.pLightningShield &&
        CanTryToCastSpell(me, m_spells.shaman.pLightningShield))
    {
        if (DoCastSpell(me, m_spells.shaman.pLightningShield) == SPELL_CAST_OK)
            return;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;

    if (me->GetVictim())
    {
        if (SummonShamanTotems())
            return;

        UpdateInCombatAI_Shaman();
    }
}

void PartyBotAI::UpdateInCombatAI_Shaman()
{
    ConfigureRaidSupport();
    if(m_quickRaid && SummonShamanTotems())return;
    if (m_spells.shaman.pManaTideTotem &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.shaman.pManaTideTotem))
    {
        if (DoCastSpell(me, m_spells.shaman.pManaTideTotem) == SPELL_CAST_OK)
            return;
    }

    if (GetRole() != ROLE_HEALER)
    {
        if (Unit* pVictim = me->GetVictim())
        {
            if (m_spells.shaman.pElementalMastery &&
                me->GetAttackers().empty() &&
                CanTryToCastSpell(me, m_spells.shaman.pElementalMastery))
            {
                if (DoCastSpell(me, m_spells.shaman.pElementalMastery) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pEarthShock &&
                pVictim->IsNonMeleeSpellCasted(false, false, true) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pEarthShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pEarthShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pFrostShock &&
                pVictim->IsMoving() &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pFrostShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pFrostShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pStormstrike &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pStormstrike))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pStormstrike) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pChainLightning &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pChainLightning))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pChainLightning) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pPurge &&
                IsValidDispelTarget(pVictim, m_spells.shaman.pPurge) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pPurge))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pPurge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pFlameShock &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pFlameShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pFlameShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pLightningBolt &&
               (GetRole() == ROLE_RANGE_DPS || !me->CanReachWithMeleeAutoAttack(pVictim)) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pLightningBolt))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pLightningBolt) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (SummonShamanTotems())
        return;

    if (GetRole() == ROLE_HEALER)
    {
        if (FindAndHealInjuredAlly(50.0f, 90.0f))
            return;

        if (FindAndPreHealTarget())
            return;
    }
    else if (me->GetHealthPercent() < 20.0f)
        HealInjuredTarget(me);
}

void PartyBotAI::UpdateOutOfCombatAI_Hunter()
{
    if (m_spells.hunter.pAspectOfTheHawk &&
        CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
    {
        if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.hunter.pHuntersMark &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pHuntersMark))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pHuntersMark) == SPELL_CAST_OK)
                return;
        }

        if (Pet* pPet = me->GetPet())
        {
            if (!pPet->GetVictim())
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Hunter();
    }
    else
        SummonPetIfNeeded();
}

void PartyBotAI::UpdateInCombatAI_Hunter()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (m_spells.hunter.pVolley &&
           ((RaidCoordination::Enabled(me) && me->GetMapId()==409) ? McBotTactics::WantsAoe(*this,pVictim) : me->GetEnemyCountInRadiusAround(pVictim,10.0f)>2) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pVolley))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pVolley) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_AUTO_SHOT) &&
            !me->IsMoving() &&
            (me->GetCombatDistance(pVictim) > 8.0f) &&
            !me->IsNonMeleeSpellCasted())
        {
            switch (CastPartySpell(pVictim, PB_SPELL_AUTO_SHOT, false))
            {
                case SPELL_FAILED_NEED_AMMO:
                case SPELL_FAILED_NO_AMMO:
                {
                    AddHunterAmmo();
                    break;
                }
            }
        }

        if (m_spells.hunter.pConcussiveShot &&
            pVictim->IsMoving() && (pVictim->GetVictim() == me) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pConcussiveShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pConcussiveShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pAimedShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pAimedShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pAimedShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pArcaneShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pArcaneShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pArcaneShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pSerpentSting &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pSerpentSting))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pSerpentSting) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pMultiShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pMultiShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pMultiShot) == SPELL_CAST_OK)
                return;
        }

        if (GetAttackersInRangeCount(8.0f))
        {
            Unit* pAttacker = *me->GetAttackers().begin();

            if (m_spells.hunter.pScareBeast &&
                CanTryToCastSpell(pAttacker, m_spells.hunter.pScareBeast))
            {
                if (DoCastSpell(pAttacker, m_spells.hunter.pScareBeast) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.hunter.pDisengage &&
                CanTryToCastSpell(pAttacker, m_spells.hunter.pDisengage))
            {
                if (DoCastSpell(pAttacker, m_spells.hunter.pDisengage) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.hunter.pAspectOfTheMonkey &&
                CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheMonkey))
            {
                if (DoCastSpell(me, m_spells.hunter.pAspectOfTheMonkey) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.hunter.pFeignDeath &&
               (me->GetHealthPercent() < 20.0f) &&
                CanTryToCastSpell(me, m_spells.hunter.pFeignDeath))
            {
                if (DoCastSpell(me, m_spells.hunter.pFeignDeath) == SPELL_CAST_OK)
                    return;
            }
        }

        if (pVictim->CanReachWithMeleeAutoAttack(me))
        {
            if (m_spells.hunter.pWingClip &&
                CanTryToCastSpell(pVictim, m_spells.hunter.pWingClip))
            {
                DoCastSpell(pVictim, m_spells.hunter.pWingClip);
            }

            if (m_spells.hunter.pMongooseBite &&
                CanTryToCastSpell(pVictim, m_spells.hunter.pMongooseBite))
            {
                DoCastSpell(pVictim, m_spells.hunter.pMongooseBite);
            }

            if (m_spells.hunter.pRaptorStrike &&
                CanTryToCastSpell(pVictim, m_spells.hunter.pRaptorStrike))
            {
                DoCastSpell(pVictim, m_spells.hunter.pRaptorStrike);
            }
        }
        else
        {
            if (m_spells.hunter.pAspectOfTheHawk &&
                CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
            {
                if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
                    return;
            }
        }

        if (!me->HasUnitState(UNIT_STATE_ROOT) &&
            (me->GetCombatDistance(pVictim) < 8.0f) &&
            (GetRole() != ROLE_MELEE_DPS) &&
             me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE)
        {
            if (!me->IsStopped())
                me->StopMoving();
            me->GetMotionMaster()->Clear();
            if (RunAwayFromTarget(pVictim))
                return;
        }
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Mage()
{
    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.mage.pArcaneIntellect, m_spells.mage.pArcaneBrilliance, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.mage.pIceArmor &&
        CanTryToCastSpell(me, m_spells.mage.pIceArmor))
    {
        if (DoCastSpell(me, m_spells.mage.pIceArmor) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_spells.mage.pIceBarrier &&
        CanTryToCastSpell(me, m_spells.mage.pIceBarrier))
    {
        if (DoCastSpell(me, m_spells.mage.pIceBarrier) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.mage.pArcaneIntellect ||
        !me->HasGCD(m_spells.mage.pArcaneIntellect)))
    {
        m_isBuffing = false;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Mage();
}

void PartyBotAI::UpdateInCombatAI_Mage()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.mage.pCombustion &&
            CanTryToCastSpell(me, m_spells.mage.pCombustion))
        {
            if (DoCastSpell(me, m_spells.mage.pCombustion) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPyroblast &&
           ((m_spells.mage.pPresenceOfMind && me->HasAura(m_spells.mage.pPresenceOfMind->Id)) ||
            (!pVictim->IsInCombat() && (pVictim->GetMaxHealth() > me->GetMaxHealth()) && (me->GetDistance(pVictim) > 30.0f))) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pPyroblast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pPyroblast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pIceBlock &&
           (me->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pIceBlock))
        {
            if (DoCastSpell(me, m_spells.mage.pIceBlock) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }
        else if (GetAttackersInRangeCount(10.0f))
        {
            if (m_spells.mage.pManaShield &&
               (me->GetPowerPercent(POWER_MANA) > 20.0f) &&
                CanTryToCastSpell(me, m_spells.mage.pManaShield))
            {
                if (DoCastSpell(me, m_spells.mage.pManaShield) == SPELL_CAST_OK)
                    return;
            }

            if ((GetRole() != ROLE_MELEE_DPS) &&
                (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
            {
                if (m_spells.mage.pBlink &&
                    (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE) ||
                        me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
                    CanTryToCastSpell(me, m_spells.mage.pBlink))
                {
                    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
                        me->GetMotionMaster()->Clear();

                    if (DoCastSpell(me, m_spells.mage.pBlink) == SPELL_CAST_OK)
                        return;
                }

                if (!me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))
                {
                    if (m_spells.mage.pFrostNova &&
                       !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
                       !pVictim->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                        CanTryToCastSpell(me, m_spells.mage.pFrostNova))
                    {
                        DoCastSpell(me, m_spells.mage.pFrostNova);
                    }

                    if (RunAwayFromTarget(pVictim))
                    {
                        me->SetCasterChaseDistance(25.0f);
                        return;
                    }
                }
            }
        }

        if (me->GetEnemyCountInRadiusAround(me, 10.0f) > 1)
        {
            if (m_spells.mage.pConeofCold && !me->IsMoving() &&
                CanTryToCastSpell(me, m_spells.mage.pConeofCold))
            {
                if (DoCastSpell(pVictim, m_spells.mage.pConeofCold) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pBlastWave &&
                CanTryToCastSpell(me, m_spells.mage.pBlastWave))
            {
                if (DoCastSpell(me, m_spells.mage.pBlastWave) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pArcaneExplosion &&
                CanTryToCastSpell(me, m_spells.mage.pArcaneExplosion))
            {
                if (DoCastSpell(me, m_spells.mage.pArcaneExplosion) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
            return;

        if (m_spells.mage.pCounterspell &&
            pVictim->IsNonMeleeSpellCasted(false, false, true) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pCounterspell))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pCounterspell) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pBlizzard &&
           ((RaidCoordination::Enabled(me) && me->GetMapId()==409) ? McBotTactics::WantsAoe(*this,pVictim) : me->GetEnemyCountInRadiusAround(pVictim,10.0f)>2) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pBlizzard))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pBlizzard) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPolymorph)
        {
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {
                if (pTarget->GetHealthPercent() > 20.0f &&
                    CanTryToCastSpell(pTarget, m_spells.mage.pPolymorph) &&
                    CanUseCrowdControl(m_spells.mage.pPolymorph, pTarget))
                {
                    if (DoCastSpell(pTarget, m_spells.mage.pPolymorph) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.mage.pArcanePower &&
            (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pArcanePower))
        {
            if (DoCastSpell(me, m_spells.mage.pArcanePower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPresenceOfMind &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pPresenceOfMind))
        {
            if (DoCastSpell(me, m_spells.mage.pPresenceOfMind) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pScorch &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pScorch))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pScorch) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFrostbolt &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFrostbolt))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFrostbolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireBlast &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireBlast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireball &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireball))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireball) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pEvocation &&
           (me->GetPowerPercent(POWER_MANA) < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) == 0) &&
            CanTryToCastSpell(me, m_spells.mage.pEvocation))
        {
            if (DoCastSpell(me, m_spells.mage.pEvocation) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            CastPartySpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Priest()
{
    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPowerWordFortitude, m_spells.priest.pPrayerofFortitude, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pDivineSpirit, m_spells.priest.pPrayerofSpirit, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pShadowProtection, m_spells.priest.pPrayerofShadowProtection, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.priest.pInnerFire &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFire))
    {
        if (DoCastSpell(me, m_spells.priest.pInnerFire) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.priest.pPowerWordFortitude ||
        !me->HasGCD(m_spells.priest.pPowerWordFortitude)))
    {
        m_isBuffing = false;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;

    if (me->GetVictim())
        UpdateInCombatAI_Priest();
}

void PartyBotAI::UpdateInCombatAI_Priest()
{
    if (m_spells.priest.pPowerWordShield &&
        CanTryToCastSpell(me, m_spells.priest.pPowerWordShield))
    {
        if (DoCastSpell(me, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
            return;
    }

    if (!me->GetAttackers().empty() &&
        m_role != ROLE_TANK)
    {
        if (m_spells.priest.pFade &&
            CanTryToCastSpell(me, m_spells.priest.pFade))
        {
            if (DoCastSpell(me, m_spells.priest.pFade) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pShackleUndead)
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if ((pAttacker->GetHealth() > me->GetHealth()) &&
                CanTryToCastSpell(pAttacker, m_spells.priest.pShackleUndead) &&
                CanUseCrowdControl(m_spells.priest.pShackleUndead, pAttacker))
            {
                if (DoCastSpell(pAttacker, m_spells.priest.pShackleUndead) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (m_spells.priest.pInnerFocus &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFocus))
    {
        DoCastSpell(me, m_spells.priest.pInnerFocus);
    }

    if (GetRole() == ROLE_HEALER || (!me->GetVictim() && me->GetShapeshiftForm() == FORM_NONE))
    {
        // Shield allies being attacked.
        if (m_spells.priest.pPowerWordShield)
        {
            if (Player* pTarget = SelectShieldTarget())
            {
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPowerWordShield))
                {
                    if (DoCastSpell(pTarget, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        // Direct heal more seriously injured.
        if (Unit* pTarget = SelectHealTarget(60.0f, 80.0f))
            if (HealInjuredTargetDirect(pTarget))
                return;

        // Apply HoT aura for small injuries.
        if (Unit* pTarget = SelectPeriodicHealTarget(80.0f, 90.0f))
            if (HealInjuredTargetPeriodic(pTarget))
                return;

        if (GetRole() == ROLE_HEALER && FindAndPreHealTarget())
            return;
    }
    else if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.priest.pShadowform &&
            CanTryToCastSpell(me, m_spells.priest.pShadowform))
        {
            if (DoCastSpell(me, m_spells.priest.pShadowform) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pSilence &&
            pVictim->IsNonMeleeSpellCasted() &&
            CanTryToCastSpell(pVictim, m_spells.priest.pSilence))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pSilence) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pVampiricEmbrace &&
            CanTryToCastSpell(pVictim, m_spells.priest.pVampiricEmbrace))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pVampiricEmbrace) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindBlast &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindBlast))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pShadowWordPain &&
            CanTryToCastSpell(pVictim, m_spells.priest.pShadowWordPain))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pShadowWordPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pDevouringPlague &&
            CanTryToCastSpell(pVictim, m_spells.priest.pDevouringPlague))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pDevouringPlague) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pPsychicScream &&
            GetAttackersInRangeCount(10.0f) &&
            CanTryToCastSpell(me, m_spells.priest.pPsychicScream))
        {
            if (DoCastSpell(me, m_spells.priest.pPsychicScream) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pManaBurn &&
           (pVictim->GetPowerType() == POWER_MANA) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pManaBurn))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pManaBurn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindFlay &&
           (!GetAttackersInRangeCount(10.0f) || me->HasAuraType(SPELL_AURA_SCHOOL_ABSORB)) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindFlay))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindFlay) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (me->GetShapeshiftForm() == FORM_NONE)
        {
            if (m_spells.priest.pHolyNova &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                CanTryToCastSpell(me, m_spells.priest.pHolyNova))
            {
                if (DoCastSpell(me, m_spells.priest.pHolyNova) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.priest.pSmite &&
                CanTryToCastSpell(pVictim, m_spells.priest.pSmite))
            {
                if (DoCastSpell(pVictim, m_spells.priest.pSmite) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            CastPartySpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Warlock()
{
    if (m_spells.warlock.pDetectInvisibility)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.warlock.pDetectInvisibility))
        {
            if (CanTryToCastSpell(pTarget, m_spells.warlock.pDetectInvisibility))
            {
                if (DoCastSpell(pTarget, m_spells.warlock.pDetectInvisibility) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_spells.warlock.pDemonArmor &&
        CanTryToCastSpell(me, m_spells.warlock.pDemonArmor))
    {
        if (DoCastSpell(me, m_spells.warlock.pDemonArmor) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.warlock.pDetectInvisibility ||
        !me->HasGCD(m_spells.warlock.pDetectInvisibility)))
    {
        m_isBuffing = false;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (Pet* pPet = me->GetPet())
        {
            if (!pPet->GetVictim())
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Warlock();
    }
    else
        SummonPetIfNeeded();
}

void PartyBotAI::UpdateInCombatAI_Warlock()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.warlock.pDeathCoil &&
           (pVictim->CanReachWithMeleeAutoAttack(me) || pVictim->IsNonMeleeSpellCasted()) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDeathCoil))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDeathCoil) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowburn &&
           (pVictim->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowburn))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowburn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSearingPain &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSearingPain))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSearingPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pBanish &&
            me->GetAttackers().size() > 1)
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if ((pAttacker->GetHealth() > me->GetHealth()) &&
                CanTryToCastSpell(pAttacker, m_spells.warlock.pBanish))
            {
                if (DoCastSpell(pAttacker, m_spells.warlock.pBanish) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warlock.pRainOfFire &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pRainOfFire))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pRainOfFire) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDemonicSacrifice)
        {
            if (Pet* pPet = me->GetPet())
            {
                if (pPet->IsAlive() &&
                    CanTryToCastSpell(pPet, m_spells.warlock.pDemonicSacrifice))
                {
                    if (DoCastSpell(pPet, m_spells.warlock.pDemonicSacrifice) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.warlock.pImmolate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pImmolate))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pImmolate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pConflagrate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pConflagrate))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pConflagrate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pCorruption &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCorruption))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pCorruption) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSiphonLife &&
           (me->GetHealthPercent() < 80.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSiphonLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSiphonLife) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDrainLife &&
           (me->GetHealthPercent() < 30.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDrainLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDrainLife) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pFear &&
            pVictim->GetVictim() == me &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pFear))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pFear) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pCurseofAgony &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCurseofAgony))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pCurseofAgony) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (m_spells.warlock.pHowlofTerror &&
            GetAttackersInRangeCount(10.0f) > 1 &&
            CanTryToCastSpell(me, m_spells.warlock.pHowlofTerror))
        {
            if (DoCastSpell(me, m_spells.warlock.pHowlofTerror) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowBolt &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowBolt))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowBolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pLifeTap &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
           (me->GetHealthPercent() > 70.0f) &&
            CanTryToCastSpell(me, m_spells.warlock.pLifeTap))
        {
            if (DoCastSpell(me, m_spells.warlock.pLifeTap) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            CastPartySpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Warrior()
{
    if (m_spells.warrior.pBattleStance &&
        CanTryToCastSpell(me, m_spells.warrior.pBattleStance))
    {
        if (DoCastSpell(me, m_spells.warrior.pBattleStance) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.warrior.pBattleShout &&
       !me->HasAura(m_spells.warrior.pBattleShout->Id))
    {
        if (CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
            DoCastSpell(me, m_spells.warrior.pBattleShout);
        else if (m_spells.warrior.pBloodrage &&
            (me->GetPower(POWER_RAGE) < 100) &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
        {
            DoCastSpell(me, m_spells.warrior.pBloodrage);
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.warrior.pCharge &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pCharge))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pCharge) == SPELL_CAST_OK)
                return;
        }
    }
}

void PartyBotAI::UpdateInCombatAI_Warrior()
{
    // PvE tanks never fall through into the shared DPS/PvP stance dance.
    if(m_role==ROLE_TANK && me->GetGroup() && me->GetMap()->IsDungeon() && !me->InBattleGround() && !IsInDuel()){
        auto cast=[&](Unit* target,SpellEntry const* spell){return spell&&CanTryToCastSpell(target,spell)&&DoCastSpell(target,spell)==SPELL_CAST_OK;};
        if(me->GetShapeshiftForm()!=FORM_DEFENSIVESTANCE){cast(me,m_spells.warrior.pDefensiveStance);return;}
        Unit* target=me->GetVictim();if(!target)return;
        if(me->GetHealthPercent()<20 && cast(me,m_spells.warrior.pLastStand))return;
        if(me->GetHealthPercent()<40 && IsWearingShield(me) && cast(me,m_spells.warrior.pShieldWall))return;
        if(target->IsNonMeleeSpellCasted(false,false,true)&&IsWearingShield(me)&&cast(target,m_spells.warrior.pShieldBash))return;
        // Build threat on the weakest held target, without stealing an assigned tank's mob.
        {
        std::lock_guard<std::recursive_mutex> tankGuard(DungeonTank::turnsMutex);
        auto& tankTurn=DungeonTank::turns[me->GetGUID()];
        uint32 tankNow=WorldTimer::getMSTime();
        if((DungeonTank::Active(*this)||RaidCoordination::Enabled(me))&&target->GetVictim()==me&&(!tankTurn.rotated||uint32(tankNow-tankTurn.rotated)>=2000)){
            std::list<Unit*> enemies;me->GetEnemyListInRadiusAround(me,8,enemies);
            Unit* weakest=nullptr;float margin=1.3f;
            for(Unit* e:enemies){
                if(!e||e->GetTypeId()!=TYPEID_UNIT||e->GetCharmerOrOwnerPlayerOrPlayerItself()||!e->IsAlive()||!e->IsInCombat()||e->GetVictim()!=me||!e->CanHaveThreatList()||!IsValidHostileTarget(e)||e->HasBreakableByDamageCrowdControlAura()||!me->CanReachWithMeleeAutoAttack(e)||!me->IsWithinLOSInMap(e))continue;

                bool cc=false;for(auto mark:m_marksToCC)if(e->GetObjectGuid()==me->GetGroup()->GetTargetWithIcon(mark))cc=true;
                WarriorTankPolicy::Candidate candidate{e->GetTypeId()==TYPEID_UNIT&&!e->GetCharmerOrOwnerPlayerOrPlayerItself(),e->IsAlive(),e->IsInCombat(),e->GetVictim()==me,e->CanHaveThreatList(),IsValidHostileTarget(e),cc||e->HasBreakableByDamageCrowdControlAura(),me->CanReachWithMeleeAutoAttack(e)&&me->IsWithinLOSInMap(e),!RaidCoordination::Enabled(me)||RaidCoordination::AssignedTank(me,e)==me};
                if(!WarriorTankPolicy::Eligible(candidate))continue;
                float competitor=0;for(GroupReference* ref=me->GetGroup()->GetFirstMember();ref;ref=ref->next())if(Player* member=ref->getSource())if(member!=me&&member->IsInWorld()&&member->GetMap()==me->GetMap()){
                    competitor=std::max(competitor,e->GetThreatManager().getThreat(member));
                    if(Unit* pet=member->GetPet())competitor=std::max(competitor,e->GetThreatManager().getThreat(pet));
                }
                float mine=e->GetThreatManager().getThreat(me),score=WarriorTankPolicy::Margin(mine,competitor);
                if(WarriorTankPolicy::Rotate(mine,competitor)&&score<margin){weakest=e;margin=score;}
            }
            if(weakest&&weakest!=target&&AttackStart(weakest)){tankTurn.rotated=tankNow;target=me->GetVictim();if(!target)return;}
        }
        }
        if(IsWearingShield(me)&&WarriorTankPolicy::ShieldBlock(me->GetHealthPercent(),me->GetAttackers().size(),me->GetPower(POWER_RAGE))&&cast(me,m_spells.warrior.pShieldBlock))return;
        if(cast(target,m_spells.warrior.pRevenge))return;
        if(IsWearingShield(me)&&cast(target,m_spells.warrior.pShieldSlam))return;
        // CanTryToCastSpell rejects any existing aura; Sunder must instead stack/refresh.
        if(auto* spell=m_spells.warrior.pSunderArmor){
            auto* aura=target->GetSpellAuraHolder(spell->Id);
            bool needed=!aura||WarriorTankPolicy::MaintainSunder(aura->GetStackAmount(),aura->GetAuraDuration());
            bool owner=true;if(RaidCoordination::Enabled(me))owner=RaidCoordination::AssignedTank(me,target)==me;
            if(owner&&needed&&McBotTactics::AllowSpell(*this,target,spell)&&RaidCoordination::AllowDamage(*this,target,spell)&&me->IsSpellReady(spell)&&!me->HasGCD(spell)&&me->GetPower(POWER_RAGE)>=Spell::CalculatePowerCost(spell,me)&&spell->GetErrorAtShapeshiftedCast(me->GetShapeshiftForm())==SPELL_CAST_OK&&DoCastSpell(target,spell)==SPELL_CAST_OK)return;
        }
        if(cast(target,m_spells.warrior.pBloodthirst))return;
        if(WarriorTankPolicy::DumpRage(me->GetPower(POWER_RAGE))&&!me->GetCurrentSpell(CURRENT_MELEE_SPELL)){
            bool safe=true;unsigned count=0;std::list<Unit*> enemies;me->GetEnemyListInRadiusAround(target,10,enemies);
            for(Unit* e:enemies)if(e&&e->IsAlive()&&IsValidHostileTarget(e)){
                if(!e->IsInCombat()||e->GetVictim()!=me||e->HasBreakableByDamageCrowdControlAura())safe=false;
                ++count;
            }
            if(count>1&&safe){if(cast(target,m_spells.warrior.pCleave))return;}
            else if(count<=1&&cast(target,m_spells.warrior.pHeroicStrike))return;
        }
        // Maintain mitigation after the primary threat builders, without stance switching.
        if(me->GetPower(POWER_RAGE)>=200){
            bool safe=true,needed=false;std::list<Unit*> enemies;me->GetEnemyListInRadiusAround(me,10,enemies);
            for(Unit* e:enemies)if(e&&e->IsAlive()&&IsValidHostileTarget(e)){
                if(!e->IsInCombat()||e->HasBreakableByDamageCrowdControlAura())safe=false;
                if(m_spells.warrior.pDemoralizingShout){auto* aura=e->GetSpellAuraHolder(m_spells.warrior.pDemoralizingShout->Id);if(!aura||aura->GetAuraDuration()<5000)needed=true;}
            }
            if(safe&&needed){
                std::lock_guard<std::recursive_mutex> guard(DungeonTank::turnsMutex);
                auto& last= DungeonTank::turns[me->GetGUID()].demo;uint32 now=WorldTimer::getMSTime();
                // A stronger non-stacking shout/roar can suppress our exact aura.
                // Even then a successful native attempt must not consume every GCD.
                if((!last||uint32(now-last)>=10000)&&cast(me,m_spells.warrior.pDemoralizingShout)){last=now;return;}
            }
        }
        if(!me->CanReachWithMeleeAutoAttack(target))BeginChasing(target);
        return;
    }
    if (Unit* pVictim = me->GetVictim())
    {
        if (pVictim->IsNonMeleeSpellCasted(false, false, true))
        {
            if (m_spells.warrior.pPummel &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pPummel))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pPummel) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pShieldBash &&
                IsWearingShield(me) &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldBash))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldBash) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pExecute &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pExecute))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pExecute) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pOverpower &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pOverpower))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pOverpower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pLastStand &&
            me->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(me, m_spells.warrior.pLastStand))
        {
            if (DoCastSpell(me, m_spells.warrior.pLastStand) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pConcussionBlow &&
           (pVictim->IsNonMeleeSpellCasted() || pVictim->IsMoving() || (me->GetHealthPercent() < 50.0f)) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pConcussionBlow))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pConcussionBlow) == SPELL_CAST_OK)
                return;
        }

        if (me->GetShapeshiftForm() == FORM_DEFENSIVESTANCE &&
            IsWearingShield(me))
        {
            if (!me->GetAttackers().empty())
            {
                if (m_spells.warrior.pShieldBlock &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldBlock))
                {
                    if (DoCastSpell(me, m_spells.warrior.pShieldBlock) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.warrior.pShieldWall &&
                   (me->GetHealthPercent() < 40.0f) &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldWall))
                {
                    if (DoCastSpell(me, m_spells.warrior.pShieldWall) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.warrior.pShieldSlam &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldSlam))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldSlam) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pThunderClap &&
            m_role == ROLE_TANK &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pThunderClap))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pThunderClap) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pSunderArmor &&
            m_role == ROLE_TANK &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pSunderArmor))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pSunderArmor) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pHamstring &&
            pVictim->IsMoving() &&
           !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
           !pVictim->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pHamstring))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pHamstring) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pRend &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pRend))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pRend) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pIntimidatingShout &&
           (me->GetHealthPercent() < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntimidatingShout))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pIntimidatingShout) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pRetaliation &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(me, m_spells.warrior.pRetaliation))
        {
            if (DoCastSpell(me, m_spells.warrior.pRetaliation) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pSweepingStrikes &&
            CanTryToCastSpell(me, m_spells.warrior.pSweepingStrikes) &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2))
        {
            if (DoCastSpell(me, m_spells.warrior.pSweepingStrikes) == SPELL_CAST_OK)
                return;
        }

        if (m_role != ROLE_TANK &&
           (me->GetHealthPercent() > 60.0f) && (pVictim->GetHealthPercent() > 40.0f) &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->IsImmuneToMechanic(MECHANIC_FEAR))
        {
            if (m_spells.warrior.pRecklessness &&
                CanTryToCastSpell(me, m_spells.warrior.pRecklessness))
            {
                if (DoCastSpell(me, m_spells.warrior.pRecklessness) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pDeathWish &&
                CanTryToCastSpell(me, m_spells.warrior.pDeathWish))
            {
                if (DoCastSpell(me, m_spells.warrior.pDeathWish) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pMortalStrike &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pMortalStrike))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pMortalStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pBloodthirst &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pBloodthirst))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pBloodthirst) == SPELL_CAST_OK)
                return;
        }

        if ((me->GetHealthPercent() < 20.0f) ||
            (m_role == ROLE_TANK && pVictim->GetLevel() >= me->GetLevel()))
        {
            if (m_spells.warrior.pDefensiveStance &&
                CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
            {
                DoCastSpell(me, m_spells.warrior.pDefensiveStance);
            }
        }
        else
        {
            if (m_spells.warrior.pBerserkerStance &&
                CanTryToCastSpell(me, m_spells.warrior.pBerserkerStance))
            {
                DoCastSpell(me, m_spells.warrior.pBerserkerStance);
            }
        }

        if (m_spells.warrior.pIntercept &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntercept))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pIntercept) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pWhirlwind &&
            CanTryToCastSpell(me, m_spells.warrior.pWhirlwind))
        {
            if (DoCastSpell(me, m_spells.warrior.pWhirlwind) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pDisarm &&
            IsMeleeWeaponClass(pVictim->GetClass()) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pDisarm))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pDisarm) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pDemoralizingShout &&
            m_role == ROLE_TANK &&
            CanTryToCastSpell(me, m_spells.warrior.pDemoralizingShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pDemoralizingShout) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && !me->CanReachWithMeleeAutoAttack(pVictim))
        {
            BeginChasing(pVictim);
        }

        if (me->GetPower(POWER_RAGE) > 300)
        {
            if (m_spells.warrior.pCleave && me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1)
            {
                if (CanTryToCastSpell(pVictim, m_spells.warrior.pCleave))
                {
                    if (DoCastSpell(pVictim, m_spells.warrior.pCleave) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.warrior.pHeroicStrike &&
                    CanTryToCastSpell(pVictim, m_spells.warrior.pHeroicStrike))
                {
                    if (DoCastSpell(pVictim, m_spells.warrior.pHeroicStrike) == SPELL_CAST_OK)
                        return;
                }
            }
        }
    }
    else // no victim
    {
        if (m_spells.warrior.pBattleShout &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
                return;
        }
    }
}

bool PartyBotAI::ShouldEnterStealth() const
{
    if (me->IsMounted())
        return false;

    if (me->GetVictim() || me->InBattleGround() || me->IsFFAPvP())
        return true;

    if (me->GetHealthPercent() < 10.0f)
        return true;

    if (Player* pLeader = GetPartyLeader())
    {
        if (pLeader->IsDead() || pLeader->IsFeigningDeathSuccessfully() ||
            pLeader->HasAuraType(SPELL_AURA_MOD_STEALTH) ||
            pLeader->HasAuraType(SPELL_AURA_MOD_INVISIBILITY))
            return true;
    }

    return false;
}

bool PartyBotAI::EnterStealthIfNeeded(SpellEntry const* pStealthSpell)
{
    if (pStealthSpell)
    {
        bool const shouldStealth = ShouldEnterStealth();

        if (me->HasAura(pStealthSpell->Id))
        {
            if (!shouldStealth)
                me->RemoveAurasDueToSpellByCancel(pStealthSpell->Id);
        }
        else
        {
            if (shouldStealth &&
                CanTryToCastSpell(me, pStealthSpell) &&
                DoCastSpell(me, pStealthSpell) == SPELL_CAST_OK)
                return true;
        }
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Rogue()
{
    if (m_spells.rogue.pMainHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pMainHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pMainHandPoison, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.rogue.pOffHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pOffHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pOffHandPoison, EQUIPMENT_SLOT_OFFHAND) == SPELL_CAST_OK)
            return;
    }

    if (EnterStealthIfNeeded(m_spells.rogue.pStealth))
        return;

    if (me->GetVictim())
        UpdateInCombatAI_Rogue();
}

void PartyBotAI::UpdateInCombatAI_Rogue()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pPremeditation &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pPremeditation))
            {
                DoCastSpell(pVictim, m_spells.rogue.pPremeditation);
            }

            if (pVictim->IsCaster())
            {
                if (m_spells.rogue.pGarrote &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pGarrote))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pGarrote) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.rogue.pAmbush &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pAmbush))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pAmbush) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.rogue.pCheapShot &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pCheapShot))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pCheapShot) == SPELL_CAST_OK)
                        return;
                }
            }
        }
        else
        {
            if (m_spells.rogue.pVanish &&
                (me->GetHealthPercent() < 10.0f))
            {
                if (m_spells.rogue.pPreparation &&
                    !me->IsSpellReady(m_spells.rogue.pVanish) &&
                    CanTryToCastSpell(me, m_spells.rogue.pPreparation))
                {
                    if (DoCastSpell(me, m_spells.rogue.pPreparation) == SPELL_CAST_OK)
                        return;
                }

                if (CanTryToCastSpell(me, m_spells.rogue.pVanish))
                {
                    if (DoCastSpell(me, m_spells.rogue.pVanish) == SPELL_CAST_OK)
                    {
                        if (RunAwayFromTarget(pVictim))
                            return;
                    }
                }
            }
        }

        if (me->GetComboPoints() > 4)
        {
            std::vector<SpellEntry const*> vSpells;

            // Give priority to Slice and Dice over other finishing moves.
            if (m_spells.rogue.pSliceAndDice &&
               !me->HasAura(m_spells.rogue.pSliceAndDice->Id) &&
                pVictim->GetHealthPercent() > 10.0f)
                vSpells.push_back(m_spells.rogue.pSliceAndDice);
            else
            {
                if (m_spells.rogue.pEviscerate)
                    vSpells.push_back(m_spells.rogue.pEviscerate);
                if (m_spells.rogue.pKidneyShot && !pVictim->IsImmuneToMechanic(MECHANIC_STUN))
                    vSpells.push_back(m_spells.rogue.pKidneyShot);
                if (m_spells.rogue.pExposeArmor)
                    vSpells.push_back(m_spells.rogue.pExposeArmor);
                if (m_spells.rogue.pRupture)
                    vSpells.push_back(m_spells.rogue.pRupture);
            }

            if (!vSpells.empty())
            {
                SpellEntry const* pComboSpell = SelectRandomContainerElement(vSpells);
                if (CanTryToCastSpell(pVictim, pComboSpell))
                {
                    if (DoCastSpell(pVictim, pComboSpell) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.rogue.pBlind)
        {
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {
                if (CanTryToCastSpell(pTarget, m_spells.rogue.pBlind) &&
                    CanUseCrowdControl(m_spells.rogue.pBlind, pTarget))
                {
                    if (DoCastSpell(pTarget, m_spells.rogue.pBlind) == SPELL_CAST_OK)
                    {
                        me->AttackStop();
                        AttackStart(pVictim);
                        return;
                    }
                }
            }
        }

        if (m_spells.rogue.pAdrenalineRush &&
           !me->GetPower(POWER_ENERGY) &&
            CanTryToCastSpell(me, m_spells.rogue.pAdrenalineRush))
        {
            if (DoCastSpell(me, m_spells.rogue.pAdrenalineRush) == SPELL_CAST_OK)
                return;
        }

        if (pVictim->IsNonMeleeSpellCasted())
        {
            if (m_spells.rogue.pGouge &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pGouge))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pGouge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pKick &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pKick))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pKick) == SPELL_CAST_OK)
                    return;
            }
        }

        if (!me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pEvasion &&
               (me->GetHealthPercent() < 80.0f) &&
               ((GetAttackersInRangeCount(10.0f) > 2) || !IsRangedDamageClass(pVictim->GetClass())) &&
                CanTryToCastSpell(me, m_spells.rogue.pEvasion))
            {
                if (DoCastSpell(me, m_spells.rogue.pEvasion) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pColdBlood &&
                CanTryToCastSpell(me, m_spells.rogue.pColdBlood))
            {
                DoCastSpell(me, m_spells.rogue.pColdBlood);
            }

            if (m_spells.rogue.pBladeFlurry &&
                CanTryToCastSpell(me, m_spells.rogue.pBladeFlurry))
            {
                if (DoCastSpell(me, m_spells.rogue.pBladeFlurry) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.rogue.pBackstab &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pBackstab))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pBackstab) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pGhostlyStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pGhostlyStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pGhostlyStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pHemorrhage &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pHemorrhage))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pHemorrhage) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSinisterStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pSinisterStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pSinisterStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSprint &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(me, m_spells.rogue.pSprint))
        {
            if (DoCastSpell(me, m_spells.rogue.pSprint) == SPELL_CAST_OK)
                return;
        }
    }
}

bool PartyBotAI::EnterCombatDruidForm()
{
    if (m_spells.druid.pCatForm &&
        GetRole() == ROLE_MELEE_DPS &&
        CanTryToCastSpell(me, m_spells.druid.pCatForm))
    {
        if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.druid.pBearForm &&
       (m_role == ROLE_TANK || GetRole() == ROLE_MELEE_DPS) &&
        CanTryToCastSpell(me, m_spells.druid.pBearForm))
    {
        if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.druid.pMoonkinForm &&
        GetRole() == ROLE_RANGE_DPS &&
        CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
    {
        if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
            return true;
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Druid()
{
    // Make sure bot leaves combat form if his role is changed to healer.
    if (GetRole() == ROLE_HEALER && me->GetShapeshiftForm() != FORM_NONE &&
        me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
    {
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        return;
    }

    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.druid.pMarkoftheWild, m_spells.druid.pGiftoftheWild, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.druid.pThorns)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.druid.pThorns))
        {
            if (CanTryToCastSpell(pTarget, m_spells.druid.pThorns))
            {
                if (DoCastSpell(pTarget, m_spells.druid.pThorns) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_spells.druid.pNaturesGrasp &&
        CanTryToCastSpell(me, m_spells.druid.pNaturesGrasp))
    {
        if (DoCastSpell(me, m_spells.druid.pNaturesGrasp) == SPELL_CAST_OK)
            return;
    }

    if (m_isBuffing &&
       (!m_spells.druid.pMarkoftheWild ||
        !me->HasGCD(m_spells.druid.pMarkoftheWild)))
    {
        m_isBuffing = false;
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {
        if (EnterCombatDruidForm())
            return;

        if ((me->GetPowerPercent(POWER_MANA) > 80.0f) &&
            FindAndHealInjuredAlly())
            return;
    }
    else if (me->GetShapeshiftForm() == FORM_CAT)
    {
        if (EnterStealthIfNeeded(m_spells.druid.pProwl))
            return;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Druid();
}

void PartyBotAI::UpdateInCombatAI_Druid()
{
    ShapeshiftForm const form = me->GetShapeshiftForm();

    if (m_spells.druid.pBarkskin &&
        (form == FORM_NONE || form == FORM_MOONKIN) &&
        (me->GetHealthPercent() < 50.0f) &&
        CanTryToCastSpell(me, m_spells.druid.pBarkskin))
    {
        if (DoCastSpell(me, m_spells.druid.pBarkskin) == SPELL_CAST_OK)
            return;
    }

    if (form == FORM_NONE)
    {
        if (m_spells.druid.pHibernate &&
            m_role != ROLE_TANK &&
            !me->GetAttackers().empty())
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if (CanTryToCastSpell(pAttacker, m_spells.druid.pHibernate) &&
                CanUseCrowdControl(m_spells.druid.pHibernate, pAttacker))
            {
                if (DoCastSpell(pAttacker, m_spells.druid.pHibernate) == SPELL_CAST_OK)
                    return;
            }
        }

        // Prioritize applying HoTs.
        if (Unit* pTarget = SelectPeriodicHealTarget(80.0f, 90.0f))
            if (HealInjuredTargetPeriodic(pTarget))
                return;

        // Direct heal.
        if (Unit* pTarget = SelectHealTarget(60.0f, 70.0f))
            if (HealInjuredTargetDirect(pTarget))
                return;

        if (m_spells.druid.pInnervate &&
           (me->GetHealthPercent() > 40.0f) &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
            CanTryToCastSpell(me, m_spells.druid.pInnervate))
        {
            if (DoCastSpell(me, m_spells.druid.pInnervate) == SPELL_CAST_OK)
                return;
        }

        if (GetRole() == ROLE_HEALER && FindAndPreHealTarget())
            return;

        if (EnterCombatDruidForm())
            return;
    }

    Unit* pVictim = me->GetVictim();
    if (!pVictim)
        return;

    if (form != FORM_NONE &&
        me->HasUnitState(UNIT_STATE_ROOT) &&
        me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT) &&
        (m_role != ROLE_TANK || !me->CanReachWithMeleeAutoAttack(pVictim)))
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

    if (GetRole() == ROLE_HEALER)
        return;

    switch (form)
    {
        case FORM_CAT:
        {
            if (me->HasDistanceCasterMovement())
                me->SetCasterChaseDistance(0.0f);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }

            if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
            {
                if (m_spells.druid.pPounce &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pPounce))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pPounce) == SPELL_CAST_OK)
                        return;
                }
                if (m_spells.druid.pRavage &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRavage))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pRavage) == SPELL_CAST_OK)
                        return;
                }
                if (m_spells.druid.pTigersFury &&
                    CanTryToCastSpell(me, m_spells.druid.pTigersFury))
                {
                    if (DoCastSpell(me, m_spells.druid.pTigersFury) == SPELL_CAST_OK)
                        return;
                }
                return;
            }

            if (m_spells.druid.pCower &&
                GetAttackersInRangeCount(8.0f))
            {
                Unit* pAttacker = *me->GetAttackers().begin();
                if (CanTryToCastSpell(me, m_spells.druid.pCower))
                {
                    if (DoCastSpell(me, m_spells.druid.pCower) == SPELL_CAST_OK)
                        return;
                }
            }

            if (me->GetComboPoints() > 4)
            {
                if (m_spells.druid.pFerociousBite &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFerociousBite))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFerociousBite) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pRip &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRip))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pRip) == SPELL_CAST_OK)
                        return;
                }
            }

            if (!me->CanReachWithMeleeAutoAttack(pVictim))
            {
                if (m_spells.druid.pFaerieFireFeral &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pDash &&
                    pVictim->IsMoving() &&
                    CanTryToCastSpell(me, m_spells.druid.pDash))
                {
                    if (DoCastSpell(me, m_spells.druid.pDash) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.druid.pShred &&
                CanTryToCastSpell(pVictim, m_spells.druid.pShred))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pShred) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pRake &&
                CanTryToCastSpell(pVictim, m_spells.druid.pRake))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pRake) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pClaw &&
                CanTryToCastSpell(pVictim, m_spells.druid.pClaw))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pClaw) == SPELL_CAST_OK)
                    return;
            }

            break;
        }
        case FORM_BEAR:
        case FORM_DIREBEAR:
        {
            if (me->HasDistanceCasterMovement())
                me->SetCasterChaseDistance(0.0f);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }

            if (m_spells.druid.pFeralCharge &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFeralCharge))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFeralCharge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pBash &&
                CanTryToCastSpell(pVictim, m_spells.druid.pBash))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pBash) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pFrenziedRegeneration &&
                (me->GetHealthPercent() < 30.0f) &&
                CanTryToCastSpell(me, m_spells.druid.pFrenziedRegeneration))
            {
                if (DoCastSpell(me, m_spells.druid.pFrenziedRegeneration) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pFaerieFireFeral &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                    return;
            }

            if ((me->GetPower(POWER_RAGE) > 800) ||
                (GetAttackersInRangeCount(10.0f) > 1))
            {
                if (m_spells.druid.pDemoralizingRoar &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pDemoralizingRoar))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pDemoralizingRoar) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pSwipe &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pSwipe))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pSwipe) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.druid.pMaul &&
                CanTryToCastSpell(pVictim, m_spells.druid.pMaul))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pMaul) == SPELL_CAST_OK)
                    return;
            }
            break;
        }
        case FORM_NONE:
        case FORM_MOONKIN:
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                me->GetDistance(pVictim) > 30.0f)
            {
                BeginChasing(pVictim);
            }
            else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                    (pVictim->GetVictim() == me) &&
                    !me->HasUnitState(UNIT_STATE_ROOT) &&
                    (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
            {
                if (m_spells.druid.pEntanglingRoots &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pEntanglingRoots))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
                        return;
                }
                me->SetCasterChaseDistance(25.0f);
                if (RunAwayFromTarget(pVictim))
                    return;
            }

            if (m_spells.druid.pFaerieFire &&
               (pVictim->GetClass() == CLASS_ROGUE) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFaerieFire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pInsectSwarm &&
                CanTryToCastSpell(pVictim, m_spells.druid.pInsectSwarm))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pInsectSwarm) == SPELL_CAST_OK)
                    return;
            }

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
                return;

            if (m_spells.druid.pHurricane &&
               (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pHurricane))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pHurricane) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pMoonfire &&
                CanTryToCastSpell(pVictim, m_spells.druid.pMoonfire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pMoonfire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pStarfire &&
               (pVictim->GetHealthPercent() > 50.0f) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pStarfire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pStarfire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pWrath &&
                CanTryToCastSpell(pVictim, m_spells.druid.pWrath))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pWrath) == SPELL_CAST_OK)
                    return;
            }

            break;
        }
    }
}
