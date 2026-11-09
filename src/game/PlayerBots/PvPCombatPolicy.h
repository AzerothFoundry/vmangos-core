#ifndef PVP_COMBAT_POLICY_H
#define PVP_COMBAT_POLICY_H
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
namespace PvPCombatPolicy {
struct Context {
    std::map<std::string, double> numbers;
    std::map<std::string, bool> flags;
    std::map<std::string, std::set<uint32_t>> sets;
    std::map<uint32_t, uint32_t> recentTargetSpellsMs;
};
struct Condition {
    std::string field, op;
    double number = 0;
    std::vector<double> values;
    bool flag = false;
    uint32_t spell = 0, withinMs = 0;
};
struct Adjustment {
    std::vector<Condition> conditions;
    int add = 0;
};
struct Action {
    std::string kind = "cast", actor = "owner", target = "enemy", category = "damage";
    uint32_t spell = 0;
    bool cancelOwnDamageCast = false;
};
struct Timing {
    double chance = 1;
    uint32_t reactionMinMs = 0, reactionMaxMs = 0;
    double progressMinPct = 0, progressMaxPct = 100;
};
struct Rule {
    uint32_t classId = 0, role = 0;
    std::string id;
    bool enabled = true;
    int priority = 0;
    bool fast = false;
    uint32_t minIntervalMs = 0;
    std::vector<Condition> conditions, any;
    std::vector<Adjustment> adjustments;
    Action action;
    Timing timing;
};
struct Snapshot {
    uint32_t schemaVersion = 1;
    uint64_t revision = 0, generation = 0;
    bool enabled = false, dirty = false;
    std::string serialized;
    std::vector<Rule> rules;
};
// Retain the same shared_ptr<const Snapshot> across Rank and candidate execution.
// Raw rule pointers refer to that immutable snapshot, never a later Current().
struct Candidate {
    Rule const *rule = nullptr;
    int64_t score = 0;
};
struct Result {
    bool ok;
    std::string message;
};
// Pure parser/ranker. Parse never mutates its output on failure.
Result Parse(std::string const &, Snapshot &);
std::vector<Candidate> Rank(Snapshot const &, Context const &, bool fastOnly = false);
Result Configure(std::string const &fixedPath);
std::shared_ptr<Snapshot const> Current();
void Poll();
Result Reload();
Result Patch(std::string const &ruleId, std::string const &key, std::string const &jsonValue);
Result SetEnabled(bool);
// In-process single writer; baseline checked twice, then atomic rename.
// External editors must serialize their writes: POSIX rename is not a cross-process CAS.
Result Save();
std::string Status();
std::string RuleJson(std::string const &id);
} // namespace PvPCombatPolicy
#endif
