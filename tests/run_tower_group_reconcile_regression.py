#!/usr/bin/env python3
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[1];s=(root/'src/game/PlayerBots/WorldBotTowerTask.cpp').read_text();a=s.index(' Player* primary=Find(squad->members.front());');b=s.index(' ai.m_leaderGuid=',a);fragment=s[a:b]
cpp=r"""
#include <vector>
#include <map>
#include <algorithm>
#include <cassert>
#include <string>
using uint32=unsigned;
struct Guid{unsigned n;unsigned GetCounter()const{return n;}bool operator!=(Guid b)const{return n!=b.n;}bool operator==(Guid b)const{return n==b.n;}};
struct Group;struct Player{unsigned id;Group*group=nullptr;bool online=true,combat=false;bool IsInWorld(){return online;}int GetMap(){return 1;}bool IsBot(){return true;}unsigned GetTeam(){return 469;}Group*GetGroup(){return group;}bool IsInCombat(){return combat;}bool IsNonMeleeSpellCasted(){return false;}bool GetGroupInvite(){return false;}bool m_duel=false;bool GetTradeData(){return false;}bool InBattleGround(){return false;}bool InBattleGroundQueue(){return false;}Guid GetObjectGuid(){return{id};}char const*GetName(){return "bot";}};
std::map<unsigned,Player*> players;Player*Find(unsigned id){auto i=players.find(id);return i==players.end()?nullptr:i->second;}
struct Slot{Guid guid;};struct Group{unsigned id;Guid leader;std::vector<Slot>slots;static unsigned next;Group():id(++next){}bool Create(Guid g,char const*){leader=g;return AddMember(g,"");}bool AddMember(Guid g,char const*){if(slots.size()>=5)return false;slots.push_back({g});players[g.n]->group=this;return true;}unsigned GetMembersCount(){return slots.size();}unsigned GetId(){return id;}auto const&GetMemberSlots(){return slots;}Guid GetLeaderGuid(){return leader;}void ChangeLeader(Guid g){assert(players[g.n]->group==this);leader=g;}void RemoveMember(Guid g,int){slots.erase(std::remove_if(slots.begin(),slots.end(),[&](Slot s){return s.guid==g;}),slots.end());players[g.n]->group=nullptr;if(slots.size()<2){for(auto s:slots)players[s.guid.n]->group=nullptr;delete this;}}};unsigned Group::next=0;
struct ObjectMgr{void AddGroup(Group*){}}sObjectMgr;
struct Squad{std::vector<unsigned>members;};
bool reconcile(Player*p,Squad*squad,std::vector<Player*>members){auto pause=[](char const*){return false;};
"""+fragment+r"""
return true;}
int main(){Player a{1},b{2},c{3},d{4},e{5};players={{1,&a},{2,&b},{3,&c},{4,&d},{5,&e}};Squad squad{{1,2,3,4,5}};std::vector<Player*>members{&a,&b,&c,&d,&e};auto small=new Group;small->Create({1},"");small->AddMember({2},"");auto large=new Group;large->Create({3},"");large->AddMember({4},"");large->AddMember({5},"");a.combat=true;assert(!reconcile(&a,&squad,members));assert(a.group==small&&c.group==large);a.combat=false;assert(reconcile(&a,&squad,members));for(auto p:members)assert(p->group==large);assert(large->GetMembersCount()==5&&large->leader.n==1);assert(reconcile(&a,&squad,members));assert(large->GetMembersCount()==5);a.online=false;assert(!reconcile(&b,&squad,members));assert(b.group==large);}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'case.cpp';exe=Path(d)/'case';p.write_text(cpp);subprocess.run(['c++','-std=c++17',str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS actual group reconciliation: 2+3 native merge with source disband, primary reassignment, combat hold, idempotence, no fallback leader creation')
