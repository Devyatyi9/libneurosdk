"""WS frame construction + HTTP upgrade helpers for test servers."""
import base64
import hashlib
import os
import struct

OPCODE_CONT = 0x0
OPCODE_TEXT = 0x1
OPCODE_BINARY = 0x2
OPCODE_CLOSE = 0x8
OPCODE_PING = 0x9
OPCODE_PONG = 0xA

MAGIC_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def mark_server_ready():
    """Signal that a one-shot server is listening without connecting to it."""
    path = os.environ.get("WS_TEST_READY_FILE")
    if path:
        temporary = path + ".tmp"
        with open(temporary, "x", encoding="ascii"):
            pass
        os.replace(temporary, path)


def build_frame(opcode, payload=b"", fin=True, rsv1=False):
    buf = bytearray()
    b0 = (0x80 if fin else 0) | (0x40 if rsv1 else 0) | (opcode & 0x0f)
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


def build_fragment(data, fin=True):
    return build_frame(OPCODE_CONT, data, fin=fin)


def compute_accept(client_key):
    """Compute Sec-WebSocket-Accept from the client's Sec-WebSocket-Key."""
    sha1 = hashlib.sha1(client_key.encode() + MAGIC_GUID).digest()
    return base64.b64encode(sha1).decode()


def make_101(client_key, extensions=None):
    """Build a full HTTP 101 upgrade response for a given client key."""
    accept = compute_accept(client_key)
    extension_header = (
        b"Sec-WebSocket-Extensions: " + extensions.encode("ascii") + b"\r\n"
        if extensions else b""
    )
    return (
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Accept: " + accept.encode() + b"\r\n" + extension_header
        + b"\r\n"
    )


def read_http_upgrade(conn):
    """Read and validate a complete RFC 6455 client upgrade request."""
    max_header_size = 16 * 1024
    data = bytearray()
    while b"\r\n\r\n" not in data:
        if len(data) >= max_header_size:
            raise ConnectionError("HTTP Upgrade request exceeds 16 KiB")
        chunk = conn.recv(min(4096, max_header_size - len(data)))
        if not chunk:
            raise ConnectionError("connection closed during HTTP Upgrade request")
        data.extend(chunk)

    header_end = data.index(b"\r\n\r\n") + 4
    header = bytes(data[:header_end])
    lines = header[:-4].split(b"\r\n")
    if not lines or len(lines[0].split(b" ")) != 3:
        raise ConnectionError("malformed HTTP request line")
    method, target, version = lines[0].split(b" ")
    if method != b"GET" or not target.startswith(b"/") or version != b"HTTP/1.1":
        raise ConnectionError("invalid WebSocket HTTP request line")

    headers = {}
    for line in lines[1:]:
        if b":" not in line:
            raise ConnectionError("malformed HTTP header")
        name, value = line.split(b":", 1)
        name = name.strip().lower()
        if not name or name in headers:
            raise ConnectionError("missing or duplicate HTTP header name")
        headers[name] = value.strip()

    if not headers.get(b"host"):
        raise ConnectionError("missing Host header")
    if headers.get(b"upgrade", b"").lower() != b"websocket":
        raise ConnectionError("missing Upgrade: websocket header")
    connection_tokens = {
        token.strip().lower() for token in headers.get(b"connection", b"").split(b",")
    }
    if b"upgrade" not in connection_tokens:
        raise ConnectionError("missing Connection: Upgrade token")
    if headers.get(b"sec-websocket-version") != b"13":
        raise ConnectionError("missing Sec-WebSocket-Version: 13 header")

    key_bytes = headers.get(b"sec-websocket-key")
    if key_bytes is None:
        raise ConnectionError("missing Sec-WebSocket-Key header")
    try:
        decoded_key = base64.b64decode(key_bytes, validate=True)
        key = key_bytes.decode("ascii")
    except (ValueError, UnicodeDecodeError) as exc:
        raise ConnectionError("invalid Sec-WebSocket-Key header") from exc
    if len(decoded_key) != 16:
        raise ConnectionError("Sec-WebSocket-Key must decode to 16 bytes")

    return bytes(data), key


def recv_exact(conn, size):
    """Read exactly size bytes or fail on premature EOF."""
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed during WebSocket frame")
        data.extend(chunk)
    return bytes(data)


def read_client_frame(conn, allow_rsv1=False, include_rsv1=False):
    """Read and unmask one complete client frame."""
    head = recv_exact(conn, 2)
    fin = bool(head[0] & 0x80)
    rsv = head[0] & 0x70
    opcode = head[0] & 0x0f
    masked = bool(head[1] & 0x80)
    length = head[1] & 0x7f
    if rsv & 0x30 or (rsv & 0x40 and not allow_rsv1):
        raise ConnectionError("client frame has non-zero RSV bits")
    if not masked:
        raise ConnectionError("client frame is not masked")
    if length == 126:
        length = struct.unpack(">H", recv_exact(conn, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(conn, 8))[0]
        if length >> 63:
            raise ConnectionError("client frame has invalid 64-bit length")
    mask = recv_exact(conn, 4)
    payload = bytearray(recv_exact(conn, length))
    for i in range(length):
        payload[i] ^= mask[i & 3]
    result = (fin, opcode, bytes(payload))
    if include_rsv1:
        return result + (bool(rsv & 0x40),)
    return result
