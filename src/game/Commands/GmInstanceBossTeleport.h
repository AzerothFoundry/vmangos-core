// Native spawns are anchors only; never summon, load or alter an encounter.
#ifndef GM_INSTANCE_BOSS_TELEPORT_H
#define GM_INSTANCE_BOSS_TELEPORT_H
#include "GmInstanceBossCatalog.h"
#include "GmBossThreatFaction.h"
#include "GmBossLiquidSafety.h"
#include "GmBossCollisionData.h"
#include "GmBossPathSafety.h"
#include "GmBossRouteReference.h"
#include "GridMap.h"
#include "GmInstanceBossAnchors.h"
#include "PathFinder.h"
#include "GridSearchers.h"
#include "VMapFactory.h"
namespace GmInstanceBoss {
struct Spawn { uint32 guid; CreatureData const* data; };
inline std::vector<Spawn> Spawns(uint32 map)
{
    std::vector<Spawn> result;
    auto collect = [&](CreatureDataPair const& pair) {
        if (pair.second.position.mapId == map) result.push_back({pair.first,&pair.second});
        return false;
    };
    sObjectMgr.DoCreatureData(collect);
    std::sort(result.begin(),result.end(),[](Spawn const& a,Spawn const& b){return a.guid<b.guid;});
    return result;
}
inline bool Registered(uint32 map,uint32 entry)
{
    for (auto const& row : GmInstanceBossCatalog) if(row.map==map && row.entry==entry) return true;
    return false;
}
inline std::map<uint32,Creature*> LoadedBosses(Player* self)
{
    std::map<uint32,Creature*> result;
    for(auto c:self->GetMap()->GetLoadedCreatures()) {
        if(!Registered(self->GetMapId(),c->GetEntry()))continue;
        auto found=result.find(c->GetEntry());
        if(found==result.end() || c->GetObjectGuid()<found->second->GetObjectGuid()) result[c->GetEntry()]=c;
    }
    return result;
}
inline Spawn const* Anchor(std::vector<Spawn> const& spawns,uint32 entry)
{
    for(auto const& spawn:spawns) if(spawn.data->HasCreatureId(entry)) return &spawn;
    return nullptr;
}
inline bool Fallback(std::vector<Spawn> const& spawns,uint32 map,uint32 entry,CreatureData& target)
{
    for(auto const& row:GmInstanceBossAnchors) if(row.map==map && row.entry==entry) {
        if(row.parent) {
            auto parent=Anchor(spawns,row.parent);if(!parent)return false;
            target=*parent->data;
        } else {
            target.position.mapId=map;target.position.x=row.x;target.position.y=row.y;target.position.z=row.z;target.position.o=row.o;
        }
        target.creature_id.fill(0);target.creature_id[0]=entry;
        return true;
    }
    return false;
}
inline FactionTemplateEntry const* ThreatFaction(Player* self)
{
    uint32 id=ThreatFactionId(self->IsGameMaster(),self->GetFactionTemplateId(),Player::GetFactionForRace(self->GetRace()));
    return sObjectMgr.GetFactionTemplateEntry(id);
}
inline float StaticAggro(Player* self,CreatureData const& spawn)
{
    float result=0;
    for(uint32 id:spawn.creature_id) {
        auto info=sObjectMgr.GetCreatureTemplate(id);if(!info)continue;
        auto faction=sObjectMgr.GetFactionTemplateEntry(info->faction);
        if(!ThreatFactionHostile(faction,ThreatFaction(self)))continue;
        if(info->detection_range<1)continue;
        int difference=std::max(-MAX_LEVEL_DIFF_FOR_AGGRO_RANGE,int(self->GetLevel())-int(info->rank==3?std::max(info->level_max,self->GetLevel()+3):info->level_max));
        float range=std::max(std::min(info->detection_range,5.f),info->detection_range-float(difference)+self->GetTotalAuraModifier(SPELL_AURA_MOD_DETECTED_RANGE));
        result=std::max(result,AggroStagingClearance(range*sWorld.getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO)));
    }
    return result;
}
struct Diagnostic {
    uint32 candidates=0,mapMissing=0,height=0,liquid=0,bossAggro=0,spawnAggro=0,actorAggro=0,los=0,path=0,preflight=0;
    uint32 firstBlock=0,firstPathType=0,failedSegment=0,corner=0,routeTele=0; Movement::Vector3 route; float firstHeight=0,firstRadius=0,firstAggro=0,firstPathLength=0,firstPathError=0;
};
inline bool SafePoint(Player* self,Spawn const& boss,std::vector<Spawn> const& spawns,Movement::Vector3& found,Diagnostic* diagnostic=nullptr)
{
    Diagnostic local; Diagnostic& d=diagnostic?*diagnostic:local;
    Map* map=self->GetMap();
    auto vm=VMAP::VMapFactory::createOrGetVMapManager();
    if(!vm->isHeightCalcEnabled() || !vm->isLineOfSightCalcEnabled()){d.preflight=1;return false;}
    auto const& p=boss.data->position;
    if(!CollisionDataPresent(self->GetMapId(),p.x,p.y)){d.preflight=2;return false;}
    Movement::Vector3 route(p.x,p.y,p.z);d.route=route;
    d.routeTele=NativeRouteReferenceId(self->GetMapId(),boss.data->creature_id[0]);
    if(d.routeTele) {
        auto reference=sObjectMgr.GetGameTele(d.routeTele);
        if(!reference || !RouteReferenceInBounds(self->GetMapId(),boss.data->creature_id[0],reference->mapId,p.x,p.y,p.z,reference->x,reference->y,reference->z)) {d.preflight=4;return false;}
        route=Movement::Vector3(reference->x,reference->y,reference->z);d.route=route;
        if(!CollisionDataPresent(self->GetMapId(),route.x,route.y)){d.preflight=4;return false;}
        float floor=map->GetHeight(route.x,route.y,route.z+3,true,10);
        if(!std::isfinite(floor) || std::abs(floor-route.z)>2 || std::abs(floor-p.z)>6 || !MapManager::IsValidMapCoord(self->GetMapId(),route.x,route.y,floor)){d.preflight=4;return false;}
        route.z=floor;d.route=route;GridMapLiquidData liquid{};
        if(!SafeLiquidLanding(map->GetTerrain()->getLiquidStatus(route.x,route.y,route.z,MAP_ALL_LIQUIDS,&liquid),liquid)){d.preflight=4;return false;}
    }
    // A living, patrolling or fighting boss can no longer be assumed at its spawn.
    Creature* loaded=map->GetCreature(boss.data->GetObjectGuid(boss.guid));
    if(loaded && loaded->IsInCombat()){d.preflight=3;return false;}
    auto actors=map->GetLoadedCreatures();
    bool bossPresent=std::any_of(actors.begin(),actors.end(),[&](Creature* c){return c->GetEntry()==boss.data->creature_id[0];});
    bool haveCorner=false;Movement::Vector3 corner;
    for(float radius=10;radius<=75;radius+=5) for(unsigned i=0;i<32;++i) {
        ++d.candidates;
        float angle=p.o+float(i)*float(M_PI)/16;
        float x=p.x+radius*std::cos(angle),y=p.y+radius*std::sin(angle);
        if(!CollisionDataPresent(self->GetMapId(),x,y)){++d.mapMissing;continue;}
        float z=map->GetHeight(x,y,p.z+3,true,10);
        if(!std::isfinite(z) || std::abs(z-p.z)>6 || !MapManager::IsValidMapCoord(self->GetMapId(),x,y,z)){if(!d.height){d.firstHeight=z;d.firstRadius=radius;}++d.height;continue;}
        GridMapLiquidData liquid{};
        auto liquidStatus=map->GetTerrain()->getLiquidStatus(x,y,z,MAP_ALL_LIQUIDS,&liquid);
        if(!SafeLiquidLanding(liquidStatus,liquid)){++d.liquid;continue;}
        if(!bossPresent && std::hypot(x-p.x,y-p.y)<StaticAggro(self,*boss.data)){++d.bossAggro;continue;}
        bool blocked=false;
        for(auto const& spawn:spawns) {
            Creature* c=map->GetCreature(spawn.data->GetObjectGuid(spawn.guid));
            if(c)continue; // Current live/dead position and faction supersede the spawn footprint.
            auto const& q=spawn.data->position;
            float aggro=StaticAggro(self,*spawn.data);
            if(aggro>0 && std::abs(z-q.z)<20 && std::hypot(x-q.x,y-q.y)<aggro+spawn.data->wander_distance) {if(!d.firstBlock){d.firstBlock=spawn.data->creature_id[0];d.firstAggro=aggro+spawn.data->wander_distance;}++d.spawnAggro;blocked=true;break;}
        }
        if(blocked)continue;
        for(auto c:actors) {
            if(!c->IsAlive())continue;
            bool hostile=self->IsGameMaster()?ThreatFactionHostile(c->GetFactionTemplateEntry(),ThreatFaction(self)):c->IsHostileTo(self);
            if(!hostile)continue;
            float aggro=AggroStagingClearance(c->GetAttackDistance(self));
            if(std::abs(z-c->GetPositionZ())<20 && std::hypot(x-c->GetPositionX(),y-c->GetPositionY())<aggro){if(!d.firstBlock){d.firstBlock=c->GetEntry();d.firstAggro=aggro;}++d.actorAggro;blocked=true;break;}
        }
        if(blocked)continue;
        // Native navigation must prove reachability before considering visual preference.
        PathInfo path(self); path.ExcludeSteepSlopes(); path.setPathLengthLimit(512);
        Movement::Vector3 start=route,end(x,y,z);
        bool calculated=path.calculate(start,end,false,false);
        bool usable=calculated && path.getPath().size()>=2;
        float endpointError=usable?(path.getActualEndPosition()-end).length():-1.f;
        float pathLength=usable?path.Length():0.f;
        if(!usable || path.getPathType()!=PATHFIND_NORMAL || endpointError>1 || pathLength>320){
            if(!d.path){d.firstPathType=path.getPathType();d.firstPathLength=pathLength;d.firstPathError=endpointError;}
            ++d.path;continue;
        }
        auto cast=[&](float ax,float ay,float az,float bx,float by,float bz) {
            return map->isInLineOfSight(ax,ay,az,bx,by,bz,true,false);
        };
        std::size_t failed=0;
        if(!PathSegmentsClear(path.getPath(),self->GetCollisionHeight(),cast,failed)){
            if(!d.los)d.failedSegment=uint32(failed);
            ++d.los;continue;
        }
        float center=self->GetCollisionHeight()*.5f;
        auto const& first=path.getPath().front();
        if(cast(first.x,first.y,first.z+center,x,y,z+center)){found=end;return true;}
        // Keep the first fully validated corner staging point while searching for a direct-view one.
        if(!haveCorner){haveCorner=true;corner=end;}
    }
    if(haveCorner){found=corner;d.corner=1;return true;}
    return false;
}
}
#endif
