"""Send messages one at a time and validate each client echo.

Usage:
    python rtt_echo_server.py <port> <count> <size> <text|binary>
"""
import os, socket, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, build_frame, make_101, read_client_frame, read_http_upgrade

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
COUNT = int(sys.argv[2])
SIZE = int(sys.argv[3])
KIND = sys.argv[4]
PAYLOAD_BYTE = b"A" if KIND == "text" else b"\xaa"
OPCODE = 0x1 if KIND == "text" else 0x2


def verify_echo(conn):
    fin, opcode, payload = read_client_frame(conn)
    return fin and opcode == OPCODE and payload == PAYLOAD_BYTE * SIZE


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(120)

try:
    conn, _ = srv.accept()
    conn.settimeout(30)
    _, key = read_http_upgrade(conn)
    conn.sendall(make_101(key))

    frame = build_frame(OPCODE, PAYLOAD_BYTE * SIZE)
    for index in range(COUNT):
        conn.sendall(frame)
        if not verify_echo(conn):
            raise ConnectionError(f"invalid client echo at message {index}")
    conn.sendall(build_close(1000))
    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x8 or payload != b"\x03\xe8":
        raise ConnectionError("expected clean Close response with code 1000")
    conn.close()
finally:
    srv.close()
