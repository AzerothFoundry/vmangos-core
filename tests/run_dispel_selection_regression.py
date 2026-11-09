#!/usr/bin/env python3
"""Compile the production selector against a small deterministic group fixture."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
source = (root/'src/game/PlayerBots/CombatBotBaseAI.cpp').read_text()
start = source.index('Player* CombatBotBaseAI::SelectDispelTarget(')
end = source.index('\nvoid CombatBotBaseAI::SummonPetIfNeeded()', start)
fixture = r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <iostream>
using uint8=uint8_t; using uint32=uint32_t;
constexpr int MAX_EFFECT_INDEX=3, SPELL_EFFECT_DISPEL=38;
enum DispelType { DISPEL_NONE, DISPEL_MAGIC, DISPEL_CURSE, DISPEL_POISON };
namespace Spells { uint32 GetDispellMask(DispelType d) { return 1u<<d; } }
enum { SPELL_AURA_MOD_CHARM=1, SPELL_AURA_MOD_CONFUSE, SPELL_AURA_MOD_FEAR,
 SPELL_AURA_MOD_STUN, SPELL_AURA_MOD_ROOT, SPELL_AURA_MOD_SILENCE,
 SPELL_AURA_MOD_PACIFY, SPELL_AURA_MOD_PACIFY_SILENCE,
 SPELL_AURA_PERIODIC_DAMAGE, SPELL_AURA_PERIODIC_DAMAGE_PERCENT, SPELL_AURA_PERIODIC_LEECH };
struct Player;
struct SpellEntry {
 int Effect[3]={SPELL_EFFECT_DISPEL,0,0}, EffectMiscValue[3]={DISPEL_MAGIC,0,0};
 int EffectApplyAuraName[3]={0,0,0}, Dispel=DISPEL_MAGIC;
 float range=40;
 bool IsTargetInRange(Player*,Player*) const;
};
struct SpellAuraHolder { SpellEntry* spell; bool positive=false;
 const SpellEntry* GetSpellProto() const { return spell; }
 bool IsPositive() const { return positive; }
};
struct GroupReference { Player* player; GroupReference* following=nullptr;
 Player* getSource() const { return player; } GroupReference* next() const { return following; }
};
struct Group { GroupReference* first=nullptr; GroupReference* GetFirstMember(){return first;} };
using Unit=Player;
bool IsTankClass(int cls){return cls==1;}
struct Player {
 uint32 guid; bool alive=true, friendly=true, los=true, castable=true, gm=false;
 int map=1; float distance=10, health=80; Group* group=nullptr; Player* pet=nullptr; bool otherHealer=false; int cls=1;
 std::map<int,SpellAuraHolder*> auras;
 Player* GetPet()const{return pet;} int GetClass()const{return cls;} uint32 GetObjectGuid()const{return guid;}
 bool IsAlive()const{return alive;} int GetMap()const{return map;}
 bool IsValidHelpfulTarget(Player* p)const{return p->friendly;}
 bool IsWithinLOSInMap(Player* p)const{return p->los;}
 bool IsGameMaster()const{return gm;}
 float GetHealthPercent()const{return health;} float GetDistance(Player* p)const{return p->distance;}
 uint32 GetGUID()const{return guid;} Group* GetGroup(){return group;}
 const std::map<int,SpellAuraHolder*>& GetSpellAuraHolderMap()const{return auras;}
};
bool SpellEntry::IsTargetInRange(Player*,Player* p)const{return p->distance<=range;}
struct CombatBotBaseAI {
 Player* me;
 bool CanTryToCastSpell(Player* p,const SpellEntry*)const{return p->castable;}
 bool IsValidDispelTarget(Player* p,const SpellEntry* s)const {
  for(auto const& a:p->auras) if(!a.second->positive && a.second->spell->Dispel==s->EffectMiscValue[0])return true;
  return false;
 }
 bool IsInDuel()const{return false;}
 bool AreOthersOnSameTarget(uint32 guid,bool,bool)const {
  for(auto* r=me->group->first;r;r=r->following)if(r->player->guid==guid)return r->player->otherHealer;
  return false;
 }
 bool IsValidHealTarget(Unit* p,float limit)const{return p->alive&&p->friendly&&p->los&&p->map==me->map&&p->health<limit&&p->distance<=30;}
 Unit* SelectHealTarget(float,float)const;
 Player* SelectDispelTarget(SpellEntry const*)const;
};
'''
tests=r'''
int main(){
 Player self{1}, first{2}, second{3};
 GroupReference a{&first},b{&second};a.following=&b;
 Group g{&a};self.group=&g;CombatBotBaseAI ai{&self};
 SpellEntry dispel,minor,control,dot;control.EffectApplyAuraName[0]=SPELL_AURA_MOD_ROOT;
 dot.EffectApplyAuraName[0]=SPELL_AURA_PERIODIC_DAMAGE;
 SpellAuraHolder h1{&minor},h2{&control},h3{&dot};
 first.auras[1]=&h1;second.auras[1]=&h1;
 first.castable=false;assert(ai.SelectDispelTarget(&dispel)==&second);
 first.castable=true;second.auras[1]=&h2;
 assert(ai.SelectDispelTarget(&dispel)==&second); // CC before harmless stat debuff.
 first.auras[1]=&h3;assert(ai.SelectDispelTarget(&dispel)==&second);
 second.los=false;assert(ai.SelectDispelTarget(&dispel)==&first);
 second.los=true;second.gm=true;assert(ai.SelectDispelTarget(&dispel)==&second);
 second.distance=35;assert(ai.SelectDispelTarget(&dispel)==&second); // Native 40yd range.
 second.distance=41;assert(ai.SelectDispelTarget(&dispel)==&first);
 second.distance=10;second.map=2;assert(ai.SelectDispelTarget(&dispel)==&first);
 second.map=1;second.alive=false;assert(ai.SelectDispelTarget(&dispel)==&first);
 second.alive=true;first.auras[1]=&h1;second.auras[1]=&h1;
 first.health=20;assert(ai.SelectDispelTarget(&dispel)==&first);
 first.castable=false;second.castable=false;assert(!ai.SelectDispelTarget(&dispel));
 assert(!ai.SelectDispelTarget(nullptr));self.group=nullptr;assert(!ai.SelectDispelTarget(&dispel));
 self.group=&g;self.health=100;first.castable=second.castable=true;
 Player pet{4};pet.health=5;first.health=100;first.pet=&pet;
 second.health=20;second.cls=2;second.otherHealer=true;
 assert(ai.SelectHealTarget(25,90)==&second); // Critical human beats 5% pet.
 first.health=50;assert(ai.SelectHealTarget(25,90)==&second); // Other healer is no veto at critical HP.
 first.health=100;second.health=100;assert(ai.SelectHealTarget(25,90)==&pet);
 pet.health=100;assert(!ai.SelectHealTarget(25,90));
 self.health=20;assert(ai.SelectHealTarget(25,90)==&self);
 std::cout<<"production healing: players before pets, critical concurrent healing, pet fallback PASS\n";
 std::cout<<"production dispel selector: castability, urgency, GM, native range, LOS, map, death, health PASS\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d);(p/'test.cpp').write_text(fixture+source[start:end]+source[source.index('Unit* CombatBotBaseAI::SelectHealTarget('):source.index('\nUnit* CombatBotBaseAI::SelectPeriodicHealTarget')]+tests)
 subprocess.run(['c++','-std=c++14','-Wall','-Wextra',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
