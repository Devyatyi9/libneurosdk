"""WebSocket server that sends RST after completing the upgrade.

Tests #12: TCP RST — client must detect abrupt connection reset.
Sets SO_LINGER to 0 so close() sends RST instead of FIN.
"""
import os, socket, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import make_101, mark_server_ready, read_http_upgrade

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19009

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
mark_server_ready()
srv.settimeout(30)

try:
    conn, _ = srv.accept()
    conn.settimeout(5)
    _, key = read_http_upgrade(conn)
    if key is None:
        raise ConnectionError("missing WebSocket upgrade key")
    conn.sendall(make_101(key))
    time.sleep(1)
    # Set SO_LINGER=0 to force RST after the client has observed on_open.
    l_onoff, l_linger = 1, 0
    conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                    struct.pack('ii', l_onoff, l_linger))
    time.sleep(0.1)
    conn.close()
finally:
    srv.close()
