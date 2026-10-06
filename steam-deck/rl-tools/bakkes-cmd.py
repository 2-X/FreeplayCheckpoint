#!/usr/bin/env python3
"""Send console commands to a running BakkesMod through its RCON plugin (websocket on 127.0.0.1:9002).
Usage: bakkes-cmd.py [--wait SECONDS] 'command' ['command' ...]
--wait keeps retrying until BakkesMod is injected and listening (used right after game start).
Without a command it only reports whether BakkesMod answers (exit 0 = loaded).
Commands must be whitelisted in bakkesmod/data/rcon_commands.cfg."""
import re, socket, sys, time
sys.path.insert(0, "/home/deck/rl-tools")
from wsmini import WS

CFG = ("/home/deck/.local/share/Steam/steamapps/compatdata/252950/pfx/drive_c/users/steamuser/"
       "AppData/Roaming/bakkesmod/bakkesmod/cfg/config.cfg")

def setting(name, default):
    try:
        m = re.search(rf'^{name}\s+"([^"]*)"', open(CFG, errors="replace").read(), re.M)
        return m.group(1) if m else default
    except OSError:
        return default

def main():
    argv = sys.argv[1:]
    wait = 0
    if argv[:1] == ["--wait"]:
        wait = int(argv[1]); argv = argv[2:]
    port, password = int(setting("rcon_port", "9002")), setting("rcon_password", "password")
    deadline = time.time() + wait
    retried = False
    while True:
        try:
            ws = WS("127.0.0.1", port, "/", timeout=10)
            break
        except (OSError, ConnectionError, EOFError) as e:
            if time.time() >= deadline:
                print(f"BakkesMod RCON not reachable on port {port}: {e}"); return 1
            retried = True
            time.sleep(1)
    if retried:
        time.sleep(2)   # just injected: give the mod a moment to finish loading plugins
    ws.send(f"rcon_password {password}")
    reply = ws.recv()
    if "authyes" not in reply:
        print("RCON auth failed:", reply); return 1
    for cmd in argv:
        ws.send(cmd)
        print("sent:", cmd)
    ws.s.settimeout(1.5 if argv else 0.1)
    try:
        while True:
            print("reply:", ws.recv()[:200])
    except (socket.timeout, EOFError, OSError):
        pass
    ws.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
