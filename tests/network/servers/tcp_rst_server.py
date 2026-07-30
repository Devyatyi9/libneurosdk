"""TCP server that sends RST after accepting connection.

Tests #12: TCP RST — client must detect abrupt connection reset.
Sets SO_LINGER to 0 so close() sends RST instead of FIN.
"""
import os, socket, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import read_http_upgrade, make_101

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19009

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
    # Send partial 101 then RST (intentionally drop before Accept header)
    try:
        conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\n")
    except OSError:
        pass
    # Set SO_LINGER=0 to force RST on close
    l_onoff = 1
    l_linger = 0
    conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                    struct.pack('ii', l_onoff, l_linger))
    time.sleep(0.1)
    conn.close()
except socket.timeout:
    pass
except ConnectionResetError:
    pass
finally:
    srv.close()
