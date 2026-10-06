#!/usr/bin/env python3
"""Run JavaScript inside the running Steam client (SharedJSContext) over its CEF debug port.
Usage: steam-js.py 'js expression'   (or pipe the JS on stdin). Prints the JSON result.
Lets us change shortcuts / launch options live, without closing Steam."""
import json, sys, urllib.request
sys.path.insert(0, "/home/deck/rl-tools")
from wsmini import WS

def run(js, title="SharedJSContext"):
    tabs = json.load(urllib.request.urlopen("http://127.0.0.1:8080/json", timeout=5))
    url = next(t for t in tabs if t["title"] == title)["webSocketDebuggerUrl"]
    ws = WS("127.0.0.1", 8080, url.split("127.0.0.1:8080", 1)[1], timeout=60)
    ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate", "params": {
        "expression": js, "awaitPromise": True, "returnByValue": True}}))
    while True:
        r = json.loads(ws.recv())
        if r.get("id") == 1:
            break
    ws.close()
    res = r.get("result", {})
    if "exceptionDetails" in res:
        raise RuntimeError(json.dumps(res["exceptionDetails"])[:2000])
    return res.get("result", {}).get("value")

if __name__ == "__main__":
    js = sys.argv[1] if len(sys.argv) > 1 else sys.stdin.read()
    print(json.dumps(run(js), indent=1))
