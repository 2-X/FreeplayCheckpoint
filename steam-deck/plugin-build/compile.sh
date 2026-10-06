#!/bin/sh
# Runs inside the bm-plugin-build container. /src = plugin source (read-only),
# /sdk = bakkesmodsdk (read-only), /out = where the dll lands.
set -eu
NAME=${1:-CheckpointPlugin}
B=/work
mkdir -p $B/obj

# Windows file names are case-insensitive and the SDK / plugin sources rely on
# that (and on "\" in a few #includes). Work on a copy with every file name and
# every #include line lowercased; xwin ships the system headers lowercase too.
mkdir $B/src
(cd /src && tar cf - --exclude=.git --exclude=plugins --exclude=build .) | tar xf - -C $B/src
cp -r /sdk/include $B/sdk
for t in $B/src $B/sdk; do
  find $t -depth -mindepth 1 | while read -r p; do
    b=${p##*/}; l=$(printf %s "$b" | tr 'A-Z' 'a-z')
    [ "$b" = "$l" ] || mv "$p" "${p%/*}/$l"
  done
  find $t -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' -o -name '*.c' \) | while read -r f; do
    awk '/^[ \t]*#[ \t]*include/ { gsub(/\\/, "/"); $0 = tolower($0) } { print }' "$f" > $B/tmp && cat $B/tmp > "$f"
  done
done
S=$B/src

BIN=$(dirname "$(readlink -f "$(command -v clang)")")
PATH=$BIN:$PATH
INC="/imsvc /xwin/crt/include /imsvc /xwin/sdk/include/ucrt /imsvc /xwin/sdk/include/um /imsvc /xwin/sdk/include/shared"
CFLAGS="--target=x86_64-pc-windows-msvc /nologo /c /std:c++17 /O2 /MT /EHsc /GS /Gy /W3 \
  /DNDEBUG /D_CONSOLE /DUNICODE /D_UNICODE -Wno-unused-command-line-argument \
  $INC /I $S /I $S/fmt/include /I $B/sdk"

cd $S
LNAME=$(printf %s "$NAME" | tr 'A-Z' 'a-z')
for f in $(sed -n 's/.*<ClCompile Include="\([^"]*\)".*/\1/p' "/src/$NAME.vcxproj" | tr '\\A-Z' '/a-z'); do
  o=$B/obj/$(echo "$f" | tr '/' '_').obj
  clang --driver-mode=cl $CFLAGS "/Fo$o" "$f" &
done
wait
ls $B/obj/*.obj >/dev/null

RES=
if [ -f "$LNAME.rc" ]; then
  llvm-rc /nologo /I $S /I /xwin/sdk/include/um /I /xwin/sdk/include/shared /I /xwin/sdk/include/ucrt /I /xwin/crt/include \
    /fo $B/obj/res.res "$LNAME.rc" && RES=$B/obj/res.res
fi

lld-link /nologo /dll /machine:x64 /subsystem:console /opt:ref /opt:icf \
  /libpath:/xwin/crt/lib/x86_64 /libpath:/xwin/sdk/lib/um/x86_64 /libpath:/xwin/sdk/lib/ucrt/x86_64 \
  /libpath:/sdk/lib pluginsdk.lib kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib $B/obj/*.obj $RES "/out:/out/$NAME.dll" "/implib:$B/$NAME.lib"
ls -l "/out/$NAME.dll"
