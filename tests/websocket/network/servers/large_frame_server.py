"""TCP server that sends one large WS frame (single frame, no fragmentation).

Mirrors Autobahn group 9.1/9.2: the server pushes a single frame whose
payload is far larger than the client's 256 KiB recv buffer, so the
client must stream the payload across many reads.

Usage:
    python large_frame_server.py <port> <size_bytes> [text|binary] [chop_bytes]
"""
import os, socket, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, make_101, mark_server_ready, read_client_frame, read_http_upgrade

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
SIZE = int(sys.argv[2])
KIND = sys.argv[3] if len(sys.argv) > 3 else "text"
CHOP = int(sys.argv[4]) if len(sys.argv) > 4 else 1024 * 1024

payload_byte = b"A" if KIND == "text" else b"\xaa"
if KIND == "text":
    opcode = 0x1
else:
    opcode = 0x2

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
mark_server_ready()
srv.settimeout(120)

try:
    conn, _ = srv.accept()
    conn.settimeout(30)
    _, key = read_http_upgrade(conn)
    conn.sendall(make_101(key))

    # Build the large frame header (FIN + opcode, 8-byte length).
    head = bytearray()
    head.append(0x80 | opcode)
    head.append(127)
    head.extend(struct.pack(">Q", SIZE))

    conn.sendall(bytes(head))
    if SIZE >= 1 << 63:
        fin, received_opcode, payload = read_client_frame(conn)
        if not fin or received_opcode != 0x8 or payload != b"\x03\xea":
            raise ConnectionError("expected protocol Close response with code 1002")
        conn.close()
        sys.exit(0)

    chunk = payload_byte * min(CHOP, SIZE)
    chunk_view = memoryview(chunk)
    off = 0
    while off < SIZE:
        n = min(CHOP, SIZE - off)
        conn.sendall(chunk_view[:n])
        off += n
        time.sleep(0.02)

    conn.sendall(build_close(1000))
    fin, received_opcode, payload = read_client_frame(conn)
    if not fin or received_opcode != 0x8 or payload != b"\x03\xe8":
        raise ConnectionError("expected clean Close response with code 1000")
    conn.close()
finally:
    srv.close()
