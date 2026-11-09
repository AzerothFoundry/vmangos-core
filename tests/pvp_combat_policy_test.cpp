#include "PvPCombatPolicy.h"
#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
#include <unistd.h>
using namespace PvPCombatPolicy;
using J = nlohmann::json;
static J rule(std::string id, int priority) {
    return J{{"id", id},
             {"class", 9},
             {"priority", priority},
             {"when", J::array({J{{"field", "target.distance"}, {"op", "lt"}, {"value", 30}}})},
             {"action", J{{"kind", "cast"}, {"spell", 5782}, {"category", "control"}}}};
}
static void writeFile(std::string const &path, J const &j) {
    std::ofstream f(path);
    f << j.dump();
}
int main() {
    J j = {
        {"schema_version", 1}, {"revision", 1}, {"enabled", true}, {"rules", J::array({rule("B", 10), rule("A", 10)})}};
    Snapshot s;
    assert(Parse(j.dump(), s).ok);
    Context x;
    x.numbers["self.class"] = 9;
    x.numbers["target.distance"] = 20;
    auto ranked = Rank(s, x);
    assert(ranked.size() == 2 && ranked[0].rule->id == "A");
    x.numbers.erase("target.distance");
    assert(Rank(s, x).empty());
    x.numbers["target.distance"] = 20;
    // Finisher/form/flag predicates use only observed own state; missing fields fail closed.
    {
        auto extended=j;extended["rules"][1]["enabled"]=false;
        extended["rules"][0]["when"]=J::array({
            J{{"field","self.combo_points"},{"op","gte"},{"value",4}},
            J{{"field","self.form"},{"op","eq"},{"value",1}},
            J{{"field","self.combat"},{"op","eq"},{"value",true}},
            J{{"field","self.stealthed"},{"op","eq"},{"value",false}},
            J{{"field","self.carrying_flag"},{"op","eq"},{"value",false}},
            J{{"field","self.opening_flag"},{"op","eq"},{"value",false}}});
        Snapshot own;assert(Parse(extended.dump(),own).ok);Context q=x;
        assert(Rank(own,q).empty());q.numbers["self.combo_points"]=5;q.numbers["self.form"]=1;
        q.flags["self.combat"]=true;q.flags["self.stealthed"]=false;
        q.flags["self.carrying_flag"]=false;q.flags["self.opening_flag"]=false;
        assert(Rank(own,q).size()==1);
        q.numbers["self.combo_points"]=1;assert(Rank(own,q).empty());q.numbers["self.combo_points"]=5;
        q.numbers["self.form"]=0;assert(Rank(own,q).empty());q.numbers["self.form"]=1;
        for(auto field:{"self.stealthed","self.carrying_flag","self.opening_flag"}) {
            q.flags[field]=true;assert(Rank(own,q).empty());q.flags[field]=false;
            q.flags.erase(field);assert(Rank(own,q).empty());q.flags[field]=false;
        }
        auto badOwn=extended;badOwn["rules"][0]["when"][0]["value"]=true;assert(!Parse(badOwn.dump(),own).ok);
        badOwn=extended;badOwn["rules"][0]["when"][2]["value"]=1;assert(!Parse(badOwn.dump(),own).ok);
    }
    J dynamic = j;
    dynamic["rules"][1]["adjust"] =
        J::array({J{{"when", J::array({J{{"field", "self.moving"}, {"op", "eq"}, {"value", true}}})}, {"add", 10}}});
    assert(Parse(dynamic.dump(), s).ok);
    x.flags["self.moving"] = true;
    assert(Rank(s, x)[0].score == 20);
    x.flags.clear();
    assert(Rank(s, x)[0].score == 10);
    for (std::string field : {"ignore_cd", "ignore_gcd", "script"}) {
        auto bad = j;
        bad["rules"][0]["action"][field] = true;
        auto old = s.serialized;
        assert(!Parse(bad.dump(), s).ok && s.serialized == old);
    }
    for (auto bad : {J{{"x", 1}}, J{{"schema_version", 2}, {"revision", 1}, {"enabled", true}, {"rules", J::array()}}})
        assert(!Parse(bad.dump(), s).ok);
    assert(!Parse("{\"schema_version\":1,\"schema_version\":1}", s).ok);
    assert(!Parse("{\"schema_version\":NaN}", s).ok);
    auto bad = j;
    bad["rules"][0]["when"][0]["field"] = "target.hidden_cooldown";
    assert(!Parse(bad.dump(), s).ok);
    bad = j;
    bad["rules"][0]["class"] = 6;
    assert(!Parse(bad.dump(), s).ok);
    auto observe = j;
    observe["rules"][0]["when"] =
        J::array({J{{"field", "target.used_spell"}, {"op", "seen"}, {"value", 1953}, {"within_ms", 15000}}});
    assert(Parse(observe.dump(), s).ok);
    x.recentTargetSpellsMs[1953] = 14000;
    assert(Rank(s,x).size()==1); // No observed target, even if history was supplied.
    x.flags["target.player"]=true;
    assert(Rank(s, x).size() == 2);
    x.recentTargetSpellsMs[1953] = 15001;
    assert(Rank(s, x).size() == 1);
    // Ally HoTs distinguish any caster from this bot; absent perception is not an empty set.
    for (auto field : {"ally.aura", "ally.own_aura"}) {
        auto heal=j;heal["rules"][1]["enabled"]=false;
        heal["rules"][0]["when"]=J::array({J{{"field",field},{"op","lacks"},{"value",139}}});
        Snapshot hot;assert(Parse(heal.dump(),hot).ok);Context ally=x;
        assert(Rank(hot,ally).empty());
        ally.sets[field]={};assert(Rank(hot,ally).size()==1);
        ally.sets[field].insert(139);assert(Rank(hot,ally).empty());
        heal["rules"][0]["when"][0]["op"]="has";assert(Parse(heal.dump(),hot).ok);
        assert(Rank(hot,ally).size()==1);ally.sets[field].clear();assert(Rank(hot,ally).empty());
        heal["rules"][0]["when"][0]["value"]=false;assert(!Parse(heal.dump(),hot).ok);
    }
    auto unseen=observe;unseen["rules"][0]["when"][0]["op"]="not_seen";
    Snapshot notSeen;assert(Parse(unseen.dump(),notSeen).ok);Context unknown=x;unknown.flags.erase("target.player");unknown.recentTargetSpellsMs.clear();
    assert(Rank(notSeen,unknown).size()==1);
    unknown.flags["target.player"]=false;assert(Rank(notSeen,unknown).size()==2); // Observed NPC is valid.
    unknown.recentTargetSpellsMs[1953]=0;assert(Rank(notSeen,unknown).size()==1);
    auto setRule = j;
    setRule["rules"][0]["when"] = J::array({J{{"field", "self.knows"}, {"op", "has"}, {"value", 5782}}});
    setRule["rules"][0]["any"] = J::array({J{{"field", "target.casting"}, {"op", "eq"}, {"value", true}},
                                           J{{"field", "target.distance"}, {"op", "lte"}, {"value", 10}}});
    setRule["rules"][0]["fast"] = true;
    assert(Parse(setRule.dump(), s).ok);
    assert(Rank(s, x, true).empty());
    x.sets["self.knows"].insert(5782);
    x.flags["target.casting"] = true;
    assert(Rank(s, x, true).size() == 1);
    setRule["rules"][0]["enabled"] = false;
    assert(Parse(setRule.dump(), s).ok && s.rules.size() == 2 && Rank(s, x, true).empty());
    auto timing = j;
    timing["rules"][0]["action"]["category"] = "interrupt";
    timing["rules"][0]["timing"] = J{{"chance", .5},
                                     {"reaction_min_ms", 100},
                                     {"reaction_max_ms", 250},
                                     {"progress_min_pct", 20},
                                     {"progress_max_pct", 90}};
    assert(Parse(timing.dump(), s).ok);
    timing["rules"][0]["timing"]["reaction_max_ms"] = 50;
    assert(!Parse(timing.dump(), s).ok);
    char dir[] = "/tmp/pvp-policy-test-XXXXXX";
    assert(mkdtemp(dir));
    std::string path = std::string(dir) + "/policy.json";
    writeFile(path, j);
    assert(Configure(path).ok);
    auto retained = Current();
    assert(Patch("B", "priority", "30").ok && Current()->dirty);
    assert(Rank(*Current(), x)[0].rule->id == "B");
    assert(retained->rules[0].priority == 10);
    assert(!Patch("B", "action.spell", "1").ok);
    assert(!Patch("B", "priority", "false").ok);
    assert(Patch("B", "when.0.value", "25").ok);
    assert(!Patch("B", "when.3.value", "1").ok);
    writeFile(path, dynamic);
    Poll();
    assert(Current()->dirty && Status().find("conflict=1") != std::string::npos);
    assert(!Save().ok && Current()->dirty);
    assert(Reload().ok && !Current()->dirty);
    assert(Patch("B", "priority", "35").ok);
    assert(Configure(path).ok&&Current()->dirty);
    assert(Save().ok && !Current()->dirty && Current()->revision == 2);
    {
        std::ofstream f(path);
        f << "garbage";
    }
    auto old = Current();
    assert(!Reload().ok && Current() == old);
    writeFile(path, j);
    assert(Reload().ok);
    auto external=j;external["revision"]=9;writeFile(path,external);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));Poll();
    assert(Current()->revision==9&&!Current()->dirty);writeFile(path,j);assert(Reload().ok);
    assert(!Parse(std::string(1024*1024+1,' '),s).ok);
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::thread reader([&] {
        while (!stop) {
            auto snapshot = Current();
            auto candidates = Rank(*snapshot, x);
            for (auto c : candidates)
                assert(!c.rule->id.empty());
            ++reads;
        }
    });
    while (reads.load() == 0)
        std::this_thread::yield();
    for (int i = 0; i < 200; ++i)
        assert(Patch("B", "priority", std::to_string(i)).ok);
    stop = true;
    reader.join();
    assert(reads > 0);
    assert(SetEnabled(false).ok && Rank(*Current(), x).empty());
    assert(Save().ok);
    assert(!Configure(path + "other").ok);
    unlink(path.c_str());
    rmdir(dir);
    std::cout << "parse/rank/patch/last-good/conflicts/save/concurrent immutable snapshots PASS\n";
}
