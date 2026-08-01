"""TCP server that sends a fragmented message with ping frames between fragments.

Tests #18: ping interleaving — client must handle control frames
(Ping/Pong/Close) between fragments of a large message.
"""
import os, socket, time, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, build_frame, build_ping, read_client_frame, read_http_upgrade, make_101

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19011

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(60)

try:
    conn, _ = srv.accept()
    conn.settimeout(10)
    _, key = read_http_upgrade(conn)
    if key is None:
        raise ConnectionError("missing WebSocket upgrade key")
    conn.sendall(make_101(key))
    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x1 or payload != b"Hello, WebSocket!":
        raise ConnectionError("expected the echo client's initial text frame")
    # Send fragmented message: FIN=0, opcode=text (start)
    part1 = b"Hello, this is the first part "
    conn.sendall(build_frame(0x1, part1, fin=False))
    time.sleep(0.1)
    # Ping between fragments
    conn.sendall(build_ping(b"interleave"))
    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0xA or payload != b"interleave":
        raise ConnectionError("expected masked Pong with interleave payload")
    # Continuation frame
    part2 = b"and this is the second part!"
    conn.sendall(build_frame(0x0, part2, fin=True))
    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x8 or payload != b"\x03\xe8":
        raise ConnectionError("expected masked Close with code 1000")
    conn.sendall(build_close())
    conn.close()
finally:
    srv.close()
