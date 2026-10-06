"""Tiny dependency-free WebSocket client (text frames only) shared by steam-js.py and bakkes-cmd.py."""
import base64, os, socket, struct

class WS:
    def __init__(self, host, port, path="/", timeout=30):
        self.s = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            c = self.s.recv(4096)
            if not c: raise ConnectionError("closed during handshake")
            self.buf += c
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise ConnectionError(head.decode(errors="replace"))

    def send(self, text):
        data = text.encode(); mask = os.urandom(4); n = len(data)
        hdr = b"\x81" + (bytes([0x80 | n]) if n < 126 else
                         b"\xfe" + struct.pack(">H", n) if n < 65536 else b"\xff" + struct.pack(">Q", n))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _need(self, k):
        while len(self.buf) < k:
            c = self.s.recv(65536)
            if not c: raise EOFError
            self.buf += c
        out, self.buf = self.buf[:k], self.buf[k:]
        return out

    def recv(self):
        """Return the next complete text/binary message as str."""
        msg = b""
        while True:
            b0, b1 = self._need(2)
            ln = b1 & 0x7F
            if ln == 126: ln = struct.unpack(">H", self._need(2))[0]
            elif ln == 127: ln = struct.unpack(">Q", self._need(8))[0]
            chunk = self._need(ln)
            op = b0 & 0x0F
            if op == 8: raise EOFError("closed by peer")
            if op in (0, 1, 2):
                msg += chunk
                if b0 & 0x80:
                    return msg.decode(errors="replace")

    def close(self):
        try: self.s.close()
        except OSError: pass
