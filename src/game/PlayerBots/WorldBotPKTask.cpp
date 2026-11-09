#include "WorldBotAI.h"
#include "WorldBotSupplyPolicy.h"
#include "PKPlainPolicy.h"
#include "Bag.h"
#include "Item.h"
#include "BotMountPolicy.h"
#include "BattleBotTactics.h"
#include "GridMap.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "WorldBotModes.h"
#include "WorldBotPopulation.h"
#include "Player.h"
#include "Pet.h"
#include "Group.h"
#include "Map.h"
#include "PathFinder.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "PlayerBotMgr.h"
#include "Config/Config.h"
#include "Timer.h"
#include "Log.h"
#include "Utilities/Random.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <ctime>
#include <mutex>
#include <map>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>
#include <cerrno>
using PKJson=nlohmann::json;
namespace {
struct Site {uint32 map=0,vendor=0,food=8952;float x=0,y=0,z=0,radius=40,innX=0,innY=0,innZ=0;};
struct Settings {bool enabled=false;uint32 seconds=3600;float nearby=250;Site horde,alliance;std::map<uint32,std::vector<uint32>> cooldowns;std::vector<std::string> profiles;};
Settings SettingsNow(){
 static Settings cached;static uint32 checked=0;static bool loaded=false;static std::mutex mutex;
 std::lock_guard<std::mutex> guard(mutex);uint32 now=WorldTimer::getMSTime();if(loaded&&uint32(now-checked)<5000)return cached;checked=now;loaded=true;
 try{
  std::ifstream file(sConfig.GetStringDefault("WorldBot.PKTaskFile","/workspace/vmangos/_install/etc/world-bot-pk-tasks.json"));if(!file){cached.enabled=false;return cached;}
  PKJson j;file>>j;Settings next;if(j.at("schema_version").get<unsigned>()!=1)throw std::runtime_error("PK task schema");
  next.enabled=j.at("enabled").get<bool>();next.seconds=j.value("duration_seconds",3600u);next.nearby=j.value("nearby_distance",250.0f);
  if(next.seconds<60||next.seconds>86400||!std::isfinite(next.nearby)||next.nearby<40||next.nearby>1000)throw std::runtime_error("PK task limits");
  auto site=[](PKJson const& j){Site s;s.map=j.at("map");s.vendor=j.at("vendor");s.food=j.at("food");auto gate=j.at("gate");auto inn=j.at("inn");s.x=gate.at(0);s.y=gate.at(1);s.z=gate.at(2);s.innX=inn.at(0);s.innY=inn.at(1);s.innZ=inn.at(2);s.radius=j.value("radius",40.0f);
   if(s.map>1||!s.vendor||(s.food!=8952&&s.food!=8950)||s.radius<10||s.radius>80||!MaNGOS::IsValidMapCoord(s.x,s.y,s.z)||!MaNGOS::IsValidMapCoord(s.innX,s.innY,s.innZ))throw std::runtime_error("PK site invalid");
   return s;};
  next.horde=site(j.at("horde"));next.alliance=site(j.at("alliance"));
  if(next.horde.map!=1||next.alliance.map!=0)throw std::runtime_error("PK faction map invalid");
  if(j.count("key_cooldowns"))for(auto it=j["key_cooldowns"].begin();it!=j["key_cooldowns"].end();++it){auto cls=std::stoul(it.key());auto spells=it.value().get<std::vector<uint32>>();if(cls>11||spells.size()>16)throw std::runtime_error("PK cooldown list invalid");next.cooldowns[cls]=spells;}
  next.profiles=j.value("profile_ids",std::vector<std::string>{});
  cached=next;
 }catch(std::exception const& e){cached.enabled=false;sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[WorldBotPK] config rejected: %s",e.what());}
 return cached;
}
std::string StateDir(){return "/workspace/vmangos/run/world-bot-pk-tasks";}
std::string StateFile(WorldBotProfile const& p){return StateDir()+"/"+std::to_string(p.characterGuid)+"-"+std::to_string(p.accountId)+"-"+std::to_string(p.createdAt)+".json";}
struct Lease {uint32 owner=0,peer=0,until=0;};std::map<uint32,Lease> leases;std::mutex leaseMutex,eventMutex;
Lease LeaseFor(uint32 guid){std::lock_guard<std::mutex> lock(leaseMutex);auto it=leases.find(guid);if(it==leases.end())return {};if(int32(WorldTimer::getMSTime()-it->second.until)>=0){leases.erase(it);return {};}return it->second;}
void Release(uint32 guid){std::lock_guard<std::mutex> lock(leaseMutex);auto it=leases.find(guid);if(it==leases.end())return;Lease l=it->second;for(uint32 id:{l.owner,l.peer}){auto p=leases.find(id);if(p!=leases.end()&&p->second.owner==l.owner&&p->second.peer==l.peer&&p->second.until==l.until)leases.erase(p);}}
bool Reserve(uint32 owner,uint32 peer){std::lock_guard<std::mutex> lock(leaseMutex);uint32 now=WorldTimer::getMSTime();for(uint32 id:{owner,peer}){auto it=leases.find(id);if(it!=leases.end()&&int32(now-it->second.until)<0)return false;}Lease l{owner,peer,now+30000};leases[owner]=leases[peer]=l;return true;}
bool Protected(Player* p){
 if(!p||!p->IsInWorld()||!p->IsAlive()||p->InBattleGround()||p->InBattleGroundQueue()||p->IsTaxiFlying()||p->GetTransport()||p->GetTradeData())return true;
 if(p->GetMap()->IsDungeon())return true;
 if(Group* group=p->GetGroup())for(auto const& m:group->GetMemberSlots()){Player* other=ObjectAccessor::FindPlayer(m.guid);if(!other||!other->IsBot())return true;}
 return false;
}
// A navigable hillside is not a sensible spectator/duel gathering point.
// Keep the configured anchor and use only nearby, locally flat native ground.
bool PlainPoint(Player* player,Site const& site,float x,float y,float z){
 if(player->GetMapId()!=site.map||std::hypot(x-site.x,y-site.y)>site.radius*.8f)return false;
 Map* map=player->GetMap();float floor=map->GetHeight(x,y,site.z+6.f,true,12.f);
 if(!PKPlainPolicy::Height(site.z,floor,z))return false;
 for(auto delta:std::vector<std::pair<float,float>>{{2,0},{-2,0},{0,2},{0,-2}})
  if(!PKPlainPolicy::Neighbor(floor,map->GetHeight(x+delta.first,y+delta.second,floor+4.f,true,8.f)))return false;
 auto area=AreaEntry::GetById(player->GetTerrain()->GetAreaId(x,y,floor));
 return area&&(area->Flags&AREA_FLAG_DUEL)&&map->isInLineOfSight(site.x,site.y,site.z+1.5f,x,y,floor+1.5f,true,false);
}
bool PlainRoute(Player* player,Site const& site,float x,float y,float z){
 if(!PlainPoint(player,site,x,y,z))return false;
 PathInfo anchor(player);anchor.ExcludeSteepSlopes();
 if(!anchor.calculate(Movement::Vector3{site.x,site.y,site.z},Movement::Vector3{x,y,z})||anchor.getPathType()!=PATHFIND_NORMAL||anchor.getPath().empty())return false;
 auto end=anchor.getPath().back();if(!PKPlainPolicy::Endpoint(std::hypot(end.x-x,end.y-y),end.z-z))return false;
 PathInfo own(player);own.ExcludeSteepSlopes();
 if(!own.calculate(x,y,z)||own.getPathType()!=PATHFIND_NORMAL||own.getPath().empty())return false;
 end=own.getPath().back();return PKPlainPolicy::Endpoint(std::hypot(end.x-x,end.y-y),end.z-z);
}
struct PKLeg {float targetX=0,targetY=0,targetZ=0,x=0,y=0,z=0;uint32 at=0;};
std::map<uint32,PKLeg> pkLegs;
bool PKNativeLeg(Player* player,float& x,float& y,float& z){
 auto& saved=pkLegs[player->GetGUIDLow()];uint32 now=WorldTimer::getMSTime();
 if(saved.at&&uint32(now-saved.at)<12000&&std::hypot(x-saved.targetX,y-saved.targetY)<1&&std::fabs(z-saved.targetZ)<1&&std::sqrt(player->GetDistanceSqr(saved.x,saved.y,saved.z))>1){x=saved.x;y=saved.y;z=saved.z;return true;}
 float targetX=x,targetY=y,targetZ=z;
 if(std::sqrt(player->GetDistanceSqr(x,y,z))>40){
  PathInfo global(player,8192);global.ExcludeSteepSlopes();global.setUseStrightPath(true);
  if(!global.calculate(x,y,z)||global.getPathType()!=PATHFIND_NORMAL||global.getPath().size()<2)return false;
  bool found=false;
  for(float budget:{40.f,20.f,10.f}){float travelled=0;auto from=global.getPath().front();
   for(unsigned i=1;i<global.getPath().size();++i){auto to=global.getPath()[i];float dx=to.x-from.x,dy=to.y-from.y,dz=to.z-from.z,length=std::sqrt(dx*dx+dy*dy+dz*dz);
    if(length<.01f){from=to;continue;}
    if(travelled+length>=budget||i+1==global.getPath().size()){
     float ratio=std::min(1.f,(budget-travelled)/length),tx=from.x+dx*ratio,ty=from.y+dy*ratio,tz=from.z+dz*ratio;
     float floor=player->GetMap()->GetHeight(tx,ty,tz+8.f,true,16.f);if(!std::isfinite(floor)||std::fabs(floor-tz)>8.f)break;tz=floor;
     PathInfo leg(player);leg.ExcludeSteepSlopes();if(leg.calculate(tx,ty,tz)&&leg.getPathType()==PATHFIND_NORMAL&&!leg.getPath().empty()){auto end=leg.getPath().back();if(PKPlainPolicy::Endpoint(std::hypot(end.x-tx,end.y-ty),end.z-tz)){x=tx;y=ty;z=tz;found=true;}}break;
    }travelled+=length;from=to;
   }if(found)break;
  }if(!found)return false;
 }
 saved={targetX,targetY,targetZ,x,y,z,now};return true;
}
bool Full(Player* p){return p&&p->GetHealth()==p->GetMaxHealth()&&(p->GetMaxPower(POWER_MANA)==0||p->GetPower(POWER_MANA)==p->GetMaxPower(POWER_MANA))&&(!p->GetPet()||(p->GetPet()->IsAlive()&&p->GetPet()->GetHealth()==p->GetPet()->GetMaxHealth()));}
bool DuelArea(Player* p){auto a=AreaEntry::GetById(p->GetAreaId());return a&&(a->Flags&AREA_FLAG_DUEL);}
}
bool WorldBotAI::PKTaskEnabled() const {auto c=SettingsNow();return m_profile.purpose=="city_duel"&&c.enabled&&(c.profiles.empty()||std::find(c.profiles.begin(),c.profiles.end(),m_profile.id)!=c.profiles.end());}
namespace {
SpellEntry const* ConjureSpell(Player* player,bool water){
 if(!player||player->GetClass()!=CLASS_MAGE)return nullptr;
 std::vector<uint32> ids=water?std::vector<uint32>{10140,10139,10138,6127,5506,5505,5504}:std::vector<uint32>{28612,10145,10144,6129,990,597,587};
 for(uint32 id:ids)if(player->HasSpell(id))if(auto spell=sSpellMgr.GetSpellEntry(id))return spell;
 return nullptr;
}
uint32 CreatedItem(SpellEntry const* spell){if(spell)for(unsigned i=0;i<3;++i)if(spell->Effect[i]==SPELL_EFFECT_CREATE_ITEM&&spell->EffectItemType[i])return spell->EffectItemType[i];return 0;}
}
uint32 WorldBotAI::PKFoodItem() const {
 if(me&&PKTaskEnabled()&&me->GetClass()==CLASS_MAGE){auto id=CreatedItem(ConjureSpell(me,false));if(id)return id;}
 return me&&PKTaskEnabled()?(me->GetTeam()==ALLIANCE?SettingsNow().alliance.food:SettingsNow().horde.food):8952;
}
uint32 WorldBotAI::PKWaterItem() const {
 if(me&&PKTaskEnabled()&&me->GetClass()==CLASS_MAGE){auto id=CreatedItem(ConjureSpell(me,true));if(id)return id;}
 return 8766;
}

bool WorldBotAI::PKTaskCanChallenge() const {return PKTaskEnabled()&&m_pkTask.loaded&&m_pkTask.valid&&m_pkTask.arrived&&m_pkTask.remaining>0&&!m_pkTask.rest&&m_pkTask.prepared&&!m_pkTask.supplyPending;}
bool WorldBotAI::PKTaskReady(){
 m_pkTask.blockSpell=0;if(!me||!Full(me)||me->IsNonMeleeSpellCasted()||me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))return false;
 auto cfg=SettingsNow();for(uint32 root:cfg.cooldowns[me->GetClass()])for(auto const& known:me->GetSpellMap()){
  if(known.second.state==PLAYERSPELL_REMOVED||known.second.disabled)continue;
  uint32 first=sSpellMgr.GetFirstSpellInChain(known.first);if((first?first:known.first)!=root)continue;
  auto spell=sSpellMgr.GetSpellEntry(known.first);if(spell&&!me->IsSpellReady(spell)){m_pkTask.blockSpell=known.first;return false;}
 }
 return true;
}
std::string WorldBotAI::PKTaskReport() const {
 if(!PKTaskEnabled())return "{\"enabled\":false}";
 uint32 actualMount=0;if(me)for(auto* aura:me->GetAurasByType(SPELL_AURA_MOUNTED)){actualMount=aura->GetId();break;}
 return PKJson{{"enabled",true},{"type","city_pk"},{"destination",me&&me->GetTeam()==ALLIANCE?"stormwind":"orgrimmar"},{"phase",m_pkTask.phase},{"sequence",m_pkTask.sequence},{"remaining_seconds",m_pkTask.remaining/1000},{"arrived",m_pkTask.arrived},{"entry_used",m_pkTask.entryUsed},{"transfer_pending",m_pkTask.transferPending},{"transfer_attempts",m_pkTask.transferAttempts},{"failed_tasks",m_pkTask.failures},{"failed",m_pkTask.failed},{"target",m_pkTask.target},{"blocking_spell",m_pkTask.blockSpell},{"supply_pending",m_pkTask.supplyPending},{"food_item",PKFoodItem()},{"water_item",PKWaterItem()},{"food_count",me?me->GetItemCount(PKFoodItem(),false):0},{"water_count",me?me->GetItemCount(PKWaterItem(),false):0},{"completed_tasks",m_pkTask.completed},{"duels",m_pkTask.duels},{"invitations",m_pkTask.attempts},{"route_failures",m_pkTask.routeFailures},{"mount_spell",m_pkTask.mountSpell},{"current_mount_spell",actualMount},{"run_speed",me?me->GetSpeed(MOVE_RUN):0},{"mounted",me&&me->IsMounted()},{"mount_reason",m_pkTask.mountReason},{"mount_attempts",m_pkTask.mountAttempts},{"mount_casts",m_pkTask.mountCasts},{"error",m_pkTask.error}}.dump();
}
void WorldBotAI::PKTaskEvent(char const* event,uint32 target){
 std::lock_guard<std::mutex> lock(eventMutex);std::string path="/workspace/vmangos/run/world-bot-pk-events.jsonl";struct stat st{};if(!stat(path.c_str(),&st)&&st.st_size>8*1024*1024)std::rename(path.c_str(),(path+".1").c_str());
 std::ofstream file(path,std::ios::app);file<<PKJson{{"at",uint64(time(nullptr))},{"guid",m_profile.characterGuid},{"sequence",m_pkTask.sequence},{"event",event},{"target",target},{"remaining_seconds",m_pkTask.remaining/1000}}.dump()<<'\n';
 m_pkTask.lastEvent=event;sLog.Out(LOG_BASIC,LOG_LVL_MINIMAL,"[WorldBotPK] guid=%u task=%u event=%s target=%u",m_profile.characterGuid,m_pkTask.sequence,event,target);
}
void WorldBotAI::SavePKTask(){
 if(!m_pkTask.loaded||!m_pkTask.valid)return;
 try{
  if(mkdir(StateDir().c_str(),0700)&&errno!=EEXIST)throw std::runtime_error("PK task directory");
  auto path=StateFile(m_profile),tmp=path+".tmp";
  PKJson j={{"schema_version",1},{"guid",m_profile.characterGuid},{"account",m_profile.accountId},{"created_at",m_profile.createdAt},{"sequence",m_pkTask.sequence},{"remaining_ms",m_pkTask.remaining},{"duration_ms",m_pkTask.duration},{"rest_ms",m_pkTask.rest},{"arrived",m_pkTask.arrived},{"entry_used",m_pkTask.entryUsed},{"transfer_pending",m_pkTask.transferPending},{"transfer_attempts",m_pkTask.transferAttempts},{"failed_tasks",m_pkTask.failures},{"failed",m_pkTask.failed},{"supply_pending",m_pkTask.supplyPending},{"completed",m_pkTask.completed},{"duels",m_pkTask.duels},{"invitations",m_pkTask.attempts}};
  {std::ofstream file(tmp,std::ios::trunc);file<<j.dump(2)<<'\n';if(!file)throw std::runtime_error("PK task write");}
  if(std::rename(tmp.c_str(),path.c_str()))throw std::runtime_error("PK task publish");
  m_pkTask.saveElapsed=0;
 }catch(std::exception const& e){m_pkTask.valid=false;m_pkTask.error=e.what();m_pkTask.phase="state_error";sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[WorldBotPK] state save guid=%u: %s",m_profile.characterGuid,e.what());}
}
WorldBotAI::~WorldBotAI(){Release(m_profile.characterGuid);SavePKTask();}
bool WorldBotAI::UpdatePKTask(uint32 diff){
 if(!PKTaskEnabled())return false;
 auto& t=m_pkTask;auto cfg=SettingsNow();Site site=me->GetTeam()==ALLIANCE?cfg.alliance:cfg.horde;
 if(!t.loaded){
  t.loaded=true;t.duration=t.remaining=cfg.seconds*1000;
  try{std::ifstream file(StateFile(m_profile));if(file){PKJson j;file>>j;
   if(j.at("schema_version")!=1||j.at("guid")!=m_profile.characterGuid||j.at("account")!=m_profile.accountId||j.at("created_at")!=m_profile.createdAt)throw std::runtime_error("PK state identity");
   t.duration=j.at("duration_ms");t.remaining=j.at("remaining_ms");t.sequence=j.at("sequence");t.rest=j.at("rest_ms");t.arrived=j.at("arrived");t.entryUsed=j.at("entry_used");t.completed=j.at("completed");t.duels=j.at("duels");t.attempts=j.at("invitations");t.transferPending=j.value("transfer_pending",false);t.transferAttempts=j.value("transfer_attempts",0u);t.supplyPending=j.value("supply_pending",false);t.failed=j.value("failed",false);t.failures=j.value("failed_tasks",0u);
   if(t.duration<60000||t.duration>86400000||t.remaining>t.duration||t.rest>120000||t.transferAttempts>3)throw std::runtime_error("PK state limits");
   PKTaskEvent("resumed");
  }else{PKTaskEvent("selected");SavePKTask();}}
  catch(std::exception const& e){t.valid=false;t.error=e.what();}
  WorldBotModes::SetDuelArena(me->GetGUIDLow(),site.map,site.x,site.y,site.z,site.radius);
 }
 if(!t.valid){m_activity="pk_task_state_error";return false;}
 bool protectedActivity=Protected(me);
 if(!protectedActivity&&t.arrived&&!t.rest)t.remaining=diff>=t.remaining?0:t.remaining-diff;
 t.elapsed+=diff;t.saveElapsed+=diff;
 if(t.saveElapsed>=30000)SavePKTask();
 if(m_save>=60000&&!me->IsBeingTeleported()&&!me->IsInCombat()&&!me->m_duel){me->SaveToDB();m_save=0;}
 if(protectedActivity){t.elapsed=0;t.phase="suspended";Release(me->GetGUIDLow());t.target=0;return false;}
 if(me->IsBeingTeleported()){t.phase="teleporting";return true;}
 if(me->m_duel){
  pkLegs.erase(me->GetGUIDLow());
  t.elapsed=0;t.phase=me->m_duel->startTime?"dueling":"duel_preparing";
  if(me->m_duel->startTime&&!t.duelSeen){t.duelSeen=true;PKTaskEvent("duel_started",me->m_duel->opponent?me->m_duel->opponent->GetGUIDLow():0);}
  return false;
 }
 if(t.duelSeen){t.duelSeen=false;++t.duels;t.prepared=false;t.target=0;Release(me->GetGUIDLow());t.searchAt=WorldTimer::getMSTime()+urand(4000,10000);PKTaskEvent("duel_finished");SavePKTask();}
 if(me->IsInCombat()){pkLegs.erase(me->GetGUIDLow());t.elapsed=0;return false;}
 if(t.elapsed<750)return true;
 uint32 step=t.elapsed;t.elapsed=0;
 if(me->IsNonMeleeSpellCasted()||me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))return true;
 if(t.rest&&!t.failed){t.rest=0;t.remaining=0;}
 if(!t.remaining&&!t.failed){
  ++t.completed;++t.sequence;t.duration=t.remaining=cfg.seconds*1000;t.rest=0;PKTaskEvent("continued_city_pk");SavePKTask();
 }
 if(!t.remaining&&t.failed){
  Release(me->GetGUIDLow());t.target=0;
  if(!t.rest){if(t.failed)++t.failures;else ++t.completed;t.rest=urand(30000,90000);PKTaskEvent(t.failed?"failed":"completed");SavePKTask();}
  t.rest=step>=t.rest?0:t.rest-step;t.phase="between_tasks";me->StopMoving();m_activity="pk_task_between_tasks";
  if(!t.rest){++t.sequence;t.duration=t.remaining=cfg.seconds*1000;t.arrived=false;t.entryUsed=false;t.prepared=false;t.failed=false;t.transferPending=false;t.transferAttempts=0;t.routeFailures=0;t.routeRetryAt=0;t.error.clear();t.standChosen=false;PKTaskEvent("selected");SavePKTask();}
  return true;
 }
 auto fail=[&](char const* reason){Release(me->GetGUIDLow());t.target=0;t.remaining=0;t.failed=true;t.error=reason;t.phase="failed";PKTaskEvent(reason);SavePKTask();};
 bool inArea=me->GetMapId()==site.map&&me->IsWithinDist3d(site.x,site.y,site.z,site.radius);
 bool nearRegion=me->GetMapId()==site.map&&(me->GetZoneId()==(me->GetTeam()==HORDE?14u:12u)||me->GetZoneId()==(me->GetTeam()==HORDE?1637u:1519u)||me->IsWithinDist3d(site.x,site.y,site.z,1500.f));
 auto mountForTravel=[&](float x,float y,float z){
  t.mountSpell=BotMountPolicy::Select(me->GetGUIDLow(),me->GetRace(),me->GetClass(),me->GetLevel());
  auto reject=[&](char const* reason){t.mountReason=reason;return false;};
  bool outdoors=me->GetTerrain()->IsOutdoors(me->GetPositionX(),me->GetPositionY(),me->GetPositionZ());
  if(me->IsMounted()){
   bool correct=true;for(auto* aura:me->GetAurasByType(SPELL_AURA_MOUNTED))if(!BotMountPolicy::Allows(aura->GetId(),me->GetRace(),me->GetClass(),me->GetLevel()))correct=false;
   if(!correct||!outdoors){me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);t.mountReason=correct?"dismount_indoors":"dismount_wrong_family";}
   else return reject("mounted");
  }
  uint32 now=WorldTimer::getMSTime();if(t.mountRetryAt&&int32(now-t.mountRetryAt)<0)return reject("cast_backoff");
  if(!outdoors)return reject("indoors");
  if(me->IsInCombat()||me->m_duel||me->GetVictim())return reject("combat_or_duel");
  if(me->IsInWater()||me->IsSwimming())return reject("water");
  if(me->HasAuraType(SPELL_AURA_MOD_STEALTH))return reject("stealth");
  if(me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL|UNIT_STATE_CAN_NOT_MOVE)||me->IsNonMeleeSpellCasted())return reject("control_or_cast");
  std::list<Unit*> enemies;me->GetEnemyListInRadiusAround(me,20.0f,enemies);
  for(auto* enemy:enemies)if(enemy->IsAlive()&&me->IsValidAttackTarget(enemy)&&enemy->IsVisibleForOrDetect(me,me,false)&&me->IsWithinLOSInMap(enemy))return reject("enemy_near");
  auto* spell=sSpellMgr.GetSpellEntry(t.mountSpell);if(!spell)return reject("no_mount_spell");
  PathInfo route(me,8192);route.ExcludeSteepSlopes();route.setUseStrightPath(true);if(!route.calculate(x,y,z)||route.getPathType()!=PATHFIND_NORMAL)return reject("no_complete_route");
  float increase=0;for(unsigned i=0;i<3;++i)if(spell->EffectApplyAuraName[i]==SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)increase=std::max(increase,float(spell->EffectBasePoints[i]+1));
  float bonus=std::max(me->GetTotalAuraMultiplier(SPELL_AURA_MOD_MOUNTED_SPEED_ALWAYS),(100.0f+me->GetMaxPositiveAuraModifier(SPELL_AURA_MOD_MOUNTED_SPEED_NOT_STACK))/100.0f);
  float speed=(me->GetSpeed(MOVE_RUN)/std::max(0.01f,me->GetSpeedRate(MOVE_RUN)))*bonus*(100.0f+increase)/100.0f;
  if(int32 cap=me->GetMaxPositiveAuraModifier(SPELL_AURA_USE_NORMAL_MOVEMENT_SPEED))speed=std::min(speed,float(cap));
  speed*=(100.0f+me->GetMaxNegativeAuraModifier(SPELL_AURA_MOD_DECREASE_SPEED))/100.0f;
  auto castTime=BattleBotPolicyRuntime::ActualCastTime(me,spell);
  if(!BattleBotTactics::MountSavesTime(route.Length(),me->GetSpeed(MOVE_RUN),speed,castTime/1000.0f))return reject("no_time_gain");
  if(!me->IsSpellReady(spell)||me->HasGCD(spell))return reject("cooldown_or_gcd");
  uint32 cost=Spell::CalculatePowerCost(spell,me,nullptr,nullptr,false);
  if(spell->powerType==POWER_HEALTH?me->GetHealth()<=cost:me->GetPower(Powers(spell->powerType))<cost)return reject("resource");
  me->StopMoving();StopRationAuras();
  if(me->IsInDisallowedMountForm())me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
  ++t.mountAttempts;auto result=me->CastSpell(me,t.mountSpell,false);
  t.mountReason=result==SPELL_CAST_OK?"cast_ok":"native_reject";t.mountRetryAt=now+(result==SPELL_CAST_OK?5000:30000);
  if(result==SPELL_CAST_OK){++t.mountCasts;m_activity="pk_task_mount_casting";PKTaskEvent("mount_cast",t.mountSpell);return true;}
  return false;
 };
 auto move=[&](float x,float y,float z,bool allowMount=true){
  t.phase="traveling";uint32 now=WorldTimer::getMSTime();
  if(t.routeRetryAt&&int32(now-t.routeRetryAt)<0){m_activity="pk_task_route_backoff";return false;}
  bool fresh=!t.lastMoveAt||uint32(now-t.lastMoveAt)>2000;t.lastMoveAt=now;
  if(fresh||!t.progressAt||!me->IsWithinDist3d(t.progressX,t.progressY,t.progressZ,2.0f)){
   t.progressAt=now;t.progressX=me->GetPositionX();t.progressY=me->GetPositionY();t.progressZ=me->GetPositionZ();
  }else if(uint32(now-t.progressAt)>=20000){
   ++t.routeFailures;t.progressAt=now;t.routeRetryAt=now+5000;me->StopMoving();m_navGoalValid=false;
   m_activity="pk_task_no_actual_progress";PKTaskEvent("route_no_progress");if(t.routeFailures>=5)fail("route_failed");return false;
  }
  if(allowMount&&mountForTravel(x,y,z))return true;
  // Plan through the native polygon corridor, then execute a committed short
  // normal movement leg. Long city walks must not exhaust smooth-point limits.
  if(!PKNativeLeg(me,x,y,z)){m_activity="pk_task_no_legal_travel_leg";t.routeRetryAt=now+5000;return false;}
  bool ok=Navigate(x,y,z);m_activity=ok?(me->IsMounted()?"pk_task_riding":"pk_task_walking"):"pk_task_route_blocked";
  if(!ok){++t.routeFailures;t.routeRetryAt=now+5000;if(t.routeFailures>=5)fail("route_failed");}else t.routeRetryAt=0;
  return ok;
 };
 if(t.transferPending){
  if(me->GetMapId()==site.map&&me->IsWithinDist3d(site.innX,site.innY,site.innZ,25.0f)){
   t.transferPending=false;t.entryUsed=true;t.phase="traveling";PKTaskEvent("inn_transfer_confirmed");SavePKTask();
  }else{
   t.transferPending=false;t.entryUsed=false;t.transferRetryAt=WorldTimer::getMSTime()+10000;
   PKTaskEvent("inn_transfer_unconfirmed");SavePKTask();
   if(t.transferAttempts>=3)fail("transfer_failed");
   return true;
  }
 }
 if(!nearRegion&&!inArea&&!t.entryUsed){
  if(t.transferRetryAt&&int32(WorldTimer::getMSTime()-t.transferRetryAt)<0){t.phase="transfer_backoff";return true;}
  if(t.transferAttempts>=3){fail("transfer_failed");return true;}
  pkLegs.erase(me->GetGUIDLow());t.transferPending=true;++t.transferAttempts;t.phase="teleporting";Release(me->GetGUIDLow());t.target=0;me->StopMoving();me->GetMotionMaster()->Clear();SavePKTask();
  if(!t.valid)return true;
  PKTaskEvent("inn_transfer_requested");
  if(!me->TeleportTo(site.map,site.innX,site.innY,site.innZ,0.0f)){
   t.transferPending=false;t.transferRetryAt=WorldTimer::getMSTime()+10000;t.phase="teleport_refused";SavePKTask();
  }
  return true; // No Map/AI reads after a successful far TeleportTo.
 }
 if(me->GetMapId()!=site.map){fail("wrong_map_after_entry");return true;}
 // Once empty stock starts a supply errand, health/mana regeneration
 // cannot cancel it. Finish both required stacks before leaving the vendor.
 bool mana=me->GetMaxPower(POWER_MANA)>0;
 uint32 food=PKFoodItem(),water=PKWaterItem();
 auto foodProto=sObjectMgr.GetItemPrototype(food),waterProto=sObjectMgr.GetItemPrototype(water);
 uint32 foodStack=foodProto?foodProto->GetMaxStackSize():0,waterStack=waterProto?waterProto->GetMaxStackSize():0;
 uint32 foodCount=me->GetItemCount(food,false),waterCount=me->GetItemCount(water,false);
 bool supply=WorldBotSupplyPolicy::Active(t.supplyPending,mana,foodCount,waterCount,foodStack,waterStack);
 if(supply&&!t.supplyPending){t.supplyPending=true;t.prepared=false;Release(me->GetGUIDLow());t.target=0;PKTaskEvent("supply_started");SavePKTask();}
 if(t.supplyPending&&!supply){t.supplyPending=false;ClearServiceDestination();PKTaskEvent("supply_complete");SavePKTask();if(!t.remaining)return true;}
 if(t.supplyPending){
  if(!foodStack||(mana&&!waterStack)){t.phase="supply_item_invalid";m_activity="pk_task_supply_item_invalid";return true;}
  if(me->GetClass()==CLASS_MAGE){
   if(me->IsMounted())me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
   me->StopMoving();t.phase="conjuring_supplies";m_activity="pk_task_conjuring_supplies";
   uint32 now=WorldTimer::getMSTime();
   if(t.supplyItem){
    uint32 actual=me->GetItemCount(t.supplyItem,false);
    if(actual>t.supplyBefore){PKTaskEvent("conjure_items_verified",t.supplyItem);t.supplyItem=0;t.supplyAttempts=0;me->SaveToDB();}
    else if(uint32(now-t.supplyCastAt)<5000)return true;
    else{PKTaskEvent("conjure_no_items",t.supplyItem);t.supplyItem=0;}
   }
   if(t.supplyCastAt&&uint32(now-t.supplyCastAt)<3000)return true;
   auto spell=ConjureSpell(me,foodCount<foodStack?false:true);
   if(!spell){t.phase="conjure_spell_missing";m_activity="pk_task_conjure_spell_missing";return true;}
   uint32 cost=Spell::CalculatePowerCost(spell,me,nullptr,nullptr,false);
   if(me->GetPower(POWER_MANA)<cost){me->SetStandState(UNIT_STAND_STATE_SIT);RecoverWithRations();t.phase="conjure_mana_recovery";return true;}
   ItemPosCountVec capacity;uint32 noSpace=0;
   auto canStore=me->CanStoreNewItem(NULL_BAG,NULL_SLOT,capacity,CreatedItem(spell),1,&noSpace);
   if(canStore!=EQUIP_ERR_OK){t.phase="conjure_inventory_full";return true;}
   if(!me->IsSpellReady(spell)||me->HasGCD(spell))return true;
   StopRationAuras();t.supplyCastAt=now;t.supplyBefore=me->GetItemCount(CreatedItem(spell),false);
   auto result=me->CastSpell(me,spell,false);
   if(result==SPELL_CAST_OK){t.supplyItem=CreatedItem(spell);PKTaskEvent("conjure_cast",spell->Id);}else PKTaskEvent("conjure_refused",spell->Id);
   return true;
  }
  t.phase="supply_travel";SetServiceDestination(site.map,site.innX,site.innY,site.innZ,site.vendor);
  if(me->IsWithinDist3d(site.innX,site.innY,site.innZ,12.0f)&&me->IsMounted())me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
  if(ServiceNearby()){t.phase="buying_supplies";return true;}
  move(site.innX,site.innY,site.innZ);t.phase="supply_travel";return true;
 }
 // Travel does not demand a full mana bar: class mount costs must not
 // cause an endless mount -> drink -> remount loop. Recover on arrival,
 // or before departure when the owned supplies can treat critical resources.
 bool critical=me->GetHealthPercent()<50||(me->GetMaxPower(POWER_MANA)>0&&me->GetPowerPercent(POWER_MANA)<25);
 if(!Full(me)&&(inArea||critical)){
  if(me->IsMounted())me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
  t.phase="recovering";me->StopMoving();RecoverWithRations();m_activity="pk_task_recovering";
  if(me->GetPet()&&!me->GetPet()->IsAlive())NativePracticeBuff();
  return true;
 }
 StopRationAuras();
 // Nearby existing actors return by native walking; never relocate a saved
 // character merely because a former random point was on a hill.
 bool nearSite=me->GetMapId()==site.map&&me->IsWithinDist3d(site.x,site.y,site.z,cfg.nearby);
 if(t.standChosen&&!PlainPoint(me,site,t.standX,t.standY,t.standZ)){
  t.standChosen=false;t.prepared=false;Release(me->GetGUIDLow());t.target=0;
 }
 bool plainHere=nearSite&&PlainPoint(me,site,me->GetPositionX(),me->GetPositionY(),me->GetPositionZ());
 if(nearSite&&!plainHere){t.prepared=false;Release(me->GetGUIDLow());t.target=0;}
 if(nearSite&&!t.standChosen){
  static std::map<uint32,uint32> retry;uint32 now=WorldTimer::getMSTime();
  if(retry.count(me->GetGUIDLow())&&uint32(now-retry[me->GetGUIDLow()])<5000){t.phase="plain_route_backoff";return true;}retry[me->GetGUIDLow()]=now;
  for(unsigned attempt=0;attempt<24&&!t.standChosen;++attempt){
   float angle=attempt<16?frand(0.f,6.283185f):float(attempt-16)*.785398f;
   float radius=attempt<16?frand(4.f,site.radius*.7f):4.f;
   float x=site.x+std::cos(angle)*radius,y=site.y+std::sin(angle)*radius;
   float z=me->GetMap()->GetHeight(x,y,site.z+6.f,true,12.f);
   if(!PlainRoute(me,site,x,y,z))continue;
   bool free=true;for(auto const& ref:me->GetMap()->GetPlayers())if(Player* other=ref.getSource())if(other!=me&&other->IsWithinDist3d(x,y,z,2.5f)){free=false;break;}
   if(!free)continue;
   t.standX=x;t.standY=y;t.standZ=z;t.standChosen=true;t.routeFailures=0;t.routeRetryAt=0;
  }
  if(!t.standChosen){t.phase="plain_stand_no_native_route";m_activity="pk_task_plain_route_blocked";return true;}
 }
 if(nearSite&&t.standChosen&&(!plainHere||(!t.prepared&&!me->IsWithinDist3d(t.standX,t.standY,t.standZ,2.f)))){
  move(t.standX,t.standY,t.standZ,false);t.phase="returning_to_plain";return true;
 }
 if(!inArea){move(site.x,site.y,site.z);return true;}
 if(me->IsMounted())me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
 if(!DuelArea(me)){t.phase="invalid_duel_area";m_activity="pk_task_duel_area_refused";if(++t.routeFailures>=5)fail("duel_area_failed");return true;}
 if(!t.arrived){t.arrived=true;t.entryUsed=true;PKTaskEvent("arrived");SavePKTask();}
 if(!t.prepared){if(NativePracticeBuff()){t.phase="preparing";m_activity="pk_task_preparing";return true;}t.prepared=true;}
 if(!PKTaskReady()){t.phase="waiting_cooldowns";m_activity="pk_task_waiting_key_cooldown";return true;}
 uint32 now=WorldTimer::getMSTime();Lease lease=LeaseFor(me->GetGUIDLow());
 if(lease.owner&&lease.owner!=me->GetGUIDLow()){me->StopMoving();t.phase="waiting_challenger";m_activity="pk_task_waiting_challenger";return true;}
 if(t.target){
  Player* other=ObjectAccessor::FindPlayer(ObjectGuid(HIGHGUID_PLAYER,t.target));
  bool eligible=other&&other->IsInWorld()&&other->GetMap()==me->GetMap()&&other->IsAlive()&&!other->m_duel&&!other->IsInCombat()&&!Protected(other)&&other->IsVisibleForOrDetect(me,me,false)&&other->IsWithinDist3d(site.x,site.y,site.z,site.radius);
  auto ai=eligible&&other->AI()?dynamic_cast<WorldBotAI*>(other->AI()):nullptr;
  if(!eligible||!PlainPoint(other,site,other->GetPositionX(),other->GetPositionY(),other->GetPositionZ())||!ai||!ai->PKTaskCanChallenge()||!ai->PKTaskReady()||!lease.owner||uint32(now-t.approachAt)>25000){Release(me->GetGUIDLow());t.target=0;t.searchAt=now+urand(5000,10000);t.phase="retry_backoff";return true;}
  if(!me->IsWithinDistInMap(other,8.0f)||!me->IsWithinLOSInMap(other)){move(other->GetPositionX(),other->GetPositionY(),other->GetPositionZ(),false);t.phase="approaching";return true;}
  me->StopMoving();me->SetFacingToObject(other);++t.attempts;
  auto result=me->CastSpell(other,7266,false);PKTaskEvent(result==SPELL_CAST_OK?"invitation_cast":"invitation_refused",t.target);
  t.searchAt=now+urand(15000,25000);t.target=0;Release(me->GetGUIDLow());SavePKTask();return true;
 }
 if(int32(now-t.searchAt)<0){t.phase="observing";m_activity="pk_task_observing";return true;}
 std::vector<Player*> choices;
 for(auto const& pair:me->GetMap()->GetPlayers()){
  Player* other=pair.getSource();if(!other||other==me||!other->IsBot()||other->GetTeam()!=me->GetTeam()||other->m_duel||other->IsInCombat()||Protected(other)||LeaseFor(other->GetGUIDLow()).owner||!me->IsWithinDistInMap(other,80.0f)||!other->IsVisibleForOrDetect(me,me,false)||!me->IsWithinLOSInMap(other)||!other->IsWithinDist3d(site.x,site.y,site.z,site.radius))continue;
  auto ai=other->AI()?dynamic_cast<WorldBotAI*>(other->AI()):nullptr;if(PlainPoint(other,site,other->GetPositionX(),other->GetPositionY(),other->GetPositionZ())&&ai&&ai->PKTaskCanChallenge()&&ai->PKTaskReady())choices.push_back(other);
 }
 if(!choices.empty()){
  Player* other=choices[urand(0,choices.size()-1)];
  if(Reserve(me->GetGUIDLow(),other->GetGUIDLow())){t.target=other->GetGUIDLow();t.approachAt=now;t.phase="approaching";PKTaskEvent("opponent_selected",t.target);return true;}
 }
 me->StopMoving();t.phase="observing";m_activity="pk_task_observing";t.searchAt=now+urand(3000,8000);return true;
}
