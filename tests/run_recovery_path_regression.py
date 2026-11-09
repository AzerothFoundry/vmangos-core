#!/usr/bin/env python3
"""Compile the production approach function with deterministic geometry/native-API doubles.

This verifies wiring and route safety decisions, not live mmap, aggro or packet behavior.
"""
import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
source=(root/'src/game/PlayerBots/PartyBotAI.cpp').read_text()
a=source.index('bool SafeResurrectionApproach(')
b=source.index('\n}\n}\n\nnamespace\n{\nbool SafeResurrectionMotion',a)+2
c=source.index('bool SafeResurrectionMotion(')
end=source.index('\n}\n}\n\nbool PartyBotAI::OwnsResurrectionApproach()',c)+2
fixture=r'''
#include "PartyBotRecoveryPolicy.h"
#include <vector>
#include <list>
#include <cassert>
#include <iostream>
using uint32=unsigned;using int32=int;
constexpr int PATHFIND_NORMAL=1, TYPEID_UNIT=3;
struct Vec {float x=0,y=0,z=0; Vec operator-(Vec v)const{return {x-v.x,y-v.y,z-v.z};}
 Vec operator+(Vec v)const{return {x+v.x,y+v.y,z+v.z};} Vec operator*(float t)const{return {x*t,y*t,z*t};}
 float length()const{return std::sqrt(x*x+y*y+z*z);}};
struct Unit {Vec pos;float reach=0;bool alive=true;
 bool IsAlive(){return alive;} int GetTypeId(){return TYPEID_UNIT;}
 float GetPositionX(){return pos.x;}float GetPositionY(){return pos.y;}float GetPositionZ(){return pos.z;}
 float GetCombatReach(){return reach;}};
struct Creature:Unit {float aggro=10;float GetAttackDistance(Unit*){return aggro;}};
std::list<Unit*> enemies;
struct Spline {std::vector<Vec> points{{0,0,0},{100,0,0}};int index=0;
 bool Initialized(){return !points.empty();}bool Finalized(){return false;}
 Vec ComputePosition(){return points[index];}int _currentSplineIdx(){return index;}
 uint32 CountSplinePoints(){return points.size()-1;}Vec GetPoint(int i){return points[i];}};
struct Player:Unit {float losAfter=0;Spline spline;Spline* movespline=&spline;
 bool IsWithinDist3d(float x,float y,float z,float r){return (pos-Vec{x,y,z}).length()<=r;}
 bool IsWithinLOS(float x,float,float){return x>=losAfter;}
 void GetEnemyListInRadiusAround(Player*,float,std::list<Unit*>& out){out=enemies;}};
struct SpellEntry {int rangeIndex=1;};struct SpellRangeEntry {float maxRange=30;};
struct RangeStore {SpellRangeEntry entry;SpellRangeEntry* LookupEntry(int){return &entry;}} sSpellRangeStore;
namespace Spells {float GetSpellMaxRange(SpellRangeEntry const* r){return r->maxRange;}}
bool normal=true;
struct PathFinder {Player* actor;std::vector<Vec> route;
 explicit PathFinder(Player* p):actor(p){} void ExcludeSteepSlopes(){}void setPathLengthLimit(float){}
 bool calculate(float x,float y,float z){route={actor->pos,{x,y,z}};return true;}
 int getPathType(){return normal?PATHFIND_NORMAL:2;}float Length(){return (route.back()-route.front()).length();}
 const std::vector<Vec>& getPath(){return route;}};
'''
tests=r'''
int main(){
 Player actor,corpse;corpse.pos={100,0,0};SpellEntry spell;float x=0,y=0,z=0;
 assert(SafeResurrectionApproach(&actor,&corpse,&spell,x,y,z));
 assert(x>=72 && x<=74); // First range/LOS point, not the corpse centre at 100.
 Creature enemy;enemy.pos={90,0,0};enemies={&enemy};
 assert(SafeResurrectionApproach(&actor,&corpse,&spell,x,y,z)); // Enemy beyond chosen stop.
 assert(!SafeResurrectionMotion(&actor)); // An old actual path to 100 is unsafe despite a safe new plan to 72.
 actor.spline.points={{0,0,0},{60,0,0},{100,0,0}};actor.spline.index=1;enemy.pos={20,0,0};
 assert(SafeResurrectionMotion(&actor)); // Completed segments behind the actor are excluded.
 enemy.pos={80,0,0};assert(!SafeResurrectionMotion(&actor)); // Future segment still checked.
 enemy.pos={50,0,0};assert(!SafeResurrectionApproach(&actor,&corpse,&spell,x,y,z)); // Mid-segment ADD.
 enemy.pos={50,14,0};assert(SafeResurrectionApproach(&actor,&corpse,&spell,x,y,z));
 enemy.aggro=30;assert(!SafeResurrectionApproach(&actor,&corpse,&spell,x,y,z)); // Native aggro range used.
 enemies.clear();corpse.losAfter=90;assert(SafeResurrectionApproach(&actor,&corpse,&spell,x,y,z));assert(x>=90);
 normal=false;assert(!SafeResurrectionApproach(&actor,&corpse,&spell,x,y,z));normal=true;
 corpse.pos={121,0,0};assert(!SafeResurrectionApproach(&actor,&corpse,&spell,x,y,z));
 std::cout<<"production approach: earliest range/LOS stop, no mid-segment ADD, native aggro radius, complete bounded path PASS\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d);(p/'test.cpp').write_text(fixture+source[a:b]+source[c:end]+tests)
 subprocess.run(['c++','-std=c++14','-Wall','-Wextra','-I'+str(root/'src/game/PlayerBots'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
