#include "BotDecision.h"
#include <cassert>
#include <iostream>
using namespace BotDecision;
namespace {
Observation Observe() {
    Observation o;
    o.scope = {10, 1, 1, 0, 7, 22}; o.nowMs = 1000; o.sequence = 20;
    o.targets["enemy"] = 900;
    return o;
}
Intent Make(Observation const& o, std::string owner, int priority, uint32_t resources) {
    Intent i;
    i.scope = o.scope; i.observation = o.sequence; i.policyGeneration = 1;
    i.encounter = o.encounter; i.encounterAttempt = o.encounterAttempt; i.encounterEpoch = o.encounterEpoch;
    i.issuedMs = o.nowMs; i.expiresMs = o.nowMs + 500;
    i.owner = owner; i.rule = owner; i.action = {"test.action", "enemy"};
    i.targetGuid = 900; i.priority = priority; i.resources = resources;
    return i;
}
struct Native final : Executor {
    unsigned checks = 0, executions = 0, displaced = 0;
    bool legal = true, commit = true;
    Result Check(Intent const&, Observation const&) override { ++checks; return {legal, "native_check"}; }
    Result Execute(Intent const&, Observation const&, std::vector<Intent> const& old) override {
        ++executions; displaced = unsigned(old.size()); return {commit, "native_commit"};
    }
};
}
int main() {
    auto o = Observe();
    auto move = Make(o, "travel", 10, Movement);
    auto cast = Make(o, "heal", 30, Movement | Casting);
    assert(Validate(move, o).ok);
    auto stale = move; stale.scope.actorGeneration++;
    assert(!Validate(stale, o).ok);
    stale = move; stale.scope.group++;
    assert(!Validate(stale, o).ok);
    stale = move; stale.scope.sceneGeneration++;
    assert(!Validate(stale, o).ok);
    stale = move; stale.observation = o.sequence + 1;
    assert(!Validate(stale, o).ok);
    stale = move; stale.expiresMs = o.nowMs;
    assert(!Validate(stale, o).ok);
    stale = move; stale.expiresMs = stale.issuedMs + 30001;
    assert(!Validate(stale, o).ok);
    stale = move; stale.resources = 8;
    assert(!Validate(stale, o).ok);
    auto changed = o; changed.targets["enemy"] = 901;
    assert(!Validate(move, changed).ok);

    Arbiter arbiter;
    assert(arbiter.Acquire(move, o).ok);
    auto equal = Make(o, "other", 10, Movement);
    assert(!arbiter.Acquire(equal, o).ok);
    auto interaction = Make(o, "loot", 5, Interaction);
    assert(arbiter.Acquire(interaction, o).ok);
    assert(arbiter.Active(o.nowMs).size() == 2);
    auto blocked = interaction; blocked.interruptible = false;
    arbiter.Release("loot"); assert(arbiter.Acquire(blocked, o).ok);
    auto both = Make(o, "emergency", 100, Movement | Interaction);
    assert(!arbiter.Acquire(both, o).ok);
    assert(arbiter.Active(o.nowMs).size() == 2); // No partial displacement.
    assert(arbiter.Acquire(cast, o).ok);
    assert(arbiter.Active(o.nowMs).size() == 2);
    arbiter.Release("heal"); assert(arbiter.Active(o.nowMs).size() == 1);
    assert(arbiter.Active(o.nowMs + 500).empty());

    // Renewal is exact: the same owner cannot replace an uninterruptible action.
    auto held = move; held.interruptible = false;
    assert(arbiter.Acquire(held, o).ok);
    assert(arbiter.Acquire(held, o).ok);
    auto changedAction = held; changedAction.action.ability = "another.action";
    assert(!arbiter.Acquire(changedAction, o).ok);
    auto differentResource = held; differentResource.resources = Interaction;
    assert(!arbiter.Acquire(differentResource, o).ok); // Same owner cannot silently drop a held action.
    arbiter.Reset(o.scope);
    assert(arbiter.Acquire(move, o).ok);
    EventBuffer events(4); Native native;
    auto result = Dispatch({cast}, o, arbiter, native, events, true);
    assert(result.ok && native.checks == 1 && native.executions == 0);
    assert(arbiter.Active(o.nowMs).front().owner == "travel");
    native.commit = false;
    assert(!Dispatch({cast}, o, arbiter, native, events, false).ok);
    assert(native.displaced == 1 && arbiter.Active(o.nowMs).front().owner == "travel");
    native.commit = true;
    assert(Dispatch({cast}, o, arbiter, native, events, false).ok);
    assert(arbiter.Active(o.nowMs).front().owner == "heal");
    auto records = events.Drain();
    assert(records.size() == 4 && records.back().stage == "preempted");
    native.legal = false; native.checks = 0;
    std::vector<Intent> many(80, cast);
    assert(!Dispatch(many, o, arbiter, native, events, false, 999).ok);
    assert(native.checks == 64 && events.Drain().size() == 4 && events.Dropped() >= 60);
    auto fresh = o; ++fresh.scope.actorGeneration;
    auto freshMove = Make(fresh, "new_actor", 1, Movement);
    assert(arbiter.Acquire(freshMove, fresh).ok);
    assert(arbiter.Active(o.nowMs).size() == 1);
    assert(!arbiter.Acquire(cast, fresh).ok); // Old generation cannot evict new owner.
    auto encounter = o; encounter.encounter = "test.boss"; encounter.encounterAttempt = 1; encounter.encounterEpoch = 1;
    auto phaseOne = Make(encounter, "phase_one", 20, Movement);
    assert(arbiter.Acquire(phaseOne, encounter).ok);
    ++encounter.encounterEpoch;
    assert(!Validate(phaseOne, encounter).ok);
    auto phaseTwo = Make(encounter, "phase_two", 1, Movement);
    assert(arbiter.Acquire(phaseTwo, encounter).ok); // Stale phase cannot retain an uninterruptible lease.
    ++encounter.encounterAttempt;
    assert(!Validate(phaseTwo, encounter).ok);
    EventBuffer zero(0); zero.Push({}); assert(zero.Drain().empty() && zero.Dropped() == 1);
    std::cout << "PASS decision scope, target, deadline, arbitration, preemption commit, shadow and bounded events\n";
}
