#include "BotEncounter.h"
#include "BotDecisionPolicy.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
using namespace BotEncounter;
int main(){
 {std::ifstream file("config/pve-encounter.example.json");std::ostringstream bytes;bytes<<file.rdbuf();Definition example;assert(Parse(bytes.str(),BotDecision::FieldCatalog::Standard(),example).ok);assert(example.slots[0].roles.count(3));}

 BotDecision::FieldCatalog catalog;catalog.fields={{"boss.health_pct",BotDecision::ValueType::Number},{"boss.dead",BotDecision::ValueType::Flag}};
 std::string json=R"({"schema_version":1,"revision":3,"id":"test","initial_phase":"one","slots":[{"id":"tank","count":1,"roles":[1]},{"id":"heal","count":1,"roles":[2]}],"phases":[{"id":"one","min_hold_ms":1000,"transitions":[{"to":"two","all":[{"field":"boss.health_pct","op":"lte","value":50}]}]},{"id":"two","transitions":[{"to":"one","all":[{"field":"boss.health_pct","op":"gt","value":50}]}]}]})";
 Definition d;assert(Parse(json,catalog,d).ok);auto bad=json;bad.insert(1,"\"id\":\"duplicate\",");assert(!Parse(bad,catalog,d).ok&&d.id=="test");bad=json;bad.replace(bad.find("boss.health_pct"),15,"unsupported");assert(!Parse(bad,catalog,d).ok);assert(d.revision==3);
 auto definition=std::make_shared<Definition>(d);auto policy=std::make_shared<BotDecision::PolicyRules::Policy>();policy->generation=8;Session a,b;Key ka{1,7,10,1,"test"},kb{1,7,20,1,"test"};assert(!(ka==kb));assert(a.Begin(ka,definition,policy,100,1000).ok);assert(b.Begin(kb,definition,policy,200,1000).ok);definition->revision=99;definition->initialPhase="mutated";assert(a.Snapshot().definitionRevision==3);
 assert(!a.Begin(ka,definition,policy,100,1100).ok);assert(!a.Change(999,Lifecycle::Combat,1100).ok);assert(!a.Change(100,Lifecycle::Completed,1100).ok);assert(a.Change(100,Lifecycle::Combat,1100).ok);
 std::vector<Member> roster={{3,1,1,true},{4,5,2,true},{1,1,1,true},{2,5,2,true}};assert(a.Reconcile(100,roster).ok);assert(a.Snapshot().assignments.at("tank")[0]==1);assert(a.Snapshot().assignments.at("heal")[0]==2);
 roster.push_back({5,1,1,true});assert(a.Reconcile(100,roster).ok);assert(a.Snapshot().assignments.at("tank")[0]==1);roster[2].alive=false;assert(a.Reconcile(100,roster).ok);assert(a.Snapshot().assignments.at("tank")[0]==3);assert(a.Snapshot().assignments.at("heal")[0]==2);
 auto stableEpoch=a.Snapshot().epoch;assert(a.Reconcile(100,roster).ok&&a.Snapshot().epoch==stableEpoch);assert(b.Snapshot().assignments.empty());assert(!a.Reconcile(100,{{1,1,1,true},{1,1,1,true}}).ok);assert(a.Reconcile(100,{{3,1,1,true}}).ok);assert(a.Snapshot().deficits.at("heal")==1);
 BotDecision::Observation annotated;annotated.scope={3,1,5,1,7,10};annotated.nowMs=2100;assert(a.Annotate(annotated).ok);assert(annotated.texts.at("assignment.role")=="tank"&&annotated.encounter=="test"&&annotated.encounterAttempt==1);annotated.scope.actor=9;assert(a.Annotate(annotated).ok&&annotated.texts.count("assignment.role")==0);annotated.scope.group=20;auto copy=annotated;assert(!a.Annotate(annotated).ok&&annotated.texts==copy.texts);assert(a.Policy()->generation==8);policy->generation=99;assert(a.Policy()->generation==8);
 BotDecision::Observation o;o.scope={100,1,12,1,7,10};o.encounter="test";o.encounterAttempt=1;o.nowMs=1500;o.sequence=1;o.numbers["boss.health_pct"]=30;assert(a.Annotate(o).ok);assert(a.Tick(100,o).ok&&a.Snapshot().phase=="one");o.nowMs=2100;o.sequence=2;assert(a.Annotate(o).ok);assert(a.Tick(100,o).ok&&a.Snapshot().phase=="two");assert(!a.Tick(100,o).ok);o.scope.instance=8;o.sequence=3;assert(!a.Tick(100,o).ok);o.scope.instance=7;o.scope.group=20;assert(!a.Tick(100,o).ok);o.scope.group=10;o.encounter="other";assert(!a.Tick(100,o).ok);o.encounter="test";o.encounterAttempt=2;assert(!a.Tick(100,o).ok);o.encounterAttempt=1;o.numbers.clear();o.nowMs=2200;assert(a.Annotate(o).ok);assert(a.Tick(100,o).ok&&a.Snapshot().phase=="two");
 auto phaseTwoEpoch=a.Snapshot().epoch;o.sequence=4;o.nowMs=2201;o.numbers["boss.health_pct"]=70;assert(a.Annotate(o).ok);assert(a.Tick(100,o).ok&&a.Snapshot().phase=="one"&&a.Snapshot().epoch>phaseTwoEpoch);o.sequence=5;o.nowMs=2202;assert(!a.Tick(100,o).ok);assert(a.Change(100,Lifecycle::Recovering,2250).ok);assert(a.Change(100,Lifecycle::Preparing,2300).ok);assert(a.Snapshot().phase=="one"&&a.Snapshot().phaseSinceMs==2300);assert(a.Change(100,Lifecycle::Combat,2300).ok);assert(a.Change(100,Lifecycle::Completed,2300).ok);assert(!a.Reconcile(100,roster).ok);definition->initialPhase="one";policy->generation=12;ka.attempt=2;assert(a.Begin(ka,definition,policy,100,2400).ok);assert(a.Snapshot().policyGeneration==12&&a.Snapshot().definitionRevision==99&&a.Snapshot().assignments.empty());
 {
 BotDecision::CapabilityCatalog abilities;abilities.abilities["test.cast"]={BotDecision::ActionKind::Cast,1,BotDecision::Casting,{"enemy"}};
 BotDecision::PolicyRules::Store store(BotDecision::FieldCatalog::Standard(),abilities);
 std::string rules=R"({"schema_version":1,"revision":1,"enabled":true,"rules":[{"id":"one","owner":"encounter","priority":1,"ttl_ms":500,"when":[{"field":"encounter.phase","op":"eq","value":"one"}],"action":{"ability":"test.cast","target":"enemy"}}]})";
 assert(store.Stage(rules).ok&&store.PublishAtBoundary().ok);Session pinned;auto def=std::make_shared<Definition>(d);Key key{1,7,10,5,"test"};assert(pinned.Begin(key,def,store.Current(),100,100).ok);
 auto first=pinned.Policy();assert(store.Stage(rules).ok&&store.PublishAtBoundary().ok);assert(store.Current()->generation!=first->generation&&pinned.Policy()->generation==first->generation);
 BotDecision::Observation observation;observation.scope={100,1,6,1,7,10};observation.nowMs=200;observation.sequence=1;observation.targets["enemy"]=50;assert(pinned.Annotate(observation).ok);auto ranked=BotDecision::PolicyRules::Rank(*pinned.Policy(),observation);assert(ranked.size()==1&&ranked[0].policyGeneration==first->generation&&ranked[0].encounterEpoch==observation.encounterEpoch);
 assert(BotDecision::Validate(ranked[0],observation).ok);assert(pinned.Change(100,Lifecycle::Combat,200).ok);assert(pinned.Annotate(observation).ok);assert(!BotDecision::Validate(ranked[0],observation).ok);
 }
 std::cout<<"PASS encounter strict parsing, authority, pinned definition, attempt isolation, stable assignments, phase hold and stale observations\n";
}
