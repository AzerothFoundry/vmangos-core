#include "WorldBotManager.h"
#include "WorldBotAI.h"
#include "WorldBotModes.h"
#include "PlayerBotMgr.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "AccountMgr.h"
#include "World.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Spell.h"
#include "Bag.h"
#include "Pet.h"
#include "PathFinder.h"
#include "Formulas.h"
#include "WorldBotAmbientPolicy.h"
#include "WorldBotPopulation.h"
#include <random>
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Timer.h"
#include "DBCStructure.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <set>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <sys/stat.h>
using json=nlohmann::json;
namespace WorldBotManager {namespace {
struct Profile {uint32 serviceChecked=0,campChecked=0;WorldBotProfile ai;unsigned race=0,cls=0,low=1,high=9,level=1;unsigned destinationLevel=0;bool withdrawn=false;std::string routeWarning;std::string identity,purpose="grinding",city;};
struct State {uint32 elapsed=5000,revision=0;bool enabled=false;unsigned cap=320,perSlot=1,loginBatch=1,cap60=50,sessionMin60=10800,sessionMax60=18000,cooldown60=1800;std::string error;std::vector<Profile> profiles;std::set<uint32> managed;json seeded=json::object();bool seedLoaded=false,ledgerDirty=false;std::string inventorySignature;std::set<std::string> legalSlots;std::map<std::string,unsigned> targets,cityTargets;std::map<uint32,std::string> lastFailure;std::map<uint32,uint32> loadingAt,retryAt,sessionAt,cooldownAt,sessionLength;};
State& state(){static State s;return s;}
std::string ConfigPath(){return sConfig.GetStringDefault("WorldBot.ConfigFile","/workspace/vmangos/_install/etc/world-bots.json");}
std::string InventoryPath(){return sConfig.GetStringDefault("WorldBot.ProfilesFile","/workspace/vmangos/_install/etc/world-bot-profiles.json");}
std::string StatusPath(){return sConfig.GetStringDefault("WorldBot.StatusFile","/workspace/vmangos/run/world-bots-status.json");}
std::string SeedPath(){return sConfig.GetStringDefault("WorldBot.InitializedFile","/workspace/vmangos/run/world-bots-initialized.json");}
json Read(std::string const& path){std::ifstream f(path);if(!f)throw std::runtime_error("Missing world bot file: "+path);json j;f>>j;return j;}
void Write(std::string const& path,json const& j){std::string temporary=path+".tmp";{std::ofstream f(temporary,std::ios::trunc);if(!f)throw std::runtime_error("Cannot write world bot state");f<<j.dump(2)<<'\n';if(!f)throw std::runtime_error("Incomplete world bot state");}if(std::rename(temporary.c_str(),path.c_str()))throw std::runtime_error("Cannot publish world bot state");}
std::string ProfileSlot(Profile const& p,unsigned level){if(p.purpose!="grinding")return p.purpose+":"+p.city;unsigned band=level<10?1:(level/10)*10;return "r"+std::to_string(p.race)+"-c"+std::to_string(p.cls)+"-b"+std::to_string(band);}
std::string Slot(unsigned race,unsigned cls,unsigned level){unsigned band=level<10?1:(level/10)*10;return "r"+std::to_string(race)+"-c"+std::to_string(cls)+"-b"+std::to_string(band);}
struct Camp {unsigned zone,map,minimum,maximum,entry;float x,y,z,radius;};
struct Support {unsigned entry,map,zone,flags,cls,faction;float x,y,z,orientation;};
std::vector<Support> supports;std::vector<Camp> camps;json routes;time_t campsStamp=0,routesStamp=0,supportStamp=0;
void LoadRoutes(){
 std::string sp=sConfig.GetStringDefault("WorldBot.SupportFile","/workspace/vmangos/_install/etc/world-bot-support.tsv");struct stat ss{};
 if(!stat(sp.c_str(),&ss)&&ss.st_mtime!=supportStamp){std::ifstream file(sp);std::vector<Support> next;std::string line;while(std::getline(file,line)){Support s;std::istringstream row(line);if(row>>s.entry>>s.map>>s.zone>>s.flags>>s.cls>>s.faction>>s.x>>s.y>>s.z>>s.orientation)if(s.map<=1&&std::isfinite(s.x)&&std::isfinite(s.y)&&std::isfinite(s.z))next.push_back(s);}supports.swap(next);supportStamp=ss.st_mtime;}

 std::string cp=sConfig.GetStringDefault("WorldBot.CampsFile","/workspace/vmangos/_install/etc/world-bot-camps.tsv");struct stat st{};
 if(!stat(cp.c_str(),&st)&&st.st_mtime!=campsStamp){std::ifstream file(cp);std::vector<Camp> next;std::string line;while(std::getline(file,line)){std::istringstream row(line);Camp c;if(row>>c.zone>>c.map>>c.minimum>>c.maximum>>c.entry>>c.x>>c.y>>c.z>>c.radius)if(c.map<=1&&c.minimum>=1&&c.maximum<=60&&c.minimum<=c.maximum&&c.radius>=5&&c.radius<=500&&std::isfinite(c.x)&&std::isfinite(c.y)&&std::isfinite(c.z))next.push_back(c);}camps.swap(next);campsStamp=st.st_mtime;}
 std::string rp=sConfig.GetStringDefault("WorldBot.RoutesFile","/workspace/vmangos/_install/etc/world-bot-routes.json");if(!stat(rp.c_str(),&st)&&st.st_mtime!=routesStamp){routes=Read(rp);routesStamp=st.st_mtime;}
}
void Destination(Profile& profile,Player* player){

 auto* ai=player->AI()?dynamic_cast<WorldBotAI*>(player->AI()):nullptr;if(!ai)return;
 if(profile.purpose=="plaguelands_towers")return;
 if(ai->PKTaskEnabled())return;
 auto mode=WorldBotModes::Detect(player);if(mode!=WorldBotModes::Mode::World&&mode!=WorldBotModes::Mode::City)return;
 if(player->GetLevel()>=60&&profile.purpose=="grinding"){
  GameTele const* best=nullptr;float distance=0;
  for(uint32 id:{67u,118u,225u,246u,309u,404u}){auto tele=sObjectMgr.GetGameTele(id);bool alliance=id==67||id==246||id==404;if(!tele||tele->mapId!=player->GetMapId()||alliance!=(player->GetTeam()==ALLIANCE))continue;float d=player->GetDistance(tele->x,tele->y,tele->z);if(!best||d<distance){best=tele;distance=d;}}
  if(best){ai->SetDestination(best->mapId,best->x,best->y,best->z,60);profile.city=best->name;profile.routeWarning="level60_city_walk_requires_legal_route";}else profile.routeWarning="level60_no_same_continent_friendly_city";
 }
 if(profile.purpose=="city_duel"){
  if(player->GetMapId()==profile.ai.map&&!player->IsWithinDist3d(profile.ai.x,profile.ai.y,profile.ai.z,profile.ai.radius)){
   ai->SetDestination(profile.ai.map,profile.ai.x,profile.ai.y,profile.ai.z,profile.ai.radius);profile.routeWarning="city_duel_return_to_gate";
  }
  return;
 }
 if(ai->NeedsService()){
  uint32 now=WorldTimer::getMSTime();
  if(profile.serviceChecked&&uint32(now-profile.serviceChecked)<30000)return;
  profile.serviceChecked=now;
  std::vector<std::pair<float,Support const*>> candidates;
  for(auto const& npc:supports){if(npc.map!=player->GetMapId())continue;
   if(ai->NeedsPracticeSupplies()){
    if(!(npc.flags&UNIT_NPC_FLAG_VENDOR))continue;
    bool food=false,water=false;bool needFood=player->GetItemCount(8952,false)<3,needWater=player->GetMaxPower(POWER_MANA)>0&&player->GetItemCount(8766,false)<3;
    auto info=sObjectMgr.GetCreatureTemplate(npc.entry);if(!info)continue;
    for(VendorItemData const* list:{sObjectMgr.GetNpcVendorItemList(npc.entry),sObjectMgr.GetNpcVendorTemplateItemList(info->vendor_id)})if(list)for(auto row:list->m_items){if(row->item==8952)food=true;if(row->item==8766)water=true;}
    if(!(needFood&&food)&&!(needWater&&water))continue;
   }else if(ai->NeedsTrainer()?(!(npc.flags&16)||npc.cls!=player->GetClass()):!(npc.flags&(4|16384)))continue;
   auto faction=sObjectMgr.GetFactionTemplateEntry(npc.faction);if(!faction||WorldObject::GetFactionReactionTo(faction,player)<REP_FRIENDLY)continue;
   candidates.push_back({player->GetDistance(npc.x,npc.y,npc.z),&npc});
  }
  std::sort(candidates.begin(),candidates.end(),[](auto const& a,auto const& b){return a.first<b.first;});
  Support const* best=nullptr;float approachX=0,approachY=0,approachZ=0;
  for(unsigned i=0;i<std::min<size_t>(6,candidates.size());++i){auto npc=candidates[i].second;PathInfo route(player);route.ExcludeSteepSlopes();bool calculated=route.calculate(npc->x,npc->y,npc->z);
   bool legal=calculated&&route.getPath().size()>=2&&route.getPathType()==PATHFIND_NORMAL;
   if(route.getPath().size()>=2&&route.getPathType()==PATHFIND_INCOMPLETE){auto end=route.getPath().back();float dx=end.x-npc->x,dy=end.y-npc->y,dz=end.z-npc->z;legal=WorldBotAmbient::StageProgress(player->GetDistance(end.x,end.y,end.z),std::sqrt(dx*dx+dy*dy+dz*dz));
    if(legal){PathInfo stage(player);stage.ExcludeSteepSlopes();legal=stage.calculate(end.x,end.y,end.z)&&stage.getPathType()==PATHFIND_NORMAL&&stage.getPath().size()>=2;}
   }
   if(legal){best=npc;approachX=npc->x;approachY=npc->y;approachZ=npc->z;break;}
   // A vendor is used within interaction range; an unreachable NPC center is
   // not evidence that every legal nearby standing point is unreachable.
   for(unsigned point=0;point<8;++point){float angle=point*float(M_PI)/4;
    float x=npc->x+3.5f*std::cos(angle),y=npc->y+3.5f*std::sin(angle);
    float z=player->GetMap()->GetHeight(x,y,npc->z+2,true,6);
    if(!std::isfinite(z)||std::fabs(z-npc->z)>2.5f)continue;
    PathInfo near(player);near.ExcludeSteepSlopes();if(!near.calculate(x,y,z)||near.getPathType()!=PATHFIND_NORMAL||near.getPath().size()<2)continue;
    auto end=near.getPath().back();float dx=end.x-npc->x,dy=end.y-npc->y,dz=end.z-npc->z;
    if(dx*dx+dy*dy+dz*dz>20.25f||!player->GetMap()->isInLineOfSight(end.x,end.y,end.z+1.5f,npc->x,npc->y,npc->z+1.5f,true))continue;
    best=npc;approachX=end.x;approachY=end.y;approachZ=end.z;break;
   }
   if(best)break;
  }
  if(best){ai->SetServiceDestination(best->map,approachX,approachY,approachZ,best->entry);profile.routeWarning="";}
  else{ai->ClearServiceDestination();profile.routeWarning=ai->NeedsPracticeSupplies()?"no_legal_friendly_ration_vendor_route":"no_legal_friendly_service_route";}
  return;
 }
 ai->ClearServiceDestination();
 if(profile.purpose!="grinding"||player->GetLevel()>=60)return;
 auto const& stall=ai->RouteStall();bool stalled=stall.tracking&&uint32(WorldTimer::getMSTime()-stall.since)>=60000;
 if(profile.destinationLevel==player->GetLevel()&&!stalled)return;
 uint32 campNow=WorldTimer::getMSTime();if(profile.campChecked&&uint32(campNow-profile.campChecked)<60000)return;profile.campChecked=campNow;

 std::vector<unsigned> desired;
 if(routes.is_object()&&routes.count("routes"))for(auto const& route:routes["routes"])if(route.value("race",0u)==profile.race&&route.value("class_id",0u)==profile.cls)for(auto const& stage:route["stages"])if(stage.value("min_level",1u)<=player->GetLevel()&&stage.value("max_level",0u)>=player->GetLevel()){
  if(stage.value("map",0u)!=player->GetMapId()){profile.routeWarning="cross_continent_transport_not_implemented";return;}
  for(auto const& zone:stage["candidates"])desired.push_back(zone.get<unsigned>());
 }
 std::vector<std::pair<float,Camp const*>> campChoices;
 for(auto const& c:camps){auto representative=sObjectMgr.GetCreatureTemplate(c.entry);if(!representative||representative->level_max<=MaNGOS::XP::GetGrayLevel(player->GetLevel())||representative->level_min>player->GetLevel()+1)continue;
  if(stalled&&std::fabs(c.x-stall.goal[0])<1&&std::fabs(c.y-stall.goal[1])<1&&std::fabs(c.z-stall.goal[2])<1)continue;
  if(c.map!=player->GetMapId()||player->GetLevel()<c.minimum||player->GetLevel()>c.maximum)continue;
  auto found=std::find(desired.begin(),desired.end(),c.zone);float cost=player->GetDistance(c.x,c.y,c.z)+(found==desired.end()?10000000.0f:float(found-desired.begin())*1000000.0f);
  campChoices.push_back({cost,&c});
 }
 std::sort(campChoices.begin(),campChoices.end(),[](auto const& a,auto const& b){return a.first<b.first;});
 Camp const* best=nullptr;
 for(unsigned i=0;i<std::min<size_t>(8,campChoices.size());++i){auto c=campChoices[i].second;
  PathInfo path(player);path.ExcludeSteepSlopes();if(!path.calculate(c->x,c->y,c->z)||path.getPath().size()<2)continue;
  bool legal=path.getPathType()==PATHFIND_NORMAL;
  if(path.getPathType()==PATHFIND_INCOMPLETE){auto end=path.getPath().back();float dx=end.x-c->x,dy=end.y-c->y,dz=end.z-c->z;legal=WorldBotAmbient::StageProgress(player->GetDistance(end.x,end.y,end.z),std::sqrt(dx*dx+dy*dy+dz*dz));}
  if(legal){best=c;break;}
 }
 if(!best){profile.routeWarning="no_eligible_same_continent_camp";return;}
 ai->SetDestination(best->map,best->x,best->y,best->z,best->radius);profile.destinationLevel=player->GetLevel();profile.routeWarning=std::find(desired.begin(),desired.end(),best->zone)==desired.end()?"fallback_outside_selected_corridor":"";
}
bool Load(){auto& s=state();
 json config=Read(ConfigPath());if(config.value("schema_version",0)!=1)throw std::runtime_error("Unsupported world bot config schema");
 s.enabled=config.value("enabled",false);s.revision=config.value("revision",0u);auto pop=config.value("population",json::object());s.cap=pop.value("max_online",320u);s.perSlot=pop.value("grinding_per_slot",1u);s.loginBatch=pop.value("login_batch_size",1u);if(s.loginBatch<1||s.loginBatch>10)throw std::runtime_error("Invalid world bot login batch size");
 s.cap60=pop.value("level60_max_online",50u);s.sessionMin60=pop.value("level60_session_min_seconds",10800u);s.sessionMax60=pop.value("level60_session_max_seconds",18000u);s.cooldown60=pop.value("level60_cooldown_seconds",1800u);
 if(s.cap60>2000||s.sessionMin60<60||s.sessionMax60<s.sessionMin60||s.sessionMax60>86400||s.cooldown60<60||s.cooldown60>86400)throw std::runtime_error("Invalid level60 rotation limits");
 if(!WorldBotPopulation::CapacityLimits(s.cap,s.cap60)||s.perSlot>5)throw std::runtime_error("World bot population limits invalid");
 if(s.legalSlots.empty()){
  std::unique_ptr<QueryResult> legal(WorldDatabase.Query("SELECT race,class FROM playercreateinfo"));
  if(!legal)throw std::runtime_error("Missing native character combinations");
  do{auto f=legal->Fetch();for(unsigned level:{1u,10u,20u,30u,40u,50u})s.legalSlots.insert(Slot(f[0].GetUInt32(),f[1].GetUInt32(),level));}while(legal->NextRow());
 }
 s.cityTargets["city_ambient"]=pop.value("city_ambient_per_capital",4u);s.cityTargets["city_production"]=pop.value("city_production_per_capital",4u);s.cityTargets["orgrimmar"]=pop.value("orgrimmar_duel",8u);s.cityTargets["ironforge"]=pop.value("ironforge_duel",8u);
 s.cityTargets["city_healer"]=pop.value("city_healer_per_capital",2u);
 s.targets.clear();auto targets=pop.value("slot_targets",json::object());if(!targets.is_object())throw std::runtime_error("Invalid slot targets");
 for(auto it=targets.begin();it!=targets.end();++it){if(!s.legalSlots.count(it.key())||!it.value().is_number_unsigned()||it.value().get<unsigned>()>5)throw std::runtime_error("Illegal slot override");s.targets[it.key()]=it.value().get<unsigned>();}
 if(!s.seedLoaded){std::ifstream f(SeedPath());if(f)f>>s.seeded;if(!s.seeded.is_object())throw std::runtime_error("Invalid initialization ledger");s.seedLoaded=true;}
 json inventory={{"schema_version",1},{"profiles",json::array()}};{std::ifstream f(InventoryPath());if(f)f>>inventory;}std::string signature=inventory.dump();if(signature==s.inventorySignature)return true;if(inventory.value("schema_version",0)!=1)throw std::runtime_error("Unsupported inventory schema");
 std::vector<Profile> profiles;std::set<uint32> ids;std::set<std::string> names;
 for(auto const& row:inventory.at("profiles")){
  Profile p;p.ai.id=row.at("id").get<std::string>();p.ai.characterGuid=row.at("char_guid").get<uint32>();p.ai.accountId=row.at("account_id").get<uint32>();p.ai.createdAt=row.at("created_at").get<uint64>();if(!p.ai.characterGuid)continue;
  if(!names.insert(p.ai.id).second||!ids.insert(p.ai.characterGuid).second)throw std::runtime_error("Duplicate world bot profile or character");
  p.purpose=row.value("purpose",std::string("grinding"));p.city=row.value("city",std::string());p.ai.purpose=p.purpose;if(p.purpose!="grinding"&&p.purpose!="city_ambient"&&p.purpose!="city_production"&&p.purpose!="city_duel"&&p.purpose!="city_healer"&&p.purpose!="plaguelands_towers")throw std::runtime_error("Unknown world bot purpose");
  p.race=row.at("race").get<unsigned>();p.cls=row.at("class_id").get<unsigned>();p.low=row.at("band_min").get<unsigned>();p.high=row.at("band_max").get<unsigned>();
  if((p.purpose=="grinding"&&(p.low<1||p.low>59||p.high<p.low||p.high>59))||(p.purpose!="grinding"&&(p.low!=60||p.high!=60)))throw std::runtime_error("Invalid world bot level band");
  p.ai.specId=row.value("spec_id",0u);p.ai.gearId=row.value("gear_id",0u);p.ai.ammoItem=row.contains("ammo_item")&&!row["ammo_item"].is_null()?row["ammo_item"].get<uint32>():0;
  if(p.purpose=="city_healer"){
   auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();auto spec=specs.find(p.ai.specId);
   if(spec==specs.end()||spec->second.requiredClass!=p.cls||spec->second.role!=ROLE_HEALER||!p.ai.gearId)throw std::runtime_error("City healer needs an explicit native healer spec and gear");
  }
  if(p.purpose=="city_duel"&&(!p.ai.specId||!p.ai.gearId))throw std::runtime_error("Duel profile needs explicit spec_id and gear_id");
  p.ai.seedLevel=uint8(row.value("seed_level",p.low));p.ai.allowLevel60=p.purpose!="grinding";if(p.ai.seedLevel!=p.low)throw std::runtime_error("Seed level must equal band start");
  p.ai.map=row.at("map").get<uint32>();p.ai.x=row.at("x").get<float>();p.ai.y=row.at("y").get<float>();p.ai.z=row.at("z").get<float>();p.ai.radius=row.value("radius",120.0f);
  if(p.ai.map>1||!std::isfinite(p.ai.x)||!std::isfinite(p.ai.y)||!std::isfinite(p.ai.z)||!std::isfinite(p.ai.radius)||p.ai.radius<5||p.ai.radius>500)throw std::runtime_error("Invalid grind coordinates/radius");
  auto result=std::unique_ptr<QueryResult>(CharacterDatabase.PQuery("SELECT account,level,race,class,name,create_time FROM characters WHERE guid=%u",p.ai.characterGuid));
  if(!result){throw std::runtime_error("Configured bot character does not exist");}
  auto fields=result->Fetch();uint32 account=fields[0].GetUInt32();p.level=fields[1].GetUInt32();if(account!=p.ai.accountId||fields[5].GetUInt64()!=p.ai.createdAt)throw std::runtime_error("Account or birth identity mismatch");
  std::unique_ptr<QueryResult> identity(LoginDatabase.PQuery("SELECT a.username,COALESCE((SELECT MAX(x.gmlevel) FROM account_access x WHERE x.id=a.id AND x.RealmID IN(-1,%u)),0) FROM account a WHERE a.id=%u",realmID,account));
  if(!identity)throw std::runtime_error("Missing bot account");
  std::string username=identity->Fetch()[0].GetCppString();
  if(username.size()!=14||username.compare(0,2,"WB")||username.find_first_not_of("0123456789ABCDEF",2)!=std::string::npos||identity->Fetch()[1].GetUInt32()!=0)throw std::runtime_error("Not a dedicated ordinary WB account");
  if(fields[2].GetUInt32()!=p.race||fields[3].GetUInt32()!=p.cls||fields[4].GetCppString()=="Root"||sAccountMgr.GetSecurity(account)!=SEC_PLAYER)throw std::runtime_error("Profile identity mismatch or protected account");
  if(p.ai.specId||p.ai.gearId){auto const& specs=sObjectMgr.GetPlayerPremadeSpecTemplates();auto const& gears=sObjectMgr.GetPlayerPremadeGearTemplates();auto spec=specs.find(p.ai.specId);auto gear=gears.find(p.ai.gearId);
   if(spec==specs.end()||gear==gears.end()||spec->second.requiredClass!=p.cls||gear->second.requiredClass!=p.cls||spec->second.level!=60||gear->second.level!=60||spec->second.role!=gear->second.role)throw std::runtime_error("Preset unavailable or mismatched; retain last good population");
  }
  p.identity=p.ai.id+":"+std::to_string(p.ai.characterGuid)+":"+std::to_string(account)+":"+fields[5].GetCppString();
  profiles.push_back(p);
 }
 // Removed active profiles remain visible until a safe saved logout; no forced
 // withdrawal from a real player's group/duel/BG when a config file changes.
 for(auto const& old:s.profiles)if(!ids.count(old.ai.characterGuid)&&s.managed.count(old.ai.characterGuid)){Profile retained=old;retained.withdrawn=true;profiles.push_back(retained);}
 s.profiles.swap(profiles);s.inventorySignature=signature;return true;
}
}
void Update(uint32 diff){auto& s=state();
 // World.cpp calls after map-worker barriers. Never dereference actors in map callbacks.
 for(auto const& profile:s.profiles)if(Player* player=ObjectAccessor::FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER,profile.ai.characterGuid)))
  if(player->IsBot()&&player->AI())if(auto ai=dynamic_cast<WorldBotAI*>(player->AI())){ai->UpdateWorld(diff);if(ai->botEntry&&ai->botEntry->requestRemoval){std::string reason=ai->GetActivity();if(s.managed.count(profile.ai.characterGuid)&&s.lastFailure[profile.ai.characterGuid]!=reason)sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[WorldBot] character %u requested removal: %s",profile.ai.characterGuid,reason.c_str());if(s.managed.count(profile.ai.characterGuid))s.lastFailure[profile.ai.characterGuid]=reason;}else if(ai->m_initialized)s.lastFailure.erase(profile.ai.characterGuid);}
 s.elapsed+=diff;if(s.elapsed<5000)return;s.elapsed=0;
 try{Load();LoadRoutes();if(s.ledgerDirty){Write(SeedPath(),s.seeded);s.ledgerDirty=false;}s.error.clear();}catch(std::exception const& e){s.error=e.what();s.enabled=false;}
 static std::mt19937 random(std::random_device{}());
 uint32 now=WorldTimer::getMSTime();unsigned total=0,online60=0,ready60=0;std::map<std::string,unsigned> occupied,cityClasses;
 json counts={{"registered",s.profiles.size()},{"online",0},{"loading",0},{"transferring",0},{"grinding",0},{"party",0},{"battleground",0},{"instance",0},{"city",0},{"duel",0},{"level60",0}};json rows=json::array(),deficits=json::array();
 auto target=[&](Profile const& p){if(p.withdrawn)return 0u;if(p.purpose=="plaguelands_towers")return 5u;std::string slot=ProfileSlot(p,p.level);if(p.purpose=="grinding")return s.targets.count(slot)?s.targets[slot]:s.perSlot;
  unsigned configured=p.purpose=="city_duel"?s.cityTargets[p.city]:s.cityTargets[p.purpose];
  auto role=p.purpose=="city_duel"?WorldBotPopulation::CityRole::Gate:p.purpose=="city_ambient"?WorldBotPopulation::CityRole::Ambient:p.purpose=="city_healer"?WorldBotPopulation::CityRole::Healer:WorldBotPopulation::CityRole::Other;
  return WorldBotPopulation::CityQuota(role,configured);};
 auto safety=[](Player* player){WorldBotPopulation::Safety a;a.alive=player->IsAlive();a.combat=player->IsInCombat();a.duel=player->m_duel!=nullptr;a.bg=player->InBattleGround();a.instance=player->GetMap()&&player->GetMap()->IsDungeon();a.teleport=player->IsBeingTeleported();a.casting=player->IsNonMeleeSpellCasted();if(Group* group=player->GetGroup())for(auto const& member:group->GetMemberSlots()){Player* other=ObjectAccessor::FindPlayer(member.guid);if(!other||!other->IsBot()){a.humanParty=true;break;}}return a;};
 auto retire=[&](uint32 guid){sPlayerBotMgr.RequestBotRemoval(guid);s.managed.erase(guid);s.sessionAt.erase(guid);s.sessionLength.erase(guid);s.cooldownAt[guid]=now;};
 for(auto& p:s.profiles){
  Player* player=ObjectAccessor::FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER,p.ai.characterGuid));
  json row={{"id",p.ai.id},{"char_guid",p.ai.characterGuid},{"race",p.race},{"class_id",p.cls},{"purpose",p.purpose},{"effective_purpose",p.purpose=="grinding"&&p.level>=60?"city_ambient":p.purpose},{"city",p.city},{"band_min",p.low},{"band_max",p.high},{"withdrawn",p.withdrawn},{"level",p.level},{"state","offline"},{"spec_id",p.ai.specId},{"gear_id",p.ai.gearId}};
  if(s.lastFailure.count(p.ai.characterGuid))row["last_error"]=s.lastFailure[p.ai.characterGuid];
  if(player){
   // Never adopt or mutate an ordinary session merely because a profile mentions it.
   auto ai=player->IsBot()&&player->AI()?dynamic_cast<WorldBotAI*>(player->AI()):nullptr;
   if(!ai){row["state"]="occupied_by_unmanaged_session";rows.push_back(row);continue;}
   auto presence=WorldBotPopulation::PresenceOf(ai->m_initialized,player->IsInWorld());
   if(presence!=WorldBotPopulation::Presence::Ready){
    bool transfer=presence==WorldBotPopulation::Presence::Transferring;std::string key=transfer?"transferring":"loading";
    row["state"]=key;counts[key]=counts[key].get<unsigned>()+1;++total;++occupied[ProfileSlot(p,transfer?p.level:p.ai.seedLevel)];
    if(std::max(p.level,unsigned(p.ai.seedLevel))>=60)++online60;
    if(p.purpose=="city_duel")++cityClasses[p.city+":r"+std::to_string(p.race)+":c"+std::to_string(p.cls)];
    rows.push_back(row);continue; // No navigation, safety/retirement or normal actor reads until back in world.
   }
   if(p.ai.specId&&p.ai.gearId&&(ai->PresetMigrationPending()||ai->GetProfile().specId!=p.ai.specId||ai->GetProfile().gearId!=p.ai.gearId)){
    WorldBotProfile desired=p.ai;std::string key="preset:"+p.identity+":"+std::to_string(desired.specId)+":"+std::to_string(desired.gearId);
    desired.presetInitializedCallback=[key](){auto& v=state();v.seeded[key]=true;v.ledgerDirty=true;try{Write(SeedPath(),v.seeded);v.ledgerDirty=false;}catch(std::exception const& e){v.error=e.what();v.enabled=false;}};
    row["preset_migration"]=ai->ApplyIdlePreset(desired)?"verified_and_saved":"waiting_safe_idle_or_failed";
   }
   Destination(p,player);p.level=player->GetLevel();row["level"]=p.level;row["route_warning"]=p.routeWarning;row["activity"]=ai->GetActivity();
   row["identity"]={{"account_id",p.ai.accountId},{"created_at",p.ai.createdAt},{"managed",s.managed.count(p.ai.characterGuid)!=0},{"native_bot",player->IsBot()}};
   row["wallet_copper"]=player->GetMoney();row["ammo_item"]=player->GetUInt32Value(PLAYER_AMMO_ID);
   json talentRows=json::array();std::map<uint32,std::pair<uint32,uint32>> talentRanks;
   for(auto const& known:player->GetSpellMap())if(known.second.state!=PLAYERSPELL_REMOVED&&!known.second.disabled)if(auto pos=GetTalentSpellPos(known.first)){
    auto& rank=talentRanks[pos->talent_id];if(uint32(pos->rank)+1>rank.first)rank={uint32(pos->rank)+1,known.first};
   }
   for(auto const& talent:talentRanks)talentRows.push_back({{"talent_id",talent.first},{"points",talent.second.first},{"spell_id",talent.second.second}});
   row["talents_actual"]=talentRows;
   json abilities=json::object();for(uint32 spell:{12294u,12323u,12975u,19574u,19434u,19503u,14177u,14185u,16511u,15473u,10060u,17116u,18562u,16166u,16188u,12043u,11129u,19028u,18788u,18288u,982u,883u,691u,6346u,2944u})abilities[std::to_string(spell)]=player->HasSpell(spell);
   row["core_abilities_actual"]=abilities;
   json bags=json::array();for(uint8 slot=INVENTORY_SLOT_BAG_START;slot<INVENTORY_SLOT_BAG_END;++slot){auto bag=dynamic_cast<Bag*>(player->GetItemByPos(INVENTORY_SLOT_BAG_0,slot));bags.push_back({{"slot",slot},{"item",bag?bag->GetEntry():0},{"guid",bag?std::to_string(bag->GetGUID()):"0"},{"capacity",bag?bag->GetBagSize():0},{"free",bag?bag->GetFreeSlots():0}});}row["bags_actual"]=bags;
   if(Pet* pet=player->GetPet())row["pet_actual"]={{"guid",std::to_string(pet->GetGUID())},{"entry",pet->GetEntry()},{"level",pet->GetLevel()},{"alive",pet->IsAlive()},{"health_pct",pet->GetHealthPercent()},{"victim",pet->GetVictim()?std::to_string(pet->GetVictim()->GetGUID()):"0"}};
   else row["pet_actual"]=nullptr;
   row["visibility_evidence"]="native_snapshot_only_not_observer_visibility";
   row["xp"]=player->GetUInt32Value(PLAYER_XP);row["xp_next"]=player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);row["movement_type"]=player->GetMotionMaster()->GetCurrentMovementGeneratorType();row["cast_id"]=player->GetCurrentSpell(CURRENT_GENERIC_SPELL)?player->GetCurrentSpell(CURRENT_GENERIC_SPELL)->m_spellInfo->Id:0;
   row["alive"]=player->IsAlive();row["ghost"]=player->HasFlag(PLAYER_FLAGS,PLAYER_FLAGS_GHOST);row["moving"]=player->IsMoving();row["combat"]=player->IsInCombat();row["free_talent_points"]=player->GetFreeTalentPoints();row["spent_talent_points"]=ai->TalentPointsReport();row["ration_food"]=player->GetItemCount(8952,false);row["ration_water"]=player->GetItemCount(8766,false);row["duel_phase"]=!player->m_duel?"none":player->m_duel->startTime?"active":player->m_duel->startTimer?"countdown":"preparing";row["selection_guid"]=player->GetSelectionGuid().GetCounter();row["duel_opponent"]=player->m_duel&&player->m_duel->opponent?player->m_duel->opponent->GetGUIDLow():0;
   row["needs_trainer"]=ai->NeedsTrainer();row["needs_vendor"]=ai->NeedsVendor();row["service_goal"]={{"active",ai->HasServiceGoal()},{"entry",ai->ServiceEntry()},{"map",ai->ServiceMap()},{"position",ai->ServiceGoal()}};
   auto const& recovery=ai->RouteStall();row["route_recovery"]={{"tracking",recovery.tracking},{"since_ms",recovery.since},{"last_failure_ms",recovery.lastFailure},{"last_attempt_ms",recovery.lastAttempt},{"pending",ai->RouteHearthPending()},{"native_cast_ms",ai->RouteHearthCastTime()},{"reason",ai->RouteRecoveryReason()}};
   auto const& nav=ai->NavDiagnostic();row["nav"]={{"at_ms",nav.at},{"map",nav.map},{"type",nav.type},{"points",nav.points},{"start",nav.start},{"goal",nav.goal},{"end",nav.end}};
   auto const& seen=ai->ServiceObservation();row["service_seen"]={{"at_ms",seen.at},{"entry",seen.entry},{"flags",seen.flags},{"trainer_type",seen.trainerType},{"trainer_class",seen.trainerClass},{"faction",seen.faction},{"distance",seen.distance},{"alive",seen.alive},{"friendly",seen.friendly},{"los",seen.los},{"trainer_compatible",seen.trainerCompatible},{"affordable",seen.affordable}};row["name"]=player->GetName();row["map"]=player->GetMapId();row["position"]={player->GetPositionX(),player->GetPositionY(),player->GetPositionZ()};row["goal"]={ai->GetProfile().x,ai->GetProfile().y,ai->GetProfile().z};row["health_pct"]=player->GetHealthPercent();row["mana_pct"]=player->GetPowerPercent(POWER_MANA);row["victim_guid"]=std::to_string(player->GetVictim()?player->GetVictim()->GetGUID():0);row["world_pvp"]=ai->WorldPvpFlavorReport();row["pvp_policy"]=ai->PolicyReport();row["pk_task"]=json::parse(ai->PKTaskReport());row["preset_verified"]=ai->PresetVerified();row["equipment"]=ai->EquipmentReport();
   auto mode=WorldBotModes::Detect(player);std::string key=WorldBotModes::Name(mode);row["state"]=key;
   counts["online"]=counts["online"].get<unsigned>()+1;if(counts.count(key))counts[key]=counts[key].get<unsigned>()+1;
   ++total;++occupied[ProfileSlot(p,p.level)];if(p.purpose=="city_duel")++cityClasses[p.city+":r"+std::to_string(p.race)+":c"+std::to_string(p.cls)];
   if(p.level>=60){++online60;++ready60;if(!s.sessionAt.count(p.ai.characterGuid)){s.sessionAt[p.ai.characterGuid]=now;s.sessionLength[p.ai.characterGuid]=WorldBotPopulation::SessionSeconds(s.sessionMin60,s.sessionMax60,random);}row["session_seconds"]=uint32(now-s.sessionAt[p.ai.characterGuid])/1000;}
   bool overdue=p.purpose!="plaguelands_towers"&&p.level>=60&&WorldBotPopulation::Expired(now,s.sessionAt[p.ai.characterGuid],s.sessionLength[p.ai.characterGuid]);
   row["rotation_pending"]=overdue;if(p.level>=60)row["session_limit_seconds"]=s.sessionLength[p.ai.characterGuid];
   auto safe=safety(player);
   bool excess=(p.level>=60&&online60>s.cap60)||occupied[ProfileSlot(p,p.level)]>target(p);
   if(s.managed.count(p.ai.characterGuid)&&WorldBotPopulation::Safe(safe)&&(!s.enabled||p.withdrawn||overdue||excess)){retire(p.ai.characterGuid);row["state"]="retiring";}
   s.loadingAt.erase(p.ai.characterGuid);
  }else if(s.managed.count(p.ai.characterGuid)){
   auto started=s.loadingAt.find(p.ai.characterGuid);
   if(started==s.loadingAt.end()){retire(p.ai.characterGuid);row["state"]="session_missing";}
   else if(WorldBotPopulation::LoginTimedOut(false,true,now,started->second)){retire(p.ai.characterGuid);s.retryAt[p.ai.characterGuid]=now+300000;row["state"]="login_timeout";}
   else{row["state"]="loading";counts["loading"]=counts["loading"].get<unsigned>()+1;++total;++occupied[ProfileSlot(p,p.ai.seedLevel)];if(std::max(p.level,unsigned(p.ai.seedLevel))>=60)++online60;if(p.purpose=="city_duel")++cityClasses[p.city+":r"+std::to_string(p.race)+":c"+std::to_string(p.cls)];}
  }
  if(s.cooldownAt.count(p.ai.characterGuid)){unsigned elapsed=uint32(now-s.cooldownAt[p.ai.characterGuid])/1000;row["cooldown_remaining"]=elapsed<s.cooldown60?s.cooldown60-elapsed:0;}
  rows.push_back(row);
 }
 counts["level60"]=ready60;counts["level60_reserved"]=online60;
 if(s.enabled){
  std::vector<Profile const*> candidates;for(auto const& p:s.profiles)if(!p.withdrawn)candidates.push_back(&p);
  std::shuffle(candidates.begin(),candidates.end(),random);
  // Fill explicitly provisioned non-duel city roles before gate variants; quotas
  // cap each city at 20 gate + 3 ambient + 2 healer without evicting protected groups.
  std::stable_sort(candidates.begin(),candidates.end(),[&](Profile const* a,Profile const* b){auto missing=[&](Profile const* p){return p->purpose=="city_duel"&&!cityClasses[p->city+":r"+std::to_string(p->race)+":c"+std::to_string(p->cls)];};bool fullA=std::max(a->level,unsigned(a->ai.seedLevel))>=60,fullB=std::max(b->level,unsigned(b->ai.seedLevel))>=60;auto priority=[&](Profile const* p,bool full)->int{if(p->purpose=="plaguelands_towers")return -1;bool service=(p->purpose=="city_ambient"||p->purpose=="city_healer")&&occupied[ProfileSlot(*p,p->level)]<target(*p);return WorldBotPopulation::AdmissionPriority(service,missing(p),full);};return priority(a,fullA)<priority(b,fullB);});
  unsigned attempts=0,admitted=0;
  for(auto ptr:candidates){auto const& p=*ptr;if(total>=s.cap||attempts>=s.loginBatch)break;
   bool full=std::max(p.level,unsigned(p.ai.seedLevel))>=60;
   bool cooling=s.cooldownAt.count(p.ai.characterGuid)&&!WorldBotPopulation::Expired(now,s.cooldownAt[p.ai.characterGuid],s.cooldown60);
   if(!WorldBotPopulation::Admit(full,online60,s.cap60,cooling)||s.managed.count(p.ai.characterGuid)||ObjectAccessor::FindPlayer(ObjectGuid(HIGHGUID_PLAYER,p.ai.characterGuid)))continue;
   if(s.retryAt.count(p.ai.characterGuid)&&int32(now-s.retryAt[p.ai.characterGuid])<0)continue;
   if(p.purpose=="grinding"&&!full&&occupied[ProfileSlot(p,p.level)]>=target(p))continue;
   if(p.purpose!="grinding"&&occupied[ProfileSlot(p,p.level)]>=target(p))continue;
   WorldBotProfile profile=p.ai;profile.initialize=!s.seeded.value(p.identity,false);std::string identity=p.identity;
   std::string presetKey="preset:"+identity+":"+std::to_string(profile.specId)+":"+std::to_string(profile.gearId);profile.presetInitialize=!s.seeded.value(presetKey,false);
   profile.presetInitializedCallback=[presetKey](){auto& value=state();value.seeded[presetKey]=true;value.ledgerDirty=true;try{Write(SeedPath(),value.seeded);value.ledgerDirty=false;}catch(std::exception const& e){value.error=e.what();value.enabled=false;}};
   profile.initializedCallback=[identity](){auto& value=state();value.seeded[identity]=true;value.ledgerDirty=true;try{Write(SeedPath(),value.seeded);value.ledgerDirty=false;}catch(std::exception const& e){value.error=e.what();value.enabled=false;}};
   std::unique_ptr<WorldBotAI> ai(new WorldBotAI());ai->Configure(profile);
   ++attempts;
   if(sPlayerBotMgr.AddBot(profile.characterGuid,false,ai.get())){++admitted;++total;++occupied[ProfileSlot(p,std::max(p.level,unsigned(p.ai.seedLevel)))];if(full)++online60;if(p.purpose=="city_duel")++cityClasses[p.city+":r"+std::to_string(p.race)+":c"+std::to_string(p.cls)];ai.release();s.managed.insert(profile.characterGuid);s.loadingAt[profile.characterGuid]=now;if(p.purpose=="city_duel")WorldBotModes::SetDuelArena(profile.characterGuid,profile.map,profile.x,profile.y,profile.z,profile.radius);}
   else s.error="Bot login refused; account may already be online";
   // Reservations above prevent the next admission in this batch from overfilling a slot.
  }
  counts["admission_attempts"]=attempts;counts["admissions_queued"]=admitted;counts["level60_reserved"]=online60;
 }
 for(auto const& slot:s.legalSlots){unsigned wanted=s.targets.count(slot)?s.targets[slot]:s.perSlot;if(occupied[slot]<wanted)deficits.push_back({{"slot",slot},{"missing",wanted-occupied[slot]}});}
 unsigned pool60=0,eligible60=0;for(auto const& p:s.profiles)if(!p.withdrawn&&std::max(p.level,unsigned(p.ai.seedLevel))>=60){++pool60;if((!s.retryAt.count(p.ai.characterGuid)||int32(now-s.retryAt[p.ai.characterGuid])>=0)&&!s.managed.count(p.ai.characterGuid)&&!ObjectAccessor::FindPlayer(ObjectGuid(HIGHGUID_PLAYER,p.ai.characterGuid))&&(!s.cooldownAt.count(p.ai.characterGuid)||WorldBotPopulation::Expired(now,s.cooldownAt[p.ai.characterGuid],s.cooldown60)))++eligible60;}
 json output={{"schema_version",1},{"available",true},{"revision",s.revision},{"enabled",s.enabled},{"runtime_state",!s.error.empty()?"error":s.profiles.empty()?"provisioning_required":s.enabled?"running":"paused"},{"counts",counts},{"profiles",rows},{"deficits",deficits},{"error",s.error},{"login_batch_size",s.loginBatch},{"level60_max_online",s.cap60},{"level60_pool",pool60},{"level60_eligible_offline",eligible60},{"level60_deficit",WorldBotPopulation::Deficit(ready60,s.cap60)},{"limitations",{"Only provisioned persistent WB characters; no autonomous world chat","Protected activity defers rotation; admission cap does not forcibly evict real-player companions"}}};
 try{Write(StatusPath(),output);}catch(std::exception const& e){sLog.Out(LOG_BASIC,LOG_LVL_ERROR,"[WorldBotManager] %s",e.what());}
}
}
