"""TCP server that sends HTTP 101 then closes TCP without WS Close frame.

Tests #13: TCP FIN without WS Close — client must detect EOF and
trigger on_close(code=1006) or on_error().
"""
import os, socket, time, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import make_101, mark_server_ready, read_http_upgrade

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19010

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
    time.sleep(0.5)
    # Graceful TCP close without sending WS Close frame
    conn.shutdown(socket.SHUT_WR)
    time.sleep(0.5)
    conn.close()
finally:
    srv.close()
