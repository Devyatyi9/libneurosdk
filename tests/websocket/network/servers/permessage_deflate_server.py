"""Raw RFC 7692 echo server used to verify client wire behavior."""
import socket
import sys
import zlib

from ws_frame import (OPCODE_BINARY, OPCODE_CLOSE, OPCODE_CONT, OPCODE_PING,
                      OPCODE_TEXT, build_frame, make_101, mark_server_ready,
                      read_client_frame, read_http_upgrade)


TRAILER = b"\x00\x00\xff\xff"
OFFER = (b"Sec-WebSocket-Extensions: permessage-deflate; "
         b"client_no_context_takeover; client_max_window_bits\r\n")


def compress_message(codec, payload):
    encoded = codec.compress(payload) + codec.flush(zlib.Z_SYNC_FLUSH)
    if not encoded.endswith(TRAILER):
        raise AssertionError("deflate stream has no sync-flush trailer")
    return encoded[:-4]


def run(port, mode):
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(4)
    mark_server_ready()
    for connection_index in range(4):
        conn, _ = listener.accept()
        with conn:
            request, key = read_http_upgrade(conn)
            if OFFER not in request:
                raise AssertionError("client did not send the exact RFC 7692 offer")
            if mode == "decline":
                conn.sendall(make_101(key))
            else:
                params = ("permessage-deflate; server_max_window_bits=12; "
                          "client_max_window_bits=9; "
                          "client_no_context_takeover; server_no_context_takeover")
                conn.sendall(make_101(key, params))

            fin, opcode, payload, rsv1 = read_client_frame(
                conn, allow_rsv1=True, include_rsv1=True)
            if not fin or opcode not in (OPCODE_TEXT, OPCODE_BINARY):
                raise AssertionError("unexpected client data frame")
            if mode == "decline":
                if rsv1:
                    raise AssertionError("client compressed after server decline")
                decoded = payload
                conn.sendall(build_frame(opcode, decoded))
            else:
                if not rsv1:
                    raise AssertionError("negotiated client frame has no RSV1")
                inflater = zlib.decompressobj(wbits=-15)
                decoded = inflater.decompress(payload + TRAILER)
                if inflater.unconsumed_tail:
                    raise AssertionError("client compressed payload was not consumed")
                deflater = zlib.compressobj(wbits=-15)
                encoded = compress_message(deflater, decoded)
                if connection_index == 0 and len(encoded) > 1:
                    midpoint = len(encoded) // 2
                    conn.sendall(build_frame(opcode, encoded[:midpoint], fin=False,
                                             rsv1=True))
                    conn.sendall(build_frame(OPCODE_PING, b"p"))
                    conn.sendall(build_frame(OPCODE_CONT, encoded[midpoint:]))
                    pong = read_client_frame(conn, allow_rsv1=True)
                    if pong[1] != 0xA or pong[2] != b"p":
                        raise AssertionError("client did not pong during fragments")
                elif connection_index == 2:
                    conn.sendall(build_frame(opcode, decoded))
                else:
                    conn.sendall(build_frame(opcode, encoded, rsv1=True))

            fin, opcode, _, rsv1 = read_client_frame(
                conn, allow_rsv1=True, include_rsv1=True)
            if not fin or opcode != OPCODE_CLOSE or rsv1:
                raise AssertionError("expected uncompressed client close")
            conn.sendall(build_frame(OPCODE_CLOSE, b"\x03\xe8"))
    listener.close()


if __name__ == "__main__":
    run(int(sys.argv[1]), sys.argv[2])
