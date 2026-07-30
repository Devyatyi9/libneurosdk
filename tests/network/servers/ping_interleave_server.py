"""TCP server that sends a fragmented message with ping frames between fragments.

Tests #18: ping interleaving — client must handle control frames
(Ping/Pong/Close) between fragments of a large message.
"""
import os, socket, struct, time, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_frame, build_ping, build_fragment, HTTP_101

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19011

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
    # Send fragmented message: FIN=0, opcode=text (start)
    part1 = b"Hello, this is the first part "
    conn.sendall(build_frame(0x1, part1, fin=False))
    time.sleep(0.1)
    # Ping between fragments
    conn.sendall(build_ping(b"interleave"))
    time.sleep(0.05)
    # Continuation frame
    part2 = b"and this is the second part!"
    conn.sendall(build_frame(0x0, part2, fin=True))
    time.sleep(0.5)
    # Send close
    close_frame = b"\x88\x02\x03\xe8"
    conn.sendall(close_frame)
    time.sleep(1)
    conn.close()
except socket.timeout:
    pass
finally:
    srv.close()
