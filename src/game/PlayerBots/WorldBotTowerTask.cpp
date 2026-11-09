#include "WorldBotTowerTask.h"
#include "WorldBotTowerPolicy.h"
#include "BotMountPolicy.h"
#include "WorldBotCorpsePolicy.h"
#include "WorldBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Map.h"
#include "Corpse.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "ZoneScriptMgr.h"
#include "ZoneScript.h"
#include "PathFinder.h"
#include "MotionMaster.h"
#include "WorldSession.h"
#include "Packets/Misc.h"
#include "PlayerBotMgr.h"
#include "GridMap.h"
#include "MoveSpline.h"
#include "SpellMgr.h"
#include "Spell.h"
#include "BattleBotTactics.h"
#include "Config/Config.h"
#include "Timer.h"
#include "Log.h"
#include "Utilities/Random.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <mutex>
#include <memory>
#include <map>
#include <set>
#include <ctime>
#include <cstdio>
using TowerJson=nlohmann::json;
namespace {
struct Squad {std::string id;std::vector<uint32> members;uint32 objective=0,until=0,wipeAt=0,selections=0,reclaims=0,regroupAt=0,gatherAt=0;bool patrol=false;std::string phase="gather";};
struct Actor {uint32 tick=0,mountAt=0,buffAt=0,legAt=0,legTower=0,corpseLegAt=0,corpseGuid=0,corpseQueryAt=0;float corpseLegX=0,corpseLegY=0,corpseLegZ=0;float legX=0,legY=0,legZ=0;int corpseType=-1,corpseDefaultType=-1;float corpseLength=0,corpseDefaultLength=0;std::string phase="loading";};
std::recursive_mutex mutex;std::vector<Squad> squads;std::map<uint32,Actor> actors;uint32 statusAt=0;bool loaded=false;std::string loadError;
void Load(){if(loaded)return;loaded=true;try{
 std::ifstream in(sConfig.GetStringDefault("WorldBotTower.File","/workspace/vmangos/_install/etc/world-bot-towers.json"));TowerJson config;in>>config;
 if(config.value("schema_version",0)!=1)throw std::runtime_error("unsupported tower config");std::set<uint32> seen;
 for(auto const& row:config.at("groups")){Squad s;s.id=row.at("id");s.members=row.at("members").get<std::vector<uint32>>();if(s.members.size()!=5)throw std::runtime_error("tower squad requires 5 GUIDs");for(auto id:s.members)if(!id||!seen.insert(id).second)throw std::runtime_error("duplicate tower member");squads.push_back(s);}
 std::ifstream saved(sConfig.GetStringDefault("WorldBotTower.StatusFile","/workspace/vmangos/run/world-bot-towers-status.json"));if(saved){TowerJson snapshot;saved>>snapshot;for(auto const& row:snapshot.value("groups",TowerJson::array()))for(auto& squad:squads)if(row.value("id",std::string())==squad.id){squad.objective=row.value("objective_guid",0u);squad.phase="restored_revalidate";}}
 }catch(std::exception const& e){loadError=e.what();squads.clear();sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[WorldBotTower] config rejected: %s",e.what());}}
Player* Find(uint32 guid){return ObjectAccessor::FindPlayer(ObjectGuid(HIGHGUID_PLAYER,guid));}
struct Goal {float x=0,y=0,z=0,length=0;uint32 at=0,points=0,gridSamples=0;int type=-1,beforeType=-1,defaultFilterType=-1;uint32 defaultFilterPoints=0;float defaultFilterLength=0;float startX=0,startY=0,startZ=0,endX=0,endY=0,endZ=0;bool valid=false,staged=false;float captureX=0,captureY=0,captureZ=0,remaining=0;};
std::map<std::pair<uint32,uint32>,Goal> goals;
Goal NativeGoal(Player* player,GameObject* go,uint32 now){
 auto key=std::make_pair(player->GetGUIDLow(),go->GetGUIDLow());auto old=goals.find(key);if(old!=goals.end()&&uint32(now-old->second.at)<(old->second.valid?30000u:5000u)&&!(old->second.staged&&std::sqrt(player->GetDistanceSqr(old->second.x,old->second.y,old->second.z))<3.f))return old->second;
 Goal result;result.at=now;result.startX=player->GetPositionX();result.startY=player->GetPositionY();result.startZ=player->GetPositionZ();Map* map=player->GetMap();map->ForceLoadGridsAroundPosition(go->GetPositionX(),go->GetPositionY());
 std::vector<Goal> shared;for(auto const& candidate:goals)if(shared.size()<8&&candidate.first.second==go->GetGUIDLow()&&candidate.first.first!=player->GetGUIDLow()&&candidate.second.valid&&!candidate.second.staged)shared.push_back(candidate.second);
 for(unsigned attempt=0;attempt<28+shared.size();++attempt){float angle=float(player->GetGUIDLow()%17)*.37f+attempt*.785398f;float captureRadius=go->GetGOInfo()->capturePoint.radius;float radius=attempt<4?20.f:attempt<8?25.f:attempt<12?12.f:attempt<16?40.f:attempt<20?55.f:std::max(5.f,captureRadius-15.f);
  float x=go->GetPositionX()+std::cos(angle)*radius,y=go->GetPositionY()+std::sin(angle)*radius;
  if(attempt>=28){x=shared[attempt-28].x;y=shared[attempt-28].y;}
  float z=map->GetHeight(x,y,go->GetPositionZ()+6.f,true,captureRadius);float dzCapture=z-go->GetPositionZ();if(!std::isfinite(z)||!WorldBotTowerPolicy::WithinSphere(x-go->GetPositionX(),y-go->GetPositionY(),dzCapture,captureRadius-5.f))continue;
  std::unique_ptr<PathInfo> route(new PathInfo(player,8192));route->ExcludeSteepSlopes();route->setUseStrightPath(true);bool ok=route->calculate(x,y,z);result.type=route->getPathType();result.points=route->getPath().size();result.x=result.captureX=x;result.y=result.captureY=y;result.z=result.captureZ=z;result.length=route->getPath().empty()?0.f:route->Length();
  if(!result.gridSamples&&result.type!=PATHFIND_NORMAL){
   result.beforeType=result.type;
   float dx=x-player->GetPositionX(),dy=y-player->GetPositionY(),distance=std::hypot(dx,dy);
   // Load a bounded native corridor, without moving the actor or accepting a
   // partial route. Destination-only loading can leave intervening mesh gaps.
   if(distance>60&&distance<=2500){unsigned steps=std::min(8u,std::max(1u,unsigned(std::ceil(distance/300.f))));float nx=distance>0?-dy/distance:0,ny=distance>0?dx/distance:0;
    for(unsigned step=1;step<=steps;++step)for(float side:{-200.f,0.f,200.f}){float fraction=float(step)/steps;map->ForceLoadGridsAroundPosition(player->GetPositionX()+dx*fraction+nx*side,player->GetPositionY()+dy*fraction+ny*side);++result.gridSamples;}
    route.reset(new PathInfo(player,8192));route->ExcludeSteepSlopes();route->setUseStrightPath(true);ok=route->calculate(x,y,z);result.type=route->getPathType();result.points=route->getPath().size();result.length=route->getPath().empty()?0.f:route->Length();
   }
  }
  if(!route->getPath().empty()){auto end=route->getPath().back();result.endX=end.x;result.endY=end.y;result.endZ=end.z;}
  if(ok&&result.type==PATHFIND_INCOMPLETE&&!route->getPath().empty()){
   auto endpoint=route->getPath().back();float before=std::sqrt(player->GetDistanceSqr(x,y,z));float progress=std::sqrt(player->GetDistanceSqr(endpoint.x,endpoint.y,endpoint.z));
   float dx=x-endpoint.x,dy=y-endpoint.y,dz=z-endpoint.z;float remaining=std::sqrt(dx*dx+dy*dy+dz*dz);
   if(WorldBotTowerPolicy::UsefulStage(progress,before,remaining)){
    PathInfo stage(player,8192);stage.ExcludeSteepSlopes();stage.setUseStrightPath(true);
    if(stage.calculate(endpoint.x,endpoint.y,endpoint.z)&&stage.getPathType()==PATHFIND_NORMAL&&!stage.getPath().empty()){
     auto checked=stage.getPath().back();
     if(std::hypot(checked.x-endpoint.x,checked.y-endpoint.y)<=2&&std::fabs(checked.z-endpoint.z)<=2){result.x=checked.x;result.y=checked.y;result.z=checked.z;result.remaining=remaining;result.length=stage.Length()+remaining;result.valid=true;result.staged=true;break;}
    }
   }
  }
  if(!ok||result.type!=PATHFIND_NORMAL||route->getPath().empty())continue;auto end=route->getPath().back();
  if(std::hypot(end.x-x,end.y-y)>2||std::fabs(end.z-z)>2)continue;
  result.valid=true;break;
 }
 if(!result.valid){PathInfo comparison(player,8192);comparison.setUseStrightPath(true);comparison.calculate(result.x,result.y,result.z);result.defaultFilterType=comparison.getPathType();result.defaultFilterPoints=comparison.getPath().size();result.defaultFilterLength=comparison.getPath().empty()?0.f:comparison.Length();}
 goals[key]=result;return result;
}
struct LegTrace {int globalType=-1,localType=-1;unsigned attempts=0;float requested=0,interpolatedZ=0,projectedZ=0,x=0,y=0,z=0,endX=0,endY=0,endZ=0;};
std::map<uint32,LegTrace> legTraces;
bool NativeTravelLeg(Player* player,float& x,float& y,float& z){
 auto& trace=legTraces[player->GetGUIDLow()];trace=LegTrace();
 if(std::sqrt(player->GetDistanceSqr(x,y,z))<=10)return true;
 PathInfo global(player,8192);global.ExcludeSteepSlopes();global.setUseStrightPath(true);
 bool calculated=global.calculate(x,y,z);trace.globalType=global.getPathType();
 if(!calculated||global.getPathType()!=PATHFIND_NORMAL||global.getPath().size()<2)return false;
 // Corner heights describe a navigation corridor, not a linear terrain slope.
 // Try shorter native legs when a longer interpolated point cannot be reached.
 for(float budget:{60.f,30.f,15.f,7.f}){
  float walked=0;auto from=global.getPath().front();
  for(unsigned i=1;i<global.getPath().size();++i){auto to=global.getPath()[i];float dx=to.x-from.x,dy=to.y-from.y,dz=to.z-from.z;float length=std::sqrt(dx*dx+dy*dy+dz*dz);
   if(length<=.01f){from=to;continue;}
   float remaining=budget-walked;
   if(length>=remaining||i+1==global.getPath().size()){
    float ratio=std::min(1.f,remaining/length);float tx=from.x+dx*ratio,ty=from.y+dy*ratio,tz=from.z+dz*ratio;
    trace.requested=budget;trace.x=tx;trace.y=ty;trace.interpolatedZ=tz;++trace.attempts;
    // Recast corner heights cannot be linearly interpolated across terrain.
    // Resolve a nearby native floor, then prove the whole local walk normally.
    float floor=player->GetMap()->GetHeight(tx,ty,tz+8.f,true,16.f);trace.projectedZ=floor;
    if(!std::isfinite(floor)||std::fabs(floor-tz)>8.f)break;
    tz=floor;trace.z=tz;
    PathInfo leg(player);leg.ExcludeSteepSlopes();bool ok=leg.calculate(tx,ty,tz);trace.localType=leg.getPathType();
    if(ok&&leg.getPathType()==PATHFIND_NORMAL&&!leg.getPath().empty()){auto end=leg.getPath().back();trace.endX=end.x;trace.endY=end.y;trace.endZ=end.z;
     if(std::hypot(end.x-tx,end.y-ty)<=2&&std::fabs(end.z-tz)<=2){x=tx;y=ty;z=tz;return true;}
    }
    break;
   }
   walked+=length;from=to;
  }
 }
 return false;
}
void Status(uint32 now){if(statusAt&&uint32(now-statusAt)<2000)return;statusAt=now;
 TowerJson out={{"schema_version",1},{"at",time(nullptr)},{"error",loadError},{"groups",TowerJson::array()}};
 for(auto const& s:squads){TowerJson row={{"id",s.id},{"objective_guid",s.objective},{"phase",s.phase},{"patrol",s.patrol},{"selections",s.selections},{"reclaims",s.reclaims},{"gatherAt",s.gatherAt},{"wipeAt",s.wipeAt},{"members",TowerJson::array()}};
 for(auto id:s.members){TowerJson a={{"guid",id},{"phase",actors[id].phase},{"corpse_type",actors[id].corpseType},{"corpse_default_type",actors[id].corpseDefaultType},{"corpse_length",actors[id].corpseLength},{"corpse_default_length",actors[id].corpseDefaultLength}};if(auto p=Find(id)){a["name"]=p->GetName();a["online"]=p->IsInWorld();a["alive"]=p->IsAlive();a["map"]=p->GetMapId();a["x"]=p->GetPositionX();a["y"]=p->GetPositionY();a["z"]=p->GetPositionZ();a["hp"]=p->GetHealthPercent();a["mana"]=p->GetPowerPercent(POWER_MANA);a["combat"]=p->IsInCombat();a["moving"]=p->IsMoving();a["capture_eligible"]=p->IsOutdoorPvPActive();a["ghost"]=p->HasFlag(PLAYER_FLAGS,PLAYER_FLAGS_GHOST);a["pvp_desired"]=p->IsPvPDesired();a["motion"]=uint32(p->GetMotionMaster()->GetCurrentMovementGeneratorType());a["mounted"]=p->IsMounted();if(auto ai=dynamic_cast<WorldBotAI*>(p->AI())){a["role"]=uint32(ai->GetRole());a["preset_verified"]=ai->PresetVerified();a["nav_type"]=ai->NavDiagnostic().type;a["nav_points"]=ai->NavDiagnostic().points;}a["group"]=p->GetGroup()?p->GetGroup()->GetLeaderGuid().GetCounter():0;}row["members"].push_back(a);}out["groups"].push_back(row);}
 auto* outdoor=dynamic_cast<OutdoorPvP*>(sZoneScriptMgr.GetZoneScript(139));out["towers"]=TowerJson::array();if(outdoor)for(auto const& cp:outdoor->CapturePoints()){TowerJson t={{"guid",cp.first},{"value",cp.second->CaptureValue()},{"maximum",cp.second->CaptureMaximum()},{"alliance_active",cp.second->CaptureActive(0)},{"horde_active",cp.second->CaptureActive(1)}};if(auto go=cp.second->CaptureObject()){t["entry"]=go->GetEntry();t["x"]=go->GetPositionX();t["y"]=go->GetPositionY();t["z"]=go->GetPositionZ();}out["towers"].push_back(t);}
 out["routes"]=TowerJson::array();for(auto const& row:goals)out["routes"].push_back({{"actor",row.first.first},{"tower",row.first.second},{"valid",row.second.valid},{"staged",row.second.staged},{"capture_x",row.second.captureX},{"capture_y",row.second.captureY},{"capture_z",row.second.captureZ},{"remaining_estimate",row.second.remaining},{"type",row.second.type},{"before_type",row.second.beforeType},{"default_filter_type",row.second.defaultFilterType},{"default_filter_points",row.second.defaultFilterPoints},{"default_filter_length",row.second.defaultFilterLength},{"grid_samples",row.second.gridSamples},{"start_x",row.second.startX},{"start_y",row.second.startY},{"start_z",row.second.startZ},{"end_x",row.second.endX},{"end_y",row.second.endY},{"end_z",row.second.endZ},{"points",row.second.points},{"length",row.second.length},{"x",row.second.x},{"y",row.second.y},{"z",row.second.z}});
 out["legs"]=TowerJson::array();for(auto const& row:legTraces)out["legs"].push_back({{"actor",row.first},{"global_type",row.second.globalType},{"local_type",row.second.localType},{"attempts",row.second.attempts},{"requested",row.second.requested},{"interpolated_z",row.second.interpolatedZ},{"projected_z",row.second.projectedZ},{"x",row.second.x},{"y",row.second.y},{"z",row.second.z},{"end_x",row.second.endX},{"end_y",row.second.endY},{"end_z",row.second.endZ}});
 std::string path=sConfig.GetStringDefault("WorldBotTower.StatusFile","/workspace/vmangos/run/world-bot-towers-status.json");std::ofstream f(path+".tmp");f<<out.dump(2)<<'\n';f.close();if(f)std::rename((path+".tmp").c_str(),path.c_str());}
}
bool WorldBotTowerTask::Tick(WorldBotAI& ai,uint32 diff){
 if(ai.m_profile.purpose!="plaguelands_towers")return false;
 std::lock_guard<std::recursive_mutex> lock(mutex);Load();Player* p=ai.me;uint32 now=WorldTimer::getMSTime();auto& actor=actors[p->GetGUIDLow()];
 auto pause=[&](char const* reason){actor.phase=reason;ai.m_activity=std::string("tower_")+reason;Status(now);return true;};
 if(!sConfig.GetBoolDefault("WorldBotTower.Enabled",false))return pause("disabled");
 Squad* squad=nullptr;for(auto& s:squads)if(std::find(s.members.begin(),s.members.end(),p->GetGUIDLow())!=s.members.end())squad=&s;
 if(!squad)return pause("roster_missing");
 if(p->InBattleGround()||p->InBattleGroundQueue()||p->GetMap()->IsDungeon()||p->m_duel||p->GetTradeData()||p->GetGroupInvite()){actor.phase="external_activity";Status(now);return false;}
 if(p->GetGroup())for(auto const& member:p->GetGroup()->GetMemberSlots())if(std::find(squad->members.begin(),squad->members.end(),member.guid.GetCounter())==squad->members.end()){actor.phase="external_group";Status(now);return false;}
 actor.tick+=diff;if(actor.tick<500)return true;actor.tick=0;
 if(p->IsBeingTeleported())return pause("teleport_wait");if(p->GetMapId()!=0)return pause("wrong_continent_no_teleport");
 std::vector<Player*> members;for(auto id:squad->members)if(auto member=Find(id))if(member->IsInWorld()&&member->GetMap()==p->GetMap())members.push_back(member);
 Player* primary=Find(squad->members.front());
 Player* leader=primary&&primary->IsInWorld()&&primary->GetMap()==p->GetMap()?primary:nullptr;
 if(!leader)return pause("leader_offline"); // Never create competing fallback groups during batched login.
 for(auto* member:members)if(!member->IsBot()||member->GetTeam()!=p->GetTeam())return pause("invalid_roster");
 auto dedicated=[&](Group* group){if(!group)return true;for(auto const& slot:group->GetMemberSlots())if(std::find(squad->members.begin(),squad->members.end(),slot.guid.GetCounter())==squad->members.end())return false;return true;};
 Group* canonical=nullptr;unsigned largest=0;
 for(auto* member:members)if(Group* group=member->GetGroup()){
  if(!dedicated(group))return pause("external_group");
  unsigned size=group->GetMembersCount();if(!canonical||size>largest||(size==largest&&group->GetId()<canonical->GetId())){canonical=group;largest=size;}
 }
 bool split=false;for(auto* member:members)if(member->GetGroup()!=canonical)split=true;
 if(!canonical||split){
  if(p!=leader)return pause("group_join_wait");
  for(auto* member:members)if(member->IsInCombat()||member->IsNonMeleeSpellCasted()||member->GetGroupInvite()||member->m_duel||member->GetTradeData()||member->InBattleGround()||member->InBattleGroundQueue())return pause("group_reconcile_protected");
  if(!canonical){canonical=new Group;if(!canonical->Create(leader->GetObjectGuid(),leader->GetName())){delete canonical;return pause("group_create_failed");}sObjectMgr.AddGroup(canonical);}
  // Native removal may disband a small source group. Re-read each member's
  // current group after every operation; never retain a source-group pointer.
  for(auto* member:members)if(member->GetGroup()!=canonical){
   if(Group* source=member->GetGroup())source->RemoveMember(member->GetObjectGuid(),0);
   if(!member->GetGroup())canonical->AddMember(member->GetObjectGuid(),member->GetName());
  }
 }
 if(!canonical||p->GetGroup()!=canonical)return pause("group_reconcile_wait");
 if(canonical->GetLeaderGuid()!=leader->GetObjectGuid()){
  bool safe=true;for(auto* member:members)if(member->IsInCombat()||member->IsNonMeleeSpellCasted())safe=false;
  if(safe&&p==leader)canonical->ChangeLeader(leader->GetObjectGuid());
 }
 ai.m_leaderGuid=leader->GetObjectGuid();p->SetPvPDesired(true);p->UpdatePvP(true);
 bool anyAlive=false,allCorpseReady=true;unsigned dead=0;for(auto member:members){if(member->IsAlive())anyAlive=true;else {++dead;auto corpse=member->GetCorpse();if(!member->HasFlag(PLAYER_FLAGS,PLAYER_FLAGS_GHOST)||!corpse||!corpse->IsWithinDistInMap(member,CORPSE_RECLAIM_RADIUS,true)||corpse->GetGhostTime()+member->GetCorpseReclaimDelay(corpse->GetType()==CORPSE_RESURRECTABLE_PVP)>time(nullptr))allCorpseReady=false;}}
 if(dead&&dead==members.size()&&!squad->wipeAt)squad->wipeAt=now;if(!dead)squad->wipeAt=0;
 if(p->IsDead()){
  if(!ai.m_deadAt)ai.m_deadAt=now;
  ai.m_externalScope=false;
  if(anyAlive&&(!squad->wipeAt)){if(!ai.m_deadAt)ai.m_deadAt=now;if(uint32(now-ai.m_deadAt)<15000)return pause("await_safe_healer_resurrection");}
  if(!p->HasFlag(PLAYER_FLAGS,PLAYER_FLAGS_GHOST)){NullClientPacket request(CMSG_REPOP_REQUEST);p->GetSession()->HandleRepopRequestOpcode(request);return pause("native_release");}
  Corpse* corpse=p->GetCorpse();if(!corpse)return pause(uint32(now-ai.m_deadAt)>180000?"failed_missing_corpse_group_degraded":"missing_corpse");if(corpse->GetMapId()!=p->GetMapId())return pause("corpse_cross_map");
  bool inside=corpse->IsWithinDistInMap(p,CORPSE_RECLAIM_RADIUS,true),delay=corpse->GetGhostTime()+p->GetCorpseReclaimDelay(corpse->GetType()==CORPSE_RESURRECTABLE_PVP)<=time(nullptr);
  if(WorldBotTowerPolicy::Reclaim(inside,delay,WorldBotTowerPolicy::BarrierReady(anyAlive,allCorpseReady,members.size()),squad->wipeAt?uint32(now-squad->wipeAt):90000)){
   WorldPackets::Misc::ReclaimCorpse request;request.guid=corpse->GetObjectGuid();p->GetSession()->HandleReclaimCorpseOpcode(request);if(p->IsAlive()){actor.corpseLegAt=0;++squad->reclaims;squad->regroupAt=now;}return pause(p->IsAlive()?"native_reclaim_success":"native_reclaim_wait");}
  if(inside)return pause("corpse_squad_barrier");
  if(actor.corpseGuid==corpse->GetGUIDLow()&&actor.corpseLegAt&&uint32(now-actor.corpseLegAt)<15000&&!WorldBotTowerPolicy::Arrived(std::sqrt(p->GetDistanceSqr(actor.corpseLegX,actor.corpseLegY,actor.corpseLegZ)))){ai.Navigate(actor.corpseLegX,actor.corpseLegY,actor.corpseLegZ);return pause("corpse_travel");}
  if(actor.corpseQueryAt&&uint32(now-actor.corpseQueryAt)<5000)return pause("corpse_route_retry_backoff");actor.corpseQueryAt=now;
  bool found=false;Movement::Vector3 end;float best=1e9;
  // A partial path toward an isolated corpse can end on ordinary ground already
  // inside the native reclaim sphere. Replan normally to that endpoint; never
  // execute the incomplete corridor itself.
  PathInfo body(p,8192);body.ExcludeSteepSlopes();body.setUseStrightPath(true);body.calculate(corpse->GetPositionX(),corpse->GetPositionY(),corpse->GetPositionZ());
  if(!body.getPath().empty()){
   auto point=body.getPath().back();float dx=point.x-corpse->GetPositionX(),dy=point.y-corpse->GetPositionY(),dz=point.z-corpse->GetPositionZ();
   if(WorldBotTowerPolicy::WithinSphere(dx,dy,dz,CORPSE_RECLAIM_RADIUS-1)){
    PathInfo reachable(p,8192);reachable.ExcludeSteepSlopes();reachable.setUseStrightPath(true);
    if(reachable.calculate(point.x,point.y,point.z)&&reachable.getPathType()==PATHFIND_NORMAL&&!reachable.getPath().empty()){
     auto checked=reachable.getPath().back();dx=checked.x-corpse->GetPositionX();dy=checked.y-corpse->GetPositionY();dz=checked.z-corpse->GetPositionZ();
     if(WorldBotTowerPolicy::WithinSphere(dx,dy,dz,CORPSE_RECLAIM_RADIUS-1)){float tx=checked.x,ty=checked.y,tz=checked.z;if(NativeTravelLeg(p,tx,ty,tz)){end=checked;found=true;}}
    }
   }
  }
  p->GetMap()->ForceLoadGridsAroundPosition(corpse->GetPositionX(),corpse->GetPositionY());
  for(unsigned attempt=0;attempt<49&&!found;++attempt){float angle=attempt*.785398f;float radius=attempt==0?0:attempt<=8?5.f:attempt<=16?10.f:attempt<=24?15.f:attempt<=32?20.f:attempt<=40?25.f:30.f;
   float x=corpse->GetPositionX()+std::cos(angle)*radius,y=corpse->GetPositionY()+std::sin(angle)*radius;
   float z=p->GetMap()->GetHeight(x,y,corpse->GetPositionZ()+5.f,true,CORPSE_RECLAIM_RADIUS);
   float dx=x-corpse->GetPositionX(),dy=y-corpse->GetPositionY(),dz=z-corpse->GetPositionZ();
   if(!std::isfinite(z)||!WorldBotTowerPolicy::WithinSphere(dx,dy,dz,CORPSE_RECLAIM_RADIUS-1))continue;
   PathInfo route(p,8192);route.ExcludeSteepSlopes();route.setUseStrightPath(true);route.calculate(x,y,z);actor.corpseType=route.getPathType();actor.corpseLength=route.getPath().empty()?0.f:route.Length();
   if(route.getPathType()!=PATHFIND_NORMAL||route.getPath().empty())continue;auto point=route.getPath().back();dx=point.x-corpse->GetPositionX();dy=point.y-corpse->GetPositionY();dz=point.z-corpse->GetPositionZ();
   if(!WorldBotTowerPolicy::WithinSphere(dx,dy,dz,CORPSE_RECLAIM_RADIUS-1))continue;
   if(route.Length()<best){float tx=point.x,ty=point.y,tz=point.z;if(!NativeTravelLeg(p,tx,ty,tz))continue;best=route.Length();end=point;found=true;break;}
  }
  if(!found)return pause("corpse_path_outside_reclaim_radius");
  float x=end.x,y=end.y,z=end.z;
  if(actor.corpseGuid==corpse->GetGUIDLow()&&actor.corpseLegAt&&uint32(now-actor.corpseLegAt)<15000&&!WorldBotTowerPolicy::Arrived(std::sqrt(p->GetDistanceSqr(actor.corpseLegX,actor.corpseLegY,actor.corpseLegZ)))){x=actor.corpseLegX;y=actor.corpseLegY;z=actor.corpseLegZ;}
  else {if(!NativeTravelLeg(p,x,y,z))return pause("corpse_no_legal_short_leg");actor.corpseGuid=corpse->GetGUIDLow();actor.corpseLegAt=now;actor.corpseLegX=x;actor.corpseLegY=y;actor.corpseLegZ=z;}
  ai.Navigate(x,y,z);return pause("corpse_travel");
 }
 ai.m_deadAt=0;
 if(p->GetGroup()!=leader->GetGroup())return pause("split_group_wait");
 if(squad->regroupAt&&uint32(now-squad->regroupAt)<30000&&!p->IsInCombat()){
  if(p!=leader&&leader->IsAlive()&&p->GetDistance(leader)>20){ai.Navigate(leader->GetPositionX(),leader->GetPositionY(),leader->GetPositionZ());return pause("post_reclaim_gather");}
  if(p==leader&&dead)return pause("post_reclaim_wait_members");
 }

 if(p->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)){ai.BreakCrowdControlEffects();return pause("controlled");}
 Unit* target=p->GetVictim();if(target&&(!target->IsAlive()||!p->IsValidAttackTarget(target)||!target->IsVisibleForOrDetect(p,p,false)||p->GetDistance(target)>60)){p->AttackStop();target=nullptr;}
 if(!target){std::list<Unit*> enemies;p->GetEnemyListInRadiusAround(p,45,enemies);float best=46;for(auto enemy:enemies)if([&](){if(enemy->GetAffectingPlayer())return true;Unit* victim=enemy->GetVictim();Player* owner=victim?victim->GetCharmerOrOwnerPlayerOrPlayerItself():nullptr;return enemy->IsInCombat()&&owner&&owner->GetGroup()==p->GetGroup();}()&&WorldBotTowerPolicy::Engage(enemy->IsAlive(),p->IsValidAttackTarget(enemy),enemy->IsVisibleForOrDetect(p,p,false),p->IsWithinLOSInMap(enemy),p->GetDistance(enemy))){float d=p->GetDistance(enemy);if(d<best){best=d;target=enemy;}}if(target)ai.AttackStart(target);}
 bool scope=ai.ExternalPvpScope();ai.m_externalScope=scope;ai.m_externalDuel=false;
 if(target||p->IsInCombat()){
  actor.legAt=0;p->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
  if(scope&&BattleBotPolicyRuntime::TickExternal(ai,ai.m_pvpPolicy,false,true))return pause("pvp_policy");
  if(!p->IsNonMeleeSpellCasted()){if(scope&&BattleBotPolicyRuntime::TickExternal(ai,ai.m_pvpPolicy,false,false))return pause("pvp_policy");ai.UpdateInCombatAI();}
  return pause("combat");
 }
 if(p->IsNonMeleeSpellCasted())return pause("casting");
 ai.SummonPetIfNeeded();if(p->IsNonMeleeSpellCasted())return pause("pet_summoning");
 if(!actor.buffAt||uint32(now-actor.buffAt)>5000){actor.buffAt=now;if(ai.NativePracticeBuff())return pause("native_self_buffs");}
 if(ai.GetRole()==ROLE_HEALER&&ai.TryResurrectParty())return pause("healer_resurrection");
 if(ai.FindAndHealInjuredAlly(65,70))return pause("group_healing");
 if((ai.HasRationAura()||p->GetHealthPercent()<85||(p->GetMaxPower(POWER_MANA)&&p->GetPowerPercent(POWER_MANA)<70))&&ai.RecoverWithRations())return pause("recovering");
 bool gathered=members.size()==5;for(auto member:members)if(!member->IsAlive()||member->GetDistance(leader)>60||member->GetHealthPercent()<70||(member->GetMaxPower(POWER_MANA)&&member->GetPowerPercent(POWER_MANA)<40))gathered=false;
 if(!gathered){if(!squad->gatherAt)squad->gatherAt=now;
  if(p!=leader&&leader->IsAlive()&&p->GetDistance(leader)>20){ai.Navigate(leader->GetPositionX(),leader->GetPositionY(),leader->GetPositionZ());return pause("squad_gather_travel");}
  if(WorldBotTowerPolicy::GatherWait(gathered,uint32(now-squad->gatherAt)))return pause("squad_gather_wait");
 }else squad->gatherAt=0;
 auto* outdoor=dynamic_cast<OutdoorPvP*>(sZoneScriptMgr.GetZoneScript(139));if(!outdoor)return pause("capture_controller_unavailable");
 auto const& points=outdoor->CapturePoints();if(points.size()!=4)return pause("capture_points_incomplete");for(auto const& cp:points)if(!cp.second->CaptureObject())return pause("capture_points_unloaded");OPvPCapturePoint* objective=nullptr;
 if(squad->objective){auto it=points.find(squad->objective);if(it!=points.end())objective=it->second;}
 bool allFull=true;for(auto const& cp:points)if(!WorldBotTowerPolicy::Full(cp.second->CaptureValue(),cp.second->CaptureMaximum(),p->GetTeam()==ALLIANCE))allFull=false;
 auto decision=WorldBotTowerPolicy::Decide(objective,objective&&WorldBotTowerPolicy::Full(objective->CaptureValue(),objective->CaptureMaximum(),p->GetTeam()==ALLIANCE),allFull,int32(now-squad->until)>=0);
 if(decision==WorldBotTowerPolicy::Action::SelectCapture||decision==WorldBotTowerPolicy::Action::SelectGuardOrPatrol){
  float best=1e9;OPvPCapturePoint* chosen=nullptr;uint32 guid=0;
  bool patrol=allFull&&urand(0,1);
  for(auto const& cp:points){if(patrol&&cp.first==squad->objective)continue;auto* go=cp.second->CaptureObject();if(!go)continue;if(!allFull&&WorldBotTowerPolicy::Full(cp.second->CaptureValue(),cp.second->CaptureMaximum(),p->GetTeam()==ALLIANCE))continue;
   auto route=NativeGoal(leader,go,now);if(!route.valid)continue;
   unsigned reserved=0;for(auto const& other:squads)if(&other!=squad&&other.objective==cp.first){auto owner=Find(other.members.front());if(owner&&owner->GetTeam()==p->GetTeam())++reserved;}
   float score=WorldBotTowerPolicy::ObjectiveCost(route.length,reserved,leader->GetGUIDLow()+cp.first+(allFull?now/90000:0));if(score<best){best=score;chosen=cp.second;guid=cp.first;}
  }
  if(!chosen){if(objective){auto* old=objective->CaptureObject();if(old){auto footing=NativeGoal(p,old,now);if(footing.valid&&!WorldBotTowerPolicy::Arrived(std::sqrt(p->GetDistanceSqr(footing.x,footing.y,footing.z)))){ai.Navigate(footing.x,footing.y,footing.z);return pause("complete_current_tower_footing");}}}return pause("no_legal_tower_route");}squad->objective=guid;squad->until=now+90000;objective=chosen;squad->patrol=patrol;++squad->selections;squad->phase=allFull?(patrol?"patrol":"guard"):"capture";
  sLog.Out(LOG_BASIC,LOG_LVL_MINIMAL,"[WorldBotTower] squad=%s objective=%u phase=%s",squad->id.c_str(),guid,squad->phase.c_str());
 }
 auto* go=objective->CaptureObject();if(!go)return pause("capture_object_unloaded");
 // Native capture eligibility excludes stealth/invisibility even when PvP flagged.
 p->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);p->RemoveSpellsCausingAura(SPELL_AURA_MOD_INVISIBILITY);
 auto goal=NativeGoal(p,go,now);if(!goal.valid)return pause("formation_no_legal_ground_route");
 float x=goal.x,y=goal.y,z=goal.z;
 float distance=std::sqrt(p->GetDistanceSqr(x,y,z));if(WorldBotTowerPolicy::Arrived(distance)){if(goal.staged){goals.erase(std::make_pair(p->GetGUIDLow(),go->GetGUIDLow()));actor.legAt=0;return pause("stage_arrived_replanning");}p->StopMoving();if(p->GetMotionMaster()->GetCurrentMovementGeneratorType()!=IDLE_MOTION_TYPE)p->GetMotionMaster()->Clear();return pause(allFull?"guard_holding":"native_capture_holding");}
 if(!p->IsMounted()&&!p->IsInWater()&&p->GetTerrain()->IsOutdoors(p->GetPositionX(),p->GetPositionY(),p->GetPositionZ())&&(!actor.mountAt||uint32(now-actor.mountAt)>15000)){
  PathInfo route(p,8192);route.setUseStrightPath(true);route.calculate(x,y,z);auto spell=sSpellMgr.GetSpellEntry(BotMountPolicy::Select(p->GetGUIDLow(),p->GetRace(),p->GetClass(),p->GetLevel()));if(spell&&route.getPathType()==PATHFIND_NORMAL){float increase=0;for(unsigned i=0;i<3;++i)if(spell->EffectApplyAuraName[i]==SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)increase=std::max(increase,float(spell->EffectBasePoints[i]+1));float speed=p->GetSpeed(MOVE_RUN)*(100+increase)/100;
   if(BattleBotTactics::MountSavesTime(route.Length(),p->GetSpeed(MOVE_RUN),speed,BattleBotPolicyRuntime::ActualCastTime(p,spell)/1000.f)&&ai.CanTryToCastSpell(p,spell)){actor.mountAt=now;p->StopMoving();if(p->IsInDisallowedMountForm())p->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);if(ai.DoCastSpell(p,spell)==SPELL_CAST_OK)return pause("mount_casting");}}
 }
 if(actor.legTower==squad->objective&&actor.legAt&&uint32(now-actor.legAt)<15000&&!WorldBotTowerPolicy::Arrived(std::sqrt(p->GetDistanceSqr(actor.legX,actor.legY,actor.legZ)))){x=actor.legX;y=actor.legY;z=actor.legZ;}
 else {if(!NativeTravelLeg(p,x,y,z))return pause("travel_no_legal_short_leg");actor.legTower=squad->objective;actor.legAt=now;actor.legX=x;actor.legY=y;actor.legZ=z;}
 ai.Navigate(x,y,z);return pause(goal.staged?"staged_tower_travel":allFull&&squad->patrol?"patrol_travel":"tower_travel");
}
