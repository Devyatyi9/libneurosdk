"""Network I/O edge case tests (platform-specific socket layer).

Tests #5-20 as defined in the test plan. Each test starts a Python
test server, runs a C test client against it, and checks exit codes.

Usage:
    python -m pytest tests/network/test_network.py -v
"""
import os, sys, subprocess, time, socket, platform

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tests"))
from test_utils import build_env, find_tool, find_echo_test

SERVERS_DIR = os.path.join(HERE, "servers")
HANDSHAKE_DIR = os.path.join(ROOT, "tests", "handshake")

import pytest


def _find_binary(name):
    return find_tool(name)


def _start_server(script, port, *args):
    cmd = [sys.executable, os.path.join(SERVERS_DIR, script), str(port)] + list(args)
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    return proc


def _wait_port(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.socket()
            s.settimeout(1)
            s.connect(("127.0.0.1", port))
            s.close()
            return True
        except (ConnectionRefusedError, OSError):
            time.sleep(0.2)
    return False


def _run_client(bin_path, url):
    return subprocess.call([bin_path, url], env=build_env(bin_path))


# ---- Sanity Checks (#5-#7) ----

ECHO_SERVER = os.path.join(ROOT, "tests", "integration", "echo_server.py")


def _find_uws():
    """Locate uws_echo binary — uws_echo_test.py copies it to project root."""
    exe = ".exe" if sys.platform == "win32" else ""
    candidates = [
        os.path.join(ROOT, "uws_echo" + exe),
        os.path.join(ROOT, "tests", "interop", "build", "Release", "uws_echo" + exe),
        os.path.join(ROOT, "tests", "interop", "build", "uws_echo" + exe),
    ]
    for p in candidates:
        if os.path.isfile(p):
            return p
    return None

def _start_uws(port):
    """Start uWS echo-server (required — no fallback)."""
    uws_bin = _find_uws()
    assert uws_bin, "uws_echo binary not found — build tests/interop first"
    proc = subprocess.Popen([uws_bin, str(port)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert _wait_port(port), f"uWS echo-server did not start on port {port}"
    sys.stderr.write(f"  server: uWS ({uws_bin})\n")
    return proc


def test_5_handshake_and_echo():
    """#5: Basic handshake + echo."""
    port = 19100
    proc = _start_uws(port)
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://127.0.0.1:{port}/")
    proc.terminate(); proc.wait()
    assert rc == 0, f"echo_test failed with exit code {rc}"


def test_6_binary_echo():
    """#6: Binary message echo — use Python echo_server for reliable large payloads."""
    port = 19101
    srv = subprocess.Popen([sys.executable, ECHO_SERVER, str(port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert _wait_port(port), "echo_server.py did not start"
    int_bin = _find_binary("integration_test")
    assert int_bin, "integration_test binary not built"
    rc = _run_client(int_bin, f"ws://127.0.0.1:{port}/")
    srv.terminate(); srv.wait()
    assert rc == 0, f"integration_test failed with exit code {rc}"


def test_7_normal_close():
    """#7: Normal closure (1000) — echo_test already covers close."""
    port = 19102
    proc = _start_uws(port)
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://127.0.0.1:{port}/")
    proc.terminate(); proc.wait()
    assert rc == 0, f"echo_test normal close failed (exit {rc})"


# ---- Network I/O Edge Cases (#8-#20) ----

def test_8_drip_feed():
    """#8: Drip-feed recv — server sends WS frame 1 byte at a time."""
    port = 19113
    srv = _start_server("drip_feed_server.py", port)
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://127.0.0.1:{port}/")
    srv.terminate(); srv.wait()
    assert rc == 0, f"drip_feed test failed (exit {rc})"


def test_9_partial_send():
    """#9: Partial send — send 10MB with slow consumer."""
    port = 19103
    srv = _start_server("slow_consumer_server.py", port)
    client_bin = _find_binary("test_partial_send")
    assert client_bin, "test_partial_send binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/")
    srv.terminate(); srv.wait()
    assert rc == 0, f"partial_send test failed (exit {rc})"


def test_10_connection_refused():
    """#10: Connection refused — connect to closed port."""
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, "ws://127.0.0.1:19999/")
    assert rc != 0, "expected connection refused error"


def test_11_blackhole():
    """#11: Blackhole timeout — server accepts but never sends data."""
    port = 19104
    listen_script = os.path.join(HANDSHAKE_DIR, "listen_only_server.py")
    assert os.path.exists(listen_script), "listen_only_server.py not found"
    srv = subprocess.Popen([sys.executable, listen_script, str(port), "10"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://127.0.0.1:{port}/")
    srv.terminate(); srv.wait()
    assert rc != 0, "expected blackhole timeout error"


def test_12_tcp_rst():
    """#12: TCP RST — server sends RST after partial handshake."""
    port = 19105
    srv = _start_server("tcp_rst_server.py", port)
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://127.0.0.1:{port}/")
    srv.terminate(); srv.wait()
    assert rc != 0, "expected TCP RST error"


def test_13_tcp_fin():
    """#13: TCP FIN without WS Close — server closes TCP gracefully."""
    port = 19106
    srv = _start_server("tcp_fin_server.py", port)
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://127.0.0.1:{port}/")
    srv.terminate(); srv.wait()
    assert rc != 0, "expected TCP FIN error (no WS close)"


def test_14_bad_handshake():
    """#14: Bad HTTP response (404) — reuse bad_handshake_server."""
    port = 19107
    bh_script = os.path.join(HANDSHAKE_DIR, "bad_handshake_server.py")
    assert os.path.exists(bh_script), "bad_handshake_server.py not found"
    srv = subprocess.Popen(
        [sys.executable, bh_script, "404", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://127.0.0.1:{port}/")
    srv.terminate(); srv.wait()
    assert rc != 0, "expected bad handshake error (404)"


def test_15_ipv4():
    """#15a: IPv4 connectivity."""
    port = 19108
    proc = _start_uws(port)
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://127.0.0.1:{port}/")
    proc.terminate(); proc.wait()
    assert rc == 0, f"IPv4 echo failed (exit {rc})"

def test_15_ipv6():
    """#15b: IPv6 connectivity — connect to loopback [::1]."""
    port = 19109
    proc = _start_uws(port)
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://[::1]:{port}/")
    proc.terminate(); proc.wait()
    assert rc == 0, f"IPv6 loopback connect failed (exit {rc})"


def test_16_flood():
    """#16: Flood — 1000 frames sent rapidly."""
    port = 19109
    srv = _start_server("flood_server.py", port, "1000")
    client_bin = _find_binary("test_flood")
    assert client_bin, "test_flood binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/")
    srv.terminate(); srv.wait()
    assert rc == 0, f"flood test failed (exit {rc})"


def test_17_double_close():
    """#17: Double close — client calls ws_close() twice."""
    port = 19110
    proc = _start_uws(port)
    client_bin = _find_binary("test_double_close")
    assert client_bin, "test_double_close binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/")
    proc.terminate(); proc.wait()
    assert rc == 0, f"double_close test failed (exit {rc})"


def test_18_ping_interleave():
    """#18: Ping interleaving — pings between fragments."""
    port = 19111
    srv = _start_server("ping_interleave_server.py", port)
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://127.0.0.1:{port}/")
    srv.terminate(); srv.wait()
    assert rc == 0, f"ping_interleave test failed (exit {rc})"


def test_19_dns_failure():
    """#19: DNS resolution failure — connect to nonexistent host."""
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, "ws://this-domain-does-not-exist-xyz.internal/")
    assert rc != 0, "expected DNS failure error"


def test_20_oversize():
    """#20: Oversize frame — server sends >262144 byte payload."""
    port = 19112
    srv = _start_server("oversize_server.py", port)
    client_bin = _find_binary("test_oversize")
    assert client_bin, "test_oversize binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/")
    srv.terminate(); srv.wait()
    assert rc == 0, f"oversize test failed (exit {rc})"


def test_21_dual_stack_fallback():
    """#21: Dual-stack fallback — 'localhost' resolves to ::1 first, but
    the server listens on IPv4 only. Client must fall back to 127.0.0.1.

    echo_server.py binds 0.0.0.0 (IPv4-only), so the ::1 attempt is
    refused and ws_client must try the next resolved address.
    """
    port = 19113
    srv = subprocess.Popen([sys.executable, ECHO_SERVER, str(port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert _wait_port(port), "echo_server.py did not start"
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://localhost:{port}/echo")
    srv.terminate(); srv.wait()
    assert rc == 0, f"dual-stack fallback failed (exit {rc})"
