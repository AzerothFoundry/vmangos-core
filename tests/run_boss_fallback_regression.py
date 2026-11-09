#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[1];s=(r/'src/game/Commands/GmUiCommands.cpp').read_text();a=s.index('            bool verifiedSafe=GmInstanceBoss::SafePoint');b=s.index('\n        }',a);fragment=s[a:b]
# The executable uses the actual action lane; verify all existing eligibility
# checks and context binding occur before it, for both initial and fallback calls.
pre=s[s.index('            bool confirmedFallback=false;'):a]
for token in ['SameContext','!self->IsAlive()','self->IsInCombat()','IsEncounterInProgress','Registered','c->IsInCombat()']:
 assert token in pre,token
assert 'SaveRecallPosition' not in s[s.index('if(!verifiedSafe&&!confirmedFallback)'):s.index('if(!verifiedSafe){auto const& position=')]
cpp=r"""
#include <cassert>
#include <sstream>
#include <string>
#include <vector>
namespace Movement{struct Vector3{float x=0,y=0,z=0;};}
namespace MaNGOS{bool IsValidMapCoord(float x,float,float,float){return x>=0;}}
struct Position{unsigned mapId=409;float x=10,y=20,z=30,o=0;};struct Data{Position position;};struct Spawn{Data*data;};
struct Player{unsigned recalls=0,teleports=0;Movement::Vector3 dest;unsigned GetMapId(){return 409;}unsigned GetInstanceId(){return 7;}float GetOrientation(){return 0;}void SaveRecallPosition(){++recalls;}bool NearTeleportTo(float x,float y,float z,float){++teleports;dest={x,y,z};return true;}};
bool safeFound=false;namespace GmInstanceBoss{bool SafePoint(Player*,Spawn const&,int,Movement::Vector3& p){p={100,200,300};return safeFound;}}
struct C{Player* self;Spawn*anchor;int spawns=0,n=12118;bool confirmedFallback;std::vector<std::string> messages;void SendSysMessage(char const* s){messages.push_back(s);}bool error(char const*){return false;}bool ok(char const* s){messages.push_back(s);return true;}bool run(){Movement::Vector3 safe;
"""+fragment+r"""
}};
int main(){Player p;Data d;Spawn anchor{&d};C command{&p,&anchor,0,12118,false};assert(command.run());assert(p.recalls==0&&p.teleports==0);assert(command.messages[0].find("BOSSUNSAFE")!=std::string::npos&&command.messages.back()=="GMUI|DONE|boss_unsafe");command.confirmedFallback=true;assert(command.run());assert(p.teleports==1&&p.dest.x==10);assert(command.messages.back()=="Teleported to unverified Boss anchor");safeFound=true;assert(command.run());assert(p.teleports==2&&p.dest.x==100);safeFound=false;d.position.x=-1;assert(!command.run());assert(p.teleports==2);}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'case.cpp';exe=Path(d)/'case';p.write_text(cpp);subprocess.run(['c++','-std=c++17',str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS actual fallback action lane: warning no mutation, explicit native anchor, prefer verified point, invalid anchor refusal; existing guards precede both calls')
