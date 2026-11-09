#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
task_output=${1:-/tmp/azeroth-policy-tool}
"${CXX:-c++}" -std=c++14 -pthread -Wall -Wextra -Werror \
  -I"$task_root/src/game/PlayerBots" -I"$task_root/dep/include" \
  "$task_root/src/game/PlayerBots/BotDecision.cpp" \
  "$task_root/src/game/PlayerBots/BotDecisionPolicy.cpp" \
  "$task_root/src/game/PlayerBots/BotEncounter.cpp" \
  "$task_root/tools/ai/policy_tool.cpp" -o "$task_output"
printf '%s\n' "$task_output"
