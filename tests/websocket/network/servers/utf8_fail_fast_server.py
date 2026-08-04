"""Send invalid UTF-8 across WebSocket fragments or TCP chops."""
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_frame, make_101, mark_server_ready, read_client_frame, read_http_upgrade


HOST = "127.0.0.1"
PORT = int(sys.argv[1])
MODE = sys.argv[2]
VALID = b"\xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5"
INVALID = b"\xf4\x90"
TAIL = b"\x80\x80edited"


def expect_invalid_payload_close(conn):
    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x8 or payload != b"\x03\xef":
        raise ConnectionError("expected masked Close with code 1007")


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
mark_server_ready()
srv.settimeout(60)

try:
    conn, _ = srv.accept()
    conn.settimeout(2)
    _, key = read_http_upgrade(conn)
    if key is None:
        raise ConnectionError("missing WebSocket upgrade key")
    conn.sendall(make_101(key))

    if MODE == "fragment":
        conn.sendall(build_frame(0x1, VALID, fin=False))
        time.sleep(0.1)
        conn.sendall(build_frame(0x0, INVALID, fin=False))
    elif MODE == "chop":
        frame = build_frame(0x1, VALID + INVALID + TAIL)
        conn.sendall(frame[:2 + len(VALID)])
        time.sleep(0.1)
        conn.sendall(frame[2 + len(VALID):2 + len(VALID) + len(INVALID)])
    else:
        raise ValueError(f"unknown mode: {MODE}")

    expect_invalid_payload_close(conn)
    conn.close()
finally:
    srv.close()
