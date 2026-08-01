"""Send and validate a clean lifecycle for one 300000-byte text frame."""
import os
import socket
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, make_101, read_client_frame, read_http_upgrade

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19008
PAYLOAD = b"X" * 300000

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
    conn.sendall(b"\x81\x7f" + struct.pack(">Q", len(PAYLOAD)) + PAYLOAD)
    conn.sendall(build_close())

    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x8 or payload != b"\x03\xe8":
        raise ConnectionError("expected masked Close response with code 1000")
    conn.close()
finally:
    srv.close()
