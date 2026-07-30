"""WS frame construction helpers for test servers (server → client, unmasked)."""
import struct

OPCODE_CONT = 0x0
OPCODE_TEXT = 0x1
OPCODE_BINARY = 0x2
OPCODE_CLOSE = 0x8
OPCODE_PING = 0x9
OPCODE_PONG = 0xA


def build_frame(opcode, payload=b"", fin=True):
    buf = bytearray()
    b0 = (0x80 if fin else 0) | (opcode & 0x0f)
    buf.append(b0)
    plen = len(payload)
    if plen < 126:
        buf.append(plen)
    elif plen < 65536:
        buf.append(126)
        buf.extend(struct.pack(">H", plen))
    else:
        buf.append(127)
        buf.extend(struct.pack(">Q", plen))
    buf.extend(payload)
    return bytes(buf)


def build_close(code=1000, reason=b""):
    payload = struct.pack(">H", code) + reason
    return build_frame(OPCODE_CLOSE, payload)


def build_text(text, fin=True):
    return build_frame(OPCODE_TEXT, text.encode("utf-8"), fin=fin)


def build_binary(data, fin=True):
    return build_frame(OPCODE_BINARY, data, fin=fin)


def build_ping(payload=b""):
    return build_frame(OPCODE_PING, payload)


def build_pong(payload=b""):
    return build_frame(OPCODE_PONG, payload)


def build_fragment(data, fin=True, binary=False):
    opcode = OPCODE_CONT
    return build_frame(opcode, data, fin=fin)


HTTP_101 = (
    b"HTTP/1.1 101 Switching Protocols\r\n"
    b"Upgrade: websocket\r\n"
    b"Connection: Upgrade\r\n"
    b"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
    b"\r\n"
)
