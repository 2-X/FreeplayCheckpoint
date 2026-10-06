#!/usr/bin/env python
"""Start RLBotServer and a match from a match toml, then keep re-starting the match
whenever one ends (after a short scoreboard pause) until Rocket League is closed.

The first match is held back until BakkesMod is in the game: ~/rl-tools/bakkes-helper.sh injects it at the
main menu and reports through a status file. Started earlier, the match is already running when the mod
arrives, and a controller that was not bound yet at that moment never gets bound.

It is also held back until the game has finished its PsyNet config download ("static data sync"), because a
match loaded before that sits on a black loading screen. With internet that takes 0.2 s. Without, rl-launch.sh
answers the download locally (~/rl-tools/psynet-offline.py) and it is just as quick; this wait only matters
when that did not work (e.g. the game has no saved config yet) and the game retries for 55-90 s on its own.

Usage: .venv/bin/python run_match.py match_1v1.toml
"""
import signal
import sys
import time
from pathlib import Path

import psutil
from rlbot import flat
from rlbot.managers import MatchManager

HERE = Path(__file__).resolve().parent
CONFIG = (HERE / sys.argv[1]).resolve()
REMATCH_AFTER_SECONDS = 20
BAKKES_STATUS = Path("/home/deck/rl-tools/cache/bakkes-status")
GAME_LOG = Path(
    "/home/deck/.local/share/Steam/steamapps/compatdata/252950/pfx/drive_c/users/steamuser/Documents"
    "/My Games/Rocket League/TAGame/Logs/Launch.log"
)
STATIC_DATA_MAX_WAIT = 150  # seconds after the game came up; the longest sync seen offline ended at ~90 s
STARTED = time.time()


def rl_running() -> bool:
    # /proc comm is truncated to 15 chars ("RocketLeague.ex"), so match on the prefix / cmdline.
    for p in psutil.process_iter(["name", "cmdline"]):
        try:
            name = (p.info["name"] or "").lower()
            cmd = " ".join(p.info["cmdline"] or [])
            if name.startswith("rocketleague") or cmd.startswith("Z:") and "RocketLeague.exe" in cmd:
                return True
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    return False


stop = False


def _on_signal(*_):
    global stop
    stop = True


signal.signal(signal.SIGTERM, _on_signal)
signal.signal(signal.SIGINT, _on_signal)

def bakkes_status() -> str | None:
    """pending / ready / none as written by bakkes-helper.sh for this launch; None = not written (yet)."""
    try:
        if BAKKES_STATUS.stat().st_mtime < STARTED:
            return None  # left over from an earlier launch
        return BAKKES_STATUS.read_text().split()[0]
    except (OSError, IndexError):
        return None


def wait_for_bakkesmod() -> float | None:
    """Returns when Rocket League came up; None when it was closed (or we were told to stop) while waiting."""
    seen = None
    while not stop:
        time.sleep(0.5)
        running = rl_running()
        if seen is None:
            if running:
                seen = time.time()
                print("[run_match] Rocket League is up; holding the match until BakkesMod is loaded", flush=True)
            continue
        if not running:
            print("[run_match] Rocket League closed", flush=True)
            return None
        status, waited = bakkes_status(), time.time() - seen
        if status in ("ready", "none"):
            print(f"[run_match] BakkesMod: {status} after {waited:.0f} s", flush=True)
            return seen
        if status is None and waited > 10:
            print("[run_match] no BakkesMod helper for this launch", flush=True)
            return seen
        if waited > 180:
            print("[run_match] gave up waiting for BakkesMod", flush=True)
            return seen
    return None


def static_data_sync() -> str | None:
    """started / done as logged by the game for this launch; None = nothing logged (yet).
    The game writes its log in 4 KB blocks, so a line shows up here up to ~8 s late."""
    try:
        if GAME_LOG.stat().st_mtime < STARTED:
            return None  # previous run's log
        log = GAME_LOG.read_bytes()
    except OSError:
        return None
    if b"PsyNetStaticData: Blocking sync complete" in log:
        return "done"
    if b"PsyNetStaticData: Blocking sync start" in log:
        return "started"
    return None


def wait_for_static_data(game_up_since: float) -> bool:
    """Returns False when Rocket League was closed (or we were told to stop) while waiting."""
    told = False
    while not stop:
        if not rl_running():
            print("[run_match] Rocket League closed", flush=True)
            return False
        sync, waited = static_data_sync(), time.time() - game_up_since
        if sync == "done":
            if told:
                print(f"[run_match] game finished its config download {waited:.0f} s after launch", flush=True)
            return True
        if sync is None and waited > 45:
            return True  # this game version does not log the sync; nothing to wait for
        if waited > STATIC_DATA_MAX_WAIT:
            print("[run_match] gave up waiting for the game's config download", flush=True)
            return True
        if not told:
            told = True
            print("[run_match] no internet? the game is still retrying its config download; holding the match"
                  " at the main menu until it gives up (a match loaded now would stay black)", flush=True)
        time.sleep(0.5)
    return False


mm = MatchManager(HERE / "RLBotServer")
mm.ensure_server_started()
ended_since = None
try:
    game_up_since = wait_for_bakkesmod()
    if game_up_since is not None and wait_for_static_data(game_up_since):
        print(f"[run_match] starting match from {CONFIG.name}", flush=True)
        mm.start_match(CONFIG, wait_for_start=True)
        print("[run_match] match started", flush=True)
    else:
        stop = True
    while not stop:
        time.sleep(1)
        if not rl_running():
            print("[run_match] Rocket League closed", flush=True)
            break
        pkt = mm.packet
        if pkt is not None and pkt.match_info.match_phase == flat.MatchPhase.Ended:
            ended_since = ended_since or time.time()
            if time.time() - ended_since > REMATCH_AFTER_SECONDS:
                print("[run_match] match over, starting a new one", flush=True)
                ended_since = None
                mm.start_match(CONFIG, wait_for_start=True)
        else:
            ended_since = None
finally:
    try:
        mm.shut_down()
    except Exception as e:  # noqa: BLE001
        print(f"[run_match] shutdown: {e}", flush=True)
