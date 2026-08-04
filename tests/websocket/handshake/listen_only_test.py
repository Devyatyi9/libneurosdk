#!/usr/bin/env python3
"""Tests ws_client when server accepts TCP but never responds.

Starts listen_only_server.py, runs echo_test against it.
Expected: echo_test times out (exit != 0).
Verifies the client doesn't hang forever on stalled recv().
"""
import subprocess, sys, os, time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from test_utils import build_env, find_echo_test, stop_process

def main():
    echo_test_bin = sys.argv[1] if len(sys.argv) > 1 else find_echo_test()
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 19003

    server_script = os.path.join(os.path.dirname(__file__), "listen_only_server.py")
    server = subprocess.Popen([sys.executable, server_script, str(port), "10"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)

    url = f"ws://127.0.0.1:{port}/"
    try:
        rc = subprocess.run(
            [echo_test_bin, url], env=build_env(echo_test_bin), timeout=30,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
    except subprocess.TimeoutExpired:
        print("Stalled TCP test FAILED (echo_test exceeded 30s)")
        return 1
    finally:
        stop_process(server)

    if rc == 0:
        print("UNEXPECTED SUCCESS: echo_test connected to listen-only server")
        sys.exit(1)

    print(f"Stalled TCP test: OK (expected failure, exit={rc})")

if __name__ == '__main__':
    main()
