#include "BotDecisionPolicy.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <thread>
#include <iostream>
#include <fstream>
#include <sstream>
using namespace BotDecision;
using namespace BotDecision::PolicyRules;
using J=nlohmann::json;
struct SafeFake : Executor {
 int executed=0;
 Result Check(Intent const& i,Observation const& o) override{return Validate(i,o);}
 Result Execute(Intent const& i,Observation const& o,std::vector<Intent> const&) override{auto r=Validate(i,o);if(r.ok)++executed;return r;}
};
int main(){
 FieldCatalog f;f.fields={{"distance",ValueType::Number},{"combat",ValueType::Flag},{"phase",ValueType::Text},{"aura",ValueType::IdSet}};
 CapabilityCatalog c;c.abilities["mage.frostbolt"]={ActionKind::Cast,116,Casting|Movement,{"enemy"}};
 J rule={{"id","B"},{"owner","rotation"},{"priority",10},{"when",J::array({J{{"field","distance"},{"op","lt"},{"value",30}}})},{"action",J{{"ability","mage.frostbolt"},{"target","enemy"}}}};
 J j={{"schema_version",1},{"revision",1},{"enabled",true},{"rules",J::array({rule})}};Policy p;assert(Parse(j.dump(),f,c,p).ok);
 Observation o;o.scope.actor=7;o.scope.actorGeneration=1;o.sequence=2;o.nowMs=100;o.targets["enemy"]=9;
 assert(Rank(p,o).empty());o.numbers["distance"]=20;auto ranked=Rank(p,o);assert(ranked.size()==1&&ranked[0].targetGuid==9&&ranked[0].resources==(Casting|Movement)&&ranked[0].expiresMs==600);
 auto bad=j;bad["rules"][0]["action"]["bypass_gcd"]=true;assert(!Parse(bad.dump(),f,c,p).ok);assert(p.rules.size()==1);
 bad=j;bad["rules"][0]["action"]["ability"]="gm.kill";assert(!Parse(bad.dump(),f,c,p).ok);
 bad=j;bad["rules"][0]["when"][0]["value"]=true;assert(!Parse(bad.dump(),f,c,p).ok);
 bad=j;bad["rules"][0]["when"][0]["field"]="unregistered";assert(!Parse(bad.dump(),f,c,p).ok);
 assert(!Parse("{\"schema_version\":1,\"schema_version\":1}",f,c,p).ok);
 for(auto field:{"distance","combat","phase","aura"}){
  auto x=j;x["rules"][0]["when"]=J::array({J{{"field",field},{"op",field==std::string("aura")?"not_contains":"ne"},{"value",1}}});
  if(field==std::string("combat"))x["rules"][0]["when"][0]["value"]=true;
  if(field==std::string("phase"))x["rules"][0]["when"][0]["value"]="p1";
  assert(Parse(x.dump(),f,c,p).ok);Observation empty=o;empty.numbers.clear();assert(Rank(p,empty).empty());
 }
 auto x=j;x["rules"][0]["when"]=J::array({J{{"field","combat"},{"op","eq"},{"value",true}},J{{"field","phase"},{"op","eq"},{"value","p1"}},J{{"field","aura"},{"op","contains"},{"value",42}}});
 x["rules"].push_back(x["rules"][0]);x["rules"][1]["id"]="A";assert(Parse(x.dump(),f,c,p).ok);o.flags["combat"]=true;o.texts["phase"]="p1";o.sets["aura"]={42};assert(Rank(p,o)[0].rule=="A");assert(Rank(p,o,1).size()==1);
 Store store(f,c);assert(store.Stage(j.dump()).ok);assert(!store.Current()->enabled);assert(store.PublishAtBoundary().ok);auto pinned=store.Current();assert(pinned->generation==1);
 x=j;x["revision"]=2;assert(store.Stage(x.dump()).ok);assert(!store.Stage("{bad").ok);assert(store.Pending()->revision==2);assert(store.Current()==pinned);assert(!store.ReloadNow(j.dump(),false).ok);assert(store.PublishAtBoundary().ok);assert(store.Current()->generation==2&&pinned->revision==1);
 assert(!store.ReloadNow("bad",true).ok);assert(store.Current()->generation==2);
 std::thread reader([&]{for(int i=0;i<2000;++i){auto s=store.Current();assert(s->rules.size()==1);}});
 for(int i=0;i<100;++i)assert(store.ReloadNow(j.dump(),true).ok);reader.join();assert(store.Current()->generation==102);
 // Predicate references are expanded with bounded depth/nodes and three-valued logic.
 auto nested=j; nested["predicates"]={{"healthy",J{{"all",J::array({J{{"field","distance"},{"op","lt"},{"value",30}},J{{"not",J{{"field","combat"},{"op","eq"},{"value",false}}}}})}}}};
 nested["rules"][0]["when_expr"]={{"ref","healthy"}};
 assert(Parse(nested.dump(),f,c,p).ok);o.flags["combat"]=true;assert(Rank(p,o).size()==1);
 o.flags.erase("combat");assert(Rank(p,o).empty());
 nested["rules"][0]["when_expr"]={{"not",J{{"not",J{{"field","combat"},{"op","eq"},{"value",true}}}}}};
 assert(Parse(nested.dump(),f,c,p).ok);assert(Rank(p,o).empty());o.flags["combat"]=true;assert(Rank(p,o).size()==1);
 bad=nested;bad["predicates"]["cycle"]={{"ref","cycle"}};assert(!Parse(bad.dump(),f,c,p).ok);
 bad=nested;bad["predicates"]["a"]={{"ref","b"}};bad["predicates"]["b"]={{"ref","a"}};assert(!Parse(bad.dump(),f,c,p).ok);
 bad=nested;bad["rules"][0]["when_expr"]={{"ref","absent"}};assert(!Parse(bad.dump(),f,c,p).ok);
 bad=nested;J deep={{"field","combat"},{"op","eq"},{"value",true}};for(int n=0;n<10;++n)deep=J{{"not",deep}};bad["rules"][0]["when_expr"]=deep;assert(!Parse(bad.dump(),f,c,p).ok);
 bad=nested;bad["rules"][0]["when_expr"]={{"all",J::array({J{{"field","combat"},{"op","eq"},{"value",false}},J{{"not",J{{"field","phase"},{"op","eq"},{"value","absent"}}}}})}};
 assert(Parse(bad.dump(),f,c,p).ok);assert(Rank(p,o).empty());
 bad=nested;bad["rules"][0]["when_expr"]={{"any",J::array({J{{"field","combat"},{"op","eq"},{"value",true}},J{{"field","phase"},{"op","eq"},{"value","missing"}}})}};
 assert(Parse(bad.dump(),f,c,p).ok);assert(Rank(p,o).size()==1);
 // Expanded references cannot hide exponential expression size behind a short document.
 bad=nested;J leaf={{"field","combat"},{"op","eq"},{"value",true}};bad["predicates"]=J::object();
 bad["predicates"]["base"]={{"all",J::array({leaf,leaf,leaf,leaf,leaf,leaf,leaf,leaf,leaf,leaf,leaf,leaf,leaf,leaf,leaf,leaf})}};
 J refs=J::array();for(int n=0;n<32;++n)refs.push_back(J{{"ref","base"}});
 bad["rules"][0]["when_expr"]={{"all",refs}};assert(!Parse(bad.dump(),f,c,p).ok);
 // Global expanded-node budget also limits repeated individually valid references.
 bad=j;J manyLeaves=J::array();for(int n=0;n<32;++n)manyLeaves.push_back(J{{"field","combat"},{"op","eq"},{"value",true}});
 bad["predicates"]={{"wide",J{{"all",manyLeaves}}}};bad["rules"]=J::array();
 for(int n=0;n<500;++n){auto repeat=rule;repeat["id"]="repeat-"+std::to_string(n);repeat["when_expr"]={{"ref","wide"}};bad["rules"].push_back(repeat);}
 assert(!Parse(bad.dump(),f,c,p).ok);
 // Parse the actual sample using the exact registered native adapter capabilities.
 std::ifstream sample("config/pve-decision.example.json");assert(sample);std::stringstream bytes;bytes<<sample.rdbuf();
 auto standard=FieldCatalog::Standard();CapabilityCatalog native;
 native.abilities["mage.frostbolt"]={ActionKind::Cast,116,Casting|Movement,{"enemy"}};
 native.abilities["warrior.sunder_armor"]={ActionKind::Cast,7386,Casting,{"enemy"}};
 native.abilities["priest.heal"]={ActionKind::Cast,2054,Casting|Movement,{"self"}};
 assert(Parse(bytes.str(),standard,native,p).ok);assert(!p.enabled);p.enabled=true;
 Observation actual;actual.scope.actor=7;actual.scope.actorGeneration=1;actual.sequence=1;actual.nowMs=100;actual.targets["self"]=7;
 actual.numbers["self.class"]=5;actual.numbers["self.health_pct"]=30;
 assert(Rank(p,actual).size()==1&&Rank(p,actual)[0].action.target=="self");
 assert(Trace(p,actual)[2].eligible);
 actual.scope.sceneGeneration=1;p.generation=1;auto intents=Rank(p,actual);Arbiter arbiter;SafeFake executor;EventBuffer events;
 auto dispatched=Dispatch(intents,actual,arbiter,executor,events,true);assert(dispatched.ok&&executor.executed==0);
 actual.numbers.erase("self.health_pct");auto traces=Trace(p,actual);assert(traces[2].unknown&&traces[2].reason=="unknown_observation");
 assert(Trace(p,actual,1).size()==1);p.enabled=false;assert(Trace(p,actual)[0].reason=="disabled");
 std::cout<<"PASS policy parsing, fail-closed conditions, deterministic ranking, safe publication and concurrent snapshots\n";
}
