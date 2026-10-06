#!/bin/bash
# Make the running game's Freeplay Checkpoint plugin pick up synced files, optionally with a new dll.
#   cpt-reload.sh                       re-read the checkpoint .data files (plugin unload + load)
#   cpt-reload.sh path/to/CheckpointPlugin.dll   swap the plugin dll in and load it
# Why: the plugin only reads a store when the mode changes (freeplay <-> match, map change), so a
# .data file copied in from elsewhere is ignored until then, and the next checkpoint saved on this
# side would overwrite it from memory. Unloading saves nothing, so a reload is safe at any time.
# With the game closed only the dll copy happens; files are read at the next start anyway.
# Meant to be run over ssh from the PC, e.g.:
#   scp *.data deck@kris-deck.local:'<bakkesmod>/data/' && ssh deck@kris-deck.local rl-tools/cpt-reload.sh
B=$HOME/.local/share/Steam/steamapps/compatdata/252950/pfx/drive_c/users/steamuser/AppData/Roaming/bakkesmod/bakkesmod
CMD=$HOME/rl-tools/bakkes-cmd.py
DLL=$1
if [ -n "$DLL" ] && [ ! -f "$DLL" ]; then echo "no such file: $DLL"; exit 1; fi
if ! "$CMD" >/dev/null 2>&1; then
    if [ -n "$DLL" ]; then
        cp "$DLL" "$B/plugins/CheckpointPlugin.dll.new" && mv "$B/plugins/CheckpointPlugin.dll.new" "$B/plugins/CheckpointPlugin.dll"
        echo "BakkesMod not running: dll copied, loads at the next start"
    else
        echo "BakkesMod not running: nothing to reload, files are read at the next start"
    fi
    exit 0
fi
"$CMD" "plugin unload checkpointplugin" >/dev/null || exit 1
sleep 1
if [ -n "$DLL" ]; then
    # never cp over a loaded dll; after the unload a rename is safe
    cp "$DLL" "$B/plugins/CheckpointPlugin.dll.new" && mv "$B/plugins/CheckpointPlugin.dll.new" "$B/plugins/CheckpointPlugin.dll"
    echo "dll swapped"
fi
"$CMD" "plugin load checkpointplugin" >/dev/null && echo "checkpoint plugin reloaded ($(ls "$B"/data/*checkpoint*.data | wc -l) checkpoint files on disk)"
