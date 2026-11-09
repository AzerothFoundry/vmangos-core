// GPL-2.0-or-later. One coordinator per encounter; value data only.
#ifndef MANGOS_BOT_ENCOUNTER_H
#define MANGOS_BOT_ENCOUNTER_H
#include "BotDecision.h"
#include <memory>
namespace BotDecision { namespace PolicyRules { struct Policy; }}
namespace BotEncounter {
using BotDecision::Result;
struct Key {
 uint32_t map=0,instance=0; uint64_t group=0,attempt=0; std::string encounter;
 bool operator==(Key const& o)const{return map==o.map&&instance==o.instance&&group==o.group&&attempt==o.attempt&&encounter==o.encounter;}
};
enum class Lifecycle {Idle,Preparing,Combat,Recovering,Completed,Failed};
struct Member {uint64_t actor=0;uint32_t classId=0,role=0;bool alive=true;};
struct Slot {std::string id;uint32_t count=1;std::set<uint32_t> classes,roles;};
struct Condition {std::string field,op;double number=0;bool flag=false;std::string text;BotDecision::ValueType type=BotDecision::ValueType::Number;};
struct Transition {std::string to;std::vector<Condition> all;};
struct Phase {std::string id;uint64_t minHoldMs=0;std::vector<Transition> transitions;};
struct Definition {uint32_t schemaVersion=1;uint64_t revision=0;std::string id,initialPhase;std::vector<Slot> slots;std::vector<Phase> phases;};
// Parsed definitions are immutable once published. Retain previous definition on failure.
Result Parse(std::string const&,BotDecision::FieldCatalog const&,Definition&);
struct State {Key key;Lifecycle lifecycle=Lifecycle::Idle;std::string phase;uint64_t policyGeneration=0,definitionRevision=0,phaseSinceMs=0,epoch=1;std::map<std::string,std::vector<uint64_t>> assignments;std::map<std::string,uint32_t> deficits;};
// Thread-confined; callers own synchronization and a bounded session registry.
// All members read the same snapshot; only the encounter authority may update it.
class Session {
public:
 Result Begin(Key const&,std::shared_ptr<Definition const>,std::shared_ptr<BotDecision::PolicyRules::Policy const>,uint64_t authority,uint64_t nowMs);
 Result Change(uint64_t authority,Lifecycle,uint64_t nowMs);
 Result Reconcile(uint64_t authority,std::vector<Member> const&);
 Result Tick(uint64_t authority,BotDecision::Observation const&);
 State Snapshot()const{return m_state;}
 std::shared_ptr<BotDecision::PolicyRules::Policy const> Policy()const{return m_policy;}
 Result Annotate(BotDecision::Observation&)const;
private:
 bool Authorized(uint64_t authority)const{return m_definition&&authority&&authority==m_authority;}
 State m_state;std::shared_ptr<Definition const> m_definition;std::shared_ptr<BotDecision::PolicyRules::Policy const> m_policy;uint64_t m_authority=0,m_lastNow=0,m_lastObservation=0,m_beginAt=0;
};
}
#endif
