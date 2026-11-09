#!/usr/bin/env python3
from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[1];s=(r/'src/scripts/kalimdor/silithus/temple_of_ahnqiraj/boss_skeram.cpp').read_text();a=s.index('    void Aggro(Unit *who) override');brace=s.index('{',a);depth=1;end=brace+1
while depth:
 if s[end]=='{':depth+=1
 if s[end]=='}':depth-=1
 end+=1
actual=s[a:end].replace(' override','')
cpp=r"""
#include <cassert>
unsigned const TYPE_SKERAM=0,IN_PROGRESS=1,WOW_PATCH_112=10,SAY_AGGRO_1=1,SAY_AGGRO_2=2,SAY_AGGRO_3=3;
struct Unit{};struct Map{unsigned players=40;unsigned GetPlayersCountExceptGMs(){return players;}};struct Creature{Map map;Map*GetMap(){return &map;}};struct Instance{unsigned state=0;unsigned writes=0;unsigned GetData(unsigned){return state;}void SetData(unsigned,unsigned value){state=value;++writes;}};
struct World{unsigned patch=10;unsigned GetWowPatch(){return patch;}}sWorld;unsigned PickRandomValue(unsigned a,unsigned,unsigned){return a;}void DoScriptText(unsigned,Creature*){}
struct AI{bool IsImage=false;Instance* m_pInstance=nullptr;Creature* m_creature;unsigned m_maxMeleeAllowed=999;
"""+actual+r"""
};
int main(){Creature creature;AI outside{false,nullptr,&creature};outside.Aggro(nullptr);assert(outside.m_maxMeleeAllowed==4);creature.map.players=20;outside.Aggro(nullptr);assert(outside.m_maxMeleeAllowed==2);Instance instance;AI raid{false,&instance,&creature};raid.Aggro(nullptr);assert(instance.writes==1&&raid.m_maxMeleeAllowed==2);raid.m_maxMeleeAllowed=8;raid.Aggro(nullptr);assert(raid.m_maxMeleeAllowed==8&&instance.writes==1);AI image{true,nullptr,&creature};image.Aggro(nullptr);assert(image.m_maxMeleeAllowed==999);sWorld.patch=9;outside.Aggro(nullptr);assert(outside.m_maxMeleeAllowed==4);}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'case.cpp';exe=Path(d)/'case';p.write_text(cpp);subprocess.run(['c++','-std=c++17',str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS actual Skeram Aggro: absent instance, raid controller, image guard, repeat aggro and pre-1.12 behavior')
