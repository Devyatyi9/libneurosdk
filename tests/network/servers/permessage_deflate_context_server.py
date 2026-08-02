"""Verify persistent and no-context-takeover in both RFC 7692 directions."""
import socket
import sys
import zlib

from ws_frame import OPCODE_CLOSE, OPCODE_TEXT, build_frame, make_101, mark_server_ready
from ws_frame import read_client_frame, read_http_upgrade


TRAILER = b"\x00\x00\xff\xff"


def encode(codec, payload):
    wire = codec.compress(payload) + codec.flush(zlib.Z_SYNC_FLUSH)
    return wire[:-4]


def run(port, no_context):
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(1)
    mark_server_ready()
    conn, _ = listener.accept()
    with conn:
        request, key = read_http_upgrade(conn)
        if b"Sec-WebSocket-Extensions: permessage-deflate\r\n" not in request:
            raise AssertionError("missing permessage-deflate offer")
        parameters = "permessage-deflate"
        if no_context:
            parameters += "; client_no_context_takeover; server_no_context_takeover"
        conn.sendall(make_101(key, parameters))
        inflater = zlib.decompressobj(wbits=-15)
        deflater = zlib.compressobj(wbits=-15)
        for _ in range(2):
            _, opcode, wire, rsv1 = read_client_frame(
                conn, allow_rsv1=True, include_rsv1=True)
            if opcode != OPCODE_TEXT or not rsv1:
                raise AssertionError("expected compressed text")
            payload = inflater.decompress(wire + TRAILER)
            if no_context:
                inflater = zlib.decompressobj(wbits=-15)
            conn.sendall(build_frame(OPCODE_TEXT, encode(deflater, payload), rsv1=True))
            if no_context:
                deflater = zlib.compressobj(wbits=-15)
        _, opcode, _ = read_client_frame(conn, allow_rsv1=True)
        if opcode != OPCODE_CLOSE:
            raise AssertionError("expected Close")
        conn.sendall(build_frame(OPCODE_CLOSE, b"\x03\xe8"))
    listener.close()


if __name__ == "__main__":
    run(int(sys.argv[1]), sys.argv[2] == "no-context")
