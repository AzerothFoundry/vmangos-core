#!/usr/bin/env python3
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[1];source=(root/'src/game/Maps/MapManager.cpp').read_text();start=source.index('Map* MapManager::CreateContinentMap(');brace=source.index('{',start);depth=1;end=brace+1
while depth:
 if source[end]=='{':depth+=1
 if source[end]=='}':depth-=1
 end+=1
body=source[start:end]
cpp=r"""
#include <map>
#include <utility>
#include <cmath>
#include <cassert>
namespace MaNGOS{bool IsValidMapCoord(float x,float y){return std::fabs(x)<17066&&std::fabs(y)<17066;}}
using uint32=unsigned;struct MapEntry{bool Instanceable()const{return false;}};struct Store{template<class T>T const* LookupEntry(unsigned)const{static T entry;return &entry;}}sMapStorage;
struct Map{unsigned id,instance;unsigned data=0,active=0;Map(unsigned i,unsigned n):id(i),instance(n){}void CreateInstanceData(bool){++data;}void SpawnActiveObjects(){++active;}};struct WorldMap:Map{WorldMap(unsigned id,unsigned,unsigned instance):Map(id,instance){}};
struct Scripts{unsigned calls=0;void MapLoaded(unsigned,Map*){++calls;}}sZoneScriptMgr;
using MapID=std::pair<unsigned,unsigned>;
struct MapManager{struct Guard{Guard(MapManager&){}};unsigned i_gridCleanUpDelay=100;std::map<MapID,Map*>i_maps;unsigned GetContinentInstanceId(unsigned,float x,float){return x>0?17:18;}Map* FindMap(unsigned id,unsigned instance){auto it=i_maps.find({id,instance});return it==i_maps.end()?nullptr:it->second;}Map* CreateContinentMap(unsigned,float,float);};
"""+body+r"""
int main(){MapManager mgr;auto north=mgr.CreateContinentMap(0,2574,-4794);assert(north&&north->instance==17&&north->data==1&&north->active==1);assert(sZoneScriptMgr.calls==1);assert(mgr.CreateContinentMap(0,2574,-4794)==north&&sZoneScriptMgr.calls==1);auto south=mgr.CreateContinentMap(0,-100,-500);assert(south&&south!=north&&south->instance==18);assert(!mgr.CreateContinentMap(30,0,0));assert(!mgr.CreateContinentMap(0,999999,0));assert(!mgr.CreateContinentMap(0,NAN,0));assert(!mgr.CreateContinentMap(1,0,INFINITY));assert(sZoneScriptMgr.calls==2);}
"""
with tempfile.TemporaryDirectory() as d:
 path=Path(d)/'case.cpp';exe=Path(d)/'case';path.write_text(cpp);subprocess.run(['c++','-std=c++17',str(path),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS extracted native continent factory: correct instance, reuse, one-time setup, invalid map/coordinates')
