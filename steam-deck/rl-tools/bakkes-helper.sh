#!/bin/bash
# Started by rl-launch.sh in mods mode. Waits for RocketLeague.exe to reach the main menu, runs the
# BakkesMod injector inside the game's own Proton prefix (same wineserver, so it can inject), and quits
# with the game.   Usage: bakkes-helper.sh [freeplay|rlbot]
#   freeplay = jump straight into freeplay once injected
#   rlbot    = a Nexto match is waiting (play-nexto.sh): get BakkesMod in and the title screen passed first, then
#              tell run_match.py through the status file that the match may start
# Without internet BakkesMod.exe never injects (it cannot ask its update server whether the mod matches the
# game), so then the mod is injected directly with rl-inject.exe, checked against the last saved server answer.
unset LD_PRELOAD
MODE="$1"
PFX_ROOT="${STEAM_COMPAT_DATA_PATH:-/home/deck/.local/share/Steam/steamapps/compatdata/252950}"
PFX="$PFX_ROOT/pfx"
BAKKES_DIR="$PFX/drive_c/Program Files/BakkesMod"
GAME_LOG="$PFX/drive_c/users/steamuser/Documents/My Games/Rocket League/TAGame/Logs/Launch.log"
WINE="${RL_WINE:-}"
if [ ! -x "$WINE" ]; then
    WINE="$(dirname "$(sed -n 3p "$PFX_ROOT/config_info")")/bin/wine64"   # .../files/lib/ -> .../files/bin/wine64
fi
say() { echo "[bakkes-helper $(date '+%T')] $*"; }
# ~/rlbot5/run_match.py holds the Nexto match while this says "pending" and starts it on "ready" (BakkesMod is
# loaded) or "none" (no BakkesMod this time). A match that starts before the mod is in ends badly: the mod then
# arrives mid-match, and a controller that was not bound yet at that moment stays dead for the whole match.
STATUS=/home/deck/rl-tools/cache/bakkes-status
set_status() { echo "$1 $(date +%s) ${*:2}" >"$STATUS.$BASHPID" && mv "$STATUS.$BASHPID" "$STATUS"; }
status_is() { [ "$(cut -d' ' -f1 "$STATUS" 2>/dev/null)" = "$1" ]; }
mkdir -p "$(dirname "$STATUS")"
set_status pending
trap 'set_status none "helper finished"' EXIT
is_rl_running() { pgrep -f '^Z:.*RocketLeague\.exe' >/dev/null; }
START=$(date +%s)
# true once this launch's Launch.log (not the previous run's) contains the given text
fresh_log_has() { [ -f "$GAME_LOG" ] && [ "$(stat -c %Y "$GAME_LOG")" -ge "$START" ] && grep -aqi "$1" "$GAME_LOG"; }
say "waiting for RocketLeague.exe (wine: $WINE)${MODE:+, then $MODE}"
for _ in $(seq 1 240); do is_rl_running && break; sleep 1; done
if ! is_rl_running; then say "game never started, giving up"; exit 1; fi
# Wait for the main menu before starting the injector, so it injects about 13 s after the menu loads.
#  - earlier than the game log is written: the injector cannot read the game folder and either sits in safe
#    mode for 3 minutes or pops up a "version mismatch, inject anyway?" dialog that cannot be clicked;
#  - while the menu is still loading (tried: ~6 s after the menu map load): the game crashes on injection.
for _ in $(seq 1 180); do
    is_rl_running || { say "game exited before the main menu"; exit 0; }
    fresh_log_has 'Bringing World menu_main_p' && break
    sleep 1
done
MENU_AT=$(date +%s)
export WINEPREFIX="$PFX" WINEESYNC=1 WINEFSYNC=1 WINEDEBUG=-all
INJECTOR_LOG="$PFX/drive_c/users/steamuser/AppData/Local/Temp/injectorlog.log"
# Injects bakkesmod.dll without BakkesMod.exe. Only when the saved update-server answer says the installed mod
# matches the installed game build (an outdated mod crashes the game), and no earlier than 15 s after the menu
# appeared, which is when BakkesMod.exe gets around to it.
inject_direct() {
    local verdict
    if ! verdict=$(env -u LD_LIBRARY_PATH /home/deck/rl-tools/bakkes-update.py check 2>&1); then
        say "NOT injecting BakkesMod: $verdict"
        set_status none "$verdict"
        return 1
    fi
    say "$verdict; injecting directly"
    while [ "$(date +%s)" -lt $((MENU_AT + 15)) ]; do sleep 1; done
    is_rl_running || return 1
    "$WINE" /home/deck/rl-tools/rl-inject.exe RocketLeague.exe \
        'C:\users\steamuser\AppData\Roaming\bakkesmod\bakkesmod\dll\bakkesmod.dll' 2>/dev/null | tr -d '\r' | sed 's/^/[rl-inject] /' &
}
# Also refreshes the saved answer when the server is reachable. RL_BAKKES_OFFLINE=1 (-bakkesoffline) = pretend not.
if [ -z "$RL_BAKKES_OFFLINE" ] && online=$(env -u LD_LIBRARY_PATH /home/deck/rl-tools/bakkes-update.py refresh 2>&1); then
    ONLINE=1
    say "main menu reached (or timed out waiting); $online; starting BakkesMod.exe"
    # Safe mode on: if Rocket League updated and BakkesMod has not caught up yet, the injector waits quietly
    # instead of asking "inject anyway?". (This call also takes ~4 s, which is part of the safe delay above.)
    "$WINE" reg add 'HKCU\Software\BakkesMod' /v EnableSafeMode /t REG_DWORD /d 1 /f >/dev/null 2>&1
else
    ONLINE=
    say "main menu reached (or timed out waiting); no internet (${online:-forced offline}), not using BakkesMod.exe"
fi
# The game must have picked up the controller before injection: when it had not (game window unfocused or
# Steam's on-screen keyboard open in Desktop Mode), it never detected the controller afterwards.
for _ in $(seq 1 20); do
    fresh_log_has 'Detected new XInput controller' && break
    is_rl_running || exit 0
    sleep 1
done
BAKKES_PID=
if [ -n "$ONLINE" ]; then
    cd "$BAKKES_DIR" || exit 1
    "$WINE" BakkesMod.exe &
    BAKKES_PID=$!
    (
        # The connection can still fail for BakkesMod.exe itself; it then waits forever, so take over.
        for _ in $(seq 1 60); do
            sleep 1
            is_rl_running || exit 0
            [ "$(stat -c %Y "$INJECTOR_LOG" 2>/dev/null || echo 0)" -ge "$MENU_AT" ] || continue
            if grep -aq 'to INJECTED' "$INJECTOR_LOG"; then
                # Connection dropped in between: the mod then asks "could not verify RL version, inject anyway?"
                # in a box that Gaming Mode cannot reach. rl-inject.exe answers it (and does nothing else here).
                env -u LD_LIBRARY_PATH /home/deck/rl-tools/bakkes-update.py check >/dev/null 2>&1 && inject_direct >/dev/null
                wait
                exit 0
            fi
            if grep -aq 'Error connecting to update server' "$INJECTOR_LOG"; then
                say "BakkesMod.exe could not reach its update server; stopping it"
                kill "$BAKKES_PID" 2>/dev/null; pkill -x BakkesMod.exe 2>/dev/null
                inject_direct
                wait
                exit 0
            fi
        done
        # e.g. Rocket League updated and BakkesMod has not caught up yet: the injector sits in safe mode
        say "BakkesMod.exe has not injected after 60 s"
        set_status none "BakkesMod.exe did not inject"
    ) &
else
    inject_direct
fi
# Desktop Mode only: the injector takes window focus while it starts, the fullscreen game minimizes itself and
# Steam drops the controller back to the desktop layout. Hand focus back to the game for the next 30 s.
# (Gaming Mode has no window manager that does this.)
if [ -z "$GAMESCOPE_WAYLAND_DISPLAY" ] && [ "$XDG_CURRENT_DESKTOP" != gamescope ] && [ -n "$DISPLAY" ] && command -v xdotool >/dev/null; then
    (
        for _ in $(seq 1 30); do
            sleep 1
            win=$(env -u LD_LIBRARY_PATH xdotool search --name '^Rocket League \(64-bit' 2>/dev/null | head -1)
            [ -n "$win" ] || continue
            [ "$(env -u LD_LIBRARY_PATH xdotool getactivewindow 2>/dev/null)" = "$win" ] || env -u LD_LIBRARY_PATH xdotool windowactivate "$win" 2>/dev/null
        done
    ) &
fi
# Rocket League only hands the car to the controller once its "press any button" title screen has been
# passed; freeplay or a match loaded before that, with BakkesMod already in, leaves the car uncontrollable.
# So get past the title first: press A on Steam's virtual gamepad every 2 s until the game reports the start
# menu opened (presses are swallowed while BakkesMod is being injected, hence the retries). The user
# pressing/tapping works too.   $1 = give up after this many seconds
title_passed() { fresh_log_has 'bOpenedStartMenu=(True)' || fresh_log_has 'Assigning XInput controller'; }
pass_title_screen() {
    presses=0 warned=
    for n in $(seq 1 "$1"); do
        is_rl_running || exit 0
        title_passed && return 0
        if [ "$MODE" = rlbot ]; then
            status_is none && return 1                           # no BakkesMod after all: the match is starting, hands off
            [ "$n" -ge 20 ] && [ $presses -eq 0 ] && return 1    # no gamepad to press (Desktop Mode, keyboard open)
        fi
        if [ $((n % 2)) -eq 0 ] && [ $presses -lt 60 ]; then
            if why=$(/home/deck/rl-tools/pad-press.py 2>&1); then
                presses=$((presses + 1))
            elif [ -z "$warned" ]; then
                warned=1; say "cannot press A for you: $why"
            fi
        fi
        sleep 1
    done
    return 1
}
if [ "$MODE" = freeplay ]; then
    (
        pass_title_screen 240
        say "title screen passed (A pressed $presses time(s) for you); loading freeplay"
        /home/deck/rl-tools/bakkes-cmd.py --wait 240 load_freeplay 2>&1 | sed 's/^/[bakkes-cmd] /'
    ) &
elif [ "$MODE" = rlbot ] && status_is pending; then
    (
        if pass_title_screen 60; then
            say "title screen passed (A pressed $presses time(s) for you)"
        else
            status_is none && exit 0
            say "could not get past the title screen (A pressed $presses time(s)); going on anyway"
        fi
        # BakkesMod answers on its RCON port once it has loaded its plugins, normally ~20 s after the menu.
        left=$((MENU_AT + 75 - $(date +%s)))
        if /home/deck/rl-tools/bakkes-cmd.py --wait $((left > 5 ? left : 5)) >/dev/null 2>&1; then
            status_is none && exit 0
            say "BakkesMod is loaded; the Nexto match can start"
            set_status ready
        else
            say "BakkesMod did not come up; the Nexto match starts without it"
            set_status none "BakkesMod did not load"
        fi
    ) &
fi
while is_rl_running; do sleep 3; done
say "game exited, stopping BakkesMod"
[ -n "$BAKKES_PID" ] && kill "$BAKKES_PID" 2>/dev/null
pkill -x BakkesMod.exe 2>/dev/null
exit 0
