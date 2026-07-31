"""Send messages one at a time and validate each client echo.

Usage:
    python rtt_echo_server.py <port> <count> <size> <text|binary>
"""
import os, socket, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, build_frame, make_101, read_http_upgrade

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
COUNT = int(sys.argv[2])
SIZE = int(sys.argv[3])
KIND = sys.argv[4]
PAYLOAD_BYTE = b"A" if KIND == "text" else b"\xaa"
OPCODE = 0x1 if KIND == "text" else 0x2


def recv_exact(conn, size):
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed while reading echo")
        data.extend(chunk)
    return bytes(data)


def verify_echo(conn):
    head = recv_exact(conn, 2)
    fin = bool(head[0] & 0x80)
    opcode = head[0] & 0x0f
    masked = bool(head[1] & 0x80)
    length = head[1] & 0x7f
    if length == 126:
        length = struct.unpack(">H", recv_exact(conn, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(conn, 8))[0]
    mask = recv_exact(conn, 4) if masked else b""
    payload = recv_exact(conn, length)
    if not fin or opcode != OPCODE or not masked or length != SIZE:
        return False
    return all(value ^ mask[i & 3] == PAYLOAD_BYTE[0]
               for i, value in enumerate(payload))


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(120)

try:
    conn, _ = srv.accept()
    conn.settimeout(30)
    _, key = read_http_upgrade(conn)
    if key is None:
        raise ConnectionError("missing WebSocket upgrade key")
    conn.sendall(make_101(key))

    frame = build_frame(OPCODE, PAYLOAD_BYTE * SIZE)
    valid = True
    for _ in range(COUNT):
        conn.sendall(frame)
        if not verify_echo(conn):
            valid = False
            break
    conn.sendall(build_close(1000 if valid else 1008))
except (BrokenPipeError, ConnectionResetError, ConnectionError, OSError, socket.timeout):
    pass
finally:
    try:
        srv.close()
    except OSError:
        pass
