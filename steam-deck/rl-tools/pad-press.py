#!/usr/bin/env python3
"""Press a button on Steam's virtual gamepad ("Microsoft X-Box 360 pad 0") by writing to its evdev node.
Usage: pad-press.py [button-code]   (default 304 = A).
bakkes-helper.sh uses it to get past Rocket League's "press any button" title screen, which is where the
game binds the controller to the player; freeplay loaded before that leaves the car uncontrollable."""
import os, re, struct, sys, time

def main():
    code = int(sys.argv[1]) if len(sys.argv) > 1 else 304
    txt = open("/proc/bus/input/devices").read()
    m = re.search(r'N: Name="Microsoft X-Box 360 pad 0"\n(?:(?!\n\n).)*?H: Handlers=[^\n]*?(event\d+)', txt, re.S)
    if not m:
        print("Steam virtual gamepad not found"); return 1
    fd = os.open("/dev/input/" + m.group(1), os.O_WRONLY)
    def ev(t, c, v): os.write(fd, struct.pack("llHHi", 0, 0, t, c, v))
    ev(1, code, 1); ev(0, 0, 0); time.sleep(0.15); ev(1, code, 0); ev(0, 0, 0)
    os.close(fd)
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except OSError as e:
        print("pad-press failed:", e); sys.exit(1)
