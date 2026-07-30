"""TCP server that sends a WS frame exceeding the client's max payload buffer.

Tests #20: Max Payload Size — client must reject oversized frame
(code 1009) rather than overflow the receive buffer.
The client's recv buffer is WS_RECV_BUF_SIZE = 262144 bytes.
We send a frame larger than that.
"""
import os, socket, struct, time, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import read_http_upgrade, make_101

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19008
OVERSIZE = 300000  # > WS_RECV_BUF_SIZE (262144)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(30)

try:
    conn, addr = srv.accept()
    conn.settimeout(10)
    data, key = read_http_upgrade(conn)
    if key is None:
        conn.close(); srv.close(); sys.exit(0)
    conn.sendall(make_101(key))
    time.sleep(0.1)
    # Build oversized frame (FIN + text, no mask since server->client)
    frame = bytearray()
    frame.append(0x81)  # FIN + text
    if OVERSIZE < 65536:
        frame.append(126)
        frame.extend(struct.pack(">H", OVERSIZE))
    else:
        frame.append(127)
        frame.extend(struct.pack(">Q", OVERSIZE))
    # Send the header but NOT the full payload — just enough to trigger detection
    conn.sendall(bytes(frame))
    # Send a chunk of payload, then stop — client should reject before reading all
    chunk_size = min(OVERSIZE, 300000)
    conn.sendall(b"X" * chunk_size)
    time.sleep(2)
    conn.close()
except socket.timeout:
    pass
except BrokenPipeError:
    pass
finally:
    srv.close()
