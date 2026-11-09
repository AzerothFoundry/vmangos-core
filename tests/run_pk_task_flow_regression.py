from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/game/PlayerBots/WorldBotPKTask.cpp').read_text()
assert 'entryRequired' not in s
continuation=s[s.index(' if(!t.remaining&&!t.failed){'):s.index(' if(!t.remaining&&t.failed){')]
assert all(x not in continuation for x in ['standChosen=','prepared=','entryUsed=','supplyPending='])
near=re.search(r' bool nearRegion=(.*);',s)[1]
code='''#include <cassert>
constexpr unsigned HORDE=67;
struct P {unsigned map=1,zone=14,team=67;bool nearby=true;unsigned GetMapId(){return map;}unsigned GetZoneId(){return zone;}unsigned GetTeam(){return team;}bool IsWithinDist3d(float,float,float,float){return nearby;}};
struct Site {unsigned map=1;float x=0,y=0,z=0;};
int main(){P actor;P* me=&actor;Site site;
auto near=[&](){return '''+near+''';};
assert(near());actor.nearby=false;assert(near());actor.zone=1637;assert(near());actor.zone=999;assert(!near());actor.nearby=true;assert(near());actor.map=0;assert(!near());actor.team=469;site.map=0;actor.zone=1519;actor.nearby=false;assert(near());actor.zone=12;assert(near());actor.zone=999;assert(!near());
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'case.cpp';p.write_text(code);out=Path(d)/'case';subprocess.run(['c++','-std=c++17',str(p),'-o',str(out)],check=True);subprocess.run([str(out)],check=True)
assert 'if(!nearRegion&&!inArea&&!t.entryUsed)' in s
assert s.index('if(protectedActivity)')<s.index('if(!nearRegion&&!inArea&&!t.entryUsed)')
assert s.index('if(me->m_duel)')<s.index('if(!nearRegion&&!inArea&&!t.entryUsed)')
assert 'SetPosition(' not in s
print('PASS actual PK region predicate; successful continuation retains stand/entry/supply; native entry guarded; no mandatory inn field')
