"""Send a large fragmented message and verify the client's echoed message.

Usage:
    python fragmented_large_frame_server.py <port> <size> <text|binary> <fragment_size>
"""
import os, socket, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, build_frame, make_101, read_http_upgrade

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
SIZE = int(sys.argv[2])
KIND = sys.argv[3]
FRAGMENT_SIZE = int(sys.argv[4])
FRAGMENT_DELAY = float(sys.argv[5]) if len(sys.argv) > 5 else 0
PAYLOAD_BYTE = b"A" if KIND == "text" else b"\xaa"
OPCODE = 0x1 if KIND == "text" else 0x2


def recv_exact(conn, size):
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed while reading frame")
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

    valid = fin and opcode == OPCODE and masked and length == SIZE
    offset = 0
    remaining = length
    while remaining:
        chunk = recv_exact(conn, min(65536, remaining))
        if valid:
            for i, value in enumerate(chunk):
                if value ^ mask[(offset + i) & 3] != PAYLOAD_BYTE[0]:
                    valid = False
                    break
        offset += len(chunk)
        remaining -= len(chunk)
    return valid


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

    payload = PAYLOAD_BYTE * FRAGMENT_SIZE
    batch = bytearray()
    offset = 0
    first = True
    while offset < SIZE:
        length = min(FRAGMENT_SIZE, SIZE - offset)
        fin = offset + length == SIZE
        opcode = OPCODE if first else 0x0
        batch.extend(build_frame(opcode, payload[:length], fin=fin))
        if len(batch) >= 65536 or fin or FRAGMENT_DELAY:
            conn.sendall(batch)
            batch.clear()
            if FRAGMENT_DELAY and not fin:
                time.sleep(FRAGMENT_DELAY)
        first = False
        offset += length

    conn.sendall(build_close(1000 if verify_echo(conn) else 1008))
except (BrokenPipeError, ConnectionResetError, ConnectionError, OSError, socket.timeout):
    pass
finally:
    try:
        srv.close()
    except OSError:
        pass
