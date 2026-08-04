"""Send one invalid RFC 7692 frame and verify the client's Close code."""
import socket
import struct
import sys
import zlib

from ws_frame import (OPCODE_CONT, OPCODE_PING, OPCODE_TEXT, build_frame, make_101,
                      mark_server_ready, read_client_frame, read_http_upgrade)


def run(port, mode):
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(1)
    mark_server_ready()
    conn, _ = listener.accept()
    with conn:
        request, key = read_http_upgrade(conn)
        offered = b"Sec-WebSocket-Extensions: permessage-deflate; " in request
        if not offered:
            raise AssertionError("missing permessage-deflate offer")
        negotiated = mode != "unnegotiated"
        conn.sendall(make_101(key, "permessage-deflate") if negotiated else make_101(key))
        if mode == "malformed":
            conn.sendall(build_frame(OPCODE_TEXT, b"\xff\xff\xff", rsv1=True))
            expected_code = 1002
        elif mode == "limit":
            compressor = zlib.compressobj(wbits=-15)
            wire = compressor.compress(b"a" * (16 * 1024 * 1024 + 1))
            wire += compressor.flush(zlib.Z_SYNC_FLUSH)
            conn.sendall(build_frame(OPCODE_TEXT, wire[:-4], rsv1=True))
            expected_code = 1009
        elif mode == "unnegotiated":
            conn.sendall(build_frame(OPCODE_TEXT, b"invalid", rsv1=True))
            expected_code = 1002
        elif mode == "control-rsv1":
            conn.sendall(build_frame(OPCODE_PING, b"invalid", rsv1=True))
            expected_code = 1002
        elif mode == "continuation-rsv1":
            conn.sendall(build_frame(OPCODE_TEXT, b"", fin=False, rsv1=True))
            conn.sendall(build_frame(OPCODE_CONT, b"", rsv1=True))
            expected_code = 1002
        else:
            raise AssertionError(f"unknown mode: {mode}")
        _, opcode, payload = read_client_frame(conn, allow_rsv1=True)
        if opcode != 0x8 or len(payload) < 2:
            raise AssertionError("client did not send Close")
        code = struct.unpack(">H", payload[:2])[0]
        if code != expected_code:
            raise AssertionError(f"expected Close {expected_code}, got {code}")
    listener.close()


if __name__ == "__main__":
    run(int(sys.argv[1]), sys.argv[2])
