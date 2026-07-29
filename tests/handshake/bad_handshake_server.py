#!/usr/bin/env python3
"""TCP server that responds with deliberately malformed HTTP to test
the WS client's parse_upgrade_response() error handling.

Usage:
    python bad_handshake_server.py <mode> [port]

Modes:
  200       HTTP/1.1 200 OK (not 101)
  404       HTTP/1.1 404 Not Found
  301       HTTP/1.1 301 Moved (redirect, client must not follow)
  no_upgrade  101 without Upgrade: websocket
  no_conn     101 without Connection: Upgrade
  bad_accept  101 with wrong Sec-WebSocket-Accept
  no_accept   101 without Sec-WebSocket-Accept
"""
import socket, sys

RESPONSES = {
    # (status_line, headers)
    "200":     (b"HTTP/1.1 200 OK\r\n", b"Content-Length: 0\r\n\r\n"),
    "404":     (b"HTTP/1.1 404 Not Found\r\n", b"Content-Length: 0\r\n\r\n"),
    "301":     (b"HTTP/1.1 301 Moved\r\n", b"Location: ws://other/\r\n\r\n"),
    "no_upgrade": (b"HTTP/1.1 101 Switching Protocols\r\n",
                   b"Connection: Upgrade\r\n"
                   b"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
    "no_conn":    (b"HTTP/1.1 101 Switching Protocols\r\n",
                   b"Upgrade: websocket\r\n"
                   b"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
    "bad_accept": (b"HTTP/1.1 101 Switching Protocols\r\n",
                   b"Upgrade: websocket\r\n"
                   b"Connection: Upgrade\r\n"
                   b"Sec-WebSocket-Accept: zzz\r\n\r\n"),
    "no_accept":  (b"HTTP/1.1 101 Switching Protocols\r\n",
                   b"Upgrade: websocket\r\n"
                   b"Connection: Upgrade\r\n\r\n"),
}

def handle(conn, status_line, headers):
    # Drain the incoming HTTP request
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk: break
        data += chunk
    conn.sendall(status_line + headers)
    conn.close()

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "200"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9998
    if mode not in RESPONSES:
        print(f"Unknown mode: {mode}. Available: {', '.join(RESPONSES.keys())}")
        sys.exit(1)

    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', port))
    s.listen(5)
    print(f"bad_handshake_server [{mode}] on port {port}")
    status_line, headers = RESPONSES[mode]
    try:
        while True:
            conn, addr = s.accept()
            handle(conn, status_line, headers)
    except KeyboardInterrupt:
        pass
    finally:
        s.close()

if __name__ == '__main__':
    main()
