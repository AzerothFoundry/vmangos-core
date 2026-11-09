#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[1];s=(r/'src/game/Commands/GmMassRevive.h').read_text();actual=s[s.index('namespace GmMassRevive {'):];cpp=r"""
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include <cassert>
using uint32=unsigned;constexpr unsigned POWER_MANA=0;
struct ObjectGuid{unsigned n;};struct Slot{ObjectGuid guid;};struct Group{std::vector<Slot> members;auto const& GetMemberSlots(){return members;}};
struct AI{virtual~AI(){}};struct PlayerBotAI:AI{bool persistent=false;bool AllowsPersistentSave(){return persistent;}};
namespace WorldPackets{namespace Spell{struct ResurrectRequest{ObjectGuid casterGuid;bool sickness,delayed;};}}
struct Session{unsigned offers=0;template<class T>void SendPacket(std::unique_ptr<T>){++offers;}};
struct Player{unsigned id;bool online=true,alive=false,bot=true,teleport=false,bg=false,duel=false,pending=false;Group*group;int map=1;unsigned instance=7,team=1,revives=0,saves=0,bones=0,pets=0,power=0,requests=0;PlayerBotAI ai;Session session;bool m_duel=false;bool IsInWorld(){return online;}bool IsAlive(){return alive;}bool IsBot(){return bot;}Group*GetGroup(){return group;}int GetMap(){return map;}unsigned GetMapId(){return map;}unsigned GetInstanceId(){return instance;}bool IsBeingTeleported(){return teleport;}bool InBattleGround(){return bg;}bool GetTransport(){return false;}bool IsTaxiFlying(){return false;}unsigned GetTeam(){return team;}bool IsRessurectRequested(){return pending;}void ResurrectPlayer(float f){assert(f==1.f);alive=true;++revives;}void SetPower(unsigned,unsigned p){power=p;}unsigned GetMaxPower(unsigned){return 100;}void SpawnCorpseBones(){++bones;}void AutoReSummonPet(){++pets;}AI* GetAI(){return &ai;}AI* AI(){return &ai;}void SaveToDB(){++saves;}Session*GetSession(){return &session;}ObjectGuid GetObjectGuid(){return{id};}float GetPositionX(){return 1;}float GetPositionY(){return 2;}float GetPositionZ(){return 3;}float GetOrientation(){return 0;}unsigned GetMaxHealth(){return 100;}void SetResurrectRequestData(ObjectGuid,unsigned,unsigned,float x,float y,float z,float,unsigned h,unsigned m){assert(x==1&&y==2&&z==3&&h==100&&m==100);pending=true;++requests;}};
std::map<unsigned,Player*> players;namespace ObjectAccessor{Player*FindPlayer(ObjectGuid guid){auto i=players.find(guid.n);return i==players.end()?nullptr:i->second;}}
"""+actual+r"""
int main(){Group group{{{{1}},{{2}},{{3}},{{4}},{{5}},{{6}},{{7}},{{8}}}};Player gm{1};gm.group=&group;gm.alive=true;Player temp{2},persistent{3},human{4},existing{5},otherInstance{6},alive{7};for(auto p:{&temp,&persistent,&human,&existing,&otherInstance,&alive})p->group=&group;persistent.ai.persistent=true;human.bot=false;existing.bot=false;existing.pending=true;otherInstance.instance=8;alive.alive=true;players={{1,&gm},{2,&temp},{3,&persistent},{4,&human},{5,&existing},{6,&otherInstance},{7,&alive}};
auto preview=GmMassRevive::Run(&gm,false);assert(preview.eligible==3&&preview.bots==2&&preview.players==1&&preview.skipped==5);assert(!temp.alive&&!persistent.alive&&!human.pending&&persistent.saves==0);auto result=GmMassRevive::Run(&gm,true);assert(result.revived==2&&result.offered==1&&result.skipped==5);assert(temp.power==100&&temp.bones==1&&temp.pets==1&&temp.saves==0);assert(persistent.saves==1);assert(!human.alive&&human.pending&&human.requests==1&&human.session.offers==1);assert(existing.requests==0&&existing.pending);auto again=GmMassRevive::Run(&gm,true);assert(again.eligible==0&&again.revived==0&&again.offered==0);}
"""
# Avoid a C++ type/member name collision in fixture only.
cpp=cpp.replace('struct AI{','struct BaseAI{').replace('virtual~AI()','virtual~BaseAI()').replace('PlayerBotAI:AI','PlayerBotAI:BaseAI').replace('AI* GetAI()','BaseAI* GetAI()').replace('AI* AI()','BaseAI* AI()')
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'case.cpp';exe=Path(d)/'case';p.write_text(cpp);subprocess.run(['c++','-std=c++17',str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
source=(r/'src/game/Commands/GmUiCommands.cpp').read_text();a=source.index('if(words[0]=="raidtools"');b=source.index('    if (words[0] == "money"',a);lane=source[a:b];assert lane.index('SameContext')<lane.index('GmMassRevive::Run');assert 'self->IsInCombat()' not in lane;assert 'words[2]=="now"' in lane and 'confirm||immediate' in lane;assert 'self->IsAlive()' in lane and 'self->InBattleGround()' in lane
print('PASS actual native revival adapter: preview zero mutation, temporary no save, persistent save, human offer, pending offer preserved, context/offline/alive skipped, idempotence; no combat gate')
