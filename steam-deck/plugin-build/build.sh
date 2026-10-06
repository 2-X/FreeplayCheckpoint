#!/bin/bash
# Build a BakkesMod plugin (MSVC project) on the Deck with clang-cl in podman.
#   build.sh <source dir> [project name]     -> <source dir>/plugins/<name>.dll
# The image is built once from Containerfile (clang + MSVC CRT/SDK through xwin).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "${1:?source dir}" && pwd)
NAME=${2:-$(basename "$SRC"/*.vcxproj .vcxproj)}
SDK=${BAKKESMOD_SDK:-$HOME/.local/share/Steam/steamapps/compatdata/252950/pfx/drive_c/users/steamuser/AppData/Roaming/bakkesmod/bakkesmod/bakkesmodsdk}

podman image exists bm-plugin-build || podman build -t bm-plugin-build -f "$HERE/Containerfile" "$HERE"
mkdir -p "$SRC/plugins"
podman run --rm -v "$SRC:/src:ro" -v "$SDK:/sdk:ro" -v "$SRC/plugins:/out" -v "$HERE/compile.sh:/compile.sh:ro" \
  bm-plugin-build sh /compile.sh "$NAME"
