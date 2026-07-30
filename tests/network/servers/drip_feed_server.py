"""TCP server that sends WS frames one byte at a time with delay.

Tests #8: partial recv() — client must reassemble bytes into complete frames.
"""
import os, socket, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_frame import read_http_upgrade, make_101

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19004

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(120)

try:
    conn, addr = srv.accept()
    conn.settimeout(10)
    data, key = read_http_upgrade(conn)
    if key is None:
        conn.close(); srv.close(); sys.exit(0)
    conn.sendall(make_101(key))
    # Read the masked client frame
    header = b""
    while len(header) < 2:
        b = conn.recv(1)
        if not b:
            break
        header += b
        time.sleep(0.01)
    if len(header) < 2:
        conn.close()
        sys.exit(0)
    b1 = header[1]
    mask = (b1 & 0x80) != 0
    length = b1 & 0x7f
    if length == 126:
        ext = b""
        while len(ext) < 2:
            b = conn.recv(1)
            if not b:
                break
            ext += b
            time.sleep(0.01)
        length = struct.unpack(">H", ext)[0]
    elif length == 127:
        ext = b""
        while len(ext) < 8:
            b = conn.recv(1)
            if not b:
                break
            ext += b
            time.sleep(0.01)
        length = struct.unpack(">Q", ext)[0]
    if mask:
        mask_key = b""
        while len(mask_key) < 4:
            b = conn.recv(1)
            if not b:
                break
            mask_key += b
            time.sleep(0.01)
    payload = b""
    while len(payload) < length:
        b = conn.recv(1)
        if not b:
            break
        payload += b
        time.sleep(0.01)
    if mask:
        payload = bytes(payload[i] ^ mask_key[i % 4] for i in range(len(payload)))
    # Build echo response frame (unmasked)
    resp_frame = bytearray()
    resp_frame.append(0x81)  # FIN + text
    if length < 126:
        resp_frame.append(length)
    elif length < 65536:
        resp_frame.append(126)
        resp_frame.extend(struct.pack(">H", length))
    else:
        resp_frame.append(127)
        resp_frame.extend(struct.pack(">Q", length))
    # Send response one byte at a time
    for b in resp_frame:
        conn.sendall(bytes([b]))
        time.sleep(0.01)
    for b in payload:
        conn.sendall(bytes([b]))
        time.sleep(0.01)
    time.sleep(1)
    conn.close()
except socket.timeout:
    pass
finally:
    srv.close()
