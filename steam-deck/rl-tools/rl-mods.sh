#!/bin/bash
# Rocket League in mods mode: Anti-Cheat Disabled + BakkesMod (Hitbox + Freeplay Checkpoint plugins).
# Freeplay, training, exhibition vs bots, replays. Online matchmaking is NOT available in this mode.
# Usage: rl-mods.sh            -> main menu with BakkesMod
#        rl-mods.sh freeplay   -> straight into freeplay with BakkesMod
if [ "$1" = freeplay ]; then
    exec steam -applaunch 252950 -bakkesmod -freeplay -nomovie
fi
exec steam -applaunch 252950 -bakkesmod -nomovie
