#pragma once
#include <string>
#include <sstream>
#include <set>
namespace GmSkillFilterPolicy {
struct Tags {unsigned schoolMask=0;bool teaching=false;std::set<std::string> effects,targets;};
struct Filter {std::string kind="all",school="all",effect="all",target="all";};
inline bool One(std::string const& value,std::string const& allowed){return !value.empty()&&value.find('/')==value.npos&&("/"+allowed+"/").find("/"+value+"/")!=std::string::npos;}
inline bool Parse(std::string const& token,Filter& f){
 if(token.empty()||token[0]!='@')return false;
 std::string parts[4];std::size_t pos=1;
 for(unsigned i=0;i<4;++i){auto end=token.find(':',pos);if((i<3&&end==token.npos)||(i==3&&end!=token.npos))return false;parts[i]=token.substr(pos,end==token.npos?end:end-pos);pos=end+1;}
 if(!One(parts[0],"all/skill/teaching")||!One(parts[1],"all/physical/holy/fire/nature/frost/shadow/arcane")||!One(parts[2],"all/damage/dot/heal/hot/slow/root/stun/silence/interrupt/fear/dispel/buff/debuff/summon/taunt/shield")||!One(parts[3],"all/single/aoe/chain/self/passive"))return false;
 f={parts[0],parts[1],parts[2],parts[3]};return true;
}
inline unsigned SchoolBit(std::string const& school){char const* names[]={"physical","holy","fire","nature","frost","shadow","arcane"};for(unsigned i=0;i<7;++i)if(school==names[i])return 1u<<i;return 0;}
inline bool Match(Filter const& f,Tags const& t){return (f.kind=="all"||(f.kind=="teaching")==t.teaching)&&(f.school=="all"||(t.schoolMask&SchoolBit(f.school)))&&(f.effect=="all"||t.effects.count(f.effect))&&(f.target=="all"||t.targets.count(f.target));}
inline std::string Join(std::set<std::string> const& tags){std::string s;for(auto const& tag:tags){if(!s.empty())s+=',';s+=tag;}return s;}
template<class Spell> Tags Classify(Spell const& s,bool teaching){
 Tags t;t.teaching=teaching;t.schoolMask=unsigned(s.GetSpellSchoolMask());
 // Teaching entries describe the teaching operation itself, never its child spell.
 bool aura=false,chain=false,self=false,other=false,aoe=s.IsAreaOfEffectSpell()||s.HasAreaAuraEffect();
 for(unsigned i=0;i<3;++i){if(!s.Effect[i])continue;
  unsigned a=s.EffectImplicitTargetA[i],b=s.EffectImplicitTargetB[i];
  if(a==TARGET_UNIT_CASTER||b==TARGET_UNIT_CASTER)self=true;
  if((a&&a!=TARGET_UNIT_CASTER)||(b&&b!=TARGET_UNIT_CASTER))other=true;
  if(s.EffectChainTarget[i]>1||a==TARGET_UNIT_FRIEND_CHAIN_HEAL||b==TARGET_UNIT_FRIEND_CHAIN_HEAL)chain=true;
  switch(s.Effect[i]){
   case SPELL_EFFECT_SCHOOL_DAMAGE:case SPELL_EFFECT_HEALTH_LEECH:case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:case SPELL_EFFECT_WEAPON_DAMAGE:t.effects.insert("damage");break;
   case SPELL_EFFECT_HEAL:case SPELL_EFFECT_HEAL_MAX_HEALTH:case SPELL_EFFECT_HEAL_MECHANICAL:t.effects.insert("heal");break;
   case SPELL_EFFECT_DISPEL:case SPELL_EFFECT_DISPEL_MECHANIC:t.effects.insert("dispel");break;
   case SPELL_EFFECT_INTERRUPT_CAST:t.effects.insert("interrupt");break;
   case SPELL_EFFECT_SUMMON:case SPELL_EFFECT_SUMMON_WILD:case SPELL_EFFECT_SUMMON_GUARDIAN:case SPELL_EFFECT_SUMMON_POSSESSED:case SPELL_EFFECT_SUMMON_TOTEM:case SPELL_EFFECT_SUMMON_OBJECT_WILD:case SPELL_EFFECT_SUMMON_PLAYER:case SPELL_EFFECT_SUMMON_PHANTASM:case SPELL_EFFECT_SUMMON_CRITTER:case SPELL_EFFECT_SUMMON_OBJECT_SLOT1:case SPELL_EFFECT_SUMMON_OBJECT_SLOT2:case SPELL_EFFECT_SUMMON_OBJECT_SLOT3:case SPELL_EFFECT_SUMMON_OBJECT_SLOT4:case SPELL_EFFECT_SUMMON_DEAD_PET:case SPELL_EFFECT_SUMMON_DEMON:case SPELL_EFFECT_SUMMON_PET:case SPELL_EFFECT_SUMMON_TOTEM_SLOT1:case SPELL_EFFECT_SUMMON_TOTEM_SLOT2:case SPELL_EFFECT_SUMMON_TOTEM_SLOT3:case SPELL_EFFECT_SUMMON_TOTEM_SLOT4:t.effects.insert("summon");break;
   case SPELL_EFFECT_ATTACK_ME:t.effects.insert("taunt");break;
   default:break;
  }
  if((s.Effect[i]==SPELL_EFFECT_APPLY_AURA||s.Effect[i]==SPELL_EFFECT_PERSISTENT_AREA_AURA||s.Effect[i]==SPELL_EFFECT_APPLY_AREA_AURA_PARTY||s.Effect[i]==SPELL_EFFECT_APPLY_AREA_AURA_PET||s.Effect[i]==SPELL_EFFECT_APPLY_AREA_AURA_FRIEND||s.Effect[i]==SPELL_EFFECT_APPLY_AREA_AURA_ENEMY||s.Effect[i]==SPELL_EFFECT_APPLY_AREA_AURA_RAID||s.Effect[i]==SPELL_EFFECT_APPLY_AREA_AURA_OWNER)&&s.EffectApplyAuraName[i]){aura=true;switch(s.EffectApplyAuraName[i]){
   case SPELL_AURA_PERIODIC_DAMAGE:case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:case SPELL_AURA_PERIODIC_LEECH:t.effects.insert("damage");t.effects.insert("dot");break;
   case SPELL_AURA_PERIODIC_HEAL:case SPELL_AURA_PERIODIC_HEALTH_FUNNEL:t.effects.insert("heal");t.effects.insert("hot");break;
   case SPELL_AURA_MOD_DECREASE_SPEED:t.effects.insert("slow");break;
   case SPELL_AURA_MOD_ROOT:t.effects.insert("root");break;
   case SPELL_AURA_MOD_STUN:t.effects.insert("stun");break;
   case SPELL_AURA_MOD_SILENCE:t.effects.insert("silence");break;
   case SPELL_AURA_MOD_FEAR:t.effects.insert("fear");break;
   case SPELL_AURA_MOD_TAUNT:t.effects.insert("taunt");break;
   case SPELL_AURA_SCHOOL_ABSORB:case SPELL_AURA_MANA_SHIELD:t.effects.insert("shield");break;
   default:break;
  }}
 }
 if(aura)t.effects.insert(s.IsPositiveSpell()?"buff":"debuff");
 if(s.IsPassiveSpell())t.targets.insert("passive");
 else if(chain){t.targets.insert("chain");t.targets.insert("aoe");}
 else if(aoe)t.targets.insert("aoe");
 else if(self&&!other)t.targets.insert("self");
 else t.targets.insert("single");
 return t;
}
template<class Spell,class Lookup> Tags ClassifyTree(Spell const& s,bool teaching,Lookup lookup,unsigned depth,std::set<unsigned>& seen){
 auto t=Classify(s,teaching);if(teaching||depth>=2||!seen.insert(s.Id).second)return t;
 for(unsigned i=0;i<3;++i)if(s.EffectTriggerSpell[i]&&(s.Effect[i]==SPELL_EFFECT_TRIGGER_SPELL||((s.Effect[i]==SPELL_EFFECT_APPLY_AURA||s.Effect[i]==SPELL_EFFECT_PERSISTENT_AREA_AURA)&&s.EffectApplyAuraName[i]==SPELL_AURA_PERIODIC_TRIGGER_SPELL)))
  if(auto child=lookup(s.EffectTriggerSpell[i])){auto tags=ClassifyTree(*child,false,lookup,depth+1,seen);t.effects.insert(tags.effects.begin(),tags.effects.end());}
 return t;
}

}
