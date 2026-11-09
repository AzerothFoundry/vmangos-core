#!/usr/bin/env python3
"""Compile the production waypoint methods with a minimal native-movement seam."""
import argparse,pathlib,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--source-root',default=str(pathlib.Path(__file__).resolve().parents[1]));p.add_argument('--baseline',action='store_true');a=p.parse_args();root=pathlib.Path(a.source_root)
src=subprocess.check_output(['git','-C',str(root),'show','c296c486:src/game/PlayerBots/BattleBotWaypoints.cpp'],text=True) if a.baseline else (root/'src/game/PlayerBots/BattleBotWaypoints.cpp').read_text()
ai_src=subprocess.check_output(["git","-C",str(root),"show","c296c486:src/game/PlayerBots/BattleBotAI.cpp"],text=True) if a.baseline else (root/"src/game/PlayerBots/BattleBotAI.cpp").read_text()
def extract(name):
 global src
 source=src;src=ai_src if name=="BattleBotAI::StopMoving" else source
 start=src.index('void '+name+'(');begin=src.index('{',start);depth=1;i=begin+1
 while depth:
  if src[i]=='{':depth+=1
  elif src[i]=='}':depth-=1
  i+=1
 result=src[start:i];src=source;return result
methods=['BattleBotAI::StopMoving','BattleBotAI::MovementInform','BattleBotAI::MoveToNextPoint','BattleBotAI::ClearPath','MoveToNextPointSpecial']
if not a.baseline:methods.append('BattleBotAI::AdvanceLegacyWaypoint')
prefix=r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include "BattleBotWaypointPolicy.h"
using uint32=std::uint32_t;
constexpr uint32 POINT_MOTION_TYPE=1,MOVE_NONE=0,MOVE_PATHFINDING=1,MOVE_EXCLUDE_STEEP_SLOPES=128;
float frand(float,float){return 0;}
class BattleBotAI;
struct BattleBotWaypoint{float x=0,y=0,z=0;void(*pFunc)(BattleBotAI*)=nullptr;};
using BattleBotPath=std::vector<BattleBotWaypoint>;
struct Motion{BattleBotAI* owner=nullptr;bool replay=false,replayClear=false;uint32 id=0;float x=0,y=0,z=0;std::vector<uint32> issued;void MovePoint(uint32,float,float,float,uint32);void Clear();void MoveIdle(){};};
struct Player{bool combat=false,alive=true,world=true,teleport=false;float x=0,y=0,z=0;Motion motion;bool IsInWorld(){return world;}bool IsBeingTeleported(){return teleport;}bool IsInCombat(){return combat;}bool IsAlive(){return alive;}Motion* GetMotionMaster(){return &motion;}void StopMoving(){}float GetDistance(float a,float b,float c){return std::sqrt((x-a)*(x-a)+(y-b)*(y-b)+(z-c)*(z-c));}};
class BattleBotAI{public:Player storage;Player* me=&storage;BattleBotPath* m_currentPath=nullptr;uint32 m_currentPoint=0,m_avRouteStaleAt=0;bool m_movingInReverse=false,m_wsgTravel=false;BattleBotWaypointPolicy::Ticket m_waypointTicket;unsigned activations=0,actions=0;bool ShouldIgnoreCombat(){return false;}void ActivateNearbyAreaTrigger(){++activations;}void StopMoving();void MovementInform(uint32,uint32);void MoveToNextPoint();void AdvanceLegacyWaypoint(uint32,bool);void ClearPath();};
void Motion::MovePoint(uint32 next,float a,float b,float c,uint32){auto old=id;id=next;x=a;y=b;z=c;issued.push_back(next);if(replay&&owner&&old)owner->MovementInform(POINT_MOTION_TYPE,old);}
void Motion::Clear(){auto old=id;id=0;if(owner&&replayClear&&old)owner->MovementInform(POINT_MOTION_TYPE,old);}
void arrive(BattleBotAI& ai){auto& m=ai.storage.motion;ai.storage.x=m.x;ai.storage.y=m.y;ai.storage.z=m.z;ai.MovementInform(POINT_MOTION_TYPE,m.id);}
void restart(BattleBotAI* ai){++ai->actions;ai->m_currentPoint=std::numeric_limits<uint32>::max();ai->MoveToNextPoint();}
'''
base=r'''
int main(){BattleBotAI ai;BattleBotPath path(12);ai.m_currentPath=&path;bool threw=false;try{ai.MovementInform(POINT_MOTION_TYPE,12);}catch(std::out_of_range const& e){threw=true;std::cout<<"REPRODUCED_BASELINE "<<e.what()<<"\n";}assert(threw);BattleBotAI other;BattleBotPath longer(13),shorter(12);other.storage.motion.owner=&other;other.storage.motion.replay=true;other.m_currentPath=&longer;other.m_currentPoint=11;other.MoveToNextPoint();other.ClearPath();other.m_currentPath=&shorter;other.m_currentPoint=0;threw=false;try{other.MoveToNextPoint();}catch(std::out_of_range const& e){threw=true;std::cout<<"REPRODUCED_REPLACE_FINALIZE "<<e.what()<<"\n";}assert(threw);}
'''
fixed=r'''
int main(){
 const uint32 before=std::numeric_limits<uint32>::max();BattleBotPath path(12);for(unsigned i=0;i<12;++i)path[i].x=float(i*10);
 {BattleBotAI ai;ai.m_currentPath=&path;ai.m_currentPoint=before;ai.MoveToNextPoint();for(unsigned i=0;i<12;++i){assert(ai.m_currentPoint==i);arrive(ai);}assert(!ai.m_currentPath);assert(ai.storage.motion.issued.size()==12&&ai.activations==12);}
 {BattleBotAI ai;ai.m_currentPath=&path;ai.m_movingInReverse=true;ai.m_currentPoint=path.size();ai.MoveToNextPoint();for(int i=11;i>=0;--i){assert(ai.m_currentPoint==unsigned(i));arrive(ai);}assert(!ai.m_currentPath);assert(ai.storage.motion.issued.size()==12);}
 {BattleBotAI ai;BattleBotPath empty;ai.m_currentPath=&empty;ai.MoveToNextPoint();assert(!ai.m_currentPath&&ai.storage.motion.issued.empty());ai.m_currentPath=&path;ai.m_currentPoint=12;ai.MoveToNextPoint();assert(!ai.m_currentPath);}
 {BattleBotAI ai;BattleBotPath single(1);ai.m_currentPath=&single;ai.m_currentPoint=before;ai.MoveToNextPoint();arrive(ai);assert(!ai.m_currentPath&&ai.storage.motion.issued.size()==1);}
 {BattleBotAI ai;ai.storage.motion.owner=&ai;ai.storage.motion.replay=true;ai.m_currentPath=&path;ai.m_currentPoint=before;ai.MoveToNextPoint();auto stale=ai.storage.motion.id;BattleBotPath shorter(2);ai.m_currentPath=&shorter;ai.m_currentPoint=before;ai.MoveToNextPoint();auto current=ai.storage.motion.id;assert(current!=stale&&ai.m_currentPoint==0);ai.MovementInform(POINT_MOTION_TYPE,stale);ai.MovementInform(POINT_MOTION_TYPE,12);assert(ai.m_currentPoint==0&&ai.m_waypointTicket.pending==current);arrive(ai);assert(ai.m_currentPoint==1);auto count=ai.storage.motion.issued.size();ai.MovementInform(POINT_MOTION_TYPE,current);assert(ai.storage.motion.issued.size()==count);}
 {BattleBotAI ai;ai.m_currentPath=&path;ai.m_currentPoint=before;ai.MoveToNextPoint();auto stale=ai.storage.motion.id;ai.m_avRouteStaleAt=1234;ai.ClearPath();assert(ai.m_avRouteStaleAt==0);ai.MovementInform(POINT_MOTION_TYPE,stale);assert(!ai.m_currentPath&&ai.activations==0);}
 {BattleBotAI ai;ai.m_currentPath=&path;ai.m_currentPoint=before;ai.MoveToNextPoint();ai.storage.x=1000;ai.MovementInform(POINT_MOTION_TYPE,ai.storage.motion.id);assert(!ai.m_currentPath&&ai.actions==0&&ai.activations==0);}
 {BattleBotAI ai;BattleBotPath loop(1);loop[0].pFunc=restart;ai.m_currentPath=&loop;ai.m_currentPoint=before;ai.MoveToNextPoint();for(unsigned i=0;i<4;++i){auto old=ai.storage.motion.id;arrive(ai);assert(ai.actions==i+1);ai.MovementInform(POINT_MOTION_TYPE,old);assert(ai.actions==i+1);}assert(ai.storage.motion.issued.size()==5);}
 {BattleBotAI ai;ai.m_currentPath=&path;ai.m_currentPoint=before;MoveToNextPointSpecial(&ai);assert(ai.m_currentPoint==0&&BattleBotWaypointPolicy::IsTicket(ai.storage.motion.id));arrive(ai);assert(ai.m_currentPoint==1);}
 {BattleBotAI ai;ai.storage.combat=true;ai.m_wsgTravel=true;ai.m_currentPath=&path;ai.m_currentPoint=before;ai.MoveToNextPoint();assert(ai.m_currentPath);MoveToNextPointSpecial(&ai);assert(!ai.m_currentPath);}
 {BattleBotAI ai;ai.m_currentPath=&path;ai.m_currentPoint=before;ai.MoveToNextPoint();ai.storage.world=false;arrive(ai);assert(!ai.m_currentPath&&ai.activations==0);}
 {BattleBotAI ai;BattleBotPath single(1);single[0].pFunc=restart;ai.storage.motion.owner=&ai;ai.storage.motion.replayClear=true;ai.m_currentPath=&single;ai.m_currentPoint=before;ai.MoveToNextPoint();ai.storage.x=ai.storage.motion.x;ai.storage.y=ai.storage.motion.y;ai.storage.z=ai.storage.motion.z;auto ticket=ai.storage.motion.id;ai.StopMoving();ai.MovementInform(POINT_MOTION_TYPE,ticket);assert(ai.actions==0&&ai.activations==0&&!ai.m_waypointTicket.pending);}
 std::cout<<"PASS production forward/reverse/sentinels/end/empty/single/loop/replace/reentrant-stale/duplicate/cancel/special\n";
}
'''
with tempfile.TemporaryDirectory(prefix='waypoint-regression-') as t:
 cpp=pathlib.Path(t)/'case.cpp';exe=pathlib.Path(t)/'case';cpp.write_text(prefix+'\n'+'\n'.join(extract(n) for n in methods)+'\n'+(base if a.baseline else fixed));subprocess.run(['c++','-std=c++14','-I'+str(root/'src/game/PlayerBots'),str(cpp),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
