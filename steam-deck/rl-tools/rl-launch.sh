#!/bin/bash
# Steam launch wrapper for Rocket League (appid 252950).
# Steam launch options must be:   /home/deck/rl-tools/rl-launch.sh %command%
#
# Normal launch from Steam  -> unchanged: RocketLeague_EAC.exe, Easy Anti-Cheat on, online play. No mods.
# Mods mode -> RocketLeague.exe (no anti-cheat) + BakkesMod auto-started. Triggered by either:
#   * picking Steam's own "Rocket League with Anti-Cheat Disabled (Mods and Limited Online Play)" play option
#   * marker args:  -bakkesmod   (used by rl-mods.sh;  stripped before the game sees it)
#                   -freeplay    (rl-mods.sh freeplay; stripped; BakkesMod then loads straight into freeplay)
#                   -rlbot       (used by play-nexto.sh; passed through, RLBot needs it; the Nexto match is held
#                                 back until BakkesMod is loaded, see bakkes-helper.sh)
#                   -nobakkes    (with -bakkesmod: no-anti-cheat exe but BakkesMod is NOT started; for testing)
#                   -bakkesoffline (with -bakkesmod: inject BakkesMod the no-internet way even when online; for testing)
#                   -psyoffline  (answer the game's config download locally even when online; for testing)
#                   -psymitm     (relay the game's config download and log its headers; for testing)
#                   -gamescopetest (Desktop Mode: run the game inside a nested gamescope, as Gaming Mode does; for testing)
# Mods mode without internet: the game's config download and BakkesMod's version check are answered locally
# (psynet-offline.py). Otherwise the game holds every map load for 55-90 s while it retries, and BakkesMod asks
# "could not verify RL version" in a box that leaves a black screen in Gaming Mode.
LOG=/home/deck/rl-tools/logs/rl-launch.log
mkdir -p "$(dirname "$LOG")"
for a in "$@"; do
    if [ "$a" = -gamescopetest ]; then
        # Desktop Mode stand-in for Gaming Mode: the same compositor (gamescope) around everything started
        # from here, the game's frame rate shown top-left.
        rest=(); for b in "$@"; do [ "$b" = -gamescopetest ] || rest+=("$b"); done
        preload="$LD_PRELOAD"
        export DXVK_HUD=fps
        exec env -u LD_PRELOAD gamescope -W 1280 -H 800 -w 1280 -h 800 -f -- env LD_PRELOAD="$preload" "$0" "${rest[@]}" 2>>/home/deck/rl-tools/logs/gamescope-test.log
    fi
done
mode=normal
freeplay=
rlbot=
args=()
for a in "$@"; do
    case "$a" in
        -bakkesmod) mode=mods; continue ;;
        -freeplay)  mode=mods; freeplay=freeplay; continue ;;
        -nobakkes)  nobakkes=1; continue ;;
        -bakkesoffline) export RL_BAKKES_OFFLINE=1; continue ;;
        -psyoffline) psy=--force; continue ;;
        -psymitm)   psy=--mitm-log; continue ;;
        -rlbot)     mode=mods; rlbot=rlbot ;;
        */RocketLeague.exe) mode=mods ;;
    esac
    case "$a" in
        */proton) export RL_WINE="$(dirname "$a")/files/bin/wine64" ;;
    esac
    args+=("$a")
done
if [ "$mode" = mods ]; then
    for i in "${!args[@]}"; do
        case "${args[$i]}" in
            *RocketLeague_EAC.exe) args[$i]="${args[$i]%RocketLeague_EAC.exe}RocketLeague.exe" ;;
        esac
    done
    if proxy=$(env -u LD_PRELOAD /home/deck/rl-tools/psynet-offline.py start $psy 2>>"$LOG"); then
        export https_proxy="http://$proxy" no_proxy="localhost,127.0.0.1"
        echo "[$(date '+%F %T')] no internet: game config and BakkesMod version check are answered locally ($proxy)" >>"$LOG"
    fi
    [ -n "$nobakkes" ] || setsid nohup env -u LD_PRELOAD -u https_proxy -u no_proxy /home/deck/rl-tools/bakkes-helper.sh ${freeplay:-$rlbot} >>"$LOG" 2>&1 </dev/null &
fi
echo "[$(date '+%F %T')] mode=$mode${freeplay:+ +freeplay}${rlbot:+ +rlbot} wine=${RL_WINE:-?} cmd: ${args[*]}" >>"$LOG"
exec "${args[@]}"
