#!/usr/bin/env python3
# Compile actual production classifier against constants read from native headers.
from pathlib import Path
import re,tempfile,subprocess
root=Path(__file__).resolve().parents[1]
header=root/'src/game/Commands/GmSkillFilterPolicy.h'
text=header.read_text();names=set(re.findall(r'\b(?:SPELL_EFFECT_|SPELL_AURA_|TARGET_)[A-Z0-9_]+',text))
defs='\n'.join((root/f'src/game/Spells/{f}').read_text() for f in ['SpellDefines.h','SpellAuraDefines.h'])
names.add("TARGET_UNIT_ENEMY")
constants=[]
for name in names:
 m=re.search(r'\b'+name+r'\s*=\s*(\d+)',defs);assert m,name;constants.append(f'constexpr unsigned {name}={m[1]};')
cpp='\n'.join(constants)+r"""
#include <cassert>
#include <map>
#include "GmSkillFilterPolicy.h"
struct S {unsigned Id=1,Effect[3]{},EffectApplyAuraName[3]{},EffectImplicitTargetA[3]{},EffectImplicitTargetB[3]{},EffectChainTarget[3]{},EffectTriggerSpell[3]{};unsigned school=0;bool positive=false,area=false,passive=false;unsigned GetSpellSchoolMask()const{return 1u<<school;}bool IsAreaOfEffectSpell()const{return area;}bool HasAreaAuraEffect()const{return false;}bool IsPositiveSpell()const{return positive;}bool IsPassiveSpell()const{return passive;}};
int main(){using namespace GmSkillFilterPolicy;Filter f;
assert(Parse("@skill:frost:slow:single",f));S frost;frost.school=4;frost.Effect[0]=SPELL_EFFECT_SCHOOL_DAMAGE;frost.Effect[1]=SPELL_EFFECT_APPLY_AURA;frost.EffectApplyAuraName[1]=SPELL_AURA_MOD_DECREASE_SPEED;frost.EffectImplicitTargetA[0]=TARGET_UNIT_ENEMY;auto t=Classify(frost,false);assert(Match(f,t));assert(t.schoolMask==16&&t.effects.count("damage"));
S shadow;shadow.school=5;shadow.Effect[0]=SPELL_EFFECT_SCHOOL_DAMAGE;shadow.EffectImplicitTargetA[0]=TARGET_UNIT_ENEMY;assert(Parse("@skill:shadow:damage:single",f)&&Match(f,Classify(shadow,false)));
S corruption=shadow;corruption.Effect[0]=SPELL_EFFECT_APPLY_AURA;corruption.EffectApplyAuraName[0]=SPELL_AURA_PERIODIC_DAMAGE;assert(Parse("@skill:shadow:dot:single",f)&&Match(f,Classify(corruption,false)));
S curse=corruption;curse.EffectApplyAuraName[0]=22;assert(!Classify(curse,false).effects.count("damage"));assert(Classify(curse,false).effects.count("debuff"));
S heal;heal.positive=true;heal.Effect[0]=SPELL_EFFECT_HEAL;assert(Classify(heal,false).effects.count("heal")&&!Classify(heal,false).effects.count("damage"));
S silence=corruption;silence.EffectApplyAuraName[0]=SPELL_AURA_MOD_SILENCE;assert(Classify(silence,false).effects.count("silence"));silence.area=true;assert(Parse("@skill:shadow:silence:aoe",f)&&Match(f,Classify(silence,false)));
S buff;buff.positive=true;buff.Effect[0]=SPELL_EFFECT_APPLY_AURA;buff.EffectApplyAuraName[0]=99;buff.EffectImplicitTargetA[0]=TARGET_UNIT_CASTER;assert(Classify(buff,false).targets.count("self")&&!Classify(buff,false).targets.count("aoe"));buff.passive=true;assert(Classify(buff,false).targets.count("passive"));
S chain=heal;chain.EffectChainTarget[0]=3;assert(Classify(chain,false).targets.count("aoe")&&Classify(chain,false).targets.count("chain"));
assert(Parse("@teaching:all:all:all",f)&&Match(f,Classify(heal,true))&&!Match(f,Classify(heal,false)));assert(!Parse("@skill/teaching:all:all:all",f));assert(!Parse("@skill:frost:fake:all",f));assert(!Parse("@skill:frost:slow",f));assert(!Parse("@skill:frost:slow:single:extra",f));
// Trigger recursion is bounded and cycles terminate; teaching never inherits children.
std::map<unsigned,S> spells;S trigger;trigger.Id=2;trigger.Effect[0]=SPELL_EFFECT_TRIGGER_SPELL;trigger.EffectTriggerSpell[0]=1;spells[1]=shadow;spells[1].Effect[1]=SPELL_EFFECT_TRIGGER_SPELL;spells[1].EffectTriggerSpell[1]=2;spells[2]=trigger;auto lookup=[&](unsigned id)->S const*{auto it=spells.find(id);return it==spells.end()?nullptr:&it->second;};std::set<unsigned> seen;assert(ClassifyTree(trigger,false,lookup,0,seen).effects.count("damage"));seen.clear();assert(!ClassifyTree(trigger,true,lookup,0,seen).effects.count("damage"));
}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'case.cpp';p.write_text(cpp);exe=Path(d)/'case';subprocess.run(['c++','-std=c++17','-I'+str(header.parent),str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS production classifier native constants: frostbolt, shadowbolt, corruption, curse, healing, silence, aoe, chain, self, passive, teaching, bounded triggers, invalid filters')
