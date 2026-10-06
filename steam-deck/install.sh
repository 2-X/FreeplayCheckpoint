#!/bin/bash
# Rocket League mods on a Steam Deck: BakkesMod + Freeplay Checkpoint (built from this branch) + Nexto
# (RLBot v5), started from Gaming Mode, working without internet.  Details: README.md in this folder.
#
#   ./install.sh                 everything
#   ./install.sh --no-nexto      skip RLBot / Nexto (no Python, torch or bot download)
#   ./install.sh --no-steam      do not touch Steam (launch options, library entries)
#   ./install.sh --only STEP     one step only: tools, bakkesmod, plugin, nexto, steam
#
# Safe to run again at any time: every step only does what is still missing.
# Run it in Desktop Mode, in a terminal (Konsole), with internet, while Rocket League is NOT running.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
STEAM=$HOME/.local/share/Steam
APPID=252950
TOOLS=$HOME/rl-tools
RLBOT=$HOME/rlbot5
BAKKES_SETUP_URL=https://github.com/bakkesmodorg/BakkesModInjectorCpp/releases/latest/download/BakkesModSetup.zip
RLBOT_SERVER_URL=https://github.com/RLBot/core/releases/download/v5.0.0-rc17/RLBotServer
RLBOT_SERVER_SHA256=6ecf4905e777daa4d1c0bd09e982bc3449083c169778940be752ff23b3fa1154
RLBOT_PY_VERSION="rlbot==2.0.0b56"
NECTO_REPO=https://github.com/VirxEC/NectoFamily.git
NECTO_COMMIT=e5510350299e15a850d4388705ad4ee7dfe0e9ed

NEXTO=1 STEAM_STEP=1 ONLY=
while [ $# -gt 0 ]; do
    case "$1" in
        --no-nexto) NEXTO= ;;
        --no-steam) STEAM_STEP= ;;
        --only) ONLY=${2:-}; shift ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done
case "$ONLY" in ""|tools|bakkesmod|plugin|nexto|steam) ;; *) echo "unknown step: $ONLY" >&2; exit 2 ;; esac

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
warn() { printf '\033[33mWARNING:\033[0m %s\n' "$*"; }
die()  { printf '\033[31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }
rl_running() { pgrep -f '^Z:.*RocketLeague\.exe' >/dev/null; }

# ---------------------------------------------------------------- where things are
[ "$HOME" = /home/deck ] || warn "the scripts are written for the Steam Deck's 'deck' user: they use /home/deck/rl-tools" \
    "and /home/deck/rlbot5 as absolute paths. Installing as $HOME will need those paths edited."
LIB=
while IFS= read -r lib; do
    if [ -f "$lib/steamapps/appmanifest_$APPID.acf" ]; then LIB=$lib; break; fi
done < <(printf '%s\n' "$STEAM"; sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$STEAM/steamapps/libraryfolders.vdf" 2>/dev/null)
[ -n "$LIB" ] || die "Rocket League (Steam app $APPID) is not installed"
[ "$LIB" = "$STEAM" ] || warn "Rocket League is installed in $LIB, not on the internal drive. The scripts expect" \
    "$STEAM/steamapps/compatdata/$APPID (move the game to internal storage, or edit the paths in rl-tools)."
PFX_ROOT=$LIB/steamapps/compatdata/$APPID
PFX=$PFX_ROOT/pfx
[ -d "$PFX/drive_c" ] || die "Rocket League has no Proton prefix yet ($PFX). Start the game once from Steam, quit it, run this again."
BAKKES_DATA="$PFX/drive_c/users/steamuser/AppData/Roaming/bakkesmod/bakkesmod"
BAKKES_PROG="$PFX/drive_c/Program Files/BakkesMod"
PROTON_FILES=$(dirname "$(sed -n 3p "$PFX_ROOT/config_info" 2>/dev/null)")     # .../Proton X/files/lib/ -> .../files
WINE="$PROTON_FILES/bin/wine64"
[ -x "$WINE" ] || die "cannot find the Proton wine64 that made this prefix (looked for $WINE)"
wine() { env -u LD_PRELOAD WINEPREFIX="$PFX" WINEESYNC=1 WINEFSYNC=1 WINEDEBUG=-all "$WINE" "$@"; }
echo "Rocket League prefix: $PFX"
echo "Proton:               $PROTON_FILES"

# ---------------------------------------------------------------- 1. scripts
if want tools; then
    say "Launch scripts -> $TOOLS"
    mkdir -p "$TOOLS/cache" "$TOOLS/logs" "$TOOLS/plugin-build"
    cp "$HERE"/rl-tools/* "$TOOLS/" || die "copy failed"
    cp "$HERE"/plugin-build/* "$TOOLS/plugin-build/" || die "copy failed"
    chmod +x "$TOOLS"/*.sh "$TOOLS"/*.py "$TOOLS"/rl-inject.exe "$TOOLS"/plugin-build/*.sh
    echo "installed: $(cd "$HERE/rl-tools" && ls | tr '\n' ' ')"
fi

# ---------------------------------------------------------------- 2. BakkesMod
if want bakkesmod; then
    say "BakkesMod inside the game's Proton prefix"
    rl_running && die "Rocket League is running; close it first"
    if [ -f "$BAKKES_PROG/BakkesMod.exe" ]; then
        echo "BakkesMod.exe is already installed"
    else
        tmp=$(mktemp -d)
        echo "downloading BakkesModSetup.zip"
        curl -fL --progress-bar -o "$tmp/BakkesModSetup.zip" "$BAKKES_SETUP_URL" || die "download failed"
        unzip -q -o "$tmp/BakkesModSetup.zip" -d "$tmp" || die "unzip failed"
        echo "running the installer (silent) inside the prefix"
        wine "$tmp/BakkesModSetup.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
        rm -rf "$tmp"
        [ -f "$BAKKES_PROG/BakkesMod.exe" ] || die "the installer did not leave BakkesMod.exe in '$BAKKES_PROG'"
        echo "installed"
    fi
    # No pop-ups, start hidden (there is no tray in Gaming Mode), wait quietly after a game update.
    for k in DisableWarnings EnableSafeMode HideOnBoot HideOnMinimize; do
        wine reg add 'HKCU\Software\BakkesMod' /v $k /t REG_DWORD /d 1 /f >/dev/null 2>&1
    done
    if [ -f "$BAKKES_DATA/dll/bakkesmod.dll" ] && [ -f "$BAKKES_DATA/cfg/plugins.cfg" ]; then
        echo "mod files present (BakkesMod version $(cat "$BAKKES_DATA/version.txt" 2>/dev/null))"
    else
        echo "starting BakkesMod.exe once so it downloads the mod files (needs internet, up to 3 minutes)"
        pgrep -x BakkesMod.exe >/dev/null || (cd "$BAKKES_PROG" && wine BakkesMod.exe >/dev/null 2>&1 &)
        for _ in $(seq 1 180); do
            [ -f "$BAKKES_DATA/dll/bakkesmod.dll" ] && [ -f "$BAKKES_DATA/cfg/plugins.cfg" ] && [ -f "$BAKKES_DATA/version.txt" ] && break
            sleep 1
        done
        sleep 5
        pkill -x BakkesMod.exe 2>/dev/null
        [ -f "$BAKKES_DATA/dll/bakkesmod.dll" ] || die "BakkesMod did not download its files. Is the Deck online? Start" \
            "'$TOOLS/rl-mods.sh' once with internet instead, close the game, and run:  $0 --only plugin"
        echo "mod files downloaded (BakkesMod version $(cat "$BAKKES_DATA/version.txt" 2>/dev/null))"
    fi
fi

# ---------------------------------------------------------------- 3. Freeplay Checkpoint plugin
if want plugin; then
    say "Freeplay Checkpoint plugin (CheckpointPlugin.dll built from this branch)"
    [ -d "$BAKKES_DATA/cfg" ] || die "BakkesMod's data folder is not there yet ($BAKKES_DATA)." \
        "Run the bakkesmod step, or start '$TOOLS/rl-mods.sh' once with internet, close the game, then:  $0 --only plugin"
    rl_running && die "Rocket League is running; close it first (a loaded plugin dll must not be overwritten)"
    mkdir -p "$BAKKES_DATA/plugins" "$TOOLS/plugin-build/backup"
    old="$BAKKES_DATA/plugins/CheckpointPlugin.dll"
    if [ -f "$old" ] && ! cmp -s "$HERE/CheckpointPlugin.dll" "$old"; then
        cp "$old" "$TOOLS/plugin-build/backup/CheckpointPlugin-replaced-$(date +%F-%H%M%S).dll"
        echo "previous CheckpointPlugin.dll saved in $TOOLS/plugin-build/backup/"
    fi
    cp "$HERE/CheckpointPlugin.dll" "$old.new" && mv "$old.new" "$old" || die "copy failed"
    cfg="$BAKKES_DATA/cfg/plugins.cfg"
    [ -f "$cfg" ] || touch "$cfg"
    [ -s "$cfg" ] && [ -n "$(tail -c1 "$cfg")" ] && echo >>"$cfg"     # file without a final newline
    grep -qx 'plugin load checkpointplugin' "$cfg" || echo 'plugin load checkpointplugin' >>"$cfg"
    echo "installed; BakkesMod loads it at start. Settings: F2 (BakkesMod menu) > Plugins > Freeplay Checkpoint"
fi

# ---------------------------------------------------------------- 4. RLBot + Nexto
if [ -n "$NEXTO" ] && want nexto; then
    say "RLBot v5 + Nexto -> $RLBOT"
    mkdir -p "$RLBOT/logs"
    export PATH="$HOME/.local/bin:$PATH"
    if ! command -v uv >/dev/null; then
        echo "installing uv (Python package manager) into ~/.local/bin"
        curl -LsSf https://astral.sh/uv/install.sh | sh || die "uv install failed"
    fi
    command -v uv >/dev/null || die "uv is not on PATH after the install; open a new terminal and run this again"
    if [ -x "$RLBOT/RLBotServer" ] && [ "$(sha256sum "$RLBOT/RLBotServer" | cut -d' ' -f1)" = "$RLBOT_SERVER_SHA256" ]; then
        echo "RLBotServer v5.0.0-rc17 present"
    else
        echo "downloading RLBotServer v5.0.0-rc17"
        curl -fL --progress-bar -o "$RLBOT/RLBotServer.new" "$RLBOT_SERVER_URL" || die "download failed"
        [ "$(sha256sum "$RLBOT/RLBotServer.new" | cut -d' ' -f1)" = "$RLBOT_SERVER_SHA256" ] || die "RLBotServer checksum mismatch"
        chmod +x "$RLBOT/RLBotServer.new" && mv "$RLBOT/RLBotServer.new" "$RLBOT/RLBotServer"
    fi
    if [ -x "$RLBOT/.venv/bin/python" ]; then
        echo "Python venv present ($("$RLBOT/.venv/bin/python" --version))"
    else
        echo "creating the Python 3.12 venv (torch 2.4 needs 3.12)"
        uv venv -q --python 3.12 "$RLBOT/.venv" || die "could not create the venv"
    fi
    if [ -d "$RLBOT/NectoFamily/.git" ]; then
        echo "Nexto sources present"
    else
        echo "cloning Nexto (VirxEC/NectoFamily, RLBot v5 port)"
        git clone -q "$NECTO_REPO" "$RLBOT/NectoFamily" || die "git clone failed"
    fi
    git -C "$RLBOT/NectoFamily" -c advice.detachedHead=false checkout -q "$NECTO_COMMIT" 2>/dev/null \
        || warn "could not pin NectoFamily to $NECTO_COMMIT; using the checked-out version"
    echo "installing Python packages (torch for CPU is ~200 MB; a few minutes the first time)"
    uv pip install -q --python "$RLBOT/.venv/bin/python" -r "$RLBOT/NectoFamily/requirements.txt" || die "pip install (Nexto) failed"
    uv pip install -q --python "$RLBOT/.venv/bin/python" "$RLBOT_PY_VERSION" psutil || die "pip install (rlbot) failed"
    # RLBotServer starts the bot with run_command_linux; the stock bot.toml only has the Windows/uv one.
    python3 - "$RLBOT/NectoFamily/nexto/bot.toml" "$RLBOT/.venv/bin/python" <<'PY'
import re, sys
path, python = sys.argv[1], sys.argv[2]
s = open(path).read()
line = f'run_command_linux = "{python} bot.py"'
if "run_command_linux" in s:
    s = re.sub(r'^run_command_linux\s*=.*$', line, s, count=1, flags=re.M)
else:
    s = re.sub(r'^(run_command\s*=.*)$', r'\1\n' + line, s, count=1, flags=re.M)
open(path, "w").write(s)
PY
    cp "$HERE"/rlbot/run_match.py "$HERE"/rlbot/match_1v1.toml "$HERE"/rlbot/match_2v2.toml "$RLBOT/" || die "copy failed"
    echo "ready: $TOOLS/play-nexto.sh 1v1|2v2"
fi

# ---------------------------------------------------------------- 5. Steam
if [ -n "$STEAM_STEP" ] && want steam; then
    say "Steam: Rocket League launch options + library entries"
    LAUNCH="$TOOLS/rl-launch.sh %command%"
    if curl -s -m 3 http://127.0.0.1:8080/json >/dev/null 2>&1; then
        echo "Steam is running with its debug port open: changing it live"
        nexto_js=$([ -n "$NEXTO" ] && echo true || echo false)
        python3 "$TOOLS/steam-js.py" "$(cat <<JS
(async () => {
  const home = "$HOME", tools = "$TOOLS", withNexto = $nexto_js;
  await SteamClient.Apps.SetAppLaunchOptions($APPID, "$LAUNCH");
  const want = [
    ["RL BakkesMod Freeplay", tools + "/rl-mods.sh", "freeplay"],
    ["RL BakkesMod Mode",     tools + "/rl-mods.sh", ""],
  ];
  if (withNexto) want.push(["RL vs Nexto 1v1", tools + "/play-nexto.sh", "1v1"],
                           ["RL vs Nexto 2v2", tools + "/play-nexto.sh", "2v2"]);
  const have = new Map(appStore.allApps.filter(a => a.BIsShortcut()).map(a => [a.display_name, a.appid]));
  const out = ["launch options for $APPID: $LAUNCH"];
  for (const [name, exe, args] of want) {
    let id = have.get(name);
    if (id) { out.push("exists: " + name + " (" + id + ")"); }
    else {
      id = await SteamClient.Apps.AddShortcut(name, exe, tools, args);
      await SteamClient.Apps.SetShortcutName(id, name);
      await SteamClient.Apps.SetShortcutLaunchOptions(id, args);
      out.push("added: " + name + " (" + id + ")");
    }
    try { collectionStore.AddOrRemoveApp([id], true, "favorite"); } catch (e) { out.push("not added to Favorites: " + e); }
  }
  return out;
})()
JS
)" || die "could not talk to Steam (see above)"
    elif pgrep -x steam >/dev/null; then
        cat <<MSG
Steam is running, but without its debug port, so this script cannot change it live. Either:
  (a) quit Steam (Steam menu > Exit, or 'steam -shutdown'), then run:   $0 --only steam
  (b) do it by hand in Steam:
      - Rocket League > Properties > Launch Options:   $LAUNCH
      - Games > Add a Non-Steam Game > Browse, for each of:
          RL BakkesMod Freeplay   $TOOLS/rl-mods.sh      launch options: freeplay
          RL BakkesMod Mode       $TOOLS/rl-mods.sh
          RL vs Nexto 1v1         $TOOLS/play-nexto.sh   launch options: 1v1
          RL vs Nexto 2v2         $TOOLS/play-nexto.sh   launch options: 2v2
MSG
    else
        echo "Steam is not running: editing its config files"
        python3 "$TOOLS/steam-config-apply.py" $([ -n "$NEXTO" ] || echo --no-nexto) || die "steam-config-apply.py failed"
    fi
fi

say "Done"
cat <<MSG
Next:
  * Gaming Mode: Library > Non-Steam (also in Favorites) > "RL BakkesMod Freeplay", "RL BakkesMod Mode",
    "RL vs Nexto 1v1/2v2". Desktop Mode: $TOOLS/rl-mods.sh [freeplay], $TOOLS/play-nexto.sh 1v1|2v2
  * Start one of them WITH internet first: BakkesMod checks for updates, and the offline support keeps that
    answer for later (same again after every Rocket League or BakkesMod update).
  * Playing Rocket League normally (online, anti-cheat) is unchanged: just press Play.
Logs if something is off: $TOOLS/logs/, $RLBOT/logs/, and the game's own Launch.log (see README.md).
MSG
