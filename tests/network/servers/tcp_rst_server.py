"""TCP server that sends RST after accepting connection.

Tests #12: TCP RST — client must detect abrupt connection reset.
Sets SO_LINGER to 0 so close() sends RST instead of FIN.
"""
import socket, struct, sys, time

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19009

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
    # Send partial 101 then RST
    resp = (
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
    )
    try:
        conn.sendall(resp)
    except OSError:
        pass
    # Set SO_LINGER=0 to force RST on close
    l_onoff = 1
    l_linger = 0
    conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                    struct.pack('ii', l_onoff, l_linger))
    time.sleep(0.1)
    conn.close()
except socket.timeout:
    pass
except ConnectionResetError:
    pass
finally:
    srv.close()
