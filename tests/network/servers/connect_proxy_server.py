"""Minimal HTTP CONNECT proxy for testing ws_client proxy support.

Accepts TCP connections, reads a CONNECT request, replies
"HTTP/1.1 200 Connection Established", then pipes bytes between the
client and the requested target (recorded in a log file).

Usage:
    python connect_proxy_server.py <listen_port> <log_file>
"""
import socket, sys, threading

def pump(src, dst):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        try:
            src.close()
        except OSError:
            pass
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass

def handle(conn, addr, log):
    upstream = None
    try:
        conn.settimeout(15)
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(4096)
            if not chunk:
                return
            data += chunk
        head = data.split(b"\r\n\r\n", 1)[0].decode("latin1")
        lines = head.split("\r\n")
        parts = lines[0].split(" ")
        if len(parts) < 2 or parts[0] != "CONNECT":
            conn.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
            return
        target = parts[1]
        if target.startswith("["):
            host, _, port_s = target[1:].split("]", 2)
            port_s = port_s.lstrip(":")
        else:
            host, _, port_s = target.rpartition(":")
        port = int(port_s or "80")
        with open(log, "a") as f:
            f.write(host + ":" + str(port) + "\n")

        upstream = socket.create_connection((host, port), timeout=15)
        conn.settimeout(None)
        upstream.settimeout(None)
        conn.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
        tail = data.split(b"\r\n\r\n", 1)[1]
        if tail:
            upstream.sendall(tail)

        t = threading.Thread(target=pump, args=(conn, upstream), daemon=True)
        t.start()
        pump(upstream, conn)
        t.join(timeout=5)
    except Exception:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass
        try:
            if upstream:
                upstream.close()
        except OSError:
            pass

def main():
    port = int(sys.argv[1])
    log = sys.argv[2]
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(50)
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle, args=(conn, addr, log), daemon=True).start()

if __name__ == "__main__":
    main()
