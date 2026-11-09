"""Compile real decision modules and exercise offline JSON workflows; no realm or game data."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "src/game/PlayerBots"


def main():
    with tempfile.TemporaryDirectory(prefix="bot-decision-") as directory:
        output = Path(directory)
        for name, sources in [
            ("bot_decision_test", ["BotDecision.cpp"]),
            ("bot_decision_policy_test", ["BotDecision.cpp", "BotDecisionPolicy.cpp"]),
            ("bot_encounter_test", ["BotDecision.cpp", "BotDecisionPolicy.cpp", "BotEncounter.cpp"]),
        ]:
            binary = output / name
            subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++14", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-I" + str(SOURCES), "-I" + str(ROOT / "dep/include"),
                *[str(SOURCES / source) for source in sources],
                str(ROOT / "tests" / (name + ".cpp")), "-o", str(binary),
            ], check=True, cwd=ROOT)
            subprocess.run([str(binary)], check=True, cwd=ROOT)

        tool = str(output / "policy-tool")
        subprocess.run(["sh", str(ROOT / "tools/ai/build-policy-tool.sh"), tool], check=True)

        def run(*args, success=True):
            process = subprocess.run([tool, *map(str, args)], cwd=ROOT, text=True, capture_output=True)
            if success:
                assert process.returncode == 0, process.stderr
                return json.loads(process.stdout)
            assert process.returncode != 0
            assert json.loads(process.stderr)["ok"] is False

        catalog = run("catalog")
        assert catalog["fields"]["self.ready_spells"] == "id_set"
        example = ROOT / "config/pve-decision.example.json"
        assert run("validate", example, ROOT / "config/pve-encounter.example.json")["ok"]
        policy = json.loads(example.read_text())
        policy["enabled"] = True
        enabled = output / "enabled.json"
        enabled.write_text(json.dumps(policy))
        result = run("evaluate", enabled, ROOT / "config/pve-observation.example.json")
        assert len(result["candidates"]) == 1
        assert result["candidates"][0]["ability"] == "priest.heal"
        observation = json.loads((ROOT / "config/pve-observation.example.json").read_text())
        del observation["numbers"]["self.health_pct"]
        missing = output / "missing.json"
        missing.write_text(json.dumps(observation))
        result = run("evaluate", enabled, missing)
        assert not result["candidates"]
        assert any(trace["unknown"] for trace in result["rules"] if trace["id"] == "priest-basic-heal")
        # A known empty cooldown set differs from an unobserved set, even under negation.
        policy["rules"] = [next(rule for rule in policy["rules"] if rule["id"] == "priest-basic-heal")]
        policy["rules"][0]["when"] = []
        policy["rules"][0]["when_expr"] = {"field": "self.ready_spells", "op": "not_contains", "value": 116}
        enabled.write_text(json.dumps(policy))
        observation["sets"]["self.ready_spells"] = []
        missing.write_text(json.dumps(observation))
        assert run("evaluate", enabled, missing)["candidates"]
        del observation["sets"]["self.ready_spells"]
        missing.write_text(json.dumps(observation))
        assert not run("evaluate", enabled, missing)["candidates"]
        invalid = output / "invalid.json"
        invalid.write_text('{"schema_version":1,"schema_version":1}')
        run("validate", invalid, success=False)
        run("evaluate", enabled, invalid, success=False)
        print("PASS offline catalog, real JSON examples, candidate tracing, missing facts and invalid input rejection")


if __name__ == "__main__":
    main()
