#!/usr/bin/env python3
"""TCP server that binds+listens but never accept()s.

NOTE: TCP connect() completes at kernel level (SYN/ACK handshake
goes to the listen backlog) regardless of accept(). So this tests:
  "TCP connected, server silent -> sock_recv returns EWOULDBLOCK"
NOT:
  "connect() still in progress -> ws_poll returns WS_EVENT_NONE"

For the latter, use an unroutable IP like 192.0.2.1 (TEST-NET-1).

Usage: python listen_only_server.py [port] [duration_sec]
"""
import socket, sys, time
from server_ready import mark_server_ready

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9999
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 10

    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', port))
    s.listen(1)
    mark_server_ready()
    # Don't accept() -- kernel completes TCP handshake, client's
    # send/recv will stall on actual data transfer.
    time.sleep(duration)
    s.close()

if __name__ == '__main__':
    main()
