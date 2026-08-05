"""Run the public libneurosdk client against the deterministic wire profile."""

import os
import socket
import subprocess
import sys
import time


def find_client():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    executable = "protocol_interop_client.exe" if os.name == "nt" else "protocol_interop_client"
    candidates = [
        os.path.join(root, "build", "Release", executable),
        os.path.join(root, "build", executable),
    ]
    configured = os.environ.get("PROTOCOL_INTEROP_CLIENT")
    if configured:
        candidates.insert(0, configured)
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    raise AssertionError("protocol_interop_client binary not found")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def test_protocol_wire_profile():
    port = free_port()
    server = subprocess.Popen(
        [sys.executable, os.path.join(os.path.dirname(__file__), "wire_server.py"), str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = server.stdout.readline()
            if line.strip() == "READY":
                break
            if server.poll() is not None:
                raise AssertionError(server.stderr.read())
        else:
            raise AssertionError("wire server did not become ready")

        client = subprocess.run(
            [find_client(), f"ws://127.0.0.1:{port}/"],
            capture_output=True,
            text=True,
            timeout=15,
        )
        assert client.returncode == 0, client.stderr
        _, server_error = server.communicate(timeout=10)
        assert server.returncode == 0, server_error
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=5)


if __name__ == "__main__":
    test_protocol_wire_profile()
