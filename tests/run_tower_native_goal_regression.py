#!/usr/bin/env python3
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[1];source=(root/'src/game/PlayerBots/WorldBotTowerTask.cpp').read_text();start=source.index('struct Goal {');end=source.index('void Status(uint32 now)',start);actual=source[start:end]
cpp=r"""
#include "WorldBotTowerPolicy.h"
#include <memory>
#include <algorithm>
#include <map>
#include <vector>
#include <cmath>
#include <cassert>
#include <utility>
using uint32=unsigned;constexpr int PATHFIND_NORMAL=1,PATHFIND_INCOMPLETE=4;
unsigned loaded=0;bool requireCorridor=false;struct Map{float ground=11;void ForceLoadGridsAroundPosition(float,float){++loaded;}float GetHeight(float,float,float,bool,float){return ground;}};
struct Player{Map map;unsigned guid=5;float px=100;float GetDistanceSqr(float x,float y,float z){return (x-px)*(x-px)+(y-200)*(y-200)+(z-11)*(z-11); }float GetPositionX(){return px;}float GetPositionY(){return 200;}float GetPositionZ(){return 11;}unsigned GetGUIDLow(){return guid;}Map* GetMap(){return &map;}};
struct Info{struct{float radius=80;}capturePoint;};struct GameObject{Info const* GetGOInfo(){static Info info;return &info;}unsigned GetGUIDLow(){return 9;}float GetPositionX(){return 100;}float GetPositionY(){return 200;}float GetPositionZ(){return 10;}};
struct Point{float x,y,z;};bool available=true,blockLeg=false,shortOnly=false,stageMode=false,blockStage=false,noStageProgress=false;unsigned cornerCalls=0,smoothCalls=0;float onlyX=NAN;unsigned calculations=0;
struct PathInfo{bool calculated=false,straight=false,partial=false;std::vector<Point>path;Player* owner;PathInfo(Player* p,unsigned=0):owner(p){}void ExcludeSteepSlopes(){}void setUseStrightPath(bool value){straight=value;}bool calculate(float x,float y,float z){assert(!calculated);calculated=true;++calculations;if(straight)++cornerCalls;else ++smoothCalls;if(stageMode&&straight&&x>0){partial=true;path.push_back({owner->GetPositionX(),200,11});path.push_back({owner->GetPositionX()+(noStageProgress?2.f:200.f),200,11});return true;}bool success=available&&(!stageMode||!blockStage)&&(!blockLeg||straight)&&(!shortOnly||straight||std::fabs(x-owner->GetPositionX())<20)&&(!requireCorridor||loaded>=4)&&(std::isnan(onlyX)||std::fabs(x-onlyX)<.001f);if(success){path.push_back({owner->GetPositionX(),owner->GetPositionY(),owner->GetPositionZ()});path.push_back({x,y,z});}return success;}int getPathType(){return partial||path.empty()?4:1;}auto const& getPath(){return path;}float Length(){assert(!path.empty());return 123;}};
"""+actual+r"""
int main(){Player p;GameObject go;auto valid=NativeGoal(&p,&go,1000);assert(valid.valid&&valid.type==1&&valid.length==123&&valid.z==11);Player other;other.guid=9;onlyX=valid.x;auto reused=NativeGoal(&other,&go,1500);assert(reused.valid&&std::fabs(reused.x-valid.x)<.001f);onlyX=NAN;float legX=500,legY=200,legZ=11;assert(NativeTravelLeg(&p,legX,legY,legZ));assert(std::fabs(legX-160)<.01f);assert(cornerCalls>0&&smoothCalls>0);blockLeg=true;legX=500;assert(!NativeTravelLeg(&p,legX,legY,legZ));blockLeg=false;p.map.ground=16;legX=500;assert(NativeTravelLeg(&p,legX,legY,legZ));assert(legZ==16);p.map.ground=30;legX=500;assert(!NativeTravelLeg(&p,legX,legY,legZ));p.map.ground=11;shortOnly=true;legX=500;assert(NativeTravelLeg(&p,legX,legY,legZ));assert(std::fabs(legX-115)<.01f);shortOnly=false;auto count=calculations;assert(NativeGoal(&p,&go,2000).valid&&calculations==count);available=false;auto fail=NativeGoal(&p,&go,32000);assert(!fail.valid&&fail.length==0&&fail.points==0);count=calculations;assert(!NativeGoal(&p,&go,33000).valid&&calculations==count);available=true;assert(NativeGoal(&p,&go,38000).valid);goals.clear();requireCorridor=true;loaded=0;p.px=-1000;auto bridged=NativeGoal(&p,&go,45000);assert(bridged.valid&&bridged.beforeType==4&&bridged.gridSamples>0&&bridged.gridSamples<=24);requireCorridor=false;goals.clear();p.map.ground=1000;assert(!NativeGoal(&p,&go,50000).valid);
goals.clear();p.map.ground=11;p.px=-1000;stageMode=true;auto staged=NativeGoal(&p,&go,60000);assert(staged.valid&&staged.staged&&staged.type==4&&staged.x==-800&&staged.captureX>0&&staged.remaining>0);
// Arrival at a stage invalidates the cached candidate instead of holding there.
p.px=-800;auto next=NativeGoal(&p,&go,60001);assert(next.valid&&next.staged&&next.x==-600);
goals.clear();blockStage=true;assert(!NativeGoal(&p,&go,70000).valid);blockStage=false;noStageProgress=true;goals.clear();assert(!NativeGoal(&p,&go,80000).valid);
}
"""
with tempfile.TemporaryDirectory() as d:
 path=Path(d)/'case.cpp';exe=Path(d)/'case';path.write_text(cpp);subprocess.run(['c++','-std=c++17','-I'+str(root/'src/game/PlayerBots'),str(path),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS extracted native goal: nearby floor, legal route, success/failure cache, empty path no Length assertion, roof rejected')
