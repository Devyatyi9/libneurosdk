"""TCP server that sends 1000+ WS text frames as fast as possible.

Tests #16: flood — client must handle rapid frame delivery without
dropping frames or leaking memory.
"""
import os, socket, struct, time, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_text, HTTP_101

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19006
COUNT = int(sys.argv[2]) if len(sys.argv) > 2 else 1000

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(60)

try:
    conn, addr = srv.accept()
    conn.settimeout(10)
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk
    conn.sendall(HTTP_101)
    time.sleep(0.1)
    for i in range(COUNT):
        frame = build_text(f"msg{i}")
        conn.sendall(frame)
    # Send close frame
    close_frame = b"\x88\x02\x03\xe8"
    conn.sendall(close_frame)
    time.sleep(1)
    conn.close()
except socket.timeout:
    pass
finally:
    srv.close()
