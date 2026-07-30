"""TCP server that sends a WS frame exceeding the client's max payload buffer.

Tests #20: Max Payload Size — client must reject oversized frame
(code 1009) rather than overflow the receive buffer.
The client's recv buffer is WS_RECV_BUF_SIZE = 262144 bytes.
We send a frame larger than that.
"""
import socket, struct, time, sys

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19008
OVERSIZE = 300000  # > WS_RECV_BUF_SIZE (262144)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(30)

try:
    conn, addr = srv.accept()
    conn.settimeout(10)
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk
    resp = (
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        b"\r\n"
    )
    conn.sendall(resp)
    time.sleep(0.1)
    # Build oversized frame (FIN + text, no mask since server->client)
    frame = bytearray()
    frame.append(0x81)  # FIN + text
    if OVERSIZE < 65536:
        frame.append(126)
        frame.extend(struct.pack(">H", OVERSIZE))
    else:
        frame.append(127)
        frame.extend(struct.pack(">Q", OVERSIZE))
    # Send the header but NOT the full payload — just enough to trigger detection
    conn.sendall(bytes(frame))
    # Send a chunk of payload, then stop — client should reject before reading all
    chunk_size = min(OVERSIZE, 300000)
    conn.sendall(b"X" * chunk_size)
    time.sleep(2)
    conn.close()
except socket.timeout:
    pass
except BrokenPipeError:
    pass
finally:
    srv.close()
