#include "BotEncounter.h"
#include "BotDecisionPolicy.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace BotEncounter {namespace {
using J=nlohmann::json;
void Require(bool b,char const* s){if(!b)throw std::runtime_error(s);}
void Keys(J const& j,std::initializer_list<char const*> keys){Require(j.is_object(),"expected object");for(auto i=j.begin();i!=j.end();++i){bool found=false;for(auto k:keys)found|=i.key()==k;Require(found,"unknown key");}}
std::string Text(J const& j){Require(j.is_string(),"expected text");auto s=j.get<std::string>();Require(!s.empty()&&s.size()<=96&&std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32||c==127;}),"invalid identifier");return s;}
uint64_t Integer(J const& j,uint64_t max){Require(j.is_number_integer(),"expected integer");Require(j.is_number_unsigned()||j.get<int64_t>()>=0,"negative integer");auto n=j.get<uint64_t>();Require(n<=max,"integer out of range");return n;}
bool Match(Condition const& c,BotDecision::Observation const& o){
 if(c.type==BotDecision::ValueType::Number){auto i=o.numbers.find(c.field);if(i==o.numbers.end()||!std::isfinite(i->second))return false;double v=i->second;
 if(c.op=="eq")return v==c.number;if(c.op=="ne")return v!=c.number;if(c.op=="lt")return v<c.number;if(c.op=="lte")return v<=c.number;if(c.op=="gt")return v>c.number;return v>=c.number;}
 if(c.type==BotDecision::ValueType::Flag){auto i=o.flags.find(c.field);return i!=o.flags.end()&&(c.op=="eq"?i->second==c.flag:i->second!=c.flag);}
 auto i=o.texts.find(c.field);return i!=o.texts.end()&&(c.op=="eq"?i->second==c.text:i->second!=c.text);
}
bool Eligible(Slot const& s,Member const& m){return m.alive&&(!s.classes.size()||s.classes.count(m.classId))&&(!s.roles.size()||s.roles.count(m.role));}
}
Result Parse(std::string const& text,BotDecision::FieldCatalog const& fields,Definition& out){try{
 Require(text.size()<=262144,"encounter exceeds 256KiB");std::vector<std::set<std::string>> seen;
 auto j=J::parse(text,[&](int depth,J::parse_event_t event,J& value){Require(depth<=24,"nesting too deep");if(event==J::parse_event_t::object_start)seen.emplace_back();if(event==J::parse_event_t::key)Require(seen.back().insert(value.get<std::string>()).second,"duplicate key");if(event==J::parse_event_t::object_end)seen.pop_back();return true;});
 Keys(j,{"schema_version","revision","id","initial_phase","slots","phases"});Definition d;
 Require(Integer(j.at("schema_version"),1)==1,"unsupported schema");d.revision=Integer(j.at("revision"),9007199254740991ULL);d.id=Text(j.at("id"));d.initialPhase=Text(j.at("initial_phase"));
 auto slots=j.at("slots");Require(slots.is_array()&&slots.size()<=40,"invalid slots");std::set<std::string> ids;unsigned total=0;
 for(auto const& v:slots){Keys(v,{"id","count","classes","roles"});Slot s;s.id=Text(v.at("id"));Require(ids.insert(s.id).second,"duplicate slot");s.count=uint32_t(Integer(v.at("count"),40));Require(s.count>0,"empty slot");total+=s.count;Require(total<=40,"too many assignments");
 for(auto key:{"classes","roles"})if(v.count(key)){Require(v.at(key).is_array()&&v.at(key).size()<=16,"invalid eligibility");for(auto const& n:v.at(key)){auto value=uint32_t(Integer(n,255));(std::string(key)=="classes"?s.classes:s.roles).insert(value);}}d.slots.push_back(std::move(s));}
 auto phases=j.at("phases");Require(phases.is_array()&&!phases.empty()&&phases.size()<=64,"invalid phases");ids.clear();
 for(auto const& v:phases){Keys(v,{"id","min_hold_ms","transitions"});Phase p;p.id=Text(v.at("id"));Require(ids.insert(p.id).second,"duplicate phase");if(v.count("min_hold_ms"))p.minHoldMs=Integer(v.at("min_hold_ms"),86400000);
 auto transitions=v.at("transitions");Require(transitions.is_array()&&transitions.size()<=32,"invalid transitions");for(auto const& t:transitions){Keys(t,{"to","all"});Transition tr;tr.to=Text(t.at("to"));auto all=t.at("all");Require(all.is_array()&&!all.empty()&&all.size()<=32,"invalid conditions");for(auto const& c:all){Keys(c,{"field","op","value"});Condition cond;cond.field=Text(c.at("field"));cond.op=Text(c.at("op"));auto f=fields.fields.find(cond.field);Require(f!=fields.fields.end()&&f->second!=BotDecision::ValueType::IdSet,"unknown or unsupported phase field");cond.type=f->second;
 Require(cond.op=="eq"||cond.op=="ne"||(cond.type==BotDecision::ValueType::Number&&(cond.op=="lt"||cond.op=="lte"||cond.op=="gt"||cond.op=="gte")),"invalid operator");auto val=c.at("value");if(cond.type==BotDecision::ValueType::Number){Require(val.is_number(),"expected number");cond.number=val.get<double>();Require(std::isfinite(cond.number),"nonfinite number");}else if(cond.type==BotDecision::ValueType::Flag){Require(val.is_boolean(),"expected bool");cond.flag=val.get<bool>();}else cond.text=Text(val);tr.all.push_back(std::move(cond));}p.transitions.push_back(std::move(tr));}d.phases.push_back(std::move(p));}
 Require(ids.count(d.initialPhase),"missing initial phase");for(auto const& p:d.phases)for(auto const& t:p.transitions)Require(ids.count(t.to)&&t.to!=p.id,"unknown or self transition");out=std::move(d);return {true,"parsed"};
 }catch(std::exception const& e){return {false,e.what()};}}
Result Session::Begin(Key const& key,std::shared_ptr<Definition const> definition,std::shared_ptr<BotDecision::PolicyRules::Policy const> policy,uint64_t authority,uint64_t now){
 if(m_definition&&m_state.lifecycle!=Lifecycle::Completed&&m_state.lifecycle!=Lifecycle::Failed)return {false,"encounter already active"};
 if(!definition||!authority||!policy||!policy->generation||!key.group||!key.attempt||key.encounter.empty()||key.encounter.size()>96||key.encounter!=definition->id||definition->phases.empty())return {false,"invalid encounter identity"};
 if(definition->phases.size()>64||definition->slots.size()>40)return {false,"definition too large"};
 std::set<std::string> phaseIds,slotIds;unsigned assignmentCount=0;
 for(auto const& phase:definition->phases){if(phase.id.empty()||!phaseIds.insert(phase.id).second||phase.transitions.size()>32)return {false,"invalid phase definition"};}
 for(auto const& phase:definition->phases)for(auto const& transition:phase.transitions)if(!phaseIds.count(transition.to)||transition.to==phase.id||transition.all.empty()||transition.all.size()>32)return {false,"invalid phase transition"};
 for(auto const& slot:definition->slots){if(slot.id.empty()||!slotIds.insert(slot.id).second||!slot.count||slot.count>40)return {false,"invalid slot definition"};assignmentCount+=slot.count;}
 if(assignmentCount>40)return {false,"too many assignments"};
 if(std::none_of(definition->phases.begin(),definition->phases.end(),[&](Phase const& p){return p.id==definition->initialPhase;}))return {false,"missing phase"};
 m_definition=std::make_shared<Definition const>(*definition);m_policy=std::make_shared<BotDecision::PolicyRules::Policy const>(*policy);m_beginAt=now;m_authority=authority;m_lastNow=now;m_lastObservation=0;m_state={};m_state.key=key;m_state.lifecycle=Lifecycle::Preparing;m_state.phase=m_definition->initialPhase;m_state.phaseSinceMs=now;m_state.policyGeneration=m_policy->generation;m_state.definitionRevision=m_definition->revision;return {true,"begun"};}
Result Session::Change(uint64_t authority,Lifecycle to,uint64_t now){if(!Authorized(authority))return {false,"not encounter authority"};if(now<m_lastNow)return {false,"clock regressed"};auto from=m_state.lifecycle;
 bool ok=(from==Lifecycle::Preparing&&(to==Lifecycle::Combat||to==Lifecycle::Failed))||(from==Lifecycle::Combat&&(to==Lifecycle::Recovering||to==Lifecycle::Completed||to==Lifecycle::Failed))||(from==Lifecycle::Recovering&&(to==Lifecycle::Preparing||to==Lifecycle::Failed));if(!ok)return {false,"invalid lifecycle transition"};m_lastNow=now;m_state.lifecycle=to;++m_state.epoch;if(from==Lifecycle::Recovering&&to==Lifecycle::Preparing){m_state.phase=m_definition->initialPhase;m_state.phaseSinceMs=now;m_lastObservation=0;m_beginAt=now;}return {true,"transitioned"};}
Result Session::Reconcile(uint64_t authority,std::vector<Member> const& roster){if(!Authorized(authority))return {false,"not encounter authority"};if(m_state.lifecycle==Lifecycle::Completed||m_state.lifecycle==Lifecycle::Failed)return {false,"encounter ended"};if(roster.size()>40)return {false,"roster too large"};std::map<uint64_t,Member> members;for(auto const& m:roster)if(!m.actor||!members.emplace(m.actor,m).second)return {false,"duplicate or empty actor"};std::map<std::string,std::vector<uint64_t>> next;std::set<uint64_t> used;
 // Reserve every surviving assignment before filling any vacancy.
 for(auto const& slot:m_definition->slots){auto found=m_state.assignments.find(slot.id);if(found!=m_state.assignments.end())for(auto id:found->second){auto it=members.find(id);if(it!=members.end()&&Eligible(slot,it->second)&&used.insert(id).second&&next[slot.id].size()<slot.count)next[slot.id].push_back(id);}}
 std::map<std::string,uint32_t> deficits;for(auto const& slot:m_definition->slots){auto& assigned=next[slot.id];for(auto const& member:members)if(assigned.size()<slot.count&&!used.count(member.first)&&Eligible(slot,member.second)){assigned.push_back(member.first);used.insert(member.first);}deficits[slot.id]=slot.count-uint32_t(assigned.size());}
 if(m_state.assignments!=next||m_state.deficits!=deficits)++m_state.epoch;m_state.assignments=std::move(next);m_state.deficits=std::move(deficits);return {true,"reconciled"};}
Result Session::Tick(uint64_t authority,BotDecision::Observation const& observation){if(!Authorized(authority))return {false,"not encounter authority"};if(observation.encounterEpoch!=m_state.epoch||!observation.scope.actorGeneration||observation.scope.actor!=authority||!observation.scope.sceneGeneration||observation.scope.group!=m_state.key.group||observation.encounter!=m_state.key.encounter||observation.encounterAttempt!=m_state.key.attempt||observation.scope.map!=m_state.key.map||observation.scope.instance!=m_state.key.instance)return {false,"wrong encounter scene"};if(observation.nowMs<m_lastNow)return {false,"clock regressed"};if(!observation.sequence||observation.sequence<=m_lastObservation)return {false,"stale observation"};m_lastObservation=observation.sequence;m_lastNow=observation.nowMs;if(m_state.lifecycle!=Lifecycle::Combat)return {false,"not in combat"};
 auto phase=std::find_if(m_definition->phases.begin(),m_definition->phases.end(),[&](Phase const& p){return p.id==m_state.phase;});if(phase==m_definition->phases.end())return {false,"unknown phase"};if(observation.nowMs-m_state.phaseSinceMs<phase->minHoldMs)return {true,"holding phase"};
 for(auto const& transition:phase->transitions)if(std::all_of(transition.all.begin(),transition.all.end(),[&](Condition const& c){return Match(c,observation);})){m_state.phase=transition.to;m_state.phaseSinceMs=observation.nowMs;++m_state.epoch;return {true,"phase transitioned"};}return {true,"phase unchanged"};}
Result Session::Annotate(BotDecision::Observation& observation)const {
 if(!m_definition||!m_policy||!observation.scope.actor||!observation.scope.actorGeneration||!observation.scope.sceneGeneration||observation.scope.map!=m_state.key.map||observation.scope.instance!=m_state.key.instance||observation.scope.group!=m_state.key.group)return {false,"wrong encounter scene"};
 if(m_state.lifecycle==Lifecycle::Idle||m_state.lifecycle==Lifecycle::Completed||m_state.lifecycle==Lifecycle::Failed)return {false,"encounter ended"};
 if(observation.nowMs<m_beginAt||observation.nowMs<m_state.phaseSinceMs)return {false,"clock regressed"};
 auto next=observation;next.encounter=m_state.key.encounter;next.encounterAttempt=m_state.key.attempt;next.encounterEpoch=m_state.epoch;
 next.texts["encounter.id"]=m_state.key.encounter;next.texts["encounter.phase"]=m_state.phase;
 next.numbers["phase.elapsed_ms"]=double(observation.nowMs-m_state.phaseSinceMs);next.numbers["encounter.elapsed_ms"]=double(observation.nowMs-m_beginAt);
 next.texts.erase("assignment.role");
 for(auto const& assignment:m_state.assignments)if(std::find(assignment.second.begin(),assignment.second.end(),observation.scope.actor)!=assignment.second.end()){next.texts["assignment.role"]=assignment.first;break;}
 observation=std::move(next);return {true,"annotated"};
}

}
