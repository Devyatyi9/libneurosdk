"""TCP server that sends HTTP 101 then closes TCP without WS Close frame.

Tests #13: TCP FIN without WS Close — client must detect EOF and
trigger on_close(code=1006) or on_error().
"""
import socket, time, sys

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19010

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(1)
srv.settimeout(30)

try:
    conn, addr = srv.accept()
    conn.settimeout(5)
    # Read HTTP upgrade
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk
    # Send full 101
    resp = (
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        b"\r\n"
    )
    conn.sendall(resp)
    time.sleep(0.5)
    # Graceful TCP close without sending WS Close frame
    conn.shutdown(socket.SHUT_WR)
    time.sleep(0.5)
    conn.close()
except socket.timeout:
    pass
finally:
    srv.close()
