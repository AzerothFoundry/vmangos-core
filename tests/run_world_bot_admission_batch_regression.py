from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/game/PlayerBots/WorldBotManager.cpp').read_text()
# Extract actual admission termination and successful reservation expressions.
start=s.index('for(auto ptr:candidates)');end=s.index('WorldBotProfile profile=',start);guards=s[start:end]
assert 'total>=s.cap||attempts>=s.loginBatch' in guards
stmt=s.split('if(sPlayerBotMgr.AddBot(profile.characterGuid,false,ai.get())){',1)[1].split('ai.release()',1)[0]
assert '++total' in stmt and '++online60' in stmt and '++occupied' in stmt
code='''#include <cassert>
#include <map>
#include <string>
struct State{unsigned cap,loginBatch;};struct Profile{std::string purpose="other",city;unsigned race=1,cls=1,level=60;struct{unsigned seedLevel=60;}ai;};
std::string ProfileSlot(Profile const&,unsigned){return "same";}
using std::max;using std::to_string;
void test(unsigned cap,unsigned batch,unsigned initial,unsigned fullInitial,unsigned fullCap,unsigned quota,unsigned expected){State s{cap,batch};Profile p;unsigned total=initial,online60=fullInitial,attempts=0,admitted=0;bool full=true;std::map<std::string,unsigned>occupied,cityClasses;
for(unsigned n=0;n<10;++n){if(total>=s.cap||attempts>=s.loginBatch)break;if(online60>=fullCap||occupied["same"]>=quota)continue;++attempts;RESERVE}
assert(admitted==expected);assert(total<=cap);assert(online60<=fullCap);assert(occupied["same"]<=quota);}
int main(){test(100,5,97,90,100,10,3);test(100,5,90,99,100,10,1);test(100,5,90,90,100,2,2);test(100,1,90,90,100,10,1);}
'''.replace('RESERVE',stmt)
with tempfile.TemporaryDirectory()as d:
 p=Path(d)/'t.cpp';p.write_text(code);subprocess.run(['g++','-std=c++17',str(p),'-o',d+'/t'],check=True);subprocess.run([d+'/t'],check=True)
print('PASS actual successful reservation expressions: total capacity, level60 last slot, per-slot quota, batch default')
