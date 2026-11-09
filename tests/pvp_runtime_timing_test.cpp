#include "BattleBotPolicyRuntime.h"
#include "PvPCombatPolicy.h"
#include <cassert>
#include <iostream>
int main()
{
    using BattleBotPolicyRuntime::TimingReady;
    PvPCombatPolicy::Snapshot snapshot;
    PvPCombatPolicy::Rule rule;
    rule.classId = 8;
    rule.enabled = false;
    rule.action.spell = 116;
    rule.action.actor = "owner";
    snapshot.rules.push_back(rule);
    auto norm = +[](uint32_t id) { return id == 205 ? 116u : id; };
    assert(BattleBotPolicyRuntime::DeclaresManaged(snapshot, 8, 1, 116, "owner", norm));
    assert(!BattleBotPolicyRuntime::DeclaresManaged(snapshot, 5, 1, 116, "owner", norm));
    assert(!BattleBotPolicyRuntime::DeclaresManaged(snapshot, 8, 1, 116, "pet", norm));
    snapshot.rules[0].role = 4;
    assert(!BattleBotPolicyRuntime::DeclaresManaged(snapshot, 8, 1, 116, "owner", norm));
    snapshot.rules[0].action.spell = 205;
    assert(BattleBotPolicyRuntime::DeclaresManaged(snapshot, 8, 4, 116, "owner", norm));
    // One sampled value is retained: tick retries/reload do not manufacture a new chance.
    for (int i = 0; i < 100; ++i)
        assert(!TimingReady(.9, .5, 500000, .8, 20, 80, 100, 300, 90, 1000));
    assert(TimingReady(.9, .5, 500000, 1, 20, 80, 100, 300, 50, 200));
    assert(!TimingReady(.9, .5, 500000, 1, 20, 80, 100, 300, 49, 200));
    assert(!TimingReady(.9, .5, 500000, 1, 20, 80, 100, 300, 50, 199));
    assert(!TimingReady(0, 0, 0, 0, 0, 100, 0, 0, 100, 1000));
    assert(TimingReady(1, 0, 0, 1, 0, 100, 0, 0, 100, 1000));
    // Pushback reduces progress: it delays the same ticket, it does not resample.
    assert(!TimingReady(.9, .5, 500000, 1, 20, 80, 100, 300, 30, 200));
    assert(TimingReady(.9, .5, 500000, 1, 20, 80, 100, 300, 51, 220));
    std::cout << "PASS production interrupt timing gate: stable chance, endpoints, depth, reaction and pushback\n";
}
