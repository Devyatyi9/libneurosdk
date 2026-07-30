"""TCP server that sends HTTP 101 then closes TCP without WS Close frame.

Tests #13: TCP FIN without WS Close — client must detect EOF and
trigger on_close(code=1006) or on_error().
"""
import os, socket, time, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import read_http_upgrade, make_101

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19010

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(30)

try:
    conn, addr = srv.accept()
    conn.settimeout(5)
    data, key = read_http_upgrade(conn)
    if key is None:
        conn.close(); srv.close(); sys.exit(0)
    conn.sendall(make_101(key))
    time.sleep(0.5)
    # Graceful TCP close without sending WS Close frame
    conn.shutdown(socket.SHUT_WR)
    time.sleep(0.5)
    conn.close()
except socket.timeout:
    pass
finally:
    srv.close()
