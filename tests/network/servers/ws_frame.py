"""WS frame construction + HTTP upgrade helpers for test servers."""
import struct, hashlib, base64

OPCODE_CONT = 0x0
OPCODE_TEXT = 0x1
OPCODE_BINARY = 0x2
OPCODE_CLOSE = 0x8
OPCODE_PING = 0x9
OPCODE_PONG = 0xA

MAGIC_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


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


def build_fragment(data, fin=True):
    return build_frame(OPCODE_CONT, data, fin=fin)


def compute_accept(client_key):
    """Compute Sec-WebSocket-Accept from the client's Sec-WebSocket-Key."""
    sha1 = hashlib.sha1(client_key.encode() + MAGIC_GUID).digest()
    return base64.b64encode(sha1).decode()


def make_101(client_key):
    """Build a full HTTP 101 upgrade response for a given client key."""
    accept = compute_accept(client_key)
    return (
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Accept: " + accept.encode() + b"\r\n"
        b"\r\n"
    )


def read_http_upgrade(conn):
    """Read until \r\n\r\n, return (full_data, key) or (None, None)."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None, None
        data += chunk
    # Extract Sec-WebSocket-Key
    key = None
    for line in data.split(b"\r\n"):
        if line.lower().startswith(b"sec-websocket-key:"):
            key = line.split(b":", 1)[1].strip().decode()
    return data, key


def recv_exact(conn, size):
    """Read exactly size bytes or fail on premature EOF."""
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed during WebSocket frame")
        data.extend(chunk)
    return bytes(data)


def read_client_frame(conn):
    """Read and unmask one complete client frame."""
    head = recv_exact(conn, 2)
    fin = bool(head[0] & 0x80)
    rsv = head[0] & 0x70
    opcode = head[0] & 0x0f
    masked = bool(head[1] & 0x80)
    length = head[1] & 0x7f
    if rsv:
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
    return fin, opcode, bytes(payload)
