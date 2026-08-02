"""Send a compressed frame whose wire payload exceeds the fixed receive buffer."""
import socket
import sys
import zlib

from ws_frame import (OPCODE_BINARY, OPCODE_CLOSE, build_frame, make_101,
                      mark_server_ready, read_client_frame, read_http_upgrade)


TRAILER = b"\x00\x00\xff\xff"
PAYLOAD_SIZE = 512 * 1024


def run(port):
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(1)
    mark_server_ready()
    conn, _ = listener.accept()
    with conn:
        request, key = read_http_upgrade(conn)
        if b"Sec-WebSocket-Extensions: permessage-deflate\r\n" not in request:
            raise AssertionError("client did not offer permessage-deflate")
        conn.sendall(make_101(key, "permessage-deflate"))

        compressor = zlib.compressobj(level=0, wbits=-15)
        encoded = compressor.compress(b"\xaa" * PAYLOAD_SIZE)
        encoded += compressor.flush(zlib.Z_SYNC_FLUSH)
        if not encoded.endswith(TRAILER):
            raise AssertionError("deflate stream has no sync-flush trailer")
        encoded = encoded[:-4]
        if len(encoded) <= 262144:
            raise AssertionError("compressed fixture does not exceed receive buffer")
        conn.sendall(build_frame(OPCODE_BINARY, encoded, rsv1=True))
        conn.sendall(build_frame(OPCODE_CLOSE, b"\x03\xe8"))
        fin, opcode, _, rsv1 = read_client_frame(
            conn, allow_rsv1=True, include_rsv1=True)
        if not fin or opcode != OPCODE_CLOSE or rsv1:
            raise AssertionError("expected uncompressed client close")
    listener.close()


if __name__ == "__main__":
    run(int(sys.argv[1]))
