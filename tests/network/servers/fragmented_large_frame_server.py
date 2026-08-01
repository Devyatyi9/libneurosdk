"""Send a large fragmented message and verify the client's echoed message.

Usage:
    python fragmented_large_frame_server.py <port> <size> <text|binary> <fragment_size>
"""
import os, socket, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, build_frame, make_101, read_client_frame, read_http_upgrade

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
SIZE = int(sys.argv[2])
KIND = sys.argv[3]
FRAGMENT_SIZE = int(sys.argv[4])
FRAGMENT_DELAY = float(sys.argv[5]) if len(sys.argv) > 5 else 0
PAYLOAD_BYTE = b"A" if KIND == "text" else b"\xaa"
OPCODE = 0x1 if KIND == "text" else 0x2


def verify_echo(conn):
    fin, opcode, payload = read_client_frame(conn)
    return fin and opcode == OPCODE and payload == PAYLOAD_BYTE * SIZE


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(120)

try:
    conn, _ = srv.accept()
    conn.settimeout(30)
    _, key = read_http_upgrade(conn)
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

    if not verify_echo(conn):
        raise ConnectionError("client echoed an invalid fragmented message")
    conn.sendall(build_close(1000))
    fin, opcode, payload = read_client_frame(conn)
    if not fin or opcode != 0x8 or payload != b"\x03\xe8":
        raise ConnectionError("expected clean Close response with code 1000")
    conn.close()
finally:
    srv.close()
