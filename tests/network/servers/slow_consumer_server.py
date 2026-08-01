"""Delay and throttle reads while validating one large client frame.

Tests #9: partial send() — causes client's TCP send buffer to fill,
triggering EAGAIN/WSAEWOULDBLOCK in sock_send_all().
"""
import os, socket, time, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, build_text, read_client_frame, read_http_upgrade, make_101

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19005

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(120)

try:
    conn, _ = srv.accept()
    conn.settimeout(60)
    _, key = read_http_upgrade(conn)
    if key is None:
        raise ConnectionError("missing WebSocket upgrade key")
    conn.sendall(make_101(key))
    time.sleep(0.5)
    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x1:
        raise ConnectionError("expected one final text frame")
    if len(payload) != 10 * 1024 * 1024 or payload != b"A" * len(payload):
        raise ConnectionError("large client payload is incomplete or corrupted")
    conn.sendall(build_text("partial-send-ok"))
    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x8 or payload != b"\x03\xe8":
        raise ConnectionError("expected masked Close response with code 1000")
    conn.sendall(build_close())
    conn.close()
finally:
    srv.close()
