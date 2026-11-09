#!/usr/bin/env python3
from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[1];s=(r/'src/game/PlayerBots/PartyBotAI.cpp').read_text();formation=(r/'src/game/PlayerBots/PartyBotFormation.cpp').read_text()
# Check production scheduling: mechanic overrides and running casts/rescue own
# their actions before the general placement lane; valid placement suppresses
# only legacy movement, not class combat actions.
a=s.index('    if(RaidCoordination::UpdatePull(*this))return;');b=s.index('void PartyBotAI::UpdateOutOfCombatAI()',a);lane=s[a:b]
assert lane.index('McBotTactics::Update')<lane.index('PartyBotFormation::Update')
normal=lane.index('if (me->IsInCombat()) {\n        if (TryEmergencyHeal()) return;')
assert lane.index('if (DungeonTank::Protect')<normal
assert lane.index('if (me->IsNonMeleeSpellCasted(false, false, true))')<normal
assert lane.index('TryEmergencyHeal()',normal)<lane.index('PartyBotFormation::Update',normal)
assert lane.index('CheckForDispelTargets()',normal)<lane.index('PartyBotFormation::Update',normal)
assert '!PartyBotFormation::Holding(*this)' in lane and lane.rfind('UpdateInCombatAI();')>normal
# Execute the actual native adapter validation lane, with engine seams that
# expose invalid mesh, LOS and idle-NPC contact between sparse route points.
a=formation.index('bool Validate(');b=formation.index('\n}\nbool Update',a);actual=formation[a:b]
cpp=r"""
#include "PartyBotFormationPolicy.h"
#include <list>
#include <vector>
#include <cmath>
#include <cassert>
constexpr unsigned TYPEID_UNIT=3,PATHFIND_NORMAL=1;
struct Unit{virtual ~Unit(){}float x=5,y=1,z=0;float GetPositionX(){return x;}float GetPositionY(){return y;}float GetPositionZ(){return z;}unsigned GetTypeId(){return 3;}bool IsAlive(){return true;}bool IsInCombat(){return false;}void*GetCharmerOrOwnerPlayerOrPlayerItself(){return nullptr;}bool IsVisibleForOrDetect(void*,void*,bool){return true;}float GetObjectBoundingRadius(){return .5;}};
struct Creature:Unit{float GetAttackDistance(void*){return 2;}};
struct Map{bool los=true;float GetHeight(float,float,float,bool,float){return 0;}bool isInLineOfSight(float,float,float,float,float,float,bool,bool){return los;}};
struct Player{Map map;bool danger=false;Creature enemy;Map*GetMap(){return &map;}bool IsValidAttackTarget(Unit*){return true;}void GetEnemyListInRadiusAround(Player*,float,std::list<Unit*>& out){if(danger)out.push_back(&enemy);}};
struct PartyBotAI{Player*me;};struct Point{float x,y,z;};unsigned type=1;
struct PathInfo{std::vector<Point>points{{0,0,0},{10,0,0}};PathInfo(Player*){}void ExcludeSteepSlopes(){}bool calculate(float,float,float){return true;}auto const&getPath(){return points;}unsigned getPathType(){return type;}float Length(){return 10;}};
"""+actual+r"""
int main(){Player p;PartyBotAI ai{&p};Unit anchor;anchor.x=10;anchor.y=0;float z=0;assert(Validate(ai,&anchor,10,0,z));p.danger=true;assert(!Validate(ai,&anchor,10,0,z));p.danger=false;type=4;assert(!Validate(ai,&anchor,10,0,z));type=1;p.map.los=false;assert(!Validate(ai,&anchor,10,0,z));}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'case.cpp';exe=Path(d)/'case';p.write_text(cpp);subprocess.run(['c++','-std=c++17','-I'+str(r/'src/game/PlayerBots'),str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS actual placement adapter: safe native path, idle aggro between nodes rejected, incomplete path/LOS rejected; production mechanic/cast/rescue/heal ordering and legacy movement suppression')
