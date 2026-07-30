"""TCP server that reads data VERY slowly (1 byte / 50ms).

Tests #9: partial send() — causes client's TCP send buffer to fill,
triggering EAGAIN/WSAEWOULDBLOCK in sock_send_all().
"""
import os, socket, time, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import read_http_upgrade, make_101

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19005

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(120)

try:
    conn, addr = srv.accept()
    conn.settimeout(60)
    data, key = read_http_upgrade(conn)
    if key is None:
        conn.close(); srv.close(); sys.exit(0)
    conn.sendall(make_101(key))
    # Now read client data as slowly as possible
    # The client is trying to send 10MB — we drain it at 1 byte/50ms
    total = 0
    start = time.time()
    while time.time() - start < 30:
        b = conn.recv(1)
        if not b:
            break
        total += 1
        time.sleep(0.05)
    # After enough data read, send a small echo back
    conn.sendall(b"\x81\x05Hello")
    time.sleep(1)
    conn.close()
except socket.timeout:
    pass
finally:
    srv.close()
