#!/usr/bin/env python3
"""Run production corpse selection/reservation against deterministic group/spell fixtures.

Movement, LOS, session and native spell validation are explicit test doubles; this is not a
live raid acceptance test. Function bodies are extracted from production at run time.
"""
import pathlib
import subprocess
import tempfile
root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'src/game/PlayerBots/PartyBotAI.cpp').read_text()
start = source.index('struct ResurrectionReservation\n')
end = source.index('\nvoid PartyBotAI::PrepareRecoveryCast(', start)
fixture = r'''
#include "PartyBotRecoveryPolicy.h"
#include "RaidBotPlanner.h"
#include <cassert>
#include <cstdint>
#include <map>
#include <vector>
#include <iostream>
using uint32=std::uint32_t; using uint64=std::uint64_t;
struct ObjectGuid {
 uint64 value=0; ObjectGuid()=default; explicit ObjectGuid(uint64 v):value(v){}
 bool operator==(ObjectGuid g)const{return value==g.value;}
 bool operator!=(ObjectGuid g)const{return !(*this==g);}
 bool IsEmpty()const{return !value;} void Clear(){value=0;}
};
enum { CORPSE=1, DEAD=2, CLASS_DRUID=11, CURRENT_GENERIC_SPELL=1, SPELL_STATE_FINISHED=3 };
struct Player;
struct BotAI {virtual ~BotAI()=default;};
struct SpellEntry {uint32 Id=2007;};
struct Targets {Player* target=nullptr; Player* getUnitTarget(){return target;}};
struct Spell {SpellEntry* m_spellInfo; Targets m_targets; int state=1; int getState(){return state;}};
struct GroupReference {Player* player; GroupReference* following=nullptr;
 Player* getSource(){return player;} GroupReference* next(){return following;}};
struct Group {GroupReference* first; GroupReference* GetFirstMember(){return first;}};
struct Player {
 uint64 guid; bool alive=false,world=true,teleport=false,combat=false,request=false,bot=false,los=true;
 int map=1,death=CORPSE,cls=5; float distance=10; Group* group=nullptr; Spell* cast=nullptr;BotAI* botAI=nullptr;
 BotAI* AI(){return botAI;}
 uint64 GetGUID(){return guid;} ObjectGuid GetObjectGuid(){return ObjectGuid(guid);}
 bool IsAlive(){return alive;} bool IsInWorld(){return world;} bool IsBeingTeleported(){return teleport;}
 Group* GetGroup(){return group;} int GetMap(){return map;} bool IsInCombat(){return combat;}
 Spell* GetCurrentSpell(int){return cast;} bool IsRessurectRequested(){return request;}
 int GetClass(){return cls;} int GetDeathState(){return death;} bool IsBot(){return bot;}
 bool IsWithinDistInMap(Player* p,float range){return p->map==map && p->distance<=range;}
 float GetDistance(Player* p){return p->distance;}
};
std::map<uint64,Player*> players;
namespace ObjectAccessor {Player* FindPlayer(ObjectGuid g){auto it=players.find(g.value);return it==players.end()?nullptr:it->second;}}
namespace WorldTimer {uint32 now=1000; uint32 getMSTime(){return now;}}
struct Entry {uint64 playerGUID;};
struct PartyBotAI:BotAI {
 PartyBotAI(Player* p,SpellEntry* s,ObjectGuid g):me(p),m_resurrectionSpell(s),m_leaderGuid(g){}
 Player* me; SpellEntry* m_resurrectionSpell; ObjectGuid m_leaderGuid;
 std::map<uint64,uint32> m_failedResurrectionTargets;
 Entry* botEntry=nullptr; bool m_initialized=true,duel=false; unsigned role=0;
 bool IsInDuel()const{return duel;} unsigned GetRole()const{return role;}
 Player* SelectResurrectionTarget()const;
};
struct BotMgr {std::vector<PartyBotAI*> bots;
 const std::vector<PartyBotAI*>& GetOwnedPartyBots(ObjectGuid){return bots;}} sPlayerBotMgr;
namespace {
'''
tests=r'''
int main(){
 SpellEntry rez; Player self{1},human{2},tank{3},healer{4};
 self.alive=true;tank.bot=healer.bot=true;human.distance=90;human.los=false;tank.distance=60;tank.los=false;
 GroupReference a{&human},b{&tank},c{&healer};a.following=&b;b.following=&c;Group g{&a};
 self.group=human.group=tank.group=healer.group=&g;
 Entry te{3},he{4};PartyBotAI tankAI{&tank,&rez,ObjectGuid(2)},healerAI{&healer,&rez,ObjectGuid(2)};
 tankAI.botEntry=&te;tankAI.role=3;healerAI.botEntry=&he;healerAI.role=4;sPlayerBotMgr.bots={&tankAI,&healerAI};
 tank.botAI=&tankAI;healer.botAI=&healerAI;
 PartyBotAI ai{&self,&rez,ObjectGuid(2)};
 players={{1,&self},{2,&human},{3,&tank},{4,&healer}};
 assert(ai.SelectResurrectionTarget()==&human); // 90yd and no direct LOS is still approachable.
 Spell cast{&rez,{&human}};self.cast=&cast;
 resurrectionReservations[2]={ObjectGuid(1),1000,10000,rez.Id};
 assert(ReservedCorpse(&human));assert(ai.SelectResurrectionTarget()==&tank);
 self.cast=nullptr;assert(!ReservedCorpse(&human)); // immediate interruption, not castTime+2s
 assert(resurrectionReservations.empty());assert(ai.SelectResurrectionTarget()==&human);
 self.cast=&cast;cast.m_targets.target=&tank;resurrectionReservations[2]={ObjectGuid(1),1000,10000,rez.Id};
 assert(!ReservedCorpse(&human)); // an unrelated heal/cast cannot maintain a resurrection lease
 cast.m_targets.target=&human;self.map=2;resurrectionReservations[2]={ObjectGuid(1),1000,10000,rez.Id};
 assert(!ReservedCorpse(&human));self.map=1;
 self.alive=false;resurrectionReservations[2]={ObjectGuid(1),1000,10000,rez.Id};assert(!ReservedCorpse(&human));self.alive=true;
 human.request=true;resurrectionReservations[2]={ObjectGuid(1),1000,10000,rez.Id};assert(!ReservedCorpse(&human));
 assert(ai.SelectResurrectionTarget()==&tank);human.request=false;
 ai.m_failedResurrectionTargets[2]=1000;
 assert(ai.SelectResurrectionTarget()==&tank);
 ai.m_failedResurrectionTargets[3]=1000;assert(ai.SelectResurrectionTarget()==&healer);
 WorldTimer::now=16000;assert(ai.SelectResurrectionTarget()==&human);
 human.distance=101;assert(ai.SelectResurrectionTarget()==&tank);human.distance=90;
 human.death=DEAD;assert(ai.SelectResurrectionTarget()==&tank);human.death=CORPSE;
 human.map=2;assert(ai.SelectResurrectionTarget()==&tank);human.map=1;
 human.world=false;assert(ai.SelectResurrectionTarget()==&tank);human.world=true;
 human.teleport=true;assert(ai.SelectResurrectionTarget()==&tank);human.teleport=false;
 human.request=true;tank.request=true;assert(ai.SelectResurrectionTarget()==&healer);
 self.cls=CLASS_DRUID;assert(!ai.SelectResurrectionTarget());self.cls=5;
 self.group=nullptr;assert(!ai.SelectResurrectionTarget());
 std::cout<<"production resurrection selector/lease: distant/LOS candidates, priority, active cast, interrupt, map, death, request and retry PASS\n";
}
'''
with tempfile.TemporaryDirectory() as d:
    p = pathlib.Path(d)
    (p / 'test.cpp').write_text(fixture + source[start:end] + tests)
    subprocess.run(['c++', '-std=c++14', '-Wall', '-Wextra', '-Wno-missing-field-initializers',
                    '-I'+str(root/'src/game/PlayerBots'), str(p/'test.cpp'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
