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
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            text=True)
    time.sleep(2)
    if proc.poll() is not None:
        stderr = proc.stderr.read()
        pytest.fail(f"{script} exited during startup ({proc.returncode}):\n{stderr}")
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


def _run_client(bin_path, url, *extra_args):
    try:
        return subprocess.run([bin_path, url] + list(extra_args),
                              env=build_env(bin_path), timeout=120).returncode
    except subprocess.TimeoutExpired:
        pytest.fail(f"client timed out after 120s: {os.path.basename(bin_path)}")


def _wait_server(proc, name, timeout=10):
    try:
        _, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.terminate()
        _, stderr = proc.communicate(timeout=5)
        pytest.fail(f"{name} did not exit after the client finished:\n{stderr}")
    assert proc.returncode == 0, f"{name} failed (exit {proc.returncode}):\n{stderr}"


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
    _wait_server(srv, "slow_consumer_server.py")
    assert rc == 0, f"partial_send test failed (exit {rc})"


def test_10_connection_refused():
    """#10: Connection refused — connect to closed port."""
    client_bin = _find_binary("test_negative")
    assert client_bin, "test_negative binary not built"
    rc = _run_client(client_bin, "ws://127.0.0.1:19999/",
                     "error-before-open", "TCP connect failed")
    assert rc == 0, "connection refusal did not produce the expected error"


def test_11_blackhole():
    """#11: Blackhole timeout — server accepts but never sends data."""
    port = 19104
    listen_script = os.path.join(HANDSHAKE_DIR, "listen_only_server.py")
    assert os.path.exists(listen_script), "listen_only_server.py not found"
    srv = subprocess.Popen([sys.executable, listen_script, str(port), "10"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    client_bin = _find_binary("test_negative")
    assert client_bin, "test_negative binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/",
                     "timeout-before-open")
    srv.terminate(); srv.wait()
    assert rc == 0, "blackhole did not remain pending until the expected timeout"


def test_12_tcp_rst():
    """#12: TCP RST — server sends RST after partial handshake."""
    port = 19105
    srv = _start_server("tcp_rst_server.py", port)
    client_bin = _find_binary("test_negative")
    assert client_bin, "test_negative binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/",
                     "error-after-open", "recv failed")
    _wait_server(srv, "tcp_rst_server.py")
    assert rc == 0, "TCP RST did not produce an error after on_open"


def test_13_tcp_fin():
    """#13: TCP FIN without WS Close — server closes TCP gracefully."""
    port = 19106
    srv = _start_server("tcp_fin_server.py", port)
    client_bin = _find_binary("test_negative")
    assert client_bin, "test_negative binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/",
                     "close-after-open", "1006")
    _wait_server(srv, "tcp_fin_server.py")
    assert rc == 0, "TCP FIN without Close did not produce close code 1006"


def test_14_bad_handshake():
    """#14: Bad HTTP response (404) — reuse bad_handshake_server."""
    port = 19107
    bh_script = os.path.join(HANDSHAKE_DIR, "bad_handshake_server.py")
    assert os.path.exists(bh_script), "bad_handshake_server.py not found"
    srv = subprocess.Popen(
        [sys.executable, bh_script, "404", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    client_bin = _find_binary("test_negative")
    assert client_bin, "test_negative binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/",
                     "error-before-open", "bad HTTP response during upgrade")
    srv.terminate(); srv.wait()
    assert rc == 0, "HTTP 404 did not produce the expected handshake error"


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
    port = 19120
    srv = _start_server("flood_server.py", port, "1000")
    client_bin = _find_binary("test_flood")
    assert client_bin, "test_flood binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/")
    _wait_server(srv, "flood_server.py")
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
    expected = "Hello, this is the first part and this is the second part!"
    rc = _run_client(echo_bin, f"ws://127.0.0.1:{port}/", expected)
    _wait_server(srv, "ping_interleave_server.py")
    assert rc == 0, f"ping_interleave test failed (exit {rc})"


def test_19_dns_failure():
    """#19: DNS resolution failure — connect to nonexistent host."""
    client_bin = _find_binary("test_negative")
    assert client_bin, "test_negative binary not built"
    rc = _run_client(client_bin, "ws://this-domain-does-not-exist-xyz.internal/",
                     "error-before-open", "DNS resolve failed")
    assert rc == 0, "DNS failure did not produce the expected resolution error"


def test_20_oversize():
    """#20: Oversize frame — server sends a >262144 byte payload; the
    client must stream it whole (Autobahn 9.x), not reject it."""
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
    port = 19121
    srv = subprocess.Popen([sys.executable, ECHO_SERVER, str(port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert _wait_port(port), "echo_server.py did not start"
    echo_bin = find_echo_test()
    rc = _run_client(echo_bin, f"ws://localhost:{port}/echo")
    srv.terminate(); srv.wait()
    assert rc == 0, f"dual-stack fallback failed (exit {rc})"


def test_22_proxy_tunnel():
    """#22: HTTP CONNECT proxy — client connects to a local echo server
    through a minimal CONNECT proxy. The proxy must record the target
    host:port it received in the CONNECT request."""
    up_port = 19114
    px_port = 19115
    log = os.path.join(HERE, "servers", "proxy.log")
    if os.path.exists(log):
        os.unlink(log)

    srv = subprocess.Popen([sys.executable, ECHO_SERVER, str(up_port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert _wait_port(up_port), "echo_server.py did not start"

    px = subprocess.Popen(
        [sys.executable, os.path.join(SERVERS_DIR, "connect_proxy_server.py"),
         str(px_port), log],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert _wait_port(px_port), "connect_proxy_server.py did not start"

    try:
        client_bin = _find_binary("proxy_test")
        assert client_bin, "proxy_test binary not built"
        rc = _run_client(client_bin, f"ws://127.0.0.1:{up_port}/",
                         f"http://127.0.0.1:{px_port}")
        assert rc == 0, f"proxy tunnel echo failed (exit {rc})"

        with open(log) as f:
            targets = f.read().strip().splitlines()
        assert len(targets) >= 1, "proxy never received a CONNECT request"
        assert targets[0] == f"127.0.0.1:{up_port}", \
            f"unexpected CONNECT target: {targets[0]}"
    finally:
        px.terminate(); px.wait()
        srv.terminate(); srv.wait()


def test_23_proxy_env_no_proxy():
    """#23: NO_PROXY bypass — HTTP_PROXY points at a dead port but the
    target is excluded via NO_PROXY, so the connection must go direct."""
    up_port = 19116
    dead_port = 19117
    srv = subprocess.Popen([sys.executable, ECHO_SERVER, str(up_port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert _wait_port(up_port), "echo_server.py did not start"
    try:
        client_bin = _find_binary("echo_test")
        assert client_bin, "echo_test binary not built"
        env = build_env(client_bin)
        env["HTTP_PROXY"] = f"http://127.0.0.1:{dead_port}"
        env["NO_PROXY"] = f"127.0.0.1,localhost,::1"
        rc = subprocess.call([client_bin, f"ws://127.0.0.1:{up_port}/"], env=env)
        assert rc == 0, f"NO_PROXY bypass failed (exit {rc})"
    finally:
        srv.terminate(); srv.wait()


def test_24_large_single_frame_text():
    """#24: Single text frame with payload > 256 KiB recv buffer —
    client must stream the frame across many reads (Autobahn 9.1)."""
    port = 19118
    size = 4 * 1024 * 1024  # 4 MiB
    srv = _start_server("large_frame_server.py", port, str(size), "text")
    client_bin = _find_binary("test_large_frame")
    assert client_bin, "test_large_frame binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/", str(size), "text")
    srv.terminate(); srv.wait()
    assert rc == 0, f"large single text frame failed (exit {rc})"


def test_25_large_single_frame_binary():
    """#25: Single binary frame with payload of 16 MiB — largest Autobahn
    single-frame size (9.2)."""
    port = 19119
    size = 16 * 1024 * 1024  # 16 MiB
    srv = _start_server("large_frame_server.py", port, str(size), "binary")
    client_bin = _find_binary("test_large_frame")
    assert client_bin, "test_large_frame binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/", str(size), "binary")
    srv.terminate(); srv.wait()
    assert rc == 0, f"large single binary frame failed (exit {rc})"


def test_26_invalid_64bit_payload_length():
    """#26: RFC 6455 forbids the high bit in a 64-bit payload length."""
    port = 19122
    size = 1 << 63
    srv = _start_server("large_frame_server.py", port, str(size), "binary")
    client_bin = _find_binary("test_negative")
    assert client_bin, "test_negative binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/",
                     "error-after-open", "invalid 64-bit payload length")
    srv.terminate(); srv.wait()
    assert rc == 0, "invalid 64-bit payload length did not produce a protocol error"


def test_27_large_fragmented_text_echo():
    """#27: Reassemble and echo 4 MiB text split into 64-byte frames."""
    port = 19123
    size = 4 * 1024 * 1024
    srv = _start_server("fragmented_large_frame_server.py", port,
                        str(size), "text", "64")
    client_bin = _find_binary("test_large_frame")
    assert client_bin, "test_large_frame binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/",
                     str(size), "text", "echo")
    srv.terminate(); srv.wait()
    assert rc == 0, f"large fragmented text echo failed (exit {rc})"


def test_28_large_fragmented_binary_echo():
    """#28: Reassemble and echo 4 MiB binary split into 1 MiB frames."""
    port = 19124
    size = 4 * 1024 * 1024
    srv = _start_server("fragmented_large_frame_server.py", port,
                        str(size), "binary", str(1024 * 1024))
    client_bin = _find_binary("test_large_frame")
    assert client_bin, "test_large_frame binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/",
                     str(size), "binary", "echo")
    srv.terminate(); srv.wait()
    assert rc == 0, f"large fragmented binary echo failed (exit {rc})"


def test_29_text_rtt_echo():
    """#29: Echo 1000 sequential 4096-byte text messages (Autobahn 9.7)."""
    port = 19125
    srv = _start_server("rtt_echo_server.py", port, "1000", "4096", "text")
    client_bin = _find_binary("test_large_frame")
    assert client_bin, "test_large_frame binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/",
                     "4096", "text", "echo")
    srv.terminate(); srv.wait()
    assert rc == 0, f"text RTT echo failed (exit {rc})"


def test_30_empty_binary_rtt_echo():
    """#30: Echo 1000 sequential empty binary messages (Autobahn 9.8)."""
    port = 19126
    srv = _start_server("rtt_echo_server.py", port, "1000", "0", "binary")
    client_bin = _find_binary("test_large_frame")
    assert client_bin, "test_large_frame binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/",
                     "0", "binary", "echo")
    srv.terminate(); srv.wait()
    assert rc == 0, f"empty binary RTT echo failed (exit {rc})"


def test_31_fragmented_text_stream_echo():
    """#31: One text message streamed as 4 KiB frames for about 3 seconds."""
    port = 19127
    size = 3 * 1024 * 1024
    srv = _start_server("fragmented_large_frame_server.py", port,
                        str(size), "text", "4096", "0.004")
    client_bin = _find_binary("test_large_frame")
    assert client_bin, "test_large_frame binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/",
                     str(size), "text", "echo")
    srv.terminate(); srv.wait()
    assert rc == 0, f"fragmented text stream echo failed (exit {rc})"


def test_32_peer_initiated_clean_close():
    """#32: Echo peer Close, then wait for the server to close TCP first."""
    port = 19128
    srv = _start_server("peer_close_server.py", port)
    client_bin = _find_binary("test_peer_close")
    assert client_bin, "test_peer_close binary not built"
    rc = _run_client(client_bin, f"ws://127.0.0.1:{port}/")
    srv.wait(timeout=5)
    assert srv.returncode == 0, f"peer-close server failed (exit {srv.returncode})"
    assert rc == 0, f"peer-initiated clean close failed (exit {rc})"
