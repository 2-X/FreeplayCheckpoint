#!/usr/bin/env python3
"""Local stand-in for the two servers Rocket League + BakkesMod need at start-up, for playing without internet.

1. At start-up the game downloads its "static data" from https://config.psynet.gg and holds every map load until
   that is done. Without internet it retries for 55-90 s before it falls back to the copy it saved last time
   (TAGame/Cache/WebCache); a Nexto match or freeplay loaded earlier sits on a black screen that long.
2. BakkesMod (inside the game) downloads https://config.bakkesmod.com/static/startupconfig.json to check that it
   supports this game build. Without an answer it asks "Could not verify RL version, inject anyway?" in a
   message box. rl-inject.exe can answer that box, but in Gaming Mode the game's picture is gone afterwards
   (black screen, ~3 fps on the performance overlay, game still running underneath).

The HTTP client of both honours the https_proxy variable. rl-launch.sh points it here when the real servers cannot
be reached, and this answers 1 with "not modified, use your saved copy" and 2 with the copy bakkes-update.py
saved at the last start with internet, so neither the wait nor the box happens. Every other request fails at
once, exactly as it does offline anyway. If the internet comes back while playing, requests are passed through
to the real servers untouched.

  psynet-offline.py start [--force]    start it in the background when the real server is not reachable (or
                                       --force). Exit 0 and print host:port when started, exit 1 when not.
  psynet-offline.py serve [--force] [--mitm-log]
                                       run in the foreground. --mitm-log (testing, needs internet): relay the
                                       config request to the real server and log both sides' headers.
"""
import base64, glob, os, select, socket, ssl, struct, subprocess, sys, threading, time

HOST = "config.psynet.gg"
BAKKES_HOSTS = ("config.bakkesmod.com", "bakkesmod.com")    # the mod tries them in this order
LISTEN = ("127.0.0.1", 23280)
TOOLS = "/home/deck/rl-tools"
CERT = TOOLS + "/cache/psynet-offline.pem"
PIDFILE = TOOLS + "/cache/psynet-offline.pid"
LOGFILE = TOOLS + "/logs/psynet-offline.log"
BAKKES_SAVED = {    # written by bakkes-update.py refresh
    "/static/startupconfig.json": TOOLS + "/cache/bakkes-startupconfig.json",
    "/static/onetime.json": TOOLS + "/cache/bakkes-onetime.json",
}
PFX_ROOT = os.environ.get("STEAM_COMPAT_DATA_PATH") or "/home/deck/.local/share/Steam/steamapps/compatdata/252950"
WEBCACHE = PFX_ROOT + "/pfx/drive_c/users/steamuser/Documents/My Games/Rocket League/TAGame/Cache/WebCache"

FORCE = "--force" in sys.argv
MITM_LOG = "--mitm-log" in sys.argv
offline_until = 0.0     # after a failed connect, do not try the real servers again before this time


def log(msg):
    print(f"[psynet-offline {time.strftime('%H:%M:%S')}] {msg}", flush=True)


def mask(text):
    """Request lines carry the game's build secret; keep it out of the log."""
    if "buildSecret=" in text:
        head, _, tail = text.partition("buildSecret=")
        secret, sep, rest = tail.partition(" ")
        text = f"{head}buildSecret=<{len(secret)} chars>{sep}{rest}"
    return text


def try_connect(host, port, timeout=3):
    """Connected socket or None, in at most ~timeout s (a name lookup on dead Wi-Fi can hang far longer)."""
    found = []

    def run():
        try:
            found.append(socket.create_connection((host, port), timeout=timeout))
        except OSError:
            pass

    t = threading.Thread(target=run, daemon=True)
    t.start()
    t.join(timeout + 0.5)
    return found[0] if found else None


def connect_real(host, port):
    global offline_until
    if FORCE or time.time() < offline_until:
        return None
    sock = try_connect(host, port)
    if sock is None:
        offline_until = time.time() + 30
    return sock


def saved_config():
    """The game's saved copy of the config answer: {Data, ETag, ContentType, Signature}, or None.
    File name = base64 of the URL's tail; content = UE3 tagged properties after a 16 byte header."""
    for path in glob.glob(WEBCACHE + "/*"):
        try:
            name = base64.urlsafe_b64decode(os.path.basename(path)).decode("latin-1")
        except ValueError:
            continue
        if "buildSecret=" not in name:
            continue
        try:
            blob = open(path, "rb").read()
            props, pos = {}, 16

            def fstring():
                nonlocal pos
                n = struct.unpack_from("<i", blob, pos)[0]
                s = blob[pos + 4:pos + 4 + n].rstrip(b"\0").decode("latin-1")
                pos += 4 + n
                return s

            while pos < len(blob):
                key = fstring()
                if key == "None":
                    break
                kind = fstring()
                size = struct.unpack_from("<i", blob, pos)[0]
                pos += 8
                value = blob[pos:pos + size]
                pos += size
                if kind == "ArrayProperty":
                    props[key] = value[4:]
                elif kind == "StrProperty":
                    props[key] = value[4:].rstrip(b"\0").decode("latin-1")
            if props.get("Data"):
                return props
        except (OSError, struct.error, UnicodeDecodeError) as e:
            log(f"cannot read the game's saved config {path}: {e}")
    return None


def read_head(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk
        if len(data) > 1 << 20:
            break
    return data


def headers_of(head):
    out = {}
    for line in head.split(b"\r\n\r\n")[0].split(b"\r\n")[1:]:
        k, _, v = line.decode("latin-1").partition(":")
        out[k.strip().lower()] = v.strip()
    return out


def tunnel(a, b):
    a.settimeout(None)
    b.settimeout(None)
    try:
        while True:
            for s in select.select([a, b], [], [], 120)[0] or [None]:
                if s is None:
                    return
                data = s.recv(65536)
                if not data:
                    return
                (b if s is a else a).sendall(data)
    except OSError:
        pass


def answer_from_saved_copy(tls, head):
    """The game asks "has the config changed since <ETag of my saved copy>?"; the real server's usual answer is
    304 Not Modified (ETag + "Cache-Control: public,max-age=30"), after which the game goes on with its copy."""
    request = head.split(b"\r\n")[0].decode("latin-1")
    saved = saved_config()
    etag = (saved or {}).get("ETag", "")
    wanted = headers_of(head).get("if-none-match", "").removeprefix("W/").strip('"')
    if etag and wanted == etag:
        log(f"{mask(request)} -> 304, the game goes on with its saved config (ETag {etag})")
        tls.sendall(f"HTTP/1.1 304 Not Modified\r\nETag: {etag}\r\nCache-Control: public,max-age=30\r\n"
                    "Connection: close\r\n\r\n".encode())
        return
    # No usable saved copy (the game needs one start with internet first): fail like the offline game would.
    log(f"{mask(request)} -> 503, no saved config to go on with (game asked for '{wanted}', saved '{etag}')")
    tls.sendall(b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")


def answer_bakkesmod(tls, head):
    """BakkesMod's start-up downloads, answered with the copies saved at the last start with internet."""
    request = head.split(b"\r\n")[0].decode("latin-1")
    words = request.split()
    path = words[1].split("?")[0] if len(words) > 1 else ""
    try:
        with open(BAKKES_SAVED[path], "rb") as f:
            body = f.read()
    except (KeyError, OSError):
        log(f"bakkesmod: {request} -> 404, no saved copy")
        tls.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        return
    log(f"bakkesmod: {request} -> 200, saved copy ({len(body)} bytes)")
    tls.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)


def relay_and_log(tls, head, upstream):
    """--mitm-log: send the request on to the real server and log what both sides say."""
    log("game  > " + mask(head.split(b"\r\n\r\n")[0].decode("latin-1")).replace("\r\n", " | "))
    real = ssl.create_default_context().wrap_socket(upstream, server_hostname=HOST)
    real.sendall(head)
    real.settimeout(10)
    answer = b""
    try:
        while True:
            chunk = real.recv(65536)
            if not chunk:
                break
            answer += chunk
            tls.sendall(chunk)
            hdrs = headers_of(answer) if b"\r\n\r\n" in answer else None
            if hdrs is not None:
                body = answer.split(b"\r\n\r\n", 1)[1]
                if answer.startswith(b"HTTP/1.1 304") or len(body) >= int(hdrs.get("content-length", 1 << 40)):
                    break
    except OSError as e:
        log(f"relay ended: {e}")
    head_part, _, body = answer.partition(b"\r\n\r\n")
    log("server> " + head_part.decode("latin-1").replace("\r\n", " | ") + f" || body {len(body)} bytes")
    saved = saved_config()
    if saved and body:
        log(f"body equals the game's saved copy: {body == saved['Data']} (saved {len(saved['Data'])} bytes,"
            f" ETag {saved.get('ETag')}, Signature {saved.get('Signature')})")


def handle(conn, tls_ctx):
    try:
        conn.settimeout(20)
        head = read_head(conn)
        words = head.split(b"\r\n")[0].decode("latin-1").split()
        if len(words) < 2 or words[0] != "CONNECT":
            if words:
                log(f"{mask(' '.join(words))} -> 502 (only https is handled)")
            conn.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return
        host, _, port = words[1].rpartition(":")
        upstream = connect_real(host, int(port))
        if upstream is not None and not (MITM_LOG and host == HOST):
            conn.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
            tunnel(conn, upstream)
            upstream.close()
            return
        if host != HOST and host not in BAKKES_HOSTS:
            log(f"{host} -> 502 (no internet)")
            conn.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return
        conn.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
        tls = tls_ctx.wrap_socket(conn, server_side=True)
        head = read_head(tls)
        if host in BAKKES_HOSTS:
            answer_bakkesmod(tls, head)
        elif upstream is not None:
            relay_and_log(tls, head, upstream)
        else:
            answer_from_saved_copy(tls, head)
        try:
            tls.unwrap()
        except OSError:
            pass
        conn = tls
    except (OSError, ValueError) as e:
        log(f"connection failed: {e!r}")
    finally:
        try:
            conn.close()
        except OSError:
            pass


def rl_running():
    return subprocess.run(["pgrep", "-f", r"^Z:.*RocketLeague\.exe"], stdout=subprocess.DEVNULL).returncode == 0


def watch_game():
    """Stop with the game (or after 5 minutes when it never shows up)."""
    seen, started = False, time.time()
    while True:
        time.sleep(3)
        running = rl_running()
        seen = seen or running
        if (seen and not running) or (not seen and time.time() - started > 300):
            log("game is gone, stopping")
            os._exit(0)


def cert_hosts():
    try:
        return {name for kind, name in ssl._ssl._test_decode_cert(CERT).get("subjectAltName", ()) if kind == "DNS"}
    except Exception:  # noqa: BLE001  (no certificate yet, or unreadable)
        return set()


def ensure_cert():
    hosts = (HOST,) + BAKKES_HOSTS
    if set(hosts) <= cert_hosts():
        return
    os.makedirs(os.path.dirname(CERT), exist_ok=True)
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650",
                    "-subj", f"/CN={HOST}", "-addext", "subjectAltName=" + ",".join("DNS:" + h for h in hosts),
                    "-keyout", CERT + ".key", "-out", CERT + ".crt"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(CERT + ".tmp", "w") as f:
        f.write(open(CERT + ".key").read() + open(CERT + ".crt").read())
    os.chmod(CERT + ".tmp", 0o600)
    os.replace(CERT + ".tmp", CERT)
    os.remove(CERT + ".key")
    os.remove(CERT + ".crt")


def stop_previous():
    try:
        pid = int(open(PIDFILE).read())
        if b"psynet-offline.py" in open(f"/proc/{pid}/cmdline", "rb").read() and pid != os.getpid():
            os.kill(pid, 15)
            time.sleep(0.3)
    except (OSError, ValueError):
        pass


def serve(ready_fd=None):
    ensure_cert()
    tls_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls_ctx.load_cert_chain(CERT)
    stop_previous()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(LISTEN)
    srv.listen(64)
    with open(PIDFILE, "w") as f:
        f.write(str(os.getpid()))
    log(f"listening on {LISTEN[0]}:{LISTEN[1]}" + (" (forced)" if FORCE else "") + (" (mitm-log)" if MITM_LOG else "")
        + ("" if saved_config() else "; WARNING: the game has no saved config yet"))
    if ready_fd is not None:
        os.write(ready_fd, b"ok")
        os.close(ready_fd)
    threading.Thread(target=watch_game, daemon=True).start()
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn, tls_ctx), daemon=True).start()


def start():
    global offline_until
    if not FORCE and not MITM_LOG:
        sock = try_connect(HOST, 443)
        if sock is not None:
            sock.close()
            return 1    # the real server is there; nothing to do
        offline_until = time.time() + 60    # the game's request comes ~25 s from now; do not probe again for it
    r, w = os.pipe()
    if os.fork() == 0:
        os.close(r)
        os.setsid()
        if os.fork() != 0:
            os._exit(0)
        os.makedirs(os.path.dirname(LOGFILE), exist_ok=True)
        out = os.open(LOGFILE, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
        null = os.open(os.devnull, os.O_RDONLY)
        os.dup2(null, 0)
        os.dup2(out, 1)
        os.dup2(out, 2)
        try:
            serve(w)
        except Exception as e:  # noqa: BLE001
            log(f"could not start: {e!r}")
        os._exit(1)
    os.close(w)
    ok = select.select([r], [], [], 10)[0] and os.read(r, 2) == b"ok"
    if not ok:
        return 1
    print(f"{LISTEN[0]}:{LISTEN[1]}")
    return 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "start":
        sys.exit(start())
    if cmd == "serve":
        serve()
    print(__doc__)
    sys.exit(2)
