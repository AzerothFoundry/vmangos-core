#include "PvPCombatPolicy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unistd.h>
namespace PvPCombatPolicy {
namespace {
using J = nlohmann::json;
constexpr size_t MaxBytes = 1024 * 1024;
void require(bool b, char const *message) {
    if (!b)
        throw std::runtime_error(message);
}
void keys(J const &j, std::initializer_list<char const *> allowed) {
    require(j.is_object(), "Expected object");
    for (auto i = j.begin(); i != j.end(); ++i) {
        bool found = false;
        for (auto k : allowed)
            if (i.key() == k)
                found = true;
        require(found, "Unknown JSON key (CD/GCD bypass keys forbidden)");
    }
}
J decode(std::string const &text) {
    require(text.size() <= MaxBytes, "Policy exceeds 1MiB");
    std::vector<std::set<std::string>> stack;
    return J::parse(text, [&](int depth, J::parse_event_t e, J &v) {
        require(depth <= 32, "JSON nesting too deep");
        if (e == J::parse_event_t::object_start)
            stack.emplace_back();
        if (e == J::parse_event_t::key)
            require(stack.back().insert(v.get<std::string>()).second, "Duplicate JSON key");
        if (e == J::parse_event_t::object_end)
            stack.pop_back();
        return true;
    });
}
bool has(J const &j, char const *k) { return j.find(k) != j.end(); }
J const &mandatory(J const &j, char const *k) {
    require(has(j, k), "Missing required key");
    return j.at(k);
}
double number(J const &j, double low, double high) {
    require(j.is_number(), "Expected number");
    double n = j.get<double>();
    require(std::isfinite(n) && n >= low && n <= high, "Number out of range/nonfinite");
    return n;
}
uint64_t integer(J const &j, uint64_t low, uint64_t high) {
    require(j.is_number_integer(), "Expected integer");
    if (j.is_number_unsigned()) {
        auto n = j.get<uint64_t>();
        require(n >= low && n <= high, "Integer out of range");
        return n;
    }
    auto n = j.get<int64_t>();
    require(n >= 0 && uint64_t(n) >= low && uint64_t(n) <= high, "Integer out of range");
    return uint64_t(n);
}
int signedInt(J const &j) {
    require(j.is_number_integer(), "Expected integer");
    return int(number(j, -100000, 100000));
}
bool boolean(J const &j) {
    require(j.is_boolean(), "Expected boolean");
    return j.get<bool>();
}
std::string string(J const &j) {
    require(j.is_string(), "Expected string");
    auto s = j.get<std::string>();
    require(s.size() <= 128, "String too long");
    return s;
}
bool member(std::string const &s, std::set<std::string> const &set) { return set.count(s) != 0; }
std::set<std::string> const Numeric = {"self.class",
                                       "self.role",
                                       "self.level",
                                       "self.health_pct",
                                       "self.mana_pct",
                                       "self.power",
                                       "self.combo_points",
                                       "self.form",
                                       "target.class",
                                       "target.health_pct",
                                       "target.mana_pct",
                                       "target.distance",
                                       "target.cast_remaining_ms",
                                       "target.cast_progress_pct",
                                       "enemies.nearby",
                                       "ally.health_pct"};
std::set<std::string> const Flags = {"self.combat", "self.stealthed", "self.carrying_flag", "self.opening_flag", "self.moving",          "self.casting",    "self.controlled", "self.mounted",
                                     "self.has_pet",         "self.has_shield", "target.casting",  "target.healing",
                                     "target.interruptible", "target.player",   "target.pet",      "target.controlled",
                                     "target.moving"};
std::set<std::string> const Sets = {"ally.aura", "ally.own_aura", "self.aura", "target.aura", "target.own_aura", "self.knows", "pet.knows"};
Condition condition(J const &j) {
    keys(j, {"field", "op", "value", "within_ms"});
    Condition c;
    c.field = string(mandatory(j, "field"));
    c.op = string(mandatory(j, "op"));
    auto const &v = mandatory(j, "value");
    if (Numeric.count(c.field)) {
        require(!has(j, "within_ms"), "within_ms requires observed spell");
        require(member(c.op, {"eq", "ne", "lt", "lte", "gt", "gte", "in"}), "Invalid numeric operator");
        if (c.op == "in") {
            require(v.is_array() && !v.empty() && v.size() <= 64, "Invalid in set");
            for (auto const &n : v)
                c.values.push_back(number(n, -1e9, 1e9));
        } else
            c.number = number(v, -1e9, 1e9);
    } else if (Flags.count(c.field)) {
        require(!has(j, "within_ms") && (c.op == "eq" || c.op == "ne"), "Invalid boolean condition");
        c.flag = boolean(v);
    } else if (Sets.count(c.field)) {
        require(!has(j, "within_ms") && (c.op == "has" || c.op == "lacks"), "Invalid set condition");
        c.spell = uint32_t(integer(v, 1, 2147483647));
    } else if (c.field == "target.used_spell") {
        require(c.op == "seen" || c.op == "not_seen", "Invalid observation operator");
        c.spell = uint32_t(integer(v, 1, 2147483647));
        c.withinMs = uint32_t(integer(mandatory(j, "within_ms"), 1, 600000));
    } else {
        throw std::runtime_error("Unknown condition field");
    }
    return c;
}
std::vector<Condition> conditions(J const &j) {
    require(j.is_array() && j.size() <= 32, "Invalid condition list");
    std::vector<Condition> out;
    for (auto const &v : j)
        out.push_back(condition(v));
    return out;
}
bool matches(Condition const &c, Context const &x) {
    if (Numeric.count(c.field)) {
        auto i = x.numbers.find(c.field);
        if (i == x.numbers.end() || !std::isfinite(i->second))
            return false;
        double n = i->second;
        if (c.op == "eq")
            return n == c.number;
        if (c.op == "ne")
            return n != c.number;
        if (c.op == "lt")
            return n < c.number;
        if (c.op == "lte")
            return n <= c.number;
        if (c.op == "gt")
            return n > c.number;
        if (c.op == "gte")
            return n >= c.number;
        return std::find(c.values.begin(), c.values.end(), n) != c.values.end();
    }
    if (Flags.count(c.field)) {
        auto i = x.flags.find(c.field);
        return i != x.flags.end() && (c.op == "eq" ? i->second == c.flag : i->second != c.flag);
    }
    if (Sets.count(c.field)) {
        auto i = x.sets.find(c.field);
        if (i == x.sets.end())
            return false;
        bool present = i->second.count(c.spell);
        return c.op == "has" ? present : !present;
    }
    // A missing observed target is not proof that the target has not cast.
    if (x.flags.find("target.player") == x.flags.end())
        return false;
    auto i = x.recentTargetSpellsMs.find(c.spell);
    bool seen = i != x.recentTargetSpellsMs.end() && i->second <= c.withinMs;
    return c.op == "seen" ? seen : !seen;
}
bool all(std::vector<Condition> const &c, Context const &x) {
    return std::all_of(c.begin(), c.end(), [&](Condition const &p) { return matches(p, x); });
}
std::mutex mutex;
std::shared_ptr<Snapshot const> active = std::make_shared<Snapshot>();
std::string path, baseline, lastError;
bool conflict = false;
std::chrono::steady_clock::time_point lastPoll;
std::string read() {
    require(!path.empty(), "Policy path not configured");
    std::ifstream f(path, std::ios::binary);
    require(bool(f), "Cannot open fixed policy file");
    std::string out;
    char buf[8192];
    while (f) {
        f.read(buf, sizeof(buf));
        out.append(buf, size_t(f.gcount()));
        require(out.size() <= MaxBytes, "Policy exceeds 1MiB");
    }
    require(f.eof(), "Policy read failed");
    return out;
}
void publish(Snapshot s, bool dirty) {
    s.generation = std::atomic_load(&active)->generation + 1;
    s.dirty = dirty;
    std::atomic_store(&active, std::shared_ptr<Snapshot const>(std::make_shared<Snapshot>(std::move(s))));
}
Result load() {
    try {
        auto text = read();
        Snapshot s;
        auto r = Parse(text, s);
        if (!r.ok) {
            lastError = r.message;
            return r;
        }
        baseline = text;
        conflict = false;
        lastError.clear();
        publish(std::move(s), false);
        return {true, "Policy reloaded"};
    } catch (std::exception const &e) {
        lastError = e.what();
        return {false, lastError};
    }
}
Result mutate(J j) {
    Snapshot s;
    auto r = Parse(j.dump(), s);
    if (!r.ok)
        return r;
    publish(std::move(s), true);
    return {true, "Policy memory updated (not saved)"};
}
} // namespace
Result Parse(std::string const &text, Snapshot &output) {
    try {
        auto j = decode(text);
        keys(j, {"schema_version", "revision", "enabled", "rules"});
        Snapshot s;
        s.schemaVersion = uint32_t(integer(mandatory(j, "schema_version"), 1, 1));
        s.revision = integer(mandatory(j, "revision"), 0, 9007199254740991ULL);
        s.enabled = boolean(mandatory(j, "enabled"));
        auto const &rules = mandatory(j, "rules");
        require(rules.is_array() && rules.size() <= 512, "Invalid rules count");
        std::set<std::string> ids;
        for (auto const &v : rules) {
            keys(v, {"id", "class", "role", "enabled", "priority", "fast", "min_interval_ms", "when", "any", "adjust",
                     "action", "timing"});
            Rule r;
            r.id = string(mandatory(v, "id"));
            require(!r.id.empty() && r.id.size() <= 64 &&
                        std::all_of(r.id.begin(), r.id.end(),
                                    [](unsigned char c) {
                                        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                               (c >= '0' && c <= '9') || c == '_' || c == '-';
                                    }) &&
                        ids.insert(r.id).second,
                    "Invalid/duplicate rule id");
            r.classId = uint32_t(integer(mandatory(v, "class"), 1, 11));
            require(r.classId != 6 && r.classId != 10, "Invalid playable class");
            if (has(v, "role"))
                r.role = uint32_t(integer(v.at("role"), 0, 4));
            r.priority = signedInt(mandatory(v, "priority"));
            if (has(v, "enabled"))
                r.enabled = boolean(v.at("enabled"));
            if (has(v, "fast"))
                r.fast = boolean(v.at("fast"));
            if (has(v, "min_interval_ms"))
                r.minIntervalMs = uint32_t(integer(v.at("min_interval_ms"), 0, 3600000));
            if (has(v, "when"))
                r.conditions = conditions(v.at("when"));
            if (has(v, "any"))
                r.any = conditions(v.at("any"));
            if (has(v, "adjust")) {
                require(v.at("adjust").is_array() && v.at("adjust").size() <= 32, "Invalid adjustments");
                for (auto const &a : v.at("adjust")) {
                    keys(a, {"when", "add"});
                    Adjustment adj;
                    adj.conditions = conditions(mandatory(a, "when"));
                    adj.add = signedInt(mandatory(a, "add"));
                    r.adjustments.push_back(adj);
                }
            }
            auto const &a = mandatory(v, "action");
            keys(a, {"kind", "spell", "actor", "target", "category", "cancel_own_damage_cast"});
            r.action.kind = string(mandatory(a, "kind"));
            require(r.action.kind == "cast", "Unknown action kind");
            r.action.spell = uint32_t(integer(mandatory(a, "spell"), 1, 2147483647));
            if (has(a, "actor"))
                r.action.actor = string(a.at("actor"));
            require(member(r.action.actor, {"owner", "pet"}), "Invalid action actor");
            if (has(a, "target"))
                r.action.target = string(a.at("target"));
            require(member(r.action.target, {"enemy", "self", "owner", "pet", "ally"}), "Invalid action target");
            r.action.category = string(mandatory(a, "category"));
            require(member(r.action.category, {"damage", "heal", "control", "interrupt", "defense", "utility"}),
                    "Invalid action category");
            if (has(a, "cancel_own_damage_cast"))
                r.action.cancelOwnDamageCast = boolean(a.at("cancel_own_damage_cast"));
            if (has(v, "timing")) {
                auto const &t = v.at("timing");
                keys(t, {"chance", "reaction_min_ms", "reaction_max_ms", "progress_min_pct", "progress_max_pct"});
                require(r.action.category == "interrupt" || t.empty(), "Timing only supported for interrupt");
                if (has(t, "chance"))
                    r.timing.chance = number(t.at("chance"), 0, 1);
                if (has(t, "reaction_min_ms"))
                    r.timing.reactionMinMs = uint32_t(integer(t.at("reaction_min_ms"), 0, 60000));
                if (has(t, "reaction_max_ms"))
                    r.timing.reactionMaxMs = uint32_t(integer(t.at("reaction_max_ms"), 0, 60000));
                if (has(t, "progress_min_pct"))
                    r.timing.progressMinPct = number(t.at("progress_min_pct"), 0, 100);
                if (has(t, "progress_max_pct"))
                    r.timing.progressMaxPct = number(t.at("progress_max_pct"), 0, 100);
                require(r.timing.reactionMinMs <= r.timing.reactionMaxMs &&
                            r.timing.progressMinPct <= r.timing.progressMaxPct,
                        "Inverted timing range");
            }
            s.rules.push_back(std::move(r));
        }
        s.serialized = j.dump(2);
        require(s.serialized.size() <= MaxBytes, "Serialized policy exceeds 1MiB");
        output = std::move(s);
        return {true, "Policy validated"};
    } catch (std::exception const &e) {
        return {false, e.what()};
    }
}
std::vector<Candidate> Rank(Snapshot const &s, Context const &x, bool fastOnly) {
    std::vector<Candidate> out;
    if (!s.enabled)
        return out;
    auto cls = x.numbers.find("self.class"), role = x.numbers.find("self.role");
    for (auto const &r : s.rules) {
        if (!r.enabled || (fastOnly && !r.fast) || cls == x.numbers.end() || cls->second != r.classId ||
            (r.role && (role == x.numbers.end() || role->second != r.role)))
            continue;
        if (!all(r.conditions, x) ||
            (!r.any.empty() &&
             !std::any_of(r.any.begin(), r.any.end(), [&](Condition const &c) { return matches(c, x); })))
            continue;
        int64_t score = r.priority;
        for (auto const &a : r.adjustments)
            if (all(a.conditions, x))
                score += a.add;
        out.push_back({&r, score});
    }
    std::sort(out.begin(), out.end(), [](Candidate const &a, Candidate const &b) {
        return a.score != b.score ? a.score > b.score : a.rule->id < b.rule->id;
    });
    return out;
}
std::shared_ptr<Snapshot const> Current() { return std::atomic_load(&active); }
Result Configure(std::string const &fixedPath) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!path.empty())
        return {path == fixedPath, path == fixedPath ? "Policy already configured; use explicit reload" : "Fixed policy path already configured"};
    path = fixedPath;
    return load();
}
Result Reload() {
    std::lock_guard<std::mutex> lock(mutex);
    return load();
}
void Poll() {
    std::lock_guard<std::mutex> lock(mutex);
    auto now = std::chrono::steady_clock::now();
    if (now - lastPoll < std::chrono::seconds(1))
        return;
    lastPoll = now;
    try {
        auto text = read();
        if (text == baseline)
            return;
        if (Current()->dirty) {
            conflict = true;
            lastError = "External change conflicts with unsaved memory policy";
            return;
        }
        load();
    } catch (std::exception const &e) {
        lastError = e.what();
        if (Current()->dirty)
            conflict = true;
    }
}
Result SetEnabled(bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
        auto j = decode(Current()->serialized);
        j["enabled"] = value;
        return mutate(j);
    } catch (std::exception const &e) {
        return {false, e.what()};
    }
}
Result Patch(std::string const &id, std::string const &key, std::string const &value) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
        auto j = decode(Current()->serialized);
        J *rule = nullptr;
        for (auto &r : j["rules"])
            if (r["id"] == id)
                rule = &r;
        require(rule, "Unknown rule id");
        auto val = decode(value);
        if (key == "priority" || key == "enabled" || key == "min_interval_ms")
            (*rule)[key] = val;
        else if (key.compare(0, 7, "timing.") == 0) {
            auto sub = key.substr(7);
            require(
                member(sub, {"chance", "reaction_min_ms", "reaction_max_ms", "progress_min_pct", "progress_max_pct"}),
                "Patch key not allowed");
            (*rule)["timing"][sub] = val;
        } else {
            std::vector<std::string> parts;
            size_t start = 0;
            for (size_t end; (end = key.find('.', start)) != std::string::npos; start = end + 1)
                parts.push_back(key.substr(start, end - start));
            parts.push_back(key.substr(start));
            require(parts.size() == 3, "Patch key not allowed");
            require((parts[0] == "when" || parts[0] == "any") ? parts[2] == "value"
                                                              : parts[0] == "adjust" && parts[2] == "add",
                    "Patch key not allowed");
            require(!parts[1].empty() && parts[1].size() <= 3 &&
                        std::all_of(parts[1].begin(), parts[1].end(), [](char c) { return c >= '0' && c <= '9'; }),
                    "Invalid patch index");
            size_t n = std::stoul(parts[1]);
            require(has(*rule, parts[0].c_str()) && (*rule)[parts[0]].is_array() && n < (*rule)[parts[0]].size(),
                    "Patch index must already exist");
            (*rule)[parts[0]][n][parts[2]] = val;
        }
        return mutate(j);
    } catch (std::exception const &e) {
        return {false, e.what()};
    }
}
Result Save() {
    std::lock_guard<std::mutex> lock(mutex);
    std::string temp;
    int fd = -1;
    try {
        require(read() == baseline, "Save conflict: file changed since load");
        auto s = Current();
        require(!s->serialized.empty(), "No valid policy loaded");
        auto j = decode(s->serialized);
        j["revision"] = s->revision + 1;
        Snapshot checked;
        auto valid = Parse(j.dump(), checked);
        require(valid.ok, valid.message.c_str());
        std::string data = checked.serialized + "\n";
        std::vector<char> name(path.begin(), path.end());
        std::string suffix = ".tmp.XXXXXX";
        name.insert(name.end(), suffix.begin(), suffix.end());
        name.push_back(0);
        fd = mkstemp(name.data());
        require(fd >= 0, "Cannot create policy temporary file");
        temp = name.data();
        size_t offset = 0;
        while (offset < data.size()) {
            ssize_t n = write(fd, data.data() + offset, data.size() - offset);
            require(n > 0, "Policy write failed");
            offset += size_t(n);
        }
        require(fsync(fd) == 0, "Policy fsync failed");
        close(fd);
        fd = -1;
        require(read() == baseline, "Save conflict: file changed during save");
        require(rename(temp.c_str(), path.c_str()) == 0, "Policy atomic rename failed");
        temp.clear();
        baseline = data;
        conflict = false;
        lastError.clear();
        publish(std::move(checked), false);
        return {true, "Policy atomically saved"};
    } catch (std::exception const &e) {
        if (fd >= 0)
            close(fd);
        if (!temp.empty())
            unlink(temp.c_str());
        lastError = e.what();
        conflict = lastError.find("conflict") != std::string::npos;
        return {false, lastError};
    }
}
std::string Status() {
    std::lock_guard<std::mutex> lock(mutex);
    auto s = Current();
    return "generation=" + std::to_string(s->generation) + " revision=" + std::to_string(s->revision) +
           " enabled=" + (s->enabled ? "1" : "0") + " dirty=" + (s->dirty ? "1" : "0") +
           " conflict=" + (conflict ? "1" : "0") + " rules=" + std::to_string(s->rules.size()) + " error=" + lastError;
}
std::string RuleJson(std::string const &id) {
    auto s = Current();
    try {
        auto j = decode(s->serialized);
        for (auto const &r : j["rules"])
            if (r["id"] == id)
                return r.dump();
    } catch (...) {
    }
    return "";
}
} // namespace PvPCombatPolicy
