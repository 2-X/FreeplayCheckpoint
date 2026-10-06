# Rocket League mods on the Steam Deck

Everything needed to run **BakkesMod**, this fork of **Freeplay Checkpoint** and **Nexto** (the
RLGym bot, through RLBot v5) on a Steam Deck, started from Gaming Mode like any other game, and
working without internet. One script installs it.

Normal Rocket League (online play with Easy Anti-Cheat) stays exactly as it is: since April 2026
mods only work in Steam's "Anti-Cheat Disabled" launch, so the mods live in separate library
entries and your Play button is untouched.

## What you get

Four non-Steam library entries (also added to Favorites):

| Entry                   | What it does                                                        |
|-------------------------|---------------------------------------------------------------------|
| RL BakkesMod Freeplay   | Rocket League without anti-cheat, BakkesMod injected, straight into freeplay (~75 s) |
| RL BakkesMod Mode       | Same, but stops at the main menu (training packs, exhibition vs bots, replays, workshop maps) |
| RL vs Nexto 1v1         | You against Nexto, 5-minute match, BakkesMod loaded; a new match starts when one ends |
| RL vs Nexto 2v2         | You and a Nexto against two Nextos                                  |

Plus: Rocket League's launch options point at a wrapper (`rl-launch.sh`) that leaves normal
launches alone and only acts on the mods launches, and Steam's own "Anti-Cheat Disabled (Mods)"
play option also gets BakkesMod.

Offline: the game normally stalls 55-90 s on every map load without internet and BakkesMod refuses
to inject at all. The scripts answer both servers locally from copies saved during the last online
launch, so freeplay and Nexto work on a plane or in a basement, in Gaming Mode too.

## Requirements

- A Steam Deck (SteamOS 3, user `deck`; the scripts use `/home/deck/...` paths).
- Rocket League installed **on the internal drive** and started once (so its Proton prefix exists).
- Desktop Mode with a terminal (Konsole) and internet, for the install only.
- About 1.5 GB free for RLBot + Nexto (PyTorch for CPU). Skip it with `--no-nexto`.

## Install

```bash
git clone -b steam-deck https://github.com/2-X/FreeplayCheckpoint.git ~/code/FreeplayCheckpoint
~/code/FreeplayCheckpoint/steam-deck/install.sh
```

The script is safe to run again at any time and only does what is missing. Steps:

1. **tools**: copies `rl-tools/` to `~/rl-tools` (launch wrapper, BakkesMod helper, offline proxy, injector).
2. **bakkesmod**: downloads the official BakkesMod installer and runs it silently inside Rocket
   League's Proton prefix, then starts BakkesMod once so it downloads the mod files. Registry
   settings: hidden on boot, no pop-ups, safe mode (waits quietly after a game update).
3. **plugin**: installs `CheckpointPlugin.dll` (built from this branch, with the offline-match
   features) into BakkesMod's plugin folder and enables it.
4. **nexto**: `~/rlbot5`: RLBotServer v5.0.0-rc17 (checksum checked), a Python 3.12 venv made with
   `uv`, `rlbot` 2.0.0b56, Nexto (VirxEC's RLBot v5 port, pinned commit) and `run_match.py`.
5. **steam**: sets Rocket League's launch options and adds the four library entries. Done live if
   Steam's debug port is on (it is when Decky Loader is installed); otherwise quit Steam and run
   `install.sh --only steam`, or follow the printed manual steps.

Then **start one mods entry while online**. BakkesMod updates itself, and the offline support keeps
that update server's answer for later. Repeat this after every Rocket League or BakkesMod update,
otherwise the offline launch refuses to inject (an outdated mod would crash the game).

Artwork for the library entries is not included; set it per entry in Steam if you want it.

## Using it

- **Gaming Mode**: Library > Non-Steam tab (or Favorites) > pick an entry. The game boots to its
  "press any button" screen, the helper presses A for you, BakkesMod injects at the main menu, then
  freeplay or the match loads. About 75 s to freeplay, about 70 s to a Nexto match.
- **Desktop Mode**: `~/rl-tools/rl-mods.sh [freeplay]`, `~/rl-tools/play-nexto.sh 1v1|2v2`.
- **Online play**: press Play on Rocket League as always.
- BakkesMod's menu is F2 (bind a controller button to `togglemenu` in BakkesMod if you have no keyboard).
  Freeplay Checkpoint: F2 > Plugins > Freeplay Checkpoint > "Apply All Bindings", see the main
  [README](../README.md) for the controls, the offline-match mode and the per-map checkpoint files.
- Workshop maps: load them from BakkesMod's Workshop plugin, or `load_workshop "Z:\...\<id>\<Map>.udk"`.
  Each map keeps its own checkpoint file.

## Troubleshooting

Logs: `~/rl-tools/logs/rl-launch.log` (launch wrapper and BakkesMod helper), `~/rl-tools/logs/psynet-offline.log`
(offline proxy), `~/rlbot5/logs/` (match and bot), and the game's own
`~/.local/share/Steam/steamapps/compatdata/252950/pfx/drive_c/users/steamuser/Documents/My Games/Rocket League/TAGame/Logs/Launch.log`.

- **"Anti-cheat violation" / kicked after the Deck slept**: the game's anti-cheat token expired in suspend.
  Quit and relaunch the game. Not related to the mods.
- **Entry does nothing right after the game crashed**: Steam keeps Rocket League "running" for ~4 minutes
  after a crash and ignores new launches. Wait, or stop it from the Steam menu.
- **No BakkesMod offline**: `rl-launch.log` says why ("needs one start with internet"). Launch once online.
- **Controller dead in game (Desktop Mode)**: Steam's on-screen keyboard was open, which takes the virtual
  gamepad away. Close it. Also, the car only becomes controllable after the title screen was passed.
- **BakkesMod menu shows no Freeplay Checkpoint**: check `plugins.cfg` has `plugin load checkpointplugin`
  (`install.sh --only plugin` does that) and that the game was closed when the dll was copied.
- **Nexto match never starts**: `~/rlbot5/logs/match-1v1.log`. The match waits for BakkesMod (up to 3
  minutes) and for the game's config download; a bot that fails to start shows up in `logs/`.

## Updating and removing

Update: `git -C ~/code/FreeplayCheckpoint pull` and run `install.sh` again (the previous plugin dll is
kept in `~/rl-tools/plugin-build/backup/`).

Remove: delete the four library entries and Rocket League's launch options in Steam, `rm -rf ~/rl-tools
~/rlbot5`, and in BakkesMod's data folder remove `plugins/CheckpointPlugin.dll` and its line in
`cfg/plugins.cfg`. BakkesMod itself uninstalls with `unins000.exe` in the prefix's `Program Files/BakkesMod`.

## For developers

- `plugin-build/build.sh <source dir>` builds a BakkesMod plugin (MSVC ABI) on the Deck with clang-cl
  and the Windows SDK in a podman container (first build downloads ~1 GB). Output: `<source dir>/plugins/<Name>.dll`.
  Rebuild this plugin with `~/rl-tools/plugin-build/build.sh ~/code/FreeplayCheckpoint`.
- `rl-inject.exe` (source `rl-inject.c`) injects `bakkesmod.dll` without BakkesMod.exe when offline:
  `zig cc -target x86_64-windows-gnu -O2 -municode -o rl-inject.exe rl-inject.c`.
- `bakkes-cmd.py 'command'` sends console commands to the running BakkesMod (RCON plugin, port 9002);
  `steam-js.py 'js'` runs JavaScript in the running Steam client.
- Test flags for `steam -applaunch 252950 ...` (see `rl-launch.sh`): `-bakkesoffline`, `-psyoffline`,
  `-psymitm`, `-nobakkes`, `-gamescopetest` (nested gamescope in Desktop Mode, reproduces Gaming Mode).
- Every script has its reasoning in its header comment: injection timing, title-screen handling,
  the two server checks that block BakkesMod offline, and the Gaming Mode black-screen cause.

## Status

Developed and used on one Steam Deck OLED (SteamOS 3.7, Proton 9.0 Beta, BakkesMod 2.0.76, game
build of October 2026). Freeplay, Nexto 1v1/2v2, offline launches and the plugin's match mode were
verified there. `install.sh` was run on that already-installed Deck but not yet on a fresh one, so
expect rough edges in the bakkesmod and nexto steps; please report them.
