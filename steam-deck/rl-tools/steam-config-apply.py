#!/usr/bin/env python3
"""Apply Steam-side config for the Rocket League mod setup. RUN ONLY WHILE STEAM IS CLOSED.
 1. localconfig.vdf: Rocket League LaunchOptions -> ~/rl-tools/rl-launch.sh %command%
 2. shortcuts.vdf: add non-Steam shortcuts 'RL BakkesMod Freeplay', 'RL BakkesMod Mode',
    'RL vs Nexto 1v1', 'RL vs Nexto 2v2'
Usage: steam-config-apply.py [--no-nexto] [--dry-run DIR]   (dry-run writes results into DIR instead of Steam's files)
While Steam is running use steam-js.py instead (install.sh does); Steam overwrites these files on exit.
"""
import re, shutil, struct, sys, time, zlib
from pathlib import Path

HOME = str(Path.home())
STEAM = Path(HOME) / ".local/share/Steam"


def find_userdata():
    """config folder of the Steam user who logged in last (the one whose localconfig.vdf was written last)."""
    found = [p for p in (STEAM / "userdata").glob("*/config/localconfig.vdf") if p.parent.parent.name not in ("0", "anonymous")]
    if not found:
        sys.exit(f"no Steam user found under {STEAM / 'userdata'}")
    return max(found, key=lambda p: p.stat().st_mtime).parent


USERDATA = find_userdata()
LAUNCH_OPTIONS = f"{HOME}/rl-tools/rl-launch.sh %command%"
ICON = f"{HOME}/.local/share/icons/hicolor/48x48/apps/steam_icon_252950.png"
SHORTCUTS = [
    ("RL BakkesMod Freeplay", f"{HOME}/rl-tools/rl-mods.sh", "freeplay"),
    ("RL BakkesMod Mode", f"{HOME}/rl-tools/rl-mods.sh", ""),
    ("RL vs Nexto 1v1", f"{HOME}/rl-tools/play-nexto.sh", "1v1"),
    ("RL vs Nexto 2v2", f"{HOME}/rl-tools/play-nexto.sh", "2v2"),
]
if "--no-nexto" in sys.argv:
    sys.argv.remove("--no-nexto")
    SHORTCUTS = [s for s in SHORTCUTS if "Nexto" not in s[0]]
dry = Path(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[1] == "--dry-run" else None

# ---------- localconfig.vdf ----------
lc = USERDATA / "localconfig.vdf"
s = lc.read_text(errors="surrogateescape")
m = re.search(r'("252950"\s*\{(?:[^{}]|\{[^{}]*\})*?)"LaunchOptions"\s*"[^"]*"', s)
if m:
    s2 = s[:m.start()] + m.group(1) + f'"LaunchOptions"\t\t"{LAUNCH_OPTIONS}"' + s[m.end():]
else:
    m = re.search(r'("252950"\s*\{\s*\n)', s)
    assert m, "252950 block not found"
    s2 = s[:m.end()] + f'\t\t\t\t\t\t"LaunchOptions"\t\t"{LAUNCH_OPTIONS}"\n' + s[m.end():]
assert s2.count(LAUNCH_OPTIONS) == 1
out = (dry / "localconfig.vdf") if dry else lc
if not dry:
    shutil.copy2(lc, lc.with_suffix(f".vdf.bak-{int(time.time())}"))
out.write_text(s2, errors="surrogateescape")
print("localconfig: LaunchOptions set ->", LAUNCH_OPTIONS)

# ---------- shortcuts.vdf (binary VDF) ----------
def parse(buf, i=0):
    d = {}
    while True:
        t = buf[i]; i += 1
        if t == 8:
            return d, i
        j = buf.index(b"\x00", i); key = buf[i:j].decode("utf-8", "surrogateescape"); i = j + 1
        if t == 0:
            d[key], i = parse(buf, i)
        elif t == 1:
            j = buf.index(b"\x00", i); d[key] = buf[i:j].decode("utf-8", "surrogateescape"); i = j + 1
        elif t == 2:
            d[key] = struct.unpack_from("<I", buf, i)[0]; i += 4
        else:
            raise ValueError(f"unknown type {t} at {i}")

def dump(d):
    out = bytearray()
    for k, v in d.items():
        kb = k.encode("utf-8", "surrogateescape")
        if isinstance(v, dict):
            out += b"\x00" + kb + b"\x00" + dump(v)
        elif isinstance(v, str):
            out += b"\x01" + kb + b"\x00" + v.encode("utf-8", "surrogateescape") + b"\x00"
        else:
            out += b"\x02" + kb + b"\x00" + struct.pack("<I", v)
    return bytes(out) + b"\x08"

sc = USERDATA / "shortcuts.vdf"
raw = sc.read_bytes() if sc.exists() else b"\x00shortcuts\x00\x08\x08"
root, _ = parse(raw)
shortcuts = root.setdefault("shortcuts", {})
existing = {str(v.get("AppName", v.get("appname", ""))).lower() for v in shortcuts.values()}
idx = max([int(k) for k in shortcuts] + [-1]) + 1
for name, exe, args in SHORTCUTS:
    if name.lower() in existing:
        print("shortcut exists:", name); continue
    appid = (zlib.crc32((f'"{exe}"' + name).encode()) | 0x80000000) & 0xFFFFFFFF
    shortcuts[str(idx)] = {
        "appid": appid, "appname": name, "exe": f'"{exe}"', "StartDir": f'"{HOME}/rl-tools/"',
        "icon": ICON, "ShortcutPath": "", "LaunchOptions": args, "IsHidden": 0, "AllowDesktopConfig": 1,
        "AllowOverlay": 1, "OpenVR": 0, "Devkit": 0, "DevkitGameID": "", "DevkitOverrideAppID": 0,
        "LastPlayTime": 0, "FlatpakAppID": "", "tags": {},
    }
    print("shortcut added:", name, hex(appid)); idx += 1
new = dump(root)
assert parse(new)[0] == root
out = (dry / "shortcuts.vdf") if dry else sc
if not dry and sc.exists():
    shutil.copy2(sc, sc.with_suffix(f".vdf.bak-{int(time.time())}"))
out.write_bytes(new)
print("shortcuts.vdf written:", out)
