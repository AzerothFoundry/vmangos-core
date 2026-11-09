// GPL-2.0-or-later. Opt-in, read-only adapter; this module never executes actions.
#ifndef MANGOS_PVE_DECISION_RUNTIME_H
#define MANGOS_PVE_DECISION_RUNTIME_H
#include "BotDecision.h"
#include <memory>
class PartyBotAI;
namespace BotDecision { namespace PolicyRules { struct Policy; } }
namespace PveDecisionRuntime {
// Owned by a single AI; no world pointers escape and no global actor registry exists.
struct State {
    uint64_t actorGeneration = 0, sceneGeneration = 0, sequence = 0, nextTickMs = 0;
    uint32_t map = 0, instance = 0;
    uint64_t target = 0, group = 0;
    bool combat = false, alive = false;
    std::shared_ptr<BotDecision::PolicyRules::Policy const> pinned;
    BotDecision::EventBuffer events{32};
    uint64_t observations = 0;
    std::string status = "disabled", policyStatus;
    std::vector<std::string> trace; // At most 16 rule summaries, refreshed only in shadow.
};
// PveDecision.Shadow=false and PveDecision.PolicyFile="" by default.
// When enabled, samples at most once a second and validates proposed spells only.
// self.knows contains all exact, non-removed, non-disabled learned IDs.
// self.ready_spells is the subset with native cooldown and GCD ready; passive
// or otherwise uncastable spells may be present. NativeCheck remains authoritative.
// Existing combat and movement ownership remain untouched. Encounter adapters are future work.
void Observe(PartyBotAI&, State&);
}
#endif
