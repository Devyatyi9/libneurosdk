"""Initiate a clean WebSocket close and validate the client's response."""
import os
import socket
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import build_close, make_101, mark_server_ready, read_http_upgrade


def recv_exact(conn, size):
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed during Close response")
        data.extend(chunk)
    return bytes(data)


def read_close(conn):
    head = recv_exact(conn, 2)
    if head[0] != 0x88 or not (head[1] & 0x80):
        raise ConnectionError("client did not send a final masked Close frame")
    length = head[1] & 0x7f
    if length > 125:
        raise ConnectionError("invalid Close payload length")
    mask = recv_exact(conn, 4)
    payload = recv_exact(conn, length)
    return bytes(value ^ mask[i & 3] for i, value in enumerate(payload))


port = int(sys.argv[1])
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(1)
mark_server_ready()

try:
    conn, _ = srv.accept()
    with conn:
        conn.settimeout(5)
        _, key = read_http_upgrade(conn)
        if key is None:
            raise ConnectionError("missing WebSocket upgrade key")
        conn.sendall(make_101(key))
        conn.sendall(build_close(1000, b"bye"))
        if read_close(conn) != struct.pack(">H", 1000) + b"bye":
            raise ConnectionError("client did not echo the Close payload")
finally:
    srv.close()
