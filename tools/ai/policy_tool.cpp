// GPL-2.0-or-later. Offline validation only; never connects to a realm or executes actions.
#include "BotDecisionPolicy.h"
#include "BotEncounter.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cmath>
#include <iostream>
#include <stdexcept>
using J = nlohmann::json;
using namespace BotDecision;
namespace {
std::string Read(std::string const& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open input file");
    std::string bytes;
    char block[4096];
    while (file && bytes.size() <= 1024 * 1024) {
        file.read(block, sizeof(block)); bytes.append(block, size_t(file.gcount()));
    }
    if (file.bad() || bytes.size() > 1024 * 1024) throw std::runtime_error("input read failed or exceeds 1 MiB");
    return bytes;
}
void Keys(J const& object, std::set<std::string> const& allowed) {
    if (!object.is_object()) throw std::runtime_error("expected object");
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.count(it.key())) throw std::runtime_error("unrecognized observation key");
}
uint64_t UInt(J const& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
        throw std::runtime_error("expected unsigned integer");
    return value.get<uint64_t>();
}
Observation Decode(std::string const& text, FieldCatalog const& fields) {
    std::vector<std::set<std::string>> stack;
    auto j = J::parse(text, [&](int depth, J::parse_event_t event, J& value) {
        if (depth > 12) throw std::runtime_error("observation too deep");
        if (event == J::parse_event_t::object_start) stack.emplace_back();
        if (event == J::parse_event_t::key && !stack.back().insert(value.get<std::string>()).second)
            throw std::runtime_error("duplicate observation key");
        if (event == J::parse_event_t::object_end) stack.pop_back();
        return true;
    });
    Keys(j, {"scope", "sequence", "now_ms", "numbers", "flags", "texts", "sets", "targets"});
    auto const& scope = j.at("scope");
    Keys(scope, {"actor", "actor_generation", "scene_generation", "map", "instance", "group"});
    Observation o;
    o.scope.actor = UInt(scope.at("actor")); o.scope.actorGeneration = UInt(scope.at("actor_generation"));
    o.scope.sceneGeneration = UInt(scope.at("scene_generation"));
    auto map = UInt(scope.at("map")), instance = UInt(scope.at("instance"));
    if (map > UINT32_MAX || instance > UINT32_MAX) throw std::runtime_error("map/instance overflow");
    o.scope.map = uint32_t(map); o.scope.instance = uint32_t(instance);
    if (scope.count("group")) o.scope.group = UInt(scope.at("group"));
    o.sequence = UInt(j.at("sequence")); o.nowMs = UInt(j.at("now_ms"));
    if (!o.scope.actor || !o.scope.actorGeneration || !o.scope.sceneGeneration || !o.sequence)
        throw std::runtime_error("invalid observation identity");
    for (auto kind : {"numbers", "flags", "texts", "sets"}) {
        if (!j.count(kind)) continue;
        auto const& object = j.at(kind);
        if (!object.is_object() || object.size() > 128) throw std::runtime_error("invalid observation fields");
        for (auto it = object.begin(); it != object.end(); ++it) {
            auto field = fields.fields.find(it.key());
            if (field == fields.fields.end()) throw std::runtime_error("unregistered observation field");
            auto const& v = it.value(); std::string type = kind;
            if (type == "numbers" && field->second == ValueType::Number && v.is_number()) {
                double n = v.get<double>();
                if (!std::isfinite(n)) throw std::runtime_error("nonfinite observation number");
                o.numbers[it.key()] = n;
            } else if (type == "flags" && field->second == ValueType::Flag && v.is_boolean())
                o.flags[it.key()] = v.get<bool>();
            else if (type == "texts" && field->second == ValueType::Text && v.is_string() && v.get<std::string>().size() <= 128)
                o.texts[it.key()] = v.get<std::string>();
            else if (type == "sets" && field->second == ValueType::IdSet && v.is_array() && v.size() <= 4096) {
                auto& values = o.sets[it.key()];
                for (auto const& id : v) { auto n = UInt(id); if (n > UINT32_MAX) throw std::runtime_error("spell ID overflow"); values.insert(uint32_t(n)); }
            } else throw std::runtime_error("observation field type mismatch");
        }
    }
    if (j.count("targets")) {
        if (!j.at("targets").is_object() || j.at("targets").size() > 32) throw std::runtime_error("invalid targets");
        for (auto it = j.at("targets").begin(); it != j.at("targets").end(); ++it) {
            if (it.key().empty() || it.key().size() > 128) throw std::runtime_error("invalid target reference");
            o.targets[it.key()] = UInt(it.value());
        }
    }
    return o;
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "catalog") {
            J catalog{{"fields", J::object()}, {"abilities", J::object()}, {"status", "foundation_examples_not_complete_rotations"}};
            for (auto const& field : FieldCatalog::Standard().fields) {
                std::string type = field.second == ValueType::Number ? "number" : field.second == ValueType::Flag ? "flag" : field.second == ValueType::Text ? "text" : "id_set";
                catalog["fields"][field.first] = type;
            }
            for (auto const& capability : FoundationCapabilities().abilities)
                catalog["abilities"][capability.first] = {{"spell", capability.second.spell}, {"resources", capability.second.resources}, {"targets", capability.second.targets}};
            std::cout << catalog.dump(2) << '\n'; return 0;
        }
        if (argc != 3 && argc != 4) throw std::runtime_error("usage: policy-tool catalog | validate policy.json [encounter.json] | evaluate policy.json observation.json");
        std::string mode = argv[1];
        auto fields = FieldCatalog::Standard(); auto abilities = FoundationCapabilities();
        PolicyRules::Store store(fields, abilities);
        auto loaded = store.ReloadNow(Read(argv[2]), true);
        if (!loaded.ok) throw std::runtime_error(loaded.reason);
        J output;
        if (mode == "validate") {
            output = {{"ok", true}, {"policy_revision", store.Current()->revision}, {"rules", store.Current()->rules.size()}, {"mode", "offline_validation"}};
            if (argc == 4) {
                BotEncounter::Definition encounter;
                auto parsed = BotEncounter::Parse(Read(argv[3]), fields, encounter);
                if (!parsed.ok) throw std::runtime_error(parsed.reason);
                output["encounter"] = encounter.id;
            }
        } else if (mode == "evaluate" && argc == 4) {
            auto o = Decode(Read(argv[3]), fields);
            output = {{"ok", true}, {"mode", "offline_no_execution"}, {"rules", J::array()}, {"candidates", J::array()}};
            for (auto const& trace : PolicyRules::Trace(*store.Current(), o))
                output["rules"].push_back({{"id", trace.id}, {"eligible", trace.eligible}, {"unknown", trace.unknown}, {"reason", trace.reason}});
            for (auto const& i : PolicyRules::Rank(*store.Current(), o))
                output["candidates"].push_back({{"rule", i.rule}, {"owner", i.owner}, {"ability", i.action.ability}, {"target", i.action.target}, {"score", i.priority}, {"expires_ms", i.expiresMs}});
        } else throw std::runtime_error("unsupported mode or argument count");
        std::cout << output.dump(2) << '\n'; return 0;
    } catch (std::exception const& e) {
        std::cerr << J{{"ok", false}, {"error", e.what()}}.dump() << '\n'; return 1;
    }
}
