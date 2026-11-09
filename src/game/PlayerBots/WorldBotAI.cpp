#include "WorldBotAI.h"
#include "WorldBotTowerTask.h"
#include "Config/Config.h"
#include "Pet.h"
#include "TargetedMovementGenerator.h"
#include "MoveSpline.h"
#include "WorldBotModes.h"
#include "WorldBotAmbientPolicy.h"
#include "Formulas.h"
#include "WorldBotPopulation.h"
#include "WorldBotCorpsePolicy.h"
#include "WorldBotAmmoPolicy.h"
#include "PlayerBotMgr.h"
#include "Creature.h"
#include "Bag.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "DBCStores.h"
#include "MotionMaster.h"
#include "PathFinder.h"
#include "World.h"
#include "Utilities/Random.h"
#include "Map.h"
#include "MapManager.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Packets/Loot.h"
#include "Packets/Npc.h"
#include "Packets/Item.h"
#include "Spell.h"
#include "LootMgr.h"
#include "AccountMgr.h"
#include "Corpse.h"
#include "Packets/Misc.h"
#include <algorithm>
#include <cmath>
#include <set>
namespace {
std::list<Creature*> Nearby(Player* me,float radius){
 std::list<Creature*> result;MaNGOS::AnyUnitInObjectRangeCheck check(me,radius);
 MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> search(result,check);
 Cell::VisitGridObjects(me,search,radius);return result;
}
}
namespace {
float FlavorNumber(char const* key,float fallback,float min,float max,bool& valid){return WorldPvpFlavor::Setting(sConfig.GetFloatDefault(key,fallback),fallback,min,max,valid);}
WorldPvpFlavor::Config WorldPvpSettings(){
 static std::mutex mutex;static WorldPvpFlavor::Config settings;static bool loaded=false;static uint32 checked=0;
 std::lock_guard<std::mutex> lock(mutex);uint32 now=WorldTimer::getMSTime();
 if(loaded&&uint32(now-checked)<1000)return settings;checked=now;
 WorldPvpFlavor::Config c;

 c.enabled=sConfig.GetBoolDefault("WorldBot.WorldPvP.Enabled",true);
 c.chance=FlavorNumber("WorldBot.WorldPvP.Chance",.15f,0,1,c.valid);
 c.intervalMs=std::max(10000,std::min(600000,sConfig.GetIntDefault("WorldBot.WorldPvP.IntervalMs",45000)));
 c.minLevel=std::max(10,std::min(60,sConfig.GetIntDefault("WorldBot.WorldPvP.MinLevel",10)));
 c.maxGap=std::max(0,std::min(10,sConfig.GetIntDefault("WorldBot.WorldPvP.MaxLevelGap",3)));
 c.radius=FlavorNumber("WorldBot.WorldPvP.Radius",40,5,60,c.valid);
 c.minHealth=FlavorNumber("WorldBot.WorldPvP.MinHealth",80,80,100,c.valid);
 c.minMana=FlavorNumber("WorldBot.WorldPvP.MinMana",60,0,100,c.valid);
 c.leash=FlavorNumber("WorldBot.WorldPvP.Leash",60,c.radius,120,c.valid);
 c.fightMs=std::max(10000,std::min(120000,sConfig.GetIntDefault("WorldBot.WorldPvP.MaxFightMs",35000)));
 c.routeVariation=sConfig.GetBoolDefault("WorldBot.WorldMovement.RouteVariation",true);
 // Read only the native in-memory Config map. Invalid floats fail closed for
 // initiation without promoting NaN to chance1; no file/database IO here.
 if(c.valid)settings=c;else {settings.enabled=false;settings.valid=false;}
 loaded=true;return settings;
}
bool ProtectedWorldPvp(Player* p){
 if(!p||p->IsGameMaster()||!p->GetSession()||p->GetSession()->GetSecurity()!=SEC_PLAYER||p->HasFlag(PLAYER_FLAGS,PLAYER_FLAGS_RESTING))return true;
 auto* area=AreaEntry::GetById(p->GetAreaId());auto* zone=AreaEntry::GetById(p->GetZoneId());if(!area||!zone)return true;
 uint32 flags=area->Flags|zone->Flags;
 for(uint32 starter:{1u,12u,14u,85u,141u,215u})if(p->GetZoneId()==starter||p->GetAreaId()==starter)return true;
 if((zone->Team!=AREATEAM_NONE&&zone->Team!=AREATEAM_ALLY&&zone->Team!=AREATEAM_HORDE))return true;
 // Vanilla has no TBC sanctuary flag. Respect native legality plus its capital,
 // settlement/resting areas; never force a protected opponent's PvP state.
 return flags&(AREA_FLAG_CAPITAL|AREA_FLAG_CITY|AREA_FLAG_SLAVE_CAPITAL2);
}
}
std::string WorldBotAI::WorldPvpFlavorReport() const {
 auto const& c=WorldPvpSettings();
 return std::string("worldPvp enabled=")+(c.enabled?"1":"0")+" config_cache=world_1s valid="+(c.valid?"1":"0")+" intervalMs="+std::to_string(c.intervalMs)+" chance="+std::to_string(c.chance)+" checks="+std::to_string(m_worldPvpClock.checks)+" rolls="+std::to_string(m_worldPvpClock.rolls)+" initiatedTarget="+std::to_string(m_voluntaryPvpTarget.GetRawValue())+" returning="+(m_voluntaryPvpReturning?"1":"0");
}
bool WorldBotAI::MaybeStartWorldPvp(){
 auto const& c=WorldPvpSettings();WorldPvpFlavor::Readiness r;
 r.world=WorldBotModes::Detect(me)==WorldBotModes::Mode::World;r.alive=me->IsAlive();r.combat=me->IsInCombat()||me->GetVictim()!=nullptr;r.duel=me->m_duel;
 r.group=me->GetGroup()&&me->GetGroup()->GetMembersCount()>1;r.service=NeedsService()||m_hasService;r.protectedArea=ProtectedWorldPvp(me);r.gm=me->IsGameMaster();r.movingTransport=me->GetTransport()||me->IsTaxiFlying();
 r.level=me->GetLevel();r.health=me->GetHealthPercent();r.mana=me->GetPowerPercent(POWER_MANA);r.usesMana=me->GetMaxPower(POWER_MANA)>0;
 if(!WorldPvpFlavor::Ready(c,r)||me->IsNonMeleeSpellCasted()||me->GetDistance(m_profile.x,m_profile.y,m_profile.z)>m_profile.radius)return false;
 uint32 now=WorldTimer::getMSTime();if(!m_worldPvpClock.Due(now,c.intervalMs))return false;
 me->UpdatePvP(true); // Legitimate self opt-in only; native five-minute timer remains authoritative.
 std::list<Player*> nearby;me->GetAlivePlayerListInRange(me,nearby,c.radius);std::vector<Player*> candidates;
 for(Player* p:nearby){
  if(p==me||!p->IsInWorld()||p->GetMap()!=me->GetMap()||p->InBattleGround()||p->GetMap()->IsDungeon()||p->GetTransport()||p->IsTaxiFlying()||p->m_duel||p->IsInCombat()||p->GetVictim()||p->IsNonMeleeSpellCasted()||(p->GetGroup()&&p->GetGroup()->GetMembersCount()>1))continue;
  bool protectedTarget=ProtectedWorldPvp(p);
  if(p->IsBot()){
   auto* bot=p->AI()?dynamic_cast<WorldBotAI*>(p->AI()):nullptr;
   if(!bot||!bot->AllowsPersistentSave()||WorldBotModes::Detect(p)!=WorldBotModes::Mode::World||bot->NeedsService()||bot->m_hasService)continue;
  }
  bool lawful=me->IsPvP()&&p->IsPvP()&&me->IsValidAttackTarget(p)&&!p->IsTotalImmune()&&p->IsVisibleForOrDetect(me,me,false)&&me->IsWithinLOSInMap(p);
  if(WorldPvpFlavor::Target(c,me->GetLevel(),p->GetLevel(),p->GetHealthPercent(),me->GetDistance(p),lawful,protectedTarget))candidates.push_back(p);
 }
 std::sort(candidates.begin(),candidates.end(),[this](Player* a,Player* b){float da=me->GetDistance(a),db=me->GetDistance(b);return da==db?a->GetGUID()<b->GetGUID():da<db;});
 if(candidates.empty())return false;
 ++m_worldPvpClock.rolls;if(!WorldPvpFlavor::Chance(frand(0,1),c.chance))return false;
 Player* target=candidates.front();PathInfo route(me);route.ExcludeSteepSlopes();route.calculate(target->GetPositionX(),target->GetPositionY(),target->GetPositionZ());if(route.getPathType()!=PATHFIND_NORMAL)return false;
 for(auto const& p:route.getPath()){float dx=p.x-me->GetPositionX(),dy=p.y-me->GetPositionY(),dz=p.z-me->GetPositionZ();if(dx*dx+dy*dy+dz*dz>c.leash*c.leash)return false;}
 if(!me->IsValidAttackTarget(target)||!target->IsPvP()||!AttackStart(target))return false;
 m_voluntaryPvpTarget=target->GetObjectGuid();m_voluntaryPvpAt=now;m_voluntaryPvpMap=me->GetMapId();m_voluntaryPvpX=me->GetPositionX();m_voluntaryPvpY=me->GetPositionY();m_voluntaryPvpZ=me->GetPositionZ();m_voluntaryPvpReturning=false;
 m_activity="world_pvp_encounter";return true;
}
void WorldBotAI::CancelWorldPvpOwnedActions(){
 if(!me||m_voluntaryPvpTarget.IsEmpty()||!me->IsInWorld()||me->IsBeingTeleported())return;
 uint64 tracked=m_voluntaryPvpTarget.GetRawValue();bool duel=me->m_duel&&me->m_duel->startTime&&me->m_duel->opponent;
 uint64 opponent=duel?me->m_duel->opponent->GetGUID():0;
 auto owned=[&](Unit* target){Player* owner=target?target->GetAffectingPlayer():nullptr;return WorldPvpFlavor::CancelOldAction(tracked,owner?owner->GetGUID():0,duel,opponent);};
 auto* motion=me->GetMotionMaster()->GetCurrent();uint32 spline=me->movespline->GetId();
 bool chase=motion&&motion->GetMovementGeneratorType()==CHASE_MOTION_TYPE&&owned(static_cast<ChaseMovementGenerator<Player> const*>(motion)->GetTarget());
 if(owned(me->GetVictim())){me->AttackStop(true);me->ClearTarget();}
 for(CurrentSpellTypes type:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL})if(Spell* cast=me->GetCurrentSpell(type)){
  Unit* target=ObjectAccessor::GetUnit(*me,cast->m_targets.getUnitTargetGuid());
  if(owned(target)&&!cast->m_spellInfo->IsPositiveSpell())me->InterruptSpell(type,false);
 }
 if(chase&&me->GetMotionMaster()->GetCurrent()==motion&&me->movespline->GetId()==spline){me->StopMoving();me->GetMotionMaster()->Clear();me->GetMotionMaster()->MoveIdle();m_navGoalValid=false;}
 if(Pet* pet=me->GetPet())if(owned(pet->GetVictim())){
  pet->AttackStop(true);
  if(!pet->GetVictim())pet->GetMotionMaster()->MoveFollow(me,PET_FOLLOW_DIST,PET_FOLLOW_ANGLE);
 }
}
bool WorldBotAI::MaintainWorldPvp(uint32 diff){
 if(m_voluntaryPvpTarget.IsEmpty())return false;
 if(!me->IsAlive()||me->IsBeingTeleported()||me->GetMapId()!=m_voluntaryPvpMap||WorldBotModes::Detect(me)!=WorldBotModes::Mode::World){CancelWorldPvpOwnedActions();m_voluntaryPvpTarget.Clear();m_voluntaryPvpReturning=false;return false;}
 m_worldPvpMaintenance+=diff;if(m_worldPvpMaintenance<250)return m_voluntaryPvpReturning;m_worldPvpMaintenance=0;
 Player* target=ObjectAccessor::FindPlayer(m_voluntaryPvpTarget);auto const& c=WorldPvpSettings();uint32 now=WorldTimer::getMSTime();
 if(!m_voluntaryPvpReturning){
  Unit* current=me->GetVictim();bool tracking=current&&current->GetAffectingPlayer()&&current->GetAffectingPlayer()->GetObjectGuid()==m_voluntaryPvpTarget;
  if(current&&!tracking){CancelWorldPvpOwnedActions();m_voluntaryPvpTarget.Clear();m_voluntaryPvpReturning=false;return false;}
  if(!me->IsInCombat()&&!tracking){CancelWorldPvpOwnedActions();m_voluntaryPvpTarget.Clear();return false;}
  bool keep=c.enabled&&target&&!target->m_duel&&(!target->GetGroup()||target->GetGroup()->GetMembersCount()<=1)&&target->IsInWorld()&&target->GetMap()==me->GetMap()&&target->IsAlive()&&target->IsPvP()&&me->IsValidAttackTarget(target)&&target->IsVisibleForOrDetect(me,me,false)&&me->IsWithinLOSInMap(target)&&!ProtectedWorldPvp(target)&&WorldPvpFlavor::WithinLeash(me->GetMapId()==m_voluntaryPvpMap&&target->GetMap()==me->GetMap(),me->GetDistance(m_voluntaryPvpX,m_voluntaryPvpY,m_voluntaryPvpZ),target->GetDistance(m_voluntaryPvpX,m_voluntaryPvpY,m_voluntaryPvpZ),uint32(now-m_voluntaryPvpAt),c);
  if(keep)return false;
  m_voluntaryPvpReturning=true;CancelWorldPvpOwnedActions();BattleBotPolicyRuntime::ForgetExternal(*this,m_pvpPolicy);m_externalScope=false;
 }
 // Disengage an initiated chase without clearing combat/threat/debuffs or health.
 // The opponent can keep attacking or kill the bot during its normal return.
 Unit* defensive=me->GetVictim();
 if(defensive&&(!defensive->GetAffectingPlayer()||defensive->GetAffectingPlayer()->GetObjectGuid()!=m_voluntaryPvpTarget)){CancelWorldPvpOwnedActions();m_voluntaryPvpTarget.Clear();m_voluntaryPvpReturning=false;return false;}
 CancelWorldPvpOwnedActions();
 if(!me->IsNonMeleeSpellCasted()&&!me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))Navigate(m_voluntaryPvpX,m_voluntaryPvpY,m_voluntaryPvpZ);
 m_activity="world_pvp_return_native_combat_retained";
 if(me->GetMapId()!=m_voluntaryPvpMap || (me->GetDistance(m_voluntaryPvpX,m_voluntaryPvpY,m_voluntaryPvpZ)<=8&&!me->IsInCombat())){m_voluntaryPvpTarget.Clear();m_voluntaryPvpReturning=false;return false;}
 return true;
}
bool WorldBotAI::OnSessionLoaded(PlayerBotEntry* entry,WorldSession* session){
 if(!entry || entry->playerGUID!=m_profile.characterGuid || m_profile.id.empty())return false;
 if(!m_profile.accountId || !m_profile.createdAt || session->GetAccountId()!=m_profile.accountId || sAccountMgr.GetSecurity(m_profile.accountId)!=SEC_PLAYER)return false;
 std::unique_ptr<QueryResult> identity(CharacterDatabase.PQuery("SELECT account,create_time FROM characters WHERE guid=%u",m_profile.characterGuid));
 if(!identity || identity->Fetch()[0].GetUInt32()!=m_profile.accountId || identity->Fetch()[1].GetUInt64()!=m_profile.createdAt)return false;
 // Validate persistent gate actors before native login: no live cross-map
 // teleport, and never reuse a center Z for a different random XY.
 bool towerPlacement=false;
 if(m_profile.purpose=="plaguelands_towers"&&m_profile.initialize){
  std::unique_ptr<QueryResult> pristine(CharacterDatabase.PQuery("SELECT level,xp,money FROM characters WHERE guid=%u AND account=%u AND create_time=%llu",m_profile.characterGuid,m_profile.accountId,(unsigned long long)m_profile.createdAt));
  if(!pristine)return false;Field* f=pristine->Fetch();towerPlacement=f[0].GetUInt32()==1&&f[1].GetUInt32()==0&&f[2].GetUInt32()==sWorld.getConfig(CONFIG_UINT32_START_PLAYER_MONEY);
 }
 if((m_profile.purpose=="city_duel"&&!PKTaskEnabled())||towerPlacement){
  std::unique_ptr<QueryResult> pos(CharacterDatabase.PQuery("SELECT map,position_x,position_y,position_z,(SELECT COUNT(*) FROM group_member WHERE member_guid=%u) FROM characters WHERE guid=%u",m_profile.characterGuid,m_profile.characterGuid));
  if(!pos)return false;
  Field* fields=pos->Fetch();
  // A persistent companion belonging to a group must keep its saved location.
  if(!fields[4].GetUInt32()){
   Map* map=towerPlacement?sMapMgr.CreateContinentMap(m_profile.map,m_profile.x,m_profile.y):sMapMgr.FindMap(m_profile.map);
   if(!map){m_activity="city_spawn_map_unavailable";sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[WorldBotTowerInit] guid=%u map_unavailable map=%u",m_profile.characterGuid,m_profile.map);return false;}
   if(towerPlacement)map->ForceLoadGridsAroundPosition(m_profile.x,m_profile.y);
   float radius=std::min(m_profile.radius,40.0f);
   float anchorZ=map->GetHeight(m_profile.x,m_profile.y,m_profile.z+6.0f,true,12.0f);
   if(!std::isfinite(anchorZ)||std::fabs(anchorZ-m_profile.z)>6.0f){m_activity="city_spawn_anchor_invalid";sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[WorldBotTowerInit] guid=%u anchor_invalid xyz=%.3f,%.3f,%.3f ground=%.3f",m_profile.characterGuid,m_profile.x,m_profile.y,m_profile.z,anchorZ);return false;}
   auto valid=[&](float x,float y,float z){
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||std::hypot(x-m_profile.x,y-m_profile.y)>radius)return false;
    float ground=map->GetHeight(x,y,z+6.0f,true,12.0f);
    if(!std::isfinite(ground)||std::fabs(ground-z)>0.5f)return false;
    float endX=x,endY=y,endZ=z;
    if(!map->GetWalkHitPosition(nullptr,m_profile.x,m_profile.y,anchorZ,endX,endY,endZ,NAV_GROUND,6.0f,false))return false;
    if(std::hypot(endX-x,endY-y)>0.75f||std::fabs(endZ-z)>1.0f)return false;
    return map->isInLineOfSight(x,y,z+0.5f,x,y,z+2.5f,true,false);
   };
   float x=fields[1].GetFloat(),y=fields[2].GetFloat(),z=fields[3].GetFloat();
   if(fields[0].GetUInt32()!=m_profile.map||!valid(x,y,z)){
    bool found=false;
    for(unsigned attempt=0;attempt<24&&!found;++attempt){
     x=m_profile.x;y=m_profile.y;z=anchorZ;
     if(!map->GetWalkRandomPosition(nullptr,x,y,z,radius,NAV_GROUND))continue;
     z=map->GetHeight(x,y,z+6.0f,true,12.0f);
     found=valid(x,y,z);
    }
    if(!found){m_activity="city_spawn_no_valid_ground";sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[WorldBotTowerInit] guid=%u no_valid_ground anchor=%.3f,%.3f,%.3f radius=%.3f",m_profile.characterGuid,m_profile.x,m_profile.y,anchorZ,radius);return false;}
    if(!CharacterDatabase.DirectPExecute("UPDATE characters SET map=%u,position_x=%.6f,position_y=%.6f,position_z=%.6f WHERE guid=%u AND account=%u AND create_time=%llu",m_profile.map,x,y,z,m_profile.characterGuid,m_profile.accountId,(unsigned long long)m_profile.createdAt))return false;
    sLog.Out(LOG_BASIC,LOG_LVL_MINIMAL,"[WorldBot] gate spawn validated guid=%u map=%u xyz=%.3f,%.3f,%.3f",m_profile.characterGuid,m_profile.map,x,y,z);
   }
  }
 }
 session->LoginPlayer(entry->playerGUID);return true;
}
bool WorldBotAI::Navigate(float x,float y,float z){
 if(!me||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return false;
 if(!m_navGoalValid||std::fabs(x-m_navX)>=1||std::fabs(y-m_navY)>=1||std::fabs(z-m_navZ)>=1)m_navProgressAt=0;
 uint32 progressNow=WorldTimer::getMSTime();
 if(m_navGoalValid&&std::fabs(x-m_navX)<1&&std::fabs(y-m_navY)<1&&std::fabs(z-m_navZ)<1&&me->IsMoving()&&me->GetMotionMaster()->GetCurrentMovementGeneratorType()==POINT_MOTION_TYPE){
  if(!m_navProgressAt||!me->IsWithinDist3d(m_navProgressX,m_navProgressY,m_navProgressZ,2)){
   m_navProgressAt=progressNow;m_navProgressX=me->GetPositionX();m_navProgressY=me->GetPositionY();m_navProgressZ=me->GetPositionZ();m_routeStall.Clear();return true;
  }
  if(uint32(progressNow-m_navProgressAt)<15000)return true;
  m_routeStall.Failed(progressNow,me->GetMapId(),{{x,y,z}},{{me->GetPositionX(),me->GetPositionY(),me->GetPositionZ()}});
  me->StopMoving();m_navGoalValid=false;m_navProgressAt=progressNow;m_activity="route_no_actual_progress_replanning";
 }
 uint32 navNow=WorldTimer::getMSTime();
 if(m_navRetryAt&&int32(navNow-m_navRetryAt)<0&&std::fabs(x-m_navDiagnostic.goal[0])<1&&std::fabs(y-m_navDiagnostic.goal[1])<1&&std::fabs(z-m_navDiagnostic.goal[2])<1&&me->IsWithinDist3d(m_navDiagnostic.start[0],m_navDiagnostic.start[1],m_navDiagnostic.start[2],3))return false;
 m_navDiagnostic.at=WorldTimer::getMSTime();m_navDiagnostic.map=me->GetMapId();m_navDiagnostic.start={{me->GetPositionX(),me->GetPositionY(),me->GetPositionZ()}};m_navDiagnostic.goal={{x,y,z}};
 PathInfo path(me);path.ExcludeSteepSlopes();path.calculate(x,y,z);
 m_navDiagnostic.type=int32(path.getPathType());m_navDiagnostic.points=path.getPath().size();m_navDiagnostic.end={{0,0,0}};
 if(!path.getPath().empty()){auto const& point=path.getPath().back();m_navDiagnostic.end={{point.x,point.y,point.z}};}
 float moveX=x,moveY=y,moveZ=z;
 if(path.getPathType()==PATHFIND_INCOMPLETE&&path.getPath().size()>=2){
  auto end=path.getPath().back();float progress=me->GetDistance(end.x,end.y,end.z);float dx=x-end.x,dy=y-end.y,dz=z-end.z;
  if(WorldBotAmbient::StageProgress(progress,std::sqrt(dx*dx+dy*dy+dz*dz))){
   PathInfo stage(me);stage.ExcludeSteepSlopes();
   if(stage.calculate(end.x,end.y,end.z)&&stage.getPathType()==PATHFIND_NORMAL&&stage.getPath().size()>=2){
    moveX=end.x;moveY=end.y;moveZ=end.z;
   }
  }
 }
 if(path.getPathType()!=PATHFIND_NORMAL&&moveX==x&&moveY==y&&moveZ==z){m_navRetryAt=navNow+5000;m_routeStall.Failed(WorldTimer::getMSTime(),me->GetMapId(),{{x,y,z}},{{me->GetPositionX(),me->GetPositionY(),me->GetPositionZ()}});m_activity="needs_route_no_legal_navmesh_path";return false;}
 m_navRetryAt=0;
 m_navGoalValid=true;m_navX=x;m_navY=y;m_navZ=z;
 if(!m_navProgressAt){m_navProgressAt=progressNow;m_navProgressX=me->GetPositionX();m_navProgressY=me->GetPositionY();m_navProgressZ=me->GetPositionZ();}
 me->GetMotionMaster()->MovePoint(0,moveX,moveY,moveZ,MOVE_PATHFINDING|MOVE_EXCLUDE_STEEP_SLOPES);return true;
}
bool WorldBotAI::TryRouteHearth(){
 if(!me)return false;
 auto spell=me->GetCurrentSpell(CURRENT_GENERIC_SPELL);
 if(m_routeHearthPending){
  if(spell&&spell->m_spellInfo->Id==8690){m_activity="route_hearth_casting";return true;}
  if(me->IsBeingTeleported())return true;
  m_routeHearthPending=false;
  auto const& home=me->GetHomeBindLocation();
  m_routeRecoveryReason=me->GetMapId()==me->GetHomeBindMap()&&me->IsWithinDist3d(home.x,home.y,home.z,20)?"hearth_at_home":"hearth_interrupted_or_unconfirmed";
 }
 uint32 now=WorldTimer::getMSTime();
 m_routeStall.Progress(me->GetMapId(),{{me->GetPositionX(),me->GetPositionY(),me->GetPositionZ()}});
 if(!m_routeStall.Due(now))return false;
 auto mode=WorldBotModes::Detect(me);WorldBotRouteRecovery::Safety safe;
 safe.freeScene=mode==WorldBotModes::Mode::World||mode==WorldBotModes::Mode::City;
 if(Group* group=me->GetGroup())safe.freeGroup=group->GetMembersCount()==1&&group->GetLeaderGuid()==me->GetObjectGuid();
 safe.alive=me->IsAlive();safe.combat=me->IsInCombat();safe.duel=me->m_duel!=nullptr;safe.bg=me->InBattleGround();safe.queue=me->InBattleGroundQueue();safe.instance=me->GetMap()&&me->GetMap()->IsDungeon();
 safe.control=me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL|UNIT_STATE_CAN_NOT_MOVE);safe.casting=me->IsNonMeleeSpellCasted();safe.water=me->IsInWater();safe.teleport=me->IsBeingTeleported();safe.taxi=me->IsTaxiFlying();safe.transport=me->GetTransport()!=nullptr;safe.trade=me->GetTradeData()!=nullptr;safe.victim=me->GetVictim()!=nullptr||!m_voluntaryPvpTarget.IsEmpty()||m_voluntaryPvpReturning;
 if(!WorldBotRouteRecovery::Safe(safe)){m_routeRecoveryReason="safety_gate_blocked";return false;}
 Item* stone=StackHearth();SpellEntry const* hearth=sSpellMgr.GetSpellEntry(8690);
 if(!stone||stone->GetEntry()!=6948||stone->GetOwnerGuid()!=me->GetObjectGuid()){m_routeRecoveryReason="own_hearthstone_missing";return false;}
 if(!hearth||me->CanUseItem(stone)!=EQUIP_ERR_OK||!me->IsSpellReady(hearth)||me->HasGCD(hearth)){m_routeRecoveryReason="native_item_cd_or_gcd_blocked";return false;}
 me->StopMoving();me->GetMotionMaster()->Clear();me->GetMotionMaster()->MoveIdle();me->SetStandState(UNIT_STAND_STATE_STAND);
 SpellCastTargets targets;targets.setUnitTarget(me);m_routeHearthCastMs=hearth->GetCastTime(me);
 me->CastItemUseSpell(stone,targets); // Native non-triggered cast, costs and cooldown; never TeleportTo.
 spell=me->GetCurrentSpell(CURRENT_GENERIC_SPELL);bool accepted=spell&&spell->m_spellInfo->Id==8690;
 m_routeStall.Attempt(now,accepted);m_routeHearthPending=accepted;
 m_routeRecoveryReason=accepted?"native_hearth_cast_accepted":"native_hearth_cast_rejected";
 m_activity=accepted?"route_hearth_casting":"route_hearth_rejected";
 return true;
}
bool WorldBotAI::ApplyIdlePreset(WorldBotProfile const& desired){
 if(desired.specId==m_profile.specId&&desired.gearId==m_profile.gearId&&!m_profile.presetInitialize)return true;
 if(!me||!m_initialized||!desired.specId||!desired.gearId||me->GetLevel()!=60||!me->IsAlive()||!me->IsInWorld()||me->IsInCombat()||me->m_duel||me->GetGroup()||me->InBattleGround()||me->InBattleGroundQueue()||me->IsBeingTeleported()||me->IsTaxiFlying()||me->GetTransport()||me->GetTradeData()||me->GetVictim()||me->IsNonMeleeSpellCasted()||me->HasUnitState(UNIT_STATE_NO_FREE_MOVE))return false;
 if(desired.characterGuid!=m_profile.characterGuid||desired.accountId!=m_profile.accountId||desired.createdAt!=m_profile.createdAt)return false;
 auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();auto const& gears=sObjectMgr.GetPlayerPremadeGearTemplates();auto spec=specs.find(desired.specId);auto gear=gears.find(desired.gearId);
 if(spec==specs.end()||gear==gears.end()||spec->second.level!=60||gear->second.level!=60||spec->second.requiredClass!=me->GetClass()||gear->second.requiredClass!=me->GetClass()||spec->second.role!=gear->second.role)return false;
 me->StopMoving();m_profile=desired;m_profile.initialize=false;m_profile.presetInitialize=true;
 // The native template reset/relearn and owned-item recovery are the same path as login.
 // A partial failure retains the WB identity/items and is reported, never reprovisioned.
 m_role=spec->second.role;
 if(!RecoverPreset()){m_activity="preset_migration_recovery_pending";return false;}
 me->UpdateSkillsToMaxSkillsForLevel();ResetSpellData();PopulateSpellData();
 if(!PresetVerified()){m_activity="preset_migration_actual_verification_failed";return false;}
 if(!me->SaveToDB(true,false,true)){m_activity="preset_migration_save_failed";return false;}
 if(m_profile.presetInitializedCallback)m_profile.presetInitializedCallback();
 m_profile.presetInitialize=false;m_activity="preset_migration_verified_saved";return true;
}
uint32 WorldBotAI::TalentPointsReport() const {
 if(!me)return 0;
 std::map<uint32,uint32> ranks;
 for(auto const& known:me->GetSpellMap())if(known.second.state!=PLAYERSPELL_REMOVED&&!known.second.disabled)
  if(auto pos=GetTalentSpellPos(known.first))ranks[pos->talent_id]=std::max(ranks[pos->talent_id],uint32(pos->rank)+1);
 uint32 points=0;for(auto const& row:ranks)points+=row.second;return points;
}
bool WorldBotAI::NeedsPracticeSupplies() const {
 if(PKTaskEnabled())return m_pkTask.supplyPending;
 return me&&me->GetLevel()==60&&m_profile.gearId&&(me->GetItemCount(PKFoodItem(),false)<3||(me->GetMaxPower(POWER_MANA)>0&&me->GetItemCount(PKWaterItem(),false)<3));
}
bool WorldBotAI::HasRationAura() const {
 for(uint32 item:{PKFoodItem(),PKWaterItem()})if(auto proto=sObjectMgr.GetItemPrototype(item))for(auto const& row:proto->Spells)if(row.SpellTrigger==ITEM_SPELLTRIGGER_ON_USE&&row.SpellId&&me->HasAura(row.SpellId))return true;
 return false;
}
void WorldBotAI::StopRationAuras(){
 for(uint32 item:{PKFoodItem(),PKWaterItem()})if(auto proto=sObjectMgr.GetItemPrototype(item))for(auto const& row:proto->Spells)if(row.SpellTrigger==ITEM_SPELLTRIGGER_ON_USE&&row.SpellId)me->RemoveAurasDueToSpell(row.SpellId);
 me->SetStandState(UNIT_STAND_STATE_STAND);
}
bool WorldBotAI::RecoverWithRations(){
 if(!me||!me->IsAlive()||me->IsInCombat()||me->IsBeingTeleported()||me->GetVictim()||(me->m_duel&&me->m_duel->startTime)||me->HasUnitState(UNIT_STATE_NO_FREE_MOVE))return false;
 bool food=PKTaskEnabled()?me->GetHealth()<me->GetMaxHealth():me->GetHealthPercent()<99;
 bool water=me->GetMaxPower(POWER_MANA)>0&&(PKTaskEnabled()?me->GetPower(POWER_MANA)<me->GetMaxPower(POWER_MANA):me->GetPowerPercent(POWER_MANA)<95);
 if(!food&&!water)return false;
 if(me->IsNonMeleeSpellCasted())return true;
 bool anyAura=HasRationAura();
 if(anyAura){me->StopMoving();me->SetStandState(UNIT_STAND_STATE_SIT);}
 uint32 now=WorldTimer::getMSTime();if(m_rationUseAt&&uint32(now-m_rationUseAt)<2500)return anyAura;
 bool recovering=anyAura;
 for(auto wanted:std::vector<std::pair<uint32,bool>>{{PKWaterItem(),water},{PKFoodItem(),food}}){if(!wanted.second)continue;
  Item* item=nullptr;
  for(uint8 slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END&&!item;++slot){auto found=me->GetItemByPos(INVENTORY_SLOT_BAG_0,slot);if(found&&found->GetEntry()==wanted.first)item=found;}
  for(uint8 bag=INVENTORY_SLOT_BAG_START;bag<INVENTORY_SLOT_BAG_END&&!item;++bag)if(auto container=dynamic_cast<Bag*>(me->GetItemByPos(INVENTORY_SLOT_BAG_0,bag)))for(uint8 slot=0;slot<container->GetBagSize();++slot){auto found=container->GetItemByPos(slot);if(found&&found->GetEntry()==wanted.first){item=found;break;}}
  if(!item||item->GetOwnerGuid()!=me->GetObjectGuid())continue;
  auto const* proto=item->GetProto();bool active=false;SpellEntry const* use=nullptr;
  for(auto const& row:proto->Spells)if(row.SpellTrigger==ITEM_SPELLTRIGGER_ON_USE&&row.SpellId){auto info=sSpellMgr.GetSpellEntry(row.SpellId);if(info&&me->HasAura(info->Id))active=true;if(info)use=info;}
  if(active){recovering=true;me->StopMoving();me->SetStandState(UNIT_STAND_STATE_SIT);m_activity="native_ration_recovering";continue;}
  if(!use||me->CanUseItem(item)!=EQUIP_ERR_OK||!me->IsSpellReady(use)||me->HasGCD(use))continue;
  me->StopMoving();me->SetStandState(UNIT_STAND_STATE_SIT);SpellCastTargets targets;targets.setUnitTarget(me);m_rationUseAt=now;me->CastItemUseSpell(item,targets);m_activity="native_ration_use_attempt";return true;
 }
 m_activity=me->GetItemCount(PKFoodItem(),false)==0||(water&&me->GetItemCount(PKWaterItem(),false)==0)?"needs_native_practice_rations":"native_ration_recovering";return recovering;
}
bool WorldBotAI::NativePracticeBuff(){
 uint32 now=WorldTimer::getMSTime();if(m_practiceBuffAt&&uint32(now-m_practiceBuffAt)<2000)return true;
 m_practiceBuffAt=now;
 auto cast=[&](Unit* target,uint32 id){auto spell=sSpellMgr.GetSpellEntry(id);if(!spell||!me->HasSpell(id)||!me->IsSpellReady(spell)||me->HasGCD(spell)||!CanTryToCastSpell(target,spell))return false;
 return me->CastSpell(target,spell,false)==SPELL_CAST_OK;};
 if(me->GetClass()==CLASS_HUNTER){
  if(Pet* pet=me->GetPet()){if(!pet->IsAlive()&&cast(pet,982))return true;}
  else if(!m_practiceBuffTried.count(883)){auto spell=sSpellMgr.GetSpellEntry(883);if(spell&&me->HasGCD(spell))return true;m_practiceBuffTried.insert(883);if(cast(me,883))return true;}
 }
 if(me->GetClass()==CLASS_WARLOCK&&(!me->GetPet()||!me->GetPet()->IsAlive())){
  for(uint32 id:{691u,697u,712u,688u})if(me->HasSpell(id)&&!m_practiceBuffTried.count(id)){auto spell=sSpellMgr.GetSpellEntry(id);if(spell&&me->HasGCD(spell))return true;m_practiceBuffTried.insert(id);if(cast(me,id))return true;break;}
 }
 std::vector<uint32> roots;
 switch(me->GetClass()){
 case CLASS_WARRIOR:roots={2457};break;
 case CLASS_PRIEST:roots={1243,14752,588,17};break;
 case CLASS_MAGE:roots={1459,168};break;
 case CLASS_WARLOCK:roots={687,19028};break;
 case CLASS_DRUID:roots={1126,467};break;
 case CLASS_SHAMAN:roots={324};break;
 case CLASS_PALADIN:roots={19740,465};break;
 case CLASS_HUNTER:roots={13165};break;
 case CLASS_ROGUE:roots={1784};break;
 }
 for(uint32 root:roots){uint32 best=0,rank=0;
  for(auto const& known:me->GetSpellMap())if(known.second.state!=PLAYERSPELL_REMOVED&&!known.second.disabled){uint32 first=sSpellMgr.GetFirstSpellInChain(known.first);if((first?first:known.first)!=root)continue;uint32 next=sSpellMgr.GetSpellRank(known.first);if(!best||next>rank){best=known.first;rank=next;}}
  if(best&&!me->HasAura(best)&&!m_practiceBuffTried.count(root)){auto spell=sSpellMgr.GetSpellEntry(best);if(spell&&me->HasGCD(spell))return true;m_practiceBuffTried.insert(root);if(cast(me,best))return true;}
 }
 return false;
}
bool WorldBotAI::PracticeCombatAction(){
 if(!me||!me->IsAlive()||m_profile.purpose!="city_duel"||!IsInDuel()||!me->m_duel->startTime||me->IsNonMeleeSpellCasted()||!me->GetVictim())return false;
 uint32 now=WorldTimer::getMSTime();if(m_practiceActionAt&&uint32(now-m_practiceActionAt)<1500)return false;
 if(me->GetVictim()->IsNonMeleeSpellCasted())return false; // Shared PvP reaction gets first opportunity.
 m_practiceActionAt=now;
 auto cast=[&](Unit* target,SpellEntry const* spell){return spell&&me->HasSpell(spell->Id)&&me->IsSpellReady(spell)&&!me->HasGCD(spell)&&CanTryToCastSpell(target,spell)&&me->CastSpell(target,spell,false)==SPELL_CAST_OK;};
 if(me->GetClass()==CLASS_HUNTER&&me->HasSpell(19574)&&me->GetPet()&&me->GetPet()->IsAlive())if(cast(me->GetPet(),sSpellMgr.GetSpellEntry(19574)))return true;
 if(me->GetClass()==CLASS_WARLOCK&&me->HasSpell(19028)&&me->GetPet()&&me->GetPet()->IsAlive()&&!me->HasAura(19028))if(cast(me,sSpellMgr.GetSpellEntry(19028)))return true;
 if(me->GetClass()==CLASS_MAGE&&m_spells.mage.pPresenceOfMind&&m_spells.mage.pPyroblast){
  if(me->HasAura(m_spells.mage.pPresenceOfMind->Id))return cast(me->GetVictim(),m_spells.mage.pPyroblast);
  if(CanTryToCastSpell(me->GetVictim(),m_spells.mage.pPyroblast)&&cast(me,m_spells.mage.pPresenceOfMind))return true;
 }
 if(me->GetClass()==CLASS_WARRIOR){
  if(!me->IsInCombat()&&cast(me->GetVictim(),m_spells.warrior.pCharge))return true;
  if(me->IsInCombat()&&me->GetPower(POWER_RAGE)<100&&me->GetHealthPercent()>60&&cast(me,m_spells.warrior.pBloodrage))return true;
 }
 return false;
}
bool WorldBotAI::PrepareDuel(){
 if(!me||!me->m_duel||me->m_duel->startTime)return true;
 Player* challenger=me->m_duel->opponent;
 if(challenger&&challenger->IsInWorld()&&challenger->GetMap()==me->GetMap()&&challenger->IsVisibleForOrDetect(me,me,false)){
  if(me->GetSelectionGuid()!=challenger->GetObjectGuid()){
   me->SetSelectionGuid(challenger->GetObjectGuid());
   if(PKTaskEnabled())PKTaskEvent("challenger_selected",challenger->GetGUIDLow());
  }
  if(!me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))me->SetFacingToObject(challenger);
 }
 // A real player can challenge a bot during a mounted supply trip.
 // Preparation owns movement from that moment, not only after gate arrival.
 me->StopMoving();
 if(me->IsMounted())me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
 if(me->m_duel->startTimer){StopRationAuras();m_activity="duel_countdown_ready";return true;}
 auto arbiter=me->GetGuidValue(PLAYER_DUEL_ARBITER);uint32 now=WorldTimer::getMSTime();
 if(arbiter!=m_duelPrepArbiter){m_duelPrepArbiter=arbiter;m_duelPrepAt=now;m_duelPrepDone=false;m_duelQuiet=0;m_practiceBuffTried.clear();m_practiceBuffAt=0;}
 if(me->IsNonMeleeSpellCasted()) {m_activity="duel_preparing_cast";return false;}
 if(!m_duelPrepDone&&WorldBotAmbient::DuelBuffBudget(now,m_duelPrepAt)){
  me->SetStandState(UNIT_STAND_STATE_STAND);
  if(me->GetClass()==CLASS_WARRIOR&&m_spells.warrior.pBattleStance&&!me->HasAura(m_spells.warrior.pBattleStance->Id)&&CanTryToCastSpell(me,m_spells.warrior.pBattleStance)){
   me->CastSpell(me,m_spells.warrior.pBattleStance,false);m_activity="duel_battle_stance";return false;
  }
  m_isBuffing=false;bool prepared=NativePracticeBuff();
  if(me->IsNonMeleeSpellCasted()||prepared){m_duelQuiet=0;m_activity="duel_preparing_buffs";return false;}
  // Quiet samples during GCD/backoff are not proof of complete preparation.
  m_activity="duel_preparing_budget";return false;
 }
 m_duelPrepDone=true;
 bool recovered=me->GetHealthPercent()>=99&&(me->GetMaxPower(POWER_MANA)==0||me->GetPowerPercent(POWER_MANA)>=95)&&(!me->GetPet()||(me->GetPet()->IsAlive()&&me->GetPet()->GetHealthPercent()>=95));
 if(PKTaskEnabled())recovered=me->GetHealth()==me->GetMaxHealth()&&(me->GetMaxPower(POWER_MANA)==0||me->GetPower(POWER_MANA)==me->GetMaxPower(POWER_MANA))&&(!me->GetPet()||(me->GetPet()->IsAlive()&&me->GetPet()->GetHealth()==me->GetPet()->GetMaxHealth()));
 if(!recovered){
  // Consume real owned food/water via native item use; missing supplies remain observable.
  if(!RecoverWithRations()){me->StopMoving();me->SetStandState(UNIT_STAND_STATE_SIT);m_activity="duel_pet_or_natural_recovery";}
  return false;
 }
 StopRationAuras();m_activity="duel_prepared";return true;
}
bool WorldBotAI::TryAmbientHeal(){
 if(m_profile.purpose!="city_healer"||!me||me->GetLevel()!=60||!me->IsAlive()||me->m_duel||me->IsFFAPvP()||me->IsInCombat()||me->IsBeingTeleported()||me->GetGroup()||me->InBattleGround()||!me->GetMap()||me->GetMap()->IsDungeon()||me->GetVictim()||me->IsNonMeleeSpellCasted()||me->HasUnitState(UNIT_STATE_NO_FREE_MOVE))return false;
 uint32 now=WorldTimer::getMSTime();if(m_helpAt&&uint32(now-m_helpAt)<5000)return false;m_helpAt=now;
 std::list<Player*> nearby;MaNGOS::AnyUnitInObjectRangeCheck check(me,30);MaNGOS::PlayerListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> search(nearby,check);Cell::VisitWorldObjects(me,search,30);
 Player* patient=nullptr;
 for(Player* p:nearby){if(p==me)continue;WorldBotAmbient::Patient a;
  a.alive=p->IsAlive();a.inWorld=p->IsInWorld();a.sameMap=p->GetMap()==me->GetMap();a.friendly=me->IsFriendlyTo(p);a.combat=p->IsInCombat();a.duel=p->m_duel!=nullptr;a.ffa=p->IsFFAPvP();a.resting=p->GetStandState()!=UNIT_STAND_STATE_STAND;a.casting=p->IsNonMeleeSpellCasted();a.health=p->GetHealthPercent();
  if(!WorldBotAmbient::MayHelp(a)||p->InBattleGround()||!me->IsWithinLOSInMap(p)||!p->IsVisibleForOrDetect(me,me,false))continue;
  if(!patient||p->GetHealthPercent()<patient->GetHealthPercent())patient=p;
 }
 if(patient)for(auto spell:m_spellListDirectHeal){if(spell->IsAreaOfEffectSpell()||!CanTryToCastSpell(patient,spell))continue;
  me->StopMoving();if(me->CastSpell(patient,spell,false)==SPELL_CAST_OK){m_activity="city_friendly_native_heal";return true;}
 }
 return false;
}
bool WorldBotAI::TrySpiritRecovery(){
 if(!me||me->IsAlive()||!me->HasFlag(PLAYER_FLAGS,PLAYER_FLAGS_GHOST)||me->InBattleGround()||me->m_duel||me->GetGroup()||me->IsBeingTeleported())return false;
 uint32 now=WorldTimer::getMSTime();if(!m_deadAt)m_deadAt=now;
 if(uint32(now-m_deadAt)<120000||(!m_routeStall.tracking&&m_activity!="corpse_no_legal_reclaim_approach")||(m_spiritAt&&uint32(now-m_spiritAt)<30000))return false;
 for(Creature* npc:Nearby(me,60))if(npc->HasFlag(UNIT_NPC_FLAGS,UNIT_NPC_FLAG_SPIRITHEALER)){
  if(me->GetNPCIfCanInteractWith(npc->GetObjectGuid(),UNIT_NPC_FLAG_SPIRITHEALER)){
   m_spiritAt=now;WorldPackets::Npc::SpiritHealerActivate request;request.guid=npc->GetObjectGuid();me->GetSession()->HandleSpiritHealerActivateOpcode(request);
   m_activity=me->IsAlive()?"native_spirit_healer_recovery":"native_spirit_healer_refused";return true;
  }
  PathInfo path(me);path.ExcludeSteepSlopes();if(path.calculate(npc->GetPositionX(),npc->GetPositionY(),npc->GetPositionZ())&&path.getPathType()==PATHFIND_NORMAL){m_activity="spirit_healer_native_route";Navigate(npc->GetPositionX(),npc->GetPositionY(),npc->GetPositionZ());return true;}
 }
 m_activity="corpse_and_spirit_routes_unavailable";return false;
}
bool WorldBotAI::Bootstrap(){
 if(!m_profile.initialize)return true;
 bool alreadySeeded=WorldBotPopulation::SeedSnapshot(me->GetLevel(),m_profile.seedLevel,me->GetMoney(),me->GetUInt32Value(PLAYER_XP));
 // Existing inventory is preserved. WorldBot bootstrap no longer creates or requires four 16-slot bags.
 if(alreadySeeded){m_profile.initialize=false;if(m_profile.initializedCallback)m_profile.initializedCallback();return true;}
 if(me->GetLevel()!=1||me->GetUInt32Value(PLAYER_XP)!=0||me->GetMoney()!=sWorld.getConfig(CONFIG_UINT32_START_PLAYER_MONEY)||m_profile.seedLevel<1||m_profile.seedLevel>(m_profile.allowLevel60?60:59)){m_activity="bootstrap_refused_nonpristine";return false;}
 me->GiveLevel(m_profile.seedLevel);me->InitTalentForLevel();me->SetUInt32Value(PLAYER_XP,0);me->SetMoney(20000000);
 me->UpdateSkillsToMaxSkillsForLevel();if(!me->SaveToDB(true,false,true)){m_activity="seed_commit_failed";return false;}m_profile.initialize=false;
 if(m_profile.initializedCallback)m_profile.initializedCallback();
 return true;
}

void WorldBotAI::OnPlayerLogin(){m_worldLoginPending.store(true);}
void WorldBotAI::CompleteWorldLogin(){
 if(!me || m_profile.id.empty())return;
 m_leaderGuid=me->GetObjectGuid();m_role=IsRangedDamageClass(me->GetClass())?ROLE_RANGE_DPS:ROLE_MELEE_DPS;
 m_initialized=true;
 if(!Bootstrap()){if(botEntry)botEntry->requestRemoval=true;return;}
 RefreshGrowth();
 if(m_profile.specId || m_profile.gearId){
  auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();auto const& gears=sObjectMgr.GetPlayerPremadeGearTemplates();
  auto spec=specs.find(m_profile.specId);auto gear=gears.find(m_profile.gearId);
  if(spec==specs.end()||gear==gears.end()||me->GetLevel()!=60||spec->second.level!=60||gear->second.level!=60||spec->second.requiredClass!=me->GetClass()||gear->second.requiredClass!=me->GetClass()||spec->second.role!=gear->second.role){m_activity="preset_identity_rejected";botEntry->requestRemoval=true;return;}
  m_role=spec->second.role;
  // Re-establish native use requirements for the declared fixed NPC kit, even
  // when its items survived the previous logout (e.g. rank-gated PvP trinkets).
  for(auto const& required:gear->second.items)if(!required.requiredTeam||required.requiredTeam==uint32(me->GetTeam()))
   if(auto prototype=sObjectMgr.GetItemPrototype(required.itemId))me->SatisfyItemRequirements(prototype);
  if((m_profile.presetInitialize||!PresetVerified())&&!RecoverPreset()){me->SaveToDB();if(m_activity!="preset_pending_owned_mail_recovery"&&m_activity!="preset_mail_lookup_failed"&&m_activity.find("preset_equip_rejected_item=")!=0)m_activity="preset_recovery_pending";botEntry->requestRemoval=true;return;}
  // Requirements can teach a newly equipped weapon proficiency; train it after that.
  me->UpdateSkillsToMaxSkillsForLevel();
  ResetSpellData();PopulateSpellData();
  if(!PresetVerified()){me->SaveToDB();m_activity="preset_actual_verification_failed";botEntry->requestRemoval=true;return;}
 }
 if(!me->SaveToDB(true,false,true)){m_activity="preset_commit_failed";botEntry->requestRemoval=true;return;}
 if(m_profile.presetInitialize&&m_profile.specId&&m_profile.presetInitializedCallback)m_profile.presetInitializedCallback();
 m_profile.presetInitialize=false;
 // A verified fixed level60 practice preset already includes its native class spells.
 // Keep practice actors at the arena rather than routing them to an unnecessary trainer.
 if(m_profile.specId&&m_profile.gearId&&me->GetLevel()==60)m_needsTrainer=false;
 m_service=30000;m_activity="ready";
 // Taming can enter combat; fixed-kit equipment must be committed before pet creation.
 if(m_profile.gearId)SummonPetIfNeeded();
}
bool WorldBotAI::RecoverPreset(){
 auto const& gear=sObjectMgr.GetPlayerPremadeGearTemplates().at(m_profile.gearId);
 sObjectMgr.ApplyPremadeSpecTemplateToPlayer(m_profile.specId,me);me->UpdateSkillsToMaxSkillsForLevel();
 std::map<uint8,PlayerPremadeItem const*> expected;bool twoHand=false;
 for(auto const& row:gear.items){if(row.requiredTeam&&row.requiredTeam!=uint32(me->GetTeam()))continue;auto proto=sObjectMgr.GetItemPrototype(row.itemId);if(!proto)return false;
  uint8 slots[4];proto->GetAllowedEquipSlots(slots,me->GetClass(),me->HasSpell(674));
  for(uint8 slot:slots){if(slot>=EQUIPMENT_SLOT_END||slot==EQUIPMENT_SLOT_BODY||slot==EQUIPMENT_SLOT_TABARD||expected.count(slot)||(slot==EQUIPMENT_SLOT_OFFHAND&&twoHand))continue;expected[slot]=&row;if(slot==EQUIPMENT_SLOT_MAINHAND&&proto->InventoryType==INVTYPE_2HWEAPON)twoHand=true;break;}
 }
 auto inventory=[&](){std::vector<Item*> items;for(uint8 slot=0;slot<INVENTORY_SLOT_ITEM_END;++slot)if(Item* item=me->GetItemByPos(INVENTORY_SLOT_BAG_0,slot)){items.push_back(item);if(slot>=INVENTORY_SLOT_BAG_START&&slot<INVENTORY_SLOT_BAG_END)if(Bag* bag=dynamic_cast<Bag*>(item))for(uint32 child=0;child<bag->GetBagSize();++child)if(Item* inside=bag->GetItemByPos(child))items.push_back(inside);}return items;};
 std::set<uint64> used;
 for(auto const& pair:expected){auto const& row=*pair.second;auto proto=sObjectMgr.GetItemPrototype(row.itemId);me->SatisfyItemRequirements(proto);
  auto find=[&](){for(Item* item:inventory())if(!used.count(item->GetGUID())&&item->GetEntry()==row.itemId&&item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)==row.enchantId)return item;return static_cast<Item*>(nullptr);};
  Item* item=find();if(!item){
   // A native inventory-recovery mail is still owned stock, not a missing item.
   // Never mint another copy while it awaits a safe mailbox/repair operation.
   std::unique_ptr<QueryResult> ownedMail(CharacterDatabase.PQuery("SELECT COUNT(*) FROM mail_items WHERE receiver_guid=%u AND item_id=%u",me->GetGUIDLow(),row.itemId));
   if(!ownedMail){m_activity="preset_mail_lookup_failed";return false;}
   if(ownedMail->Fetch()[0].GetUInt32()){m_activity="preset_pending_owned_mail_recovery";sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[WorldBot] character %u preset item %u exists in owned mail; refusing replacement",me->GetGUIDLow(),row.itemId);return false;}
   if(!me->StoreNewItemInBestSlots(row.itemId,1,row.enchantId))return false;
   item=find();
  }if(!item)return false;
  uint16 destination=uint16(INVENTORY_SLOT_BAG_0)<<8|pair.first;
  if(item->GetPos()!=destination){uint16 checked=0;auto result=me->CanEquipItem(pair.first,checked,item,true);if(result!=EQUIP_ERR_OK){m_activity="preset_equip_rejected_item="+std::to_string(row.itemId)+"_result="+std::to_string(result)+"_combat="+std::to_string(me->IsInCombat());return false;}me->SwapItem(item->GetPos(),destination);}
  if(me->GetItemByPos(INVENTORY_SLOT_BAG_0,pair.first)!=item)return false;
  used.insert(item->GetGUID());
 }
 if(me->GetClass()==CLASS_HUNTER){
  Item* ranged=me->GetItemByPos(INVENTORY_SLOT_BAG_0,EQUIPMENT_SLOT_RANGED);uint32 ammo=m_profile.ammoItem?m_profile.ammoItem:19316;
  auto prototype=sObjectMgr.GetItemPrototype(ammo);
  if(!ranged||!prototype||me->CanUseAmmo(ammo)!=EQUIP_ERR_OK)return false;
  // The configured first cohort uses bows/crossbows and arrows, not gun bullets.
  if((ranged->GetProto()->SubClass!=ITEM_SUBCLASS_WEAPON_BOW&&ranged->GetProto()->SubClass!=ITEM_SUBCLASS_WEAPON_CROSSBOW)||prototype->SubClass!=ITEM_SUBCLASS_ARROW)return false;
  uint32 quantity=std::min<uint32>(200,prototype->GetMaxStackSize());uint32 owned=me->GetItemCount(ammo,false);
  if(owned<quantity&&!me->StoreNewItemInBestSlots(ammo,quantity-owned))return false;
  me->SetAmmo(ammo);if(me->GetUInt32Value(PLAYER_AMMO_ID)!=ammo)return false;
 }
 return PresetVerified();
}
Player* WorldBotAI::GetPartyLeader() const {
 if(me&&m_leaderGuid==me->GetObjectGuid())return me;
 return PartyBotAI::GetPartyLeader();
}
void WorldBotAI::AddHunterAmmo(){
 // Called only by the World-phase combat driver. Persistent inventory is never
 // cleared to make room for consumables; temporary AIs keep the base behavior.
 if(!me||!me->IsBot()||m_profile.id.empty()||me->GetClass()!=CLASS_HUNTER)return;
 Item* ranged=me->GetItemByPos(INVENTORY_SLOT_BAG_0,EQUIPMENT_SLOT_RANGED);
 if(!ranged||ranged->GetProto()->Class!=ITEM_CLASS_WEAPON)return;
 unsigned weapon=ranged->GetProto()->SubClass;std::vector<WorldBotAmmoPolicy::Candidate> pool;
 bool fixed=m_profile.gearId!=0;uint32 declared=m_profile.ammoItem?m_profile.ammoItem:19316;
 for(auto const& entry:sObjectMgr.GetItemPrototypeMap()){
  auto const& proto=entry.second;
  if(proto.Class!=ITEM_CLASS_PROJECTILE||proto.HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE)||!WorldBotAmmoPolicy::Compatible(weapon,proto.SubClass)||(fixed&&proto.ItemId!=declared))continue;
  pool.push_back({proto.ItemId,proto.SubClass,proto.RequiredLevel,proto.ItemLevel,proto.GetMaxStackSize(),me->CanUseAmmo(proto.ItemId)==EQUIP_ERR_OK});
 }
 uint32 ammo=WorldBotAmmoPolicy::Select(fixed,declared,weapon,me->GetLevel(),pool);
 auto proto=ammo?sObjectMgr.GetItemPrototype(ammo):nullptr;
 if(!proto){m_activity="ammo_config_or_native_requirement_rejected";return;}
 uint32 add=WorldBotAmmoPolicy::Deficit(me->GetItemCount(ammo,false),proto->GetMaxStackSize());
 if(add&&!me->StoreNewItemInBestSlots(ammo,add)){m_activity="inventory_full_needs_ammo_vendor";return;}
 if(me->CanUseAmmo(ammo)!=EQUIP_ERR_OK){m_activity="ammo_native_requirement_rejected";return;}
 me->SetAmmo(ammo);
 if(me->GetUInt32Value(PLAYER_AMMO_ID)!=ammo)m_activity="ammo_native_activation_failed";
}
bool WorldBotAI::ExternalPvpScope() const {
 if(!me||!m_initialized||!me->IsInWorld()||!me->IsAlive()||me->IsBeingTeleported()||me->InBattleGround())return false;
 if(IsInDuel())return true;
 Unit* target=me->GetVictim();return target&&target->GetAffectingPlayer()&&target->GetAffectingPlayer()!=me&&me->IsValidAttackTarget(target);
}
bool WorldBotAI::AllowLegacySpell(SpellEntry const* spell) const {
 if(spell&&me&&me->GetLevel()==60&&m_profile.gearId){
  if(spell->Id==18788)return false; // PvP practice retains its pet; never legacy Demonic Sacrifice.
  if(me->GetClass()==CLASS_MAGE&&m_spells.mage.pPresenceOfMind&&m_spells.mage.pPyroblast&&me->HasAura(m_spells.mage.pPresenceOfMind->Id)&&!spell->IsPositiveSpell()&&spell->Id!=m_spells.mage.pPyroblast->Id)return false;
 }
 return !m_externalScope||BattleBotPolicyRuntime::CanLegacyExternal(*this,m_pvpPolicy,spell,m_externalDuel);
}
std::vector<std::string> WorldBotAI::EquipmentReport() const {
 std::vector<std::string> report;if(!me)return report;
 for(uint8 slot=EQUIPMENT_SLOT_START;slot<EQUIPMENT_SLOT_END;++slot){Item* item=me->GetItemByPos(INVENTORY_SLOT_BAG_0,slot);report.push_back(std::to_string(slot)+"="+std::to_string(item?item->GetEntry():0)+":"+std::to_string(item?item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT):0));}return report;
}
bool WorldBotAI::PresetVerified() const {
 if(!me || !m_profile.specId || !m_profile.gearId)return false;
 auto fail=[&](char const* reason,uint32 detail=0){sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[WorldBotPreset] guid=%u spec=%u gear=%u reason=%s detail=%u level=%u role=%u spent=%u free=%u",me->GetGUIDLow(),m_profile.specId,m_profile.gearId,reason,detail,me->GetLevel(),uint32(m_role),TalentPointsReport(),me->GetFreeTalentPoints());return false;};
 auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();auto const& gears=sObjectMgr.GetPlayerPremadeGearTemplates();
 auto spec=specs.find(m_profile.specId);auto gear=gears.find(m_profile.gearId);
 if(spec==specs.end()||gear==gears.end()||me->GetLevel()!=60||spec->second.requiredClass!=me->GetClass()||gear->second.requiredClass!=me->GetClass()||m_role!=spec->second.role)return fail("identity_or_role");
 for(uint32 spell:spec->second.spells){if(me->HasSpell(spell))continue;bool covered=false;uint32 first=sSpellMgr.GetFirstSpellInChain(spell);for(auto const& known:me->GetSpellMap())if(first&&known.second.state!=PLAYERSPELL_REMOVED&&!known.second.disabled&&sSpellMgr.GetFirstSpellInChain(known.first)==first&&sSpellMgr.GetSpellRank(known.first)>=sSpellMgr.GetSpellRank(spell)){covered=true;break;}if(!covered)return fail("missing_native_spell",spell);}
 std::multiset<std::pair<uint32,uint32>> actual;
 for(uint8 slot=EQUIPMENT_SLOT_START;slot<EQUIPMENT_SLOT_END;++slot)if(Item* item=me->GetItemByPos(INVENTORY_SLOT_BAG_0,slot))actual.insert({item->GetEntry(),item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)});
 unsigned expected=0;
 for(auto const& item:gear->second.items){if(item.requiredTeam&&item.requiredTeam!=uint32(me->GetTeam()))continue;auto proto=sObjectMgr.GetItemPrototype(item.itemId);if(!proto)return fail("missing_item_prototype",item.itemId);
  if(proto->InventoryType==INVTYPE_NON_EQUIP||proto->InventoryType==INVTYPE_BAG||proto->InventoryType==INVTYPE_AMMO||proto->InventoryType==INVTYPE_QUIVER)continue;
  auto found=actual.find({item.itemId,item.enchantId});if(found==actual.end())return fail("equipped_item_or_enchant_missing",item.itemId);actual.erase(found);++expected;
 }
 if(expected<15)return fail("expected_gear_count",expected);
 if(TalentPointsReport()!=51)return fail("spent_talents_not_51");
 if(me->GetFreeTalentPoints()!=0)return fail("free_talents_not_zero");
 return true;
}
void WorldBotAI::MovementInform(uint32 type,uint32 data){WorldBotModes::MovementInform(*this,type,data);}
void WorldBotAI::OnPacketReceived(WorldPacket const* packet){
 if(!packet)return;
 std::lock_guard<std::mutex> lock(m_worldPacketMutex);
 if(m_worldPackets.size()>=256)m_worldPackets.pop_front();
 m_worldPackets.emplace_back(new WorldPacket(*packet));
}
void WorldBotAI::HandleWorldPacket(WorldPacket const* packet){
 if(m_externalScope)BattleBotPolicyRuntime::ObserveExternal(m_pvpPolicy,packet);
 if(WorldBotModes::Packet(*this,*packet))return;
 if(packet->GetOpcode()==SMSG_TRADE_STATUS)return;
 // Never call the inherited trade-complete autoequipper: it destroys old gear.
 CombatBotBaseAI::OnPacketReceived(packet);
}
void WorldBotAI::RefreshGrowth(){
 if(m_knownLevel==me->GetLevel())return;
 m_knownLevel=me->GetLevel();m_needsTrainer=true;me->InitTalentForLevel();
 // Only class skill-line spells and their current ranks; exclude pet abilities,
 // talents, professions and explicit teaching records.
 struct Basic {uint8 cls,level;uint32 spell;};
 static Basic const basics[]={{1,10,71},{1,30,2458},{2,12,7328},{3,10,883},{3,10,1515},{3,10,2641},{9,1,688},{9,10,697},{9,20,712},{9,30,691},{9,40,5784},{11,10,5487},{11,16,1066},{11,20,768},{11,30,783},{11,40,9634}};
 for(auto basic:basics)if(me->GetClass()==basic.cls&&me->GetLevel()>=basic.level&&sSpellMgr.GetSpellEntry(basic.spell))me->LearnSpell(basic.spell,false);
 if(me->GetClass()==CLASS_SHAMAN){
  for(auto const& item:std::vector<std::pair<unsigned,unsigned>>{{4,5176},{4,5175},{20,5177},{30,5178}})if(me->GetLevel()>=item.first&&!me->HasItemCount(item.second,1))AddItemToInventory(item.second,1);
 }
 if(me->GetClass()==CLASS_DRUID)m_role=me->HasSpell(768)?ROLE_MELEE_DPS:ROLE_RANGE_DPS;
 if(me->GetClass()==CLASS_PRIEST){
  struct Racial {unsigned race,level,spell;};
  for(auto ability:std::vector<Racial>{{1,10,13908},{1,20,13896},{3,10,13908},{3,20,6346},{4,10,10797},{4,20,2651},{5,10,2652},{5,20,2944},{8,10,9035},{8,20,18137}})if(me->GetRace()==ability.race&&me->GetLevel()>=ability.level)me->LearnSpell(ability.spell,false);
 }
 // Spend only native available talent points. LearnTalent enforces row/prerequisites.
 unsigned preferred=me->GetClass()==CLASS_WARRIOR?1:me->GetClass()==CLASS_PALADIN?2:me->GetClass()==CLASS_HUNTER?1:me->GetClass()==CLASS_ROGUE?1:me->GetClass()==CLASS_PRIEST?2:me->GetClass()==CLASS_SHAMAN?0:me->GetClass()==CLASS_MAGE?2:me->GetClass()==CLASS_WARLOCK?0:1;
 for(unsigned pass=0;!m_profile.specId&&pass<60&&me->GetFreeTalentPoints();++pass){
  uint32 before=me->GetFreeTalentPoints();
  for(uint32 id=0;id<sTalentStore.GetNumRows()&&me->GetFreeTalentPoints();++id)if(TalentEntry const* talent=sTalentStore.LookupEntry(id)){
   TalentTabEntry const* tab=sTalentTabStore.LookupEntry(talent->TalentTab);if(!tab||!(tab->ClassMask&me->GetClassMask())||tab->tabpage!=preferred)continue;
   uint32 next=0;for(uint32 rank=0;rank<5;++rank)if(talent->RankID[rank]&&me->HasSpell(talent->RankID[rank]))next=rank+1;
   if(next<5&&talent->RankID[next])me->LearnTalent(id,next);
  }
  if(before==me->GetFreeTalentPoints())break;
 }
 LearnArmorProficiencies();ResetSpellData();PopulateSpellData();AddAllSpellReagents();if(!m_profile.gearId)SummonPetIfNeeded();
}
Item* WorldBotAI::StackHearth(){
 for(uint8 slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)if(Item* item=me->GetItemByPos(INVENTORY_SLOT_BAG_0,slot))if(item->GetEntry()==6948)return item;
 for(uint8 bag=INVENTORY_SLOT_BAG_START;bag<INVENTORY_SLOT_BAG_END;++bag)if(Bag* b=dynamic_cast<Bag*>(me->GetItemByPos(INVENTORY_SLOT_BAG_0,bag)))for(uint8 slot=0;slot<b->GetBagSize();++slot)if(Item* item=b->GetItemByPos(slot))if(item->GetEntry()==6948)return item;
 return nullptr;
}
bool WorldBotAI::ServiceNearby(){
 for(Creature* npc:Nearby(me,30)){
  if(npc->GetEntry()==m_serviceEntry){auto const* info=npc->GetCreatureInfo();m_serviceObservation.at=WorldTimer::getMSTime();m_serviceObservation.entry=npc->GetEntry();m_serviceObservation.flags=npc->GetUInt32Value(UNIT_NPC_FLAGS);m_serviceObservation.trainerType=info->trainer_type;m_serviceObservation.trainerClass=info->trainer_class;m_serviceObservation.faction=npc->GetFactionTemplateId();m_serviceObservation.distance=me->GetDistance(npc);m_serviceObservation.alive=npc->IsAlive();m_serviceObservation.friendly=npc->IsFriendlyTo(me);m_serviceObservation.los=me->IsWithinLOSInMap(npc);m_serviceObservation.trainerCompatible=false;m_serviceObservation.affordable=0;}
  if(!npc->IsAlive() || !npc->IsFriendlyTo(me))continue;
  if(NeedsPracticeSupplies()&&npc->IsVendor()&&me->GetNPCIfCanInteractWith(npc->GetObjectGuid(),UNIT_NPC_FLAG_VENDOR)){
   for(uint32 item:{PKFoodItem(),PKWaterItem()}){if(item==PKWaterItem()&&me->GetMaxPower(POWER_MANA)==0)continue;
    uint32 before=me->GetItemCount(item,false);auto proto=sObjectMgr.GetItemPrototype(item);if(!proto||!proto->BuyCount)continue;
    uint32 wanted=PKTaskEnabled()?proto->GetMaxStackSize():20;if(!wanted||before>=wanted)continue;
    bool sold=false;for(VendorItemData const* list:{npc->GetVendorItems(),npc->GetVendorTemplateItems()})if(list)for(auto row:list->m_items)if(row->item==item)sold=true;
    if(!sold)continue;
    uint32 now=WorldTimer::getMSTime();if(m_rationUseAt&&uint32(now-m_rationUseAt)<2500)return true;m_rationUseAt=now;
    WorldPackets::Item::BuyItem buy;buy.vendorGuid=npc->GetObjectGuid();buy.item=item;buy.count=uint8((wanted-before+proto->BuyCount-1)/proto->BuyCount);me->GetSession()->HandleBuyItemOpcode(buy);
    if(me->GetItemCount(item,false)>before){m_activity=me->SaveToDB(true,false,true)?"native_rations_bought_saved":"native_rations_save_pending";}else m_activity="native_ration_purchase_refused";
    return true;
   }
  }
  // Fixed practice kits use this pass only for actual nearby ration purchases;
  // never divert a supply journey to unrelated training or shop upgrades.
  if(m_profile.gearId)continue;
  bool vendor=npc->IsVendor();bool trainer=npc->IsTrainer()&&npc->IsTrainerOf(me,false)&&npc->GetCreatureInfo()->trainer_type==TRAINER_TYPE_CLASS&&npc->GetCreatureInfo()->trainer_class==me->GetClass();
  bool junk=false;for(uint8 slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)if(Item* item=me->GetItemByPos(INVENTORY_SLOT_BAG_0,slot))if(item->GetProto()->Quality==ITEM_QUALITY_POOR&&item->GetProto()->SellPrice)junk=true;
  for(uint8 bag=INVENTORY_SLOT_BAG_START;bag<INVENTORY_SLOT_BAG_END;++bag)if(Bag* b=dynamic_cast<Bag*>(me->GetItemByPos(INVENTORY_SLOT_BAG_0,bag)))for(uint8 slot=0;slot<b->GetBagSize();++slot)if(Item* item=b->GetItemByPos(slot))if(item->GetProto()->Quality==ITEM_QUALITY_POOR&&item->GetProto()->SellPrice)junk=true;
  bool train=false;uint32 affordable=0;
  if(trainer)for(TrainerSpellData const* data:{npc->GetTrainerSpells(),npc->GetTrainerTemplateSpells()})if(data)for(auto const& row:data->spellList)if(me->GetTrainerSpellState(&row.second)==TRAINER_SPELL_GREEN && row.second.spellCost<=me->GetMoney()){train=true;++affordable;}
  if(npc->GetEntry()==m_serviceEntry){m_serviceObservation.trainerCompatible=trainer;m_serviceObservation.affordable=affordable;}
  uint32 purchase=0;
  if(vendor&&!m_profile.gearId)for(VendorItemData const* data:{npc->GetVendorItems(),npc->GetVendorTemplateItems()})if(data)for(auto const* sale:data->m_items){
   ItemPrototype const* item=sObjectMgr.GetItemPrototype(sale->item);if(!item || item->BuyPrice>200000 || item->BuyPrice>me->GetMoney() || me->CanUseItem(item)!=EQUIP_ERR_OK || (item->Class!=ITEM_CLASS_ARMOR&&item->Class!=ITEM_CLASS_WEAPON))continue;
   uint8 slot=me->FindEquipSlot(item,NULL_SLOT,true);if(slot==NULL_SLOT)continue;Item* current=me->GetItemByPos(INVENTORY_SLOT_BAG_0,slot);
   if(!current || item->ItemLevel>current->GetProto()->ItemLevel){purchase=item->ItemId;break;}
  }
  if(trainer&&!train)m_needsTrainer=false;
  if(!train&&!(vendor&&junk)&&!purchase)continue;
  if(!me->IsWithinDistInMap(npc,INTERACTION_DISTANCE)){
   if(m_hasService&&npc->GetEntry()!=m_serviceEntry)continue;
   m_activity="service_travel";Navigate(m_hasService?m_serviceX:npc->GetPositionX(),m_hasService?m_serviceY:npc->GetPositionY(),m_hasService?m_serviceZ:npc->GetPositionZ());return true;
  }
  if(trainer && npc->IsTrainerOf(me,false)){
   for(TrainerSpellData const* data:{npc->GetTrainerSpells(),npc->GetTrainerTemplateSpells()})if(data)for(auto const& row:data->spellList)if(me->GetTrainerSpellState(&row.second)==TRAINER_SPELL_GREEN){
    WorldPackets::Npc::TrainerBuySpell buy;buy.guid=npc->GetObjectGuid();buy.spellId=row.first;me->GetSession()->HandleTrainerBuySpellOpcode(buy);
   }
   ResetSpellData();PopulateSpellData();
  }
  if(purchase && me->BuyItemFromVendor(npc->GetObjectGuid(),purchase,1,NULL_BAG,NULL_SLOT)){m_activity="bought_level_gear";return true;}
  if(vendor){
   bool sold=false;
   for(uint8 bag=INVENTORY_SLOT_BAG_0;;){
    uint8 begin=bag==INVENTORY_SLOT_BAG_0?INVENTORY_SLOT_ITEM_START:0;
    uint8 end=bag==INVENTORY_SLOT_BAG_0?INVENTORY_SLOT_ITEM_END:static_cast<Bag*>(me->GetItemByPos(INVENTORY_SLOT_BAG_0,bag))->GetBagSize();
    for(uint8 slot=begin;slot<end;++slot)if(Item* item=me->GetItemByPos(bag,slot))if(item->GetProto()->Quality==ITEM_QUALITY_POOR && item->GetProto()->SellPrice){WorldPackets::Item::SellItem packet;packet.vendorGuid=npc->GetObjectGuid();packet.itemGuid=item->GetObjectGuid();packet.count=0;me->GetSession()->HandleSellItemOpcode(packet);sold=true;}
    if(bag==INVENTORY_SLOT_BAG_0)bag=INVENTORY_SLOT_BAG_START;else ++bag;
    while(bag<INVENTORY_SLOT_BAG_END&&!me->GetItemByPos(INVENTORY_SLOT_BAG_0,bag))++bag;
    if(bag>=INVENTORY_SLOT_BAG_END)break;
   }
   if(sold){m_activity="sold_gray_items";return true;}
  }
 }
 return false;
}
bool WorldBotAI::LootNearby(){
 for(Creature* corpse:Nearby(me,25))if(!corpse->IsAlive() && corpse->GetLootRecipient()==me && !corpse->loot.isLooted()){
  if(!me->IsWithinDistInMap(corpse,INTERACTION_DISTANCE)){m_activity="loot_travel";Navigate(corpse->GetPositionX(),corpse->GetPositionY(),corpse->GetPositionZ());return true;}
  WorldPackets::Loot::LootUnit open;open.guid=corpse->GetObjectGuid();me->GetSession()->HandleLootOpcode(open);
  NullClientPacket money(CMSG_LOOT_MONEY);me->GetSession()->HandleLootMoneyOpcode(money);
  for(uint8 slot=0;slot<corpse->loot.items.size();++slot){WorldPackets::Loot::AutoStoreLootItem take;take.lootSlot=slot;me->GetSession()->HandleAutostoreLootItemOpcode(take);}
  WorldPackets::Loot::LootRelease close;close.guid=corpse->GetObjectGuid();me->GetSession()->HandleLootReleaseOpcode(close);m_activity="looting";return true;
 }
 return false;
}
void WorldBotAI::UpdateAI(uint32 /*diff*/){}
void WorldBotAI::UpdateWorld(uint32 diff){
 if(!me)return;
 // Native far-hearth transfers temporarily leave the map but still need ACK.
 if(!me->IsInWorld()){if(m_initialized&&(me->IsBeingTeleportedNear()||me->IsBeingTeleportedFar()))PlayerBotAI::UpdateAI(diff);return;}
 if(m_worldLoginPending.exchange(false))CompleteWorldLogin();
 if(!m_initialized || (botEntry&&botEntry->requestRemoval))return;
 std::deque<std::unique_ptr<WorldPacket>> packets;{std::lock_guard<std::mutex> lock(m_worldPacketMutex);packets.swap(m_worldPackets);}
 for(auto const& packet:packets)HandleWorldPacket(packet.get());
 PlayerBotAI::UpdateAI(diff);
 if(m_routeHearthPending&&TryRouteHearth())return;
 m_tick+=diff;m_save+=diff;m_service+=diff;m_roam+=diff;m_fastElapsed+=diff;
 if(WorldBotTowerTask::Tick(*this,diff))return;
 if(UpdatePKTask(diff))return;
 if(MaintainWorldPvp(diff))return;
 if(me->IsAlive()&&IsInDuel()&&me->m_duel->startTime&&WorldBotModes::Update(*this,diff))return;
 if(me->IsAlive()&&IsInDuel()&&me->m_duel->opponent&&!me->GetVictim())AttackStart(me->m_duel->opponent);
 if(me->IsInCombat()&&!me->GetVictim()&&!IsInDuel())if(Unit* attacker=me->GetAttackerForHelper())if(attacker->GetAffectingPlayer()&&me->IsValidAttackTarget(attacker))AttackStart(attacker);
 bool scope=ExternalPvpScope();if(!scope&&m_externalScope)BattleBotPolicyRuntime::ForgetExternal(*this,m_pvpPolicy);
 m_externalScope=scope;m_externalDuel=IsInDuel();
 if(scope&&m_fastElapsed>=200){m_fastElapsed=0;if(BattleBotPolicyRuntime::TickExternal(*this,m_pvpPolicy,m_externalDuel,true))return;}
 if(m_tick<750)return;
 m_tick=0;
 if(me->IsBeingTeleported())return;
 if(TryRouteHearth())return;
 if(me->m_duel&&!me->m_duel->startTime){if(me->m_duel->startTimer)PrepareDuel();WorldBotModes::Update(*this,diff);return;}
 if(WorldBotModes::Update(*this,diff))return;
 if(m_externalScope&&BattleBotPolicyRuntime::TickExternal(*this,m_pvpPolicy,m_externalDuel,false))return;
 if(PracticeCombatAction())return;
 // AttackStart may be approaching before the first real swing/spell enters
 // combat. Keep this selected encounter instead of letting NPC grinding steal it.
 if(!m_voluntaryPvpTarget.IsEmpty()&&!m_voluntaryPvpReturning&&me->GetVictim()&&me->GetVictim()->GetAffectingPlayer()&&me->GetVictim()->GetAffectingPlayer()->GetObjectGuid()==m_voluntaryPvpTarget){
  m_activity="world_pvp_encounter";
  if(me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))BreakCrowdControlEffects();
  else if(!me->IsNonMeleeSpellCasted())UpdateInCombatAI();
  return;
 }
 if(IsInDuel()){m_leaderGuid=me->GetObjectGuid();if(me->GetVictim()&&me->GetVictim()->IsAlive())UpdateInCombatAI();return;}
 if(me->InBattleGround()){m_activity="battleground_adapter_pending";return;}
 if(me->GetGroup() && me->GetGroup()->GetLeaderGuid()!=me->GetObjectGuid()){
  m_leaderGuid=me->GetGroup()->GetLeaderGuid();PartyBotAI::UpdateAI(1000);return;
 }
 m_leaderGuid=me->GetObjectGuid();

 if(me->IsDead()){
  if(TrySpiritRecovery())return;
  m_activity="corpse_recovery";
  if(!me->HasFlag(PLAYER_FLAGS,PLAYER_FLAGS_GHOST)){NullClientPacket request(CMSG_REPOP_REQUEST);me->GetSession()->HandleRepopRequestOpcode(request);return;}
  if(Corpse* corpse=me->GetCorpse()){
   if(corpse->GetMapId()!=me->GetMapId()){m_activity="blocked_cross_map_corpse";return;}
   // The native handler owns reclaim radius, delay and BG eligibility.
   if(corpse->IsWithinDistInMap(me,CORPSE_RECLAIM_RADIUS,true)){
    WorldPackets::Misc::ReclaimCorpse reclaim;reclaim.guid=corpse->GetObjectGuid();me->GetSession()->HandleReclaimCorpseOpcode(reclaim);
    m_activity=me->IsAlive()?"corpse_native_reclaim_success":"corpse_waiting_native_reclaim";if(me->IsAlive())sLog.Out(LOG_BASIC,LOG_LVL_MINIMAL,"[WorldBot] character %u native corpse reclaim succeeded",me->GetGUIDLow());return;
   }
   PathInfo path(me);path.ExcludeSteepSlopes();path.calculate(corpse->GetPositionX(),corpse->GetPositionY(),corpse->GetPositionZ());
   if(!path.getPath().empty()){
    auto const& end=path.getPath().back();float dx=end.x-corpse->GetPositionX(),dy=end.y-corpse->GetPositionY(),dz=end.z-corpse->GetPositionZ();
    if(WorldBotCorpsePolicy::LegalApproach(unsigned(path.getPathType()),path.getPath().size(),std::sqrt(dx*dx+dy*dy+dz*dz),CORPSE_RECLAIM_RADIUS)){
     m_activity="corpse_approach_native_reclaim_radius";Navigate(end.x,end.y,end.z);return;
    }
   }
   m_routeStall.Failed(WorldTimer::getMSTime(),me->GetMapId(),{{corpse->GetPositionX(),corpse->GetPositionY(),corpse->GetPositionZ()}},{{me->GetPositionX(),me->GetPositionY(),me->GetPositionZ()}});m_activity="corpse_no_legal_reclaim_approach";
  }return;
 }
 m_deadAt=0;
 if(TryAmbientHeal())return;
 RefreshGrowth();if(m_save>=60000){me->SaveToDB();m_save=0;}
 if(me->IsNonMeleeSpellCasted())return;
 if(me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)){BreakCrowdControlEffects();return;}
 Unit* victim=me->GetVictim();
 bool cityDuel=m_profile.purpose=="city_duel";
 if(me->IsInCombat()){
  if(!victim || !victim->IsAlive()){if(Unit* attacker=me->GetAttackerForHelper())AttackStart(attacker);else me->AttackStop();}
  if(me->GetVictim()){m_activity="combat";UpdateInCombatAI();}return;
 }
 if(!cityDuel && me->GetLevel()==60&&m_profile.gearId&&m_profile.purpose!="city_duel"){
  if(RecoverWithRations())return;
  if(NeedsPracticeSupplies()){
   if(ServiceNearby())return;
   if(m_hasService&&m_serviceMap==me->GetMapId()){m_activity="practice_ration_vendor_travel";Navigate(m_serviceX,m_serviceY,m_serviceZ);return;}
   m_activity="practice_ration_vendor_route_needed";return;
  }
 }
 else if(DrinkAndEat()){m_activity="recovering";return;}
 unsigned free=0;
 for(uint8 slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)if(!me->GetItemByPos(INVENTORY_SLOT_BAG_0,slot))++free;
 for(uint8 slot=INVENTORY_SLOT_BAG_START;slot<INVENTORY_SLOT_BAG_END;++slot)if(Bag* bag=dynamic_cast<Bag*>(me->GetItemByPos(INVENTORY_SLOT_BAG_0,slot)))free+=bag->GetFreeSlots();
 m_needsVendor=free<4;
 if(free<4){
  if(ServiceNearby())return;
  SpellEntry const* hearth=sSpellMgr.GetSpellEntry(8690);
  if(m_hasService && m_serviceMap==me->GetMapId() && me->GetHomeBindMap()==m_serviceMap && std::hypot(me->GetHomeBindLocation().x-m_serviceX,me->GetHomeBindLocation().y-m_serviceY)<500 && me->GetDistance(m_serviceX,m_serviceY,m_serviceZ)>1000 && hearth && me->IsSpellReady(hearth)){
   if(Item* stone=StackHearth()){SpellCastTargets targets;targets.setUnitTarget(me);me->CastItemUseSpell(stone,targets);m_activity="hearth_for_inventory";return;}
  }
  if(m_hasService && me->GetMapId()==m_serviceMap){m_activity="vendor_travel";Navigate(m_serviceX,m_serviceY,m_serviceZ);return;}
  m_activity="inventory_full_needs_vendor_route";return;
 }
 if(LootNearby())return;
 if(me->GetVictim()&&!me->GetVictim()->IsAlive())me->AttackStop();
 if(me->GetLevel()==60&&m_profile.gearId){
  if(NativePracticeBuff()){m_activity="native_practice_buff";return;}
 }else UpdateOutOfCombatAI();
 if(me->IsNonMeleeSpellCasted()||m_isBuffing){m_activity="self_buffing";return;}

 if(!m_profile.gearId)for(uint8 slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)if(Item* item=me->GetItemByPos(INVENTORY_SLOT_BAG_0,slot)){
  auto proto=item->GetProto();if((proto->Class!=ITEM_CLASS_WEAPON&&proto->Class!=ITEM_CLASS_ARMOR)||me->CanUseItem(item)!=EQUIP_ERR_OK)continue;
  uint8 dest=me->FindEquipSlot(proto,NULL_SLOT,true);if(dest==NULL_SLOT)continue;Item* old=me->GetItemByPos(INVENTORY_SLOT_BAG_0,dest);
  uint16 checked=0;if(me->CanEquipItem(dest,checked,item,true)!=EQUIP_ERR_OK)continue;
  if(!old||proto->ItemLevel>old->GetProto()->ItemLevel){WorldPackets::Item::AutoEquipItem equip;equip.srcbag=INVENTORY_SLOT_BAG_0;equip.srcslot=slot;me->GetSession()->HandleAutoEquipItemOpcode(equip);m_activity="equip_upgrade_preserving_old_item";return;}
 }

 if(!cityDuel && (m_service>=30000 || m_needsTrainer)){m_service=0;if(ServiceNearby())return;}
 if(!cityDuel && NeedsService()){
  if(!m_hasService){m_activity="needs_service_destination";return;}
  if(me->GetMapId()!=m_serviceMap){m_activity="blocked_cross_map_service";return;}
  m_activity="service_travel";Navigate(m_serviceX,m_serviceY,m_serviceZ);return;
 }

 if(me->GetLevel()>=60){
  if(me->GetMapId()!=m_profile.map){m_activity="city_route_cross_map_blocked";return;}
  if(cityDuel){me->StopMoving();m_activity="city_duel_waiting";return;}
  if(!me->IsStopped()){m_activity="city_roaming";return;}
  if(m_roam<12000){m_activity="city_roam_pause";return;}m_roam=0;
  float cityRoamRadius=m_profile.purpose=="city_duel"?std::min(m_profile.radius,40.0f):std::min(m_profile.radius,40.0f);
  for(unsigned attempt=0;attempt<6;++attempt){float x,y,z;if(!me->GetRandomPoint(m_profile.x,m_profile.y,m_profile.z,cityRoamRadius,x,y,z))continue;
   PathInfo route(me);route.calculate(x,y,z);if(route.getPathType()!=PATHFIND_NORMAL)continue;
   me->GetMotionMaster()->MovePoint(0,x,y,z,MOVE_PATHFINDING|MOVE_EXCLUDE_STEEP_SLOPES);m_activity="city_roaming";return;
  }
  m_activity="city_roam_no_reachable_point";return;
 }
 if(me->GetMapId()!=m_profile.map){m_activity="blocked_cross_map_route";return;}
 if(MaybeStartWorldPvp())return;
 Creature* best=nullptr;float closest=100000;
 for(Creature* candidate:Nearby(me,120)){
  auto info=candidate->GetCreatureInfo();
  if(!candidate->IsAlive() || !candidate->GetLootRecipientGuid().IsEmpty() || candidate->IsInCombat() || candidate->IsFriendlyTo(me) || !me->IsValidAttackTarget(candidate) || candidate->IsTotalImmune() || !candidate->IsVisibleForOrDetect(me,me,false) || info->rank!=0 || info->civilian || candidate->GetUInt32Value(UNIT_NPC_FLAGS)!=0 || (info->static_flags1&CREATURE_STATIC_FLAG_NO_XP) || candidate->GetCreatureType()==CREATURE_TYPE_CRITTER)continue;
  int delta=int(candidate->GetLevel())-int(me->GetLevel());if(delta>1||candidate->GetLevel()<=MaNGOS::XP::GetGrayLevel(me->GetLevel()))continue;
  if(candidate->GetDistance(m_profile.x,m_profile.y,m_profile.z)>m_profile.radius || !me->IsWithinLOSInMap(candidate))continue;
  float distance=me->GetDistance(candidate);if(distance<closest){best=candidate;closest=distance;}
 }
 if(best){m_activity="pulling";AttackStart(best);return;}
 if(me->GetDistance(m_profile.x,m_profile.y,m_profile.z)>m_profile.radius){m_activity="camp_travel";Navigate(m_profile.x,m_profile.y,m_profile.z);return;}
 if(me->IsMoving()){m_activity="camp_patrolling";return;}
 if(m_roam<m_patrolDelay){m_activity="waiting_safe_target";return;}
 m_roam=0;m_patrolDelay=urand(8000,16000);
 for(unsigned attempt=0;attempt<6;++attempt){
  float x,y,z;if(!me->GetRandomPoint(m_profile.x,m_profile.y,m_profile.z,WorldPvpSettings().routeVariation?WorldPvpFlavor::PatrolRadius(me->GetGUID(),m_profile.radius):std::min(70.0f,m_profile.radius*.8f),x,y,z))continue;
  PathInfo path(me);path.ExcludeSteepSlopes();path.calculate(x,y,z);if(path.getPathType()!=PATHFIND_NORMAL)continue;
  bool inside=true;for(auto const& point:path.getPath()){float dx=point.x-m_profile.x,dy=point.y-m_profile.y,dz=point.z-m_profile.z;if(!WorldBotPopulation::CampPoint(std::sqrt(dx*dx+dy*dy+dz*dz),m_profile.radius)){inside=false;break;}}
  if(!inside)continue;
  m_activity="camp_patrolling";Navigate(x,y,z);return;
 }
 m_activity="camp_patrol_needs_legal_route";
}
