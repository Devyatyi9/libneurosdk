"""Send a validated echo frame one byte at a time with a delay."""
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, build_text, make_101, read_client_frame, read_http_upgrade

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19004
MESSAGE = "Hello, WebSocket!"
EXPECTED = MESSAGE.encode()

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(30)

try:
    conn, _ = srv.accept()
    conn.settimeout(10)
    _, key = read_http_upgrade(conn)
    conn.sendall(make_101(key))

    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x1 or payload != EXPECTED:
        raise ConnectionError("expected one masked Hello, WebSocket! text frame")

    for byte in build_text(MESSAGE):
        conn.sendall(bytes([byte]))
        time.sleep(0.01)

    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x8 or payload != b"\x03\xe8":
        raise ConnectionError("expected masked Close response with code 1000")
    conn.sendall(build_close())
    conn.close()
finally:
    srv.close()
