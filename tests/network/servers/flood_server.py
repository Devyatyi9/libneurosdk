"""TCP server that sends 1000+ WS text frames as fast as possible.

Tests #16: flood — client must handle rapid frame delivery without
dropping frames or leaking memory.
"""
import os, socket, time, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, build_text, read_client_frame, read_http_upgrade, make_101

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19006
COUNT = int(sys.argv[2]) if len(sys.argv) > 2 else 1000

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
    time.sleep(0.1)
    for i in range(COUNT):
        frame = build_text(f"msg{i}")
        conn.sendall(frame)
    conn.sendall(build_close())
    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x8 or payload != b"\x03\xe8":
        raise ConnectionError("expected masked Close response with code 1000")
    conn.close()
finally:
    srv.close()
