#!/usr/bin/env python3
"""BakkesMod version info for bakkes-helper.sh, so the mod can be injected without internet.
BakkesMod.exe asks its update server which Rocket League builds the installed mod supports and refuses to
inject when the server cannot be reached. This keeps a copy of the server's last answer instead.

The mod itself (bakkesmod.dll, inside the game) asks a second server the same question and shows a
"Could not verify RL version" box when it gets no answer. That answer is saved here too; psynet-offline.py hands
it to the mod when there is no internet.

  bakkes-update.py refresh   ask the servers and save the answers.  Exit 0 = update server reachable (online).
  bakkes-update.py check     compare the saved answers with the installed game and mod.
                             Exit 0 = the installed mod matches the installed game, safe to inject."""
import json, os, re, sys, urllib.request

PFX_ROOT = os.environ.get("STEAM_COMPAT_DATA_PATH") or "/home/deck/.local/share/Steam/steamapps/compatdata/252950"
BAKKES = PFX_ROOT + "/pfx/drive_c/users/steamuser/AppData/Roaming/bakkesmod/bakkesmod"
MANIFEST = os.path.join(os.path.dirname(os.path.dirname(PFX_ROOT.rstrip("/"))), "appmanifest_252950.acf")
CACHE = "/home/deck/rl-tools/cache/updaterinfo.json"
STARTUP = "/home/deck/rl-tools/cache/bakkes-startupconfig.json"     # the mod's own version check
MOD_FILES = {   # what the mod downloads while it starts
    "https://config.bakkesmod.com/static/startupconfig.json": STARTUP,
    "https://config.bakkesmod.com/static/onetime.json": "/home/deck/rl-tools/cache/bakkes-onetime.json",
}

def mod_version():
    try:
        return int(open(BAKKES + "/version.txt").read().strip())
    except (OSError, ValueError):
        return 0

def build_ids(info):
    return [b for b in str(info.get("gameinfo", {}).get("buildids", "")).split(",") if b]

def refresh():
    try:
        req = urllib.request.Request(f"https://updater.bakkesmod.com/updater/{mod_version()}",
                                     headers={"User-Agent": "BakkesMod Updater"})
        text = urllib.request.urlopen(req, timeout=5).read().decode()
        info = json.loads(text)
    except Exception as e:
        print(f"update server not reachable: {e}")
        return 1
    if not build_ids(info):
        print("update server answered without game build ids")
        return 1
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(CACHE + ".tmp", "w") as f:
        f.write(text)
    os.replace(CACHE + ".tmp", CACHE)
    missed = []
    for url, path in MOD_FILES.items():
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "BAKKESMOD;"})
            data = urllib.request.urlopen(req, timeout=5).read()
            json.loads(data)
            with open(path + ".tmp", "wb") as f:
                f.write(data)
            os.replace(path + ".tmp", path)
        except Exception as e:  # noqa: BLE001
            missed.append(f"{os.path.basename(url)}: {e}")
    print(f"update server reachable, supported builds: {','.join(build_ids(info))}"
          + (f" (not saved: {'; '.join(missed)})" if missed else ""))
    return 0

def check():
    info = None
    for path in (CACHE, BAKKES + "/updaterinfo.txt"):   # second one is BakkesMod.exe's own copy
        try:
            info = json.load(open(path))
            if build_ids(info):
                break
            info = None
        except (OSError, ValueError):
            pass
    if info is None:
        print("no saved version info yet (start mods mode once with internet)")
        return 1
    try:
        m = re.search(r'"buildid"\s+"(\d+)"', open(MANIFEST).read())
    except OSError:
        m = None
    if not m:
        print(f"cannot read the game build id from {MANIFEST}")
        return 1
    game, mod, newest = m.group(1), mod_version(), int(info.get("update_info", {}).get("trainer_version", 0))
    if game not in build_ids(info):
        print(f"Rocket League build {game} is newer than the saved BakkesMod info ({','.join(build_ids(info))}); "
              "needs one start with internet")
        return 1
    if mod < newest:
        print(f"installed BakkesMod {mod} is older than {newest} required for this game build; "
              "needs one start with internet")
        return 1
    # The mod repeats this check itself against its own server's list; psynet-offline.py gives it this copy.
    # Without it the mod asks "Could not verify RL version, inject anyway?", and answering that box for the
    # user leaves a black screen in Gaming Mode.
    try:
        builds = json.load(open(STARTUP))["versions"][str(mod)]["steam"]
    except (OSError, ValueError, KeyError, TypeError):
        builds = None
    if builds is None or int(game) not in builds:
        print(f"no saved start-up answer for BakkesMod {mod} with Rocket League build {game}; "
              "needs one start with internet")
        return 1
    print(f"BakkesMod {mod} matches Rocket League build {game}")
    return 0

if __name__ == "__main__":
    sys.exit({"refresh": refresh, "check": check}.get(sys.argv[1] if len(sys.argv) > 1 else "", lambda: 2)())
