#include "PveDecisionRuntime.h"
#include "BotDecisionPolicy.h"
#include "PartyBotAI.h"
#include "BattleBotPolicyRuntime.h"
#include "Config/Config.h"
#include "Player.h"
#include "Map.h"
#include "SpellMgr.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>

namespace PveDecisionRuntime { namespace {
using namespace BotDecision;
uint64_t Now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
CapabilityCatalog Capabilities() { return FoundationCapabilities(); }
struct Shared {
    std::mutex mutex;
    PolicyRules::Store store{FieldCatalog::Standard(), Capabilities()};
    uint64_t nextPoll = 0;
    std::string path, acceptedText;
    std::string error;
};
Shared& SharedState() { static Shared s; return s; }
std::shared_ptr<PolicyRules::Policy const> Snapshot(uint64_t now, std::string& status) {
    auto& s = SharedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (now >= s.nextPoll) {
        s.nextPoll = now + 1000;
        std::string path = sConfig.GetStringDefault("PveDecision.PolicyFile", "");
        if (path != s.path) { s.path = path; s.acceptedText.clear(); }
        if (!path.empty()) {
            std::ifstream input(path, std::ios::binary);
            std::string text;
            if (input) {
                // Bound allocation before parsing. Poll once globally, never once per bot.
                char buffer[4096];
                while (input && text.size() <= 1024 * 1024) {
                    input.read(buffer, sizeof(buffer));
                    text.append(buffer, size_t(input.gcount()));
                }
                if (input.bad()) s.error = "policy_read_failed";
                else if (text.size() > 1024 * 1024) s.error = "policy_exceeds_1MiB";
                else if (text != s.acceptedText) {
                    auto staged = s.store.Stage(text);
                    if (staged.ok) {
                        auto result = s.store.PublishAtBoundary();
                        if (result.ok) { s.acceptedText = text; s.error.clear(); }
                        else s.error = result.reason;
                    } else s.error = staged.reason;
                } else s.error.clear();
            } else s.error = "policy_file_unavailable";
        }
    }
    status = s.error.empty() ? s.store.Status() : s.error;
    return s.path.empty() ? nullptr : s.store.Current();
}
class ShadowExecutor final : public Executor {
public:
    explicit ShadowExecutor(PartyBotAI& ai) : m_ai(ai), m_capabilities(Capabilities()) {}
    Result Check(Intent const& intent, Observation const& current) override {
        auto valid = Validate(intent, current);
        if (!valid.ok) return valid;
        auto ability = m_capabilities.abilities.find(intent.action.ability);
        if (ability == m_capabilities.abilities.end()) return {false, "unsupported_ability"};
        if (intent.resources != ability->second.resources ||
            !ability->second.targets.count(intent.action.target)) return {false, "capability_mismatch"};
        Unit* target = intent.action.target == "self" ? m_ai.me : m_ai.me->GetVictim();
        if (!target || target->GetGUID() != intent.targetGuid || !target->IsAlive() ||
            target->GetMap() != m_ai.me->GetMap()) return {false, "target_changed"};
        if (target != m_ai.me && (target->GetTypeId() != TYPEID_UNIT ||
            target->GetCharmerOrOwnerPlayerOrPlayerItself() ||
            !target->IsVisibleForOrDetect(m_ai.me, m_ai.me, false) ||
            !m_ai.me->IsWithinLOSInMap(target) || !m_ai.IsValidHostileTarget(target)))
            return {false, "target_not_observed"};
        std::string why;
        auto spell = sSpellMgr.GetSpellEntry(ability->second.spell);
        bool legal = BattleBotPolicyRuntime::NativeCheck(m_ai.me, target, spell, why);
        return {legal, legal ? "native_check_passed_shadow_only" : why};
    }
    Result Execute(Intent const&, Observation const&, std::vector<Intent> const&) override {
        return {false, "shadow_execution_forbidden"};
    }
private:
    PartyBotAI& m_ai;
    CapabilityCatalog m_capabilities;
};
std::atomic<uint64_t> nextActorGeneration{1};
}
void Observe(PartyBotAI& ai, State& state) {
    if (!sConfig.GetBoolDefault("PveDecision.Shadow", false)) return;
    try {
        if (!ai.me || !ai.me->IsInWorld() || ai.me->IsBeingTeleported() ||
            !ai.me->GetMap() || ai.me->InBattleGround() || ai.me->m_duel) return;
        uint64_t now = Now();
        if (now < state.nextTickMs) return;
        state.nextTickMs = now + 1000;
        auto policy = Snapshot(now, state.policyStatus);
        if (!policy) { state.status = "no_policy"; return; }
        Player* p = ai.me;
        Unit* target = p->GetVictim();
        if (target && (!target->IsAlive() || target->GetTypeId() != TYPEID_UNIT ||
            target->GetCharmerOrOwnerPlayerOrPlayerItself() || target->GetMap() != p->GetMap() ||
            !target->IsVisibleForOrDetect(p, p, false) || !p->IsWithinLOSInMap(target) ||
            !ai.IsValidHostileTarget(target))) target = nullptr;
        uint64_t guid = target ? target->GetGUID() : 0;
        bool combat = p->IsInCombat(), alive = p->IsAlive();
        uint64_t group = p->GetGroup() ? p->GetGroup()->GetId() : 0;
        if (!state.actorGeneration) state.actorGeneration = nextActorGeneration.fetch_add(1);
        if (!state.sceneGeneration || state.map != p->GetMapId() || state.instance != p->GetInstanceId() ||
            state.target != guid || state.group != group || state.combat != combat || state.alive != alive) ++state.sceneGeneration;
        // New policies take effect for each actor outside combat; no mid-fight reassignment.
        if (!state.pinned || !combat) state.pinned = policy;
        state.map = p->GetMapId(); state.instance = p->GetInstanceId(); state.target = guid;
        state.combat = combat; state.alive = alive; state.group = group;
        Observation o;
        o.scope = {p->GetGUID(), state.actorGeneration, state.sceneGeneration, state.map, state.instance, group};
        o.sequence = ++state.sequence; o.nowMs = now;
        o.numbers = {{"scene.map", state.map}, {"scene.instance", state.instance},
            {"self.class", p->GetClass()}, {"self.role", ai.GetRole()},
            {"self.level", p->GetLevel()}, {"self.health_pct", p->GetHealthPercent()},
            {"self.mana_pct", p->GetMaxPower(POWER_MANA) ? p->GetPowerPercent(POWER_MANA) : 0.0}};
        o.flags = {{"self.alive", alive}, {"self.combat", combat},
            {"self.casting", p->IsNonMeleeSpellCasted()},
            {"self.moving", p->IsMoving()},
            {"self.controlled", p->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)},
            {"target.present", target != nullptr}};
        o.texts["scene.kind"] = p->GetMap()->IsRaid() ? "raid" : p->GetMap()->IsDungeon() ? "dungeon" : "world";
        o.targets["self"] = p->GetGUID();
        // Exact learned IDs, including passive spells; readiness is only the native
        // cooldown/GCD fact, not a claim of castability, resources or target range.
        auto& known = o.sets["self.knows"];
        auto& ready = o.sets["self.ready_spells"];
        for (auto const& learned : p->GetSpellMap()) {
            if (learned.second.state == PLAYERSPELL_REMOVED || learned.second.disabled ||
                !p->HasSpell(learned.first)) continue;
            known.insert(learned.first);
            auto* spell = sSpellMgr.GetSpellEntry(learned.first);
            if (spell && p->IsSpellReady(spell) && !p->HasGCD(spell)) ready.insert(learned.first);
        }
        if (target) {
            o.targets["enemy"] = guid;
            o.flags["target.alive"] = true;
            o.numbers["target.entry"] = target->GetEntry();
            o.numbers["target.health_pct"] = target->GetHealthPercent();
            o.numbers["target.distance"] = p->GetCombatDistance(target);
        }
        state.trace.clear();
        for (auto const& item : PolicyRules::Trace(*state.pinned, o, 16))
            state.trace.push_back(item.id + ":" + item.reason);
        auto candidates = PolicyRules::Rank(*state.pinned, o, 8);
        ShadowExecutor executor(ai);
        Arbiter unused;
        auto result = Dispatch(candidates, o, unused, executor, state.events, true, 8);
        state.status = result.reason;
        ++state.observations;
    } catch (...) {
        // Configuration/filesystem errors must never interrupt the legacy AI.
        state.status = "shadow_adapter_error";
    }
}
}
