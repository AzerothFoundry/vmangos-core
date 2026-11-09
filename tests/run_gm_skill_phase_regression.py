#!/usr/bin/env python3
# Compile actual phase guard and ensure only single-spell mutation routes use it.
from pathlib import Path
import re,tempfile,subprocess
root=Path(__file__).resolve().parents[1];source=(root/'src/game/Commands/GmSkillTools.cpp').read_text()
guard=re.search(r'bool UnsafeSkillChange\(Player\* p\)\{[^}]+\}',source).group()
force=source[source.index(' if(w[1]=="forcelearn"'):source.index(' if(w[1]=="cleanup"')]
normal=source[source.index(' if((w[1]=="learn"'):source.index(' if(w[1]=="petall"')]
assert 'UnsafeSkillChange(p)' in force and 'UnsafeSkillChange(p)' in normal
# Bulk cleanup, petall, profession and talent operations retain the old gates.
for marker in [' if(w[1]=="cleanup"',' if(w[1]=="petall"',' if(w[1]=="profession"',' if(w[1]=="limits"',' if(w[1]=="resettalents"']:
 block=source[source.index(marker):];block=block[:block.index('return fail("Leave combat battleground and queue before changing skills");')]
 assert 'p->IsInCombat()||p->InBattleGround()||p->InBattleGroundQueue()' in block
cpp='''#include <cassert>
struct Player {bool combat=false,bg=false,queue=false;bool IsInCombat(){return combat;}bool InBattleGround(){return bg;}bool InBattleGroundQueue(){return queue;}};
'''+guard+'''
int main(){for(int mask=0;mask<8;++mask){Player p;p.combat=mask&1;p.bg=mask&2;p.queue=mask&4;assert(UnsafeSkillChange(&p)==p.combat);}}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'case.cpp';p.write_text(cpp);exe=Path(tmp)/'case';subprocess.run(['c++','-std=c++17',str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS production guard: all combat/bg/queue combinations; single-spell routes and bulk gates retained')
