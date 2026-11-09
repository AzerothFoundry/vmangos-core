#include "GmSkillTools.h"
#include "GmSkillQueryPolicy.h"
#include "CharacterOverrides.h"
#include "Player.h"
#include "Pet.h"
#include "WorldSession.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "GmSkillFilterPolicy.h"
#include "World.h"
#include "Config/Config.h"
#include "Util.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
namespace {
struct Profession {unsigned skill,spell,second,third,fourth;};
Profession const professions[]={
 {171,2259,3101,3464,11611},{164,2018,3100,3538,9785},{333,7411,7412,7413,13920},
 {202,4036,4037,4038,12656},{182,2366,2368,3570,11993},{165,2108,3104,3811,10662},
 {186,2575,2576,3564,10248},{393,8613,8617,8618,10768},{197,3908,3909,3910,12180},
 {185,2550,3102,3413,18260},{129,3273,3274,7924,10846},{356,7620,7731,7732,18248}};

bool Number(std::string const& s,unsigned maximum,unsigned& out){if(s.empty()||s.find_first_not_of("0123456789")!=std::string::npos)return false;try{unsigned long n=std::stoul(s);if(n>maximum)return false;out=n;return true;}catch(...){return false;}}
bool Teaching(SpellEntry const* s){return s->HasEffect(SPELL_EFFECT_LEARN_SPELL)||s->HasEffect(SPELL_EFFECT_LEARN_PET_SPELL);}
bool PetAbility(unsigned id){
    auto bounds=sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(id);
    for(auto i=bounds.first;i!=bounds.second;++i){
        if(i->second->skillId==SKILL_PET_TALENTS)return true;
        for(unsigned f=0;f<sCreatureFamilyStore.GetNumRows();++f)if(auto family=sCreatureFamilyStore.LookupEntry(f))
            if((family->skillLine[0]&&i->second->skillId==family->skillLine[0])||(family->skillLine[1]&&i->second->skillId==family->skillLine[1]))return true;
    }return false;
}
std::vector<unsigned> Mistaught(Player* p){
    std::vector<unsigned> ids;if(p->GetClass()!=CLASS_WARLOCK)return ids;
    for(auto const& row:p->GetSpellMap())if(row.second.state!=PLAYERSPELL_REMOVED)
        if(auto spell=sSpellMgr.GetSpellEntry(row.first))
            if(!GetTalentSpellCost(sSpellMgr.GetFirstSpellInChain(row.first))&&(PetAbility(row.first)||spell->HasEffect(SPELL_EFFECT_LEARN_PET_SPELL)))ids.push_back(row.first);
    return ids;
}
unsigned ProfessionSkill(unsigned id){auto bounds=sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(id);for(auto i=bounds.first;i!=bounds.second;++i)if(IsProfessionSkill(i->second->skillId))return i->second->skillId;return 0;}
bool PlayerAbility(Player* p,SpellEntry const* s){
    auto cls=sChrClassesStore.LookupEntry(p->GetClass());
    return cls && s->spellLevel>0 && s->SpellFamilyName==cls->spellfamily && p->IsSpellFitByClassAndRace(s->Id) &&
        !PetAbility(s->Id) && !Teaching(s) && !ProfessionSkill(s->Id) && GetTalentSpellCost(sSpellMgr.GetFirstSpellInChain(s->Id))==0 &&
        sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(s->Id).first!=sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(s->Id).second;
}
std::map<unsigned,std::pair<std::string,std::string>> const& Chinese(){
    static auto rows=[](){std::map<unsigned,std::pair<std::string,std::string>> result;
        std::ifstream in(sConfig.GetStringDefault("GmSkills.LocaleFile","gm-skills-zhCN.tsv"));std::string line;
        while(std::getline(in,line)){auto a=line.find('\t'),b=a==line.npos?line.npos:line.find('\t',a+1);unsigned id;
            if(a!=line.npos&&b!=line.npos&&Number(line.substr(0,a),1000000,id))result[id]={line.substr(a+1,b-a-1),line.substr(b+1)};}
        return result;}();return rows;
}
std::string Name(SpellEntry const* s,bool chinese,bool rank=false){if(chinese){auto i=Chinese().find(s->Id);if(i!=Chinese().end())return rank?i->second.second:i->second.first;}return rank?s->Rank[0]:s->SpellName[0];}
std::string Safe(std::string s){for(char& c:s)if(c=='|'||c=='\t'||c=='\r'||c=='\n')c=' ';if(s.size()>78){size_t n=78;while(n&&(uint8(s[n])&0xc0)==0x80)--n;s.resize(n);}return s;}
std::map<unsigned,std::string> const& Effects(){
 static auto rows=[](){std::map<unsigned,std::string> result;
  std::ifstream in(sConfig.GetStringDefault("GmSkills.EffectsFile","gm-skills-effects-zhCN.tsv"));std::string line;
  while(std::getline(in,line)){auto tab=line.find('\t');unsigned id;if(tab!=line.npos&&Number(line.substr(0,tab),1000000,id))result[id]=GmSkillQueryPolicy::Clean(line.substr(tab+1));}
  return result;}();return rows;
}
GmSkillFilterPolicy::Tags SkillTags(SpellEntry const* s,bool teaching){std::set<unsigned> seen;return GmSkillFilterPolicy::ClassifyTree(*s,teaching,[](unsigned id){return sSpellMgr.GetSpellEntry(id);},0,seen);}
bool UnsafeSkillChange(Player* p){return p->IsInCombat();}
bool Forceable(Player* p,SpellEntry const* s){return GmSkillQueryPolicy::Forceable(p->GetSession()->GetSecurity()>=SEC_ADMINISTRATOR,s&&SpellMgr::IsSpellValid(s,p,false),s&&p->HasSpell(s->Id),UnsafeSkillChange(p));}
std::string EffectDescription(SpellEntry const* s,bool& client){
 auto it=Effects().find(s->Id);client=it!=Effects().end()&&!it->second.empty();if(client)return it->second;
 std::ostringstream out;out<<"无独立文字说明；服务端基础字段（非角色实时数值）：";
 for(unsigned i=0;i<MAX_SPELL_EFFECTS;++i)if(s->Effect[i])out<<" 效果"<<i+1<<"="<<s->Effect[i]<<"，光环类型="<<s->EffectApplyAuraName[i]<<"，基础点数="<<s->EffectBasePoints[i]+1<<"，周期毫秒="<<s->EffectAmplitude[i]<<";";
 return out.str();
}
void Limits(Player* p,std::vector<std::string>& lines){auto v=CharacterOverrides::Get(p);std::ostringstream out;
 out<<"GMUI|LIMITS|talents="<<p->GetTalentPointLimit()<<"|used="<<p->GetUsedTalentPoints()<<"|free="<<p->GetFreeTalentPoints()<<"|talentOverride="<<v.talents<<"|professions="<<CharacterOverrides::ProfessionLimit(p)<<"|professionsUsed="<<CharacterOverrides::ProfessionCount(p)<<"|professionOverride="<<v.professions;lines.push_back(out.str());}
void Status(Player* p,std::vector<std::string>& lines){
 Limits(p,lines);
 for(auto const& prof:professions)if(p->HasSkill(prof.skill))
  lines.push_back("GMUI|PROFESSION|id="+std::to_string(prof.skill)+"|value="+std::to_string(p->GetSkillValuePure(prof.skill))+"|max="+std::to_string(p->GetSkillMaxPure(prof.skill)));
}
}
namespace GmSkillTools {
void Inspect(Player* p,std::vector<std::string>& lines){
    float healthTick=p->GetBaseHealthRegenPerTick()*sWorld.getConfig(CONFIG_FLOAT_RATE_HEALTH)*p->GetTotalAuraMultiplier(SPELL_AURA_MOD_HEALTH_REGEN_PERCENT);
    if(!p->IsStandingUp())healthTick*=1.5f;
    healthTick+=sWorld.getConfig(CONFIG_FLOAT_RATE_HEALTH)*2.0f*(p->GetTotalAuraModifier(SPELL_AURA_MOD_HEALTH_REGEN_IN_COMBAT)/5.0f);
    std::ostringstream stats;stats<<"GMUI|REGEN|name="<<p->GetName()<<"|hp="<<p->GetHealth()<<"|maxHp="<<p->GetMaxHealth()<<"|mana="<<p->GetPower(POWER_MANA)<<"|maxMana="<<p->GetMaxPower(POWER_MANA)<<"|spirit="<<p->GetStat(STAT_SPIRIT)<<"|intellect="<<p->GetStat(STAT_INTELLECT)<<"|combat="<<p->IsInCombat()<<"|fiveSecondRule="<<p->IsUnderLastManaUseEffect()<<"|standing="<<p->IsStandingUp();lines.push_back(stats.str());
    std::ostringstream rates;rates<<"GMUI|REGENRATE|healthRate="<<sWorld.getConfig(CONFIG_FLOAT_RATE_HEALTH)<<"|manaRate="<<sWorld.getConfig(CONFIG_FLOAT_RATE_POWER_MANA)<<"|healthTickBase="<<p->GetBaseHealthRegenPerTick()*sWorld.getConfig(CONFIG_FLOAT_RATE_HEALTH)<<"|healthTickRest="<<healthTick<<"|manaTickRest="<<p->GetNaturalManaRegenPerSecond(false)*sWorld.getConfig(CONFIG_FLOAT_RATE_POWER_MANA)*2<<"|manaTickCasting="<<p->GetNaturalManaRegenPerSecond(true)*sWorld.getConfig(CONFIG_FLOAT_RATE_POWER_MANA)*2<<"|petMistaught="<<Mistaught(p).size();lines.push_back(rates.str());
}
unsigned LearnClass(Player* p)
{
    std::set<unsigned> spells;
    for(unsigned i=0;i<sObjectMgr.GetMaxSkillLineAbilityId();++i)
    {
        auto row=sObjectMgr.GetSkillLineAbility(i);
        if(!row)continue;
        auto spell=sSpellMgr.GetSpellEntry(row->spellId);
        if(spell && PlayerAbility(p,spell) && spell->spellLevel<=p->GetLevel() && SpellMgr::IsSpellValid(spell,p,false))
            spells.insert(spell->Id);
    }
    unsigned count=0;
    for(auto id:spells)
        if(!p->HasSpell(id)){p->LearnSpell(id,false);++count;}
    return count;
}

bool Execute(Player* p,std::vector<std::string> const& w,std::vector<std::string>& lines,std::string& error){
 auto fail=[&](char const* text){error=text;return false;};unsigned id=0;
 if(w.size()<2)return fail("Skill action required");
 if(w[1]=="status"&&w.size()==2){Status(p,lines);return true;}
 if(w[1]=="inspect"&&w.size()==2){Inspect(p,lines);return true;}
 if(w[1]=="info"&&w.size()==3){
  if(!Number(w[2],1000000,id)||!id)return fail("Invalid spell ID");
  auto s=sSpellMgr.GetSpellEntry(id);if(!s)return fail("Unknown spell");
  bool client=false;auto description=EffectDescription(s,client);auto parts=GmSkillQueryPolicy::Parts(description);
  for(size_t i=0;i<parts.size();++i)lines.push_back("GMUI|SKILLEFFECT|id="+std::to_string(id)+"|part="+std::to_string(i+1)+"|total="+std::to_string(parts.size())+"|source="+(client?"client_base":"server_base_fields")+"|text="+parts[i]);
  lines.push_back("GMUI|DONE|skill_info");return true;
 }
 if(w[1]=="forcelearn"&&w.size()==4){
  if(p->GetSession()->GetSecurity()<SEC_ADMINISTRATOR)return fail("Administrator access required");
  if(w[2]!="player"||!Number(w[3],1000000,id)||!id)return fail("Force learn supports the current player and one spell ID only");
  if(UnsafeSkillChange(p))return fail("Leave combat before changing skills");
  auto s=sSpellMgr.GetSpellEntry(id);if(!s||!SpellMgr::IsSpellValid(s,p,false))return fail("Invalid player spell");
  if(!Forceable(p,s))return fail("Player already knows this spell or force learn unavailable");
  p->LearnSpell(id,false);CharacterOverrides::RefreshProfessionPoints(p);p->SaveToDB();
  if(!p->HasSpell(id))return fail("Native learn did not retain the requested spell");
  lines.push_back("GMUI|OK|Force learned spell saved");Status(p,lines);return true;
 }
 if(w[1]=="cleanup"&&w.size()==3&&(w[2]=="preview"||w[2]=="confirm")){
  if(p->GetClass()!=CLASS_WARLOCK)return fail("Demon spell cleanup is only for warlocks");
  auto ids=Mistaught(p);
  if(w[2]=="preview"){
   lines.push_back("GMUI|CLEANUP|count="+std::to_string(ids.size()));
   return Execute(p,{"skills","search","mistaught","1"},lines,error);
  }
  if(p->IsInCombat()||p->InBattleGround()||p->InBattleGroundQueue())return fail("Leave combat battleground and queue before changing skills");
  for(auto id:ids)p->RemoveSpell(id,false,false);
  LearnClass(p);if(p->GetPet())p->PetSpellInitialize();
  p->SaveToDB();lines.push_back("GMUI|CLEANUP|count=0|removed="+std::to_string(ids.size()));lines.push_back("GMUI|OK|Mistaught demon spells removed from player");return true;
 }
 if(w[1]=="search"&&w.size()>=4){
  auto const& scope=w[2];unsigned page;
  if((scope!="player"&&scope!="pet"&&scope!="profession"&&scope!="known"&&scope!="mistaught"&&scope!="all")||!Number(w[3],10000,page)||!page)return fail("Invalid skill search");
  auto mistakes=Mistaught(p);
  GmSkillFilterPolicy::Filter filter;size_t queryStart=4;
  if(w.size()>4&&!w[4].empty()&&w[4][0]=='@'){if(!GmSkillFilterPolicy::Parse(w[4],filter))return fail("Invalid skill filter");queryStart=5;}
  std::string query;for(size_t i=queryStart;i<w.size();++i){if(!query.empty())query+=' ';query+=w[i];}
  std::wstring needle;if(!Utf8toWStr(query,needle))return fail("Invalid skill search");wstrToLower(needle);
  struct Row{SpellEntry const* spell;bool pet,prof,teach;};std::vector<Row> matches;
  bool chinese=sObjectMgr.GetLocaleForIndex(p->GetSession()->GetSessionDbLocaleIndex())==LOCALE_zhCN;
  for(unsigned n=1;n<sSpellMgr.GetMaxSpellId();++n){auto s=sSpellMgr.GetSpellEntry(n);if(!s||s->SpellName[0].empty())continue;
   if(!query.empty()&&query!=std::to_string(n)&&!Utf8FitTo(Name(s,chinese),needle)&&!Utf8FitTo(Name(s,true),needle)&&!Utf8FitTo(s->SpellName[0],needle))continue;
   bool pet=PetAbility(n),prof=ProfessionSkill(n)!=0,teach=Teaching(s);
   if(!GmSkillFilterPolicy::Match(filter,SkillTags(s,teach)))continue;
   if(scope=="known"&&!p->HasSpell(n))continue;
   if(scope=="mistaught"&&std::find(mistakes.begin(),mistakes.end(),n)==mistakes.end())continue;
   if(scope=="player"&&!PlayerAbility(p,s))continue;
   if(scope=="pet"&&(!p->GetPet()||!p->GetPet()->CanLearnPetSpell(n)||teach))continue;
   if(scope=="profession"&&(!prof||teach))continue;
   matches.push_back({s,pet,prof,teach});
  }
  std::sort(matches.begin(),matches.end(),[](Row const& a,Row const& b){if(a.spell->spellLevel!=b.spell->spellLevel)return a.spell->spellLevel<b.spell->spellLevel;return a.spell->Id<b.spell->Id;});
  size_t first=(page-1)*6,last=std::min(first+6,matches.size());
  for(size_t i=first;i<last;++i){auto r=matches[i];auto s=r.spell;std::ostringstream out;
   bool petTarget=scope=="pet";bool known=petTarget?p->GetPet()->HasSpell(s->Id):p->HasSpell(s->Id);
   bool learnable=!r.teach&&GetTalentSpellCost(sSpellMgr.GetFirstSpellInChain(s->Id))==0&&(petTarget?p->GetPet()->CanLearnPetSpell(s->Id):PlayerAbility(p,s)||r.prof);
   out<<"GMUI|SKILL|id="<<s->Id<<"|name="<<Safe(Name(s,chinese))<<"|rank="<<Safe(Name(s,chinese,true))<<"|level="<<s->spellLevel<<"|scope="<<(petTarget?"pet":"player")<<"|kind="<<(r.teach?"teaching":r.pet?"pet":r.prof?"profession":"spell")<<"|known="<<known<<"|learnable="<<learnable<<"|forceable="<<Forceable(p,s);auto tags=SkillTags(s,r.teach);out<<"|recordType="<<(r.teach?"teaching":"skill")<<"|schoolMask="<<tags.schoolMask<<"|effectTags="<<GmSkillFilterPolicy::Join(tags.effects)<<"|targetTags="<<GmSkillFilterPolicy::Join(tags.targets);bool client=false;auto effect=EffectDescription(s,client);out<<"|effectSource="<<(client?"client_base":"server_base_fields")<<"|effect="<<GmSkillQueryPolicy::Clip(GmSkillQueryPolicy::Clean(effect),100);lines.push_back(out.str());}
  lines.push_back("GMUI|SKILLMETA|page="+std::to_string(page)+"|total="+std::to_string(matches.size())+"|more="+(last<matches.size()?"1":"0"));lines.push_back("GMUI|DONE|skills");return true;
 }
 if((w[1]=="learn"||w[1]=="forget")&&(w.size()==4||(w.size()==5&&w[4]=="all"))){
  if((w[2]!="player"&&w[2]!="pet")||!Number(w[3],1000000,id)||!id)return fail("Invalid skill action");
  if(UnsafeSkillChange(p))return fail("Leave combat before changing skills");
  auto s=sSpellMgr.GetSpellEntry(id);if(!s)return fail("Unknown spell");bool learn=w[1]=="learn";
  if(w[2]=="pet"){
   Pet* pet=p->GetPet();if(!pet||!pet->IsAlive())return fail("Summon a living pet first");
   if(learn){if(Teaching(s)||!pet->CanLearnPetSpell(id)||s->spellLevel>pet->GetLevel())return fail("Spell does not fit current pet or level");pet->LearnSpell(id);}
   else {if(!pet->HasSpell(id))return fail("Pet does not know this spell");pet->UnlearnSpell(id,false,true);}
   pet->SavePetToDB(PET_SAVE_AS_CURRENT);p->PetSpellInitialize();
  }else {
   if(GetTalentSpellCost(sSpellMgr.GetFirstSpellInChain(id)))return fail("Use the talent tree or reset talents");
   if(learn){unsigned prof=ProfessionSkill(id);
    if(Teaching(s)||(!PlayerAbility(p,s)&&!prof)||s->spellLevel>p->GetLevel())return fail("Spell does not fit current class or level");
    if(prof){if(sSpellMgr.IsPrimaryProfessionFirstRankSpell(id)&&p->GetFreePrimaryProfessionPoints()==0)return fail("Primary profession limit reached");
     auto range=sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(id);for(auto i=range.first;i!=range.second;++i)if(i->second->skillId==prof&&p->GetSkillValue(prof)<i->second->req_skill_value)return fail("Profession skill requirement not met");}
    if(!SpellMgr::IsSpellValid(s,p,false))return fail("Invalid player spell");
    p->LearnSpell(id,false);
   }else {if(!p->HasSpell(id))return fail("Player does not know this spell");p->RemoveSpell(w.size()==5?sSpellMgr.GetFirstSpellInChain(id):id,false,w.size()!=5);}
   CharacterOverrides::RefreshProfessionPoints(p);
  }
  p->SaveToDB();lines.push_back("GMUI|OK|Skill change saved");Status(p,lines);return true;
 }
 if(w[1]=="petall"&&w.size()==2){
  if(p->GetClass()!=CLASS_WARLOCK||!p->GetPet()||p->GetPet()->GetPetType()!=SUMMON_PET||!p->GetPet()->IsAlive())return fail("Summon a living warlock demon first");
  if(p->IsInCombat()||p->InBattleGround()||p->InBattleGroundQueue())return fail("Leave combat battleground and queue before changing skills");
  Pet* pet=p->GetPet();std::set<unsigned> ids;
  for(unsigned n=1;n<sSpellMgr.GetMaxSpellId();++n)if(auto s=sSpellMgr.GetSpellEntry(n))if(!Teaching(s)&&s->spellLevel<=pet->GetLevel()&&pet->CanLearnPetSpell(n)&&SpellMgr::IsSpellValid(s,p,false))ids.insert(n);
  for(auto n:ids)pet->LearnSpell(n);
  pet->SavePetToDB(PET_SAVE_AS_CURRENT);p->PetSpellInitialize();lines.push_back("GMUI|OK|Current demon spells learned and saved");return true;
 }
 if(w[1]=="profession"&&w.size()==4&&w[2]=="max"){
  if(p->IsInCombat()||p->InBattleGround()||p->InBattleGroundQueue())return fail("Leave combat battleground and queue before changing skills");
  if(p->GetLevel()<35)return fail("Level 35 required for the GM artisan shortcut");
  std::vector<Profession const*> selected;
  for(auto const& prof:professions)
   if((w[3]=="all" || (Number(w[3],1000,id)&&id==prof.skill))&&p->HasSkill(prof.skill))selected.push_back(&prof);
  if(selected.empty())return fail("Learn a profession first");
  for(auto prof:selected)for(unsigned spell:{prof->spell,prof->second,prof->third,prof->fourth})
   if(!sSpellMgr.GetSpellEntry(spell))return fail("Profession rank unavailable in active patch");
  for(auto prof:selected){
   for(unsigned spell:{prof->spell,prof->second,prof->third,prof->fourth})p->LearnSpell(spell,false);
   p->SetSkill(prof->skill,300,300,4);
  }
  CharacterOverrides::RefreshProfessionPoints(p);p->SaveToDB();
  lines.push_back("GMUI|OK|Learned professions raised to 300; no bulk recipes granted");Status(p,lines);return true;
 }
 if(w[1]=="profession"&&w.size()==4&&(w[2]=="learn"||w[2]=="forget")){
  if(p->IsInCombat()||p->InBattleGround()||p->InBattleGroundQueue())return fail("Leave combat battleground and queue before changing skills");
  if(!Number(w[3],1000,id))return fail("Invalid profession");
  auto match=std::find_if(std::begin(professions),std::end(professions),[id](Profession const& x){return x.skill==id;});if(match==std::end(professions))return fail("Invalid profession");
  if(w[2]=="learn"){
   if(p->HasSkill(id))return fail("Profession already learned");
   if(IsPrimaryProfessionSkill(id)&&p->GetFreePrimaryProfessionPoints()==0)return fail("Primary profession limit reached");
   auto spell=sSpellMgr.GetSpellEntry(match->spell);if(!spell||spell->spellLevel>p->GetLevel())return fail("Profession level requirement not met");
   p->LearnSpell(match->spell,false);if(!p->HasSkill(id))return fail("Profession could not be learned");
  }else {if(!p->HasSkill(id))return fail("Profession not learned");p->SetSkill(id,0,0);}
  CharacterOverrides::RefreshProfessionPoints(p);p->SaveToDB();lines.push_back("GMUI|OK|Profession change saved");Status(p,lines);return true;
 }
 if(w[1]=="limits"&&w.size()==4&&(w[2]=="talents"||w[2]=="professions")){
  if(p->IsInCombat()||p->InBattleGround()||p->InBattleGroundQueue())return fail("Leave combat battleground and queue before changing skills");
  auto limits=CharacterOverrides::Get(p);bool restore=w[3]=="default";unsigned value=0;
  if(!restore&&!Number(w[3],w[2]=="talents"?300:9,value))return fail("Invalid character limits");
  if(w[2]=="talents"){
   unsigned normal=p->GetLevel()<10?0:unsigned((p->GetLevel()-9)*sWorld.getConfig(CONFIG_FLOAT_RATE_TALENT));
   unsigned target=restore?normal:value;
   if(p->GetUsedTalentPoints()>target)return fail("Reset talents before lowering the total");
   limits.talents=restore?-1:int(value);
  }else {unsigned target=restore?sWorld.getConfig(CONFIG_UINT32_MAX_PRIMARY_TRADE_SKILL):value;
   if(!target||target<CharacterOverrides::ProfessionCount(p))return fail("Forget excess professions before lowering the limit");
   limits.professions=restore?-1:int(value);}
  if(!CharacterOverrides::Set(p,limits,error))return false;
  p->InitTalentForLevel();CharacterOverrides::RefreshProfessionPoints(p);p->SaveToDB();lines.push_back("GMUI|OK|Character limits saved");Status(p,lines);return true;
 }
 if(w[1]=="resettalents"&&w.size()==2){
  if(p->IsInCombat()||p->InBattleGround()||p->InBattleGroundQueue())return fail("Leave combat battleground and queue before changing skills");
  p->ResetTalents(true);p->SaveToDB();lines.push_back("GMUI|OK|Talents reset without gold cost");Status(p,lines);return true;
 }
 return fail("Unknown skill action");
}
}
