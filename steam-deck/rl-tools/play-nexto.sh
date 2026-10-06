#!/bin/bash
# Play Rocket League against Nexto (RLBot v5) with BakkesMod.  Usage: play-nexto.sh 1v1|2v2 [extra launch args]
# Starts RLBotServer + the match (run_match.py), then launches Rocket League through Steam with
# -rlbot, which rl-launch.sh turns into the no-anti-cheat launch and auto-starts BakkesMod.
# run_match.py starts the match only once bakkes-helper.sh reports BakkesMod loaded (or not coming).
# Extra launch args are for testing, e.g. -bakkesoffline or -nobakkes (see rl-launch.sh).
MODE="${1:-1v1}"
shift
RLBOT=/home/deck/rlbot5
LOG="$RLBOT/logs/play-nexto-$MODE.log"
mkdir -p "$RLBOT/logs"
exec >>"$LOG" 2>&1
echo "=== $(date '+%F %T') play-nexto $MODE"
is_rl() { pgrep -f '^Z:.*RocketLeague\.exe' >/dev/null; }
if is_rl; then
    echo "Rocket League is already running - close it first, then start this again."
    command -v kdialog >/dev/null && kdialog --error "Rocket League is already running.\nClose it first, then start 'RL vs Nexto' again." 2>/dev/null
    exit 1
fi
pkill -x RLBotServer 2>/dev/null
pkill -f 'rlbot5/run_match.py' 2>/dev/null
cd "$RLBOT" || exit 1
"$RLBOT/.venv/bin/python" "$RLBOT/run_match.py" "match_$MODE.toml" > "$RLBOT/logs/match-$MODE.log" 2>&1 &
PY=$!
sleep 2
steam -applaunch 252950 -rlbot RLBot_ControllerURL=127.0.0.1:23233 RLBot_PacketSendRate=240 -nomovie "$@"
for _ in $(seq 1 240); do is_rl && break; sleep 1; done
if ! is_rl; then echo "Rocket League did not start"; kill $PY 2>/dev/null; pkill -x RLBotServer; exit 1; fi
echo "Rocket League running; waiting for it to exit"
while is_rl; do sleep 3; done
echo "Rocket League exited; cleaning up"
kill $PY 2>/dev/null; sleep 3
pkill -x RLBotServer 2>/dev/null
pkill -f 'venv/bin/python bot.py' 2>/dev/null
exit 0
