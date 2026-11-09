from pathlib import Path
import subprocess,tempfile,re
root=Path(__file__).resolve().parents[1];source=(root/'src/game/PlayerBots/BattleBotWaypoints.cpp').read_text()
paths=[]
for name in ['vPath_AV_Iceblood_Garrison_to_Snowfall_Flag','vPath_AV_Snowfall_Flag_to_Field_of_Strife_Stoneheart_Snowfall_Crossroad']:
 body=source.split('BattleBotPath '+name+' =')[1].split('};')[0];points=re.findall(r'\{\s*([-\d.]+)f,\s*([-\d.]+)f,\s*([-\d.]+)f',body);paths.append(points)
assert float(paths[0][0][0]) < float(paths[0][-1][0]);assert paths[0][-1]==paths[1][0]
assert float(paths[1][-1][0])>float(paths[1][0][0])
assert 'SnowfallAllowed(me,pPath,false) && closestDistanceToTarget > distanceFromPathEndToTarget' in source
assert 'SnowfallAllowed(me,pPath,true) && closestDistanceToTarget > distanceFromPathBeginToTarget' in source
code='''#include "SnowfallApproachPolicy.h"
#include <cassert>
int main(){using SnowfallApproachPolicy::Allowed;
assert(Allowed(67,1,false));assert(!Allowed(469,1,false));assert(Allowed(469,2,true));assert(!Allowed(67,2,true));
for(auto team:{67u,469u}){assert(Allowed(team,1,true));assert(Allowed(team,2,false));assert(Allowed(team,0,false));assert(Allowed(team,0,true));}
// Mid-route reacquisition keeps the same direction gate; it does not depend on point index.
for(unsigned point=0;point<19;++point){assert(Allowed(67,1,false));assert(!Allowed(469,1,false));}
}
'''.replace('#include <cassert>','#include <cassert>\n#include <initializer_list>')
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'t.cpp';p.write_text(code);subprocess.run(['g++','-std=c++17','-I'+str(root/'src/game/PlayerBots'),str(p),'-o',d+'/t'],check=True);subprocess.run([d+'/t'],check=True)
print('PASS native authored Snowfall south/north endpoint geometry, faction ascent, both exits, recovery gates')
