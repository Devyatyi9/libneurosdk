#!/usr/bin/env python3
"""Orchestrate Toxiproxy + echo server + echo_test.

Starts Toxiproxy, creates an upstream proxy, applies a toxic,
then runs echo_test against the proxy endpoint.

Toxiproxy bandwidth rate is in KB/s (e.g. rate=1 = 1 KB/s = 1024 B/s).

Usage:
    python toxiproxy_test.py <toxic> [mode] [test_path] [port]

Modes:
    pre    Apply toxic BEFORE client connects (default).
           Uses native echo_test binary (recommended for bandwidth).
    mid    Apply toxic mid-connection after handshake.
           Uses Python websockets library.
           NOTE: bandwidth in mid mode shows no effect due to
           websockets internal buffering. Use pre + echo_test instead.

Toxics:
    slicer     - slice TCP stream into 1-byte chunks
    latency    - add 5s latency with 1s jitter
    timeout    - close connection after 500ms
    reset      - TCP RST instead of FIN
    bandwidth  - limit to N KB/s (default 10)

Requires toxiproxy-server in PATH or TOXIPROXY_BIN env var.
"""
import argparse, asyncio, json, os, socket, subprocess, sys, time
import urllib.error, urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from test_utils import build_env, find_echo_test, stop_process

TOXIPROXY_PORT = 8474
UPSTREAM_PORT = 19001
PROXY_PORT = 19002

TOXICS = {
    "slicer":  {"type": "slicer",  "attributes": {"average_size": 1, "size_variation": 0}},
    "latency": {"type": "latency", "attributes": {"latency": 3000, "jitter": 500}},
    "timeout": {"type": "timeout", "attributes": {"timeout": 500}},
    "reset":   {"type": "reset_peer", "attributes": {}},
    "bandwidth": {"type": "bandwidth", "attributes": {"rate": 10}},  # 10 KB/s
}

def find_echo_server():
    d = os.path.dirname(os.path.abspath(__file__))
    p = os.path.join(d, "..", "integration", "echo_server.py")
    if not os.path.exists(p):
        raise RuntimeError(f"echo_server.py not found at {p}")
    return p

def toxiproxy_url(path):
    return f"http://127.0.0.1:{TOXIPROXY_PORT}{path}"

def start_toxiproxy():
    # Check if already running
    try:
        urllib.request.urlopen(f"http://127.0.0.1:{TOXIPROXY_PORT}/version", timeout=2)
        return None
    except (OSError, urllib.error.URLError):
        pass

    bin_path = os.environ.get("TOXIPROXY_BIN")
    if not bin_path:
        candidates = ["toxiproxy-server",
                      "/tmp/toxiproxy-server",
                      r"C:\ProgramData\toxiproxy\toxiproxy-server-windows-amd64.exe",
                      r"C:\ProgramData\toxiproxy\toxiproxy-server.exe"]
        for c in candidates:
            if os.path.isfile(c):
                bin_path = c
                break
        if not bin_path:
            import shutil
            bin_path = shutil.which("toxiproxy-server") or "toxiproxy-server"
    proc = subprocess.Popen([bin_path], stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                f"toxiproxy exited during startup ({proc.returncode}): "
                f"{proc.stderr.read()}")
        try:
            urllib.request.urlopen(
                f"http://127.0.0.1:{TOXIPROXY_PORT}/version", timeout=1)
            break
        except (OSError, urllib.error.URLError):
            time.sleep(0.1)
    else:
        stop_process(proc)
        raise RuntimeError("toxiproxy did not become ready within 10s")
    return proc

def wait_port(port, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"server on port {port} did not become ready within {timeout}s")

def create_proxy(name, listen, upstream):
    data = json.dumps({"name": name, "listen": f"127.0.0.1:{listen}", "upstream": f"127.0.0.1:{upstream}"}).encode()
    req = urllib.request.Request(toxiproxy_url("/proxies"), data, {"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=10)

def add_toxic(proxy_name, toxic, stream=None):
    data = dict(toxic)
    if stream:
        data["stream"] = stream
    body = json.dumps(data).encode()
    req = urllib.request.Request(f"{toxiproxy_url('/proxies')}/{proxy_name}/toxics", body, {"Content-Type": "application/json"}, method="POST")
    urllib.request.urlopen(req, timeout=10)

def add_toxic_both(proxy_name, toxic):
    """Apply toxic to both upstream and downstream directions."""
    for s in ("upstream", "downstream"):
        add_toxic(proxy_name, toxic, stream=s)

def add_bandwidth(proxy_name, rate):
    for s in ("upstream", "downstream"):
        add_toxic(proxy_name, {"type": "bandwidth", "attributes": {"rate": rate}}, stream=s)

def delete_proxy(name, missing_ok=True):
    req = urllib.request.Request(f"{toxiproxy_url('/proxies')}/{name}", method="DELETE")
    try:
        urllib.request.urlopen(req, timeout=10)
    except urllib.error.HTTPError as exc:
        if not (missing_ok and exc.code == 404):
            raise

def apply_toxic(name, toxic_name):
    if toxic_name == "bandwidth":
        rate = TOXICS["bandwidth"]["attributes"]["rate"]
        add_bandwidth(name, rate)
    else:
        add_toxic_both(name, TOXICS[toxic_name])

def run_pre_connect(toxic_name, echo_test_bin, proxy_port, up_port):
    delete_proxy("ws_test")
    create_proxy("ws_test", proxy_port, up_port)
    apply_toxic("ws_test", toxic_name)
    print(f"Toxiproxy: {toxic_name} on port {proxy_port} -> {up_port} (pre-connect)")

    url = f"ws://127.0.0.1:{proxy_port}/"
    print(f"Running: {echo_test_bin} {url}")
    try:
        rc = subprocess.run([echo_test_bin, url], env=build_env(echo_test_bin),
                            timeout=60).returncode
    except subprocess.TimeoutExpired:
        print("echo_test exceeded 60s")
        return 1
    print(f"echo_test exit code: {rc}")
    # For timeout/reset toxics, echo_test SHOULD fail (connection killed)
    if toxic_name in ("timeout", "reset"):
        return 0 if rc != 0 else 1
    return rc


async def run_mid_connection(toxic_name, proxy_port, up_port):
    import websockets

    delete_proxy("ws_test")
    create_proxy("ws_test", proxy_port, up_port)
    print(f"Toxiproxy: proxy on {proxy_port} -> {up_port}, no toxic yet")
    print("Connecting websockets client...")

    rc = 0
    async with websockets.connect(f"ws://127.0.0.1:{proxy_port}/") as ws:
        print("[open] Connection established (websockets)")

        if toxic_name == "bandwidth":
            add_bandwidth("ws_test", TOXICS["bandwidth"]["attributes"]["rate"])
        else:
            add_toxic("ws_test", TOXICS[toxic_name])
        print(f"Toxiproxy: applied '{toxic_name}' mid-connection")

        if toxic_name == "bandwidth":
            rate = TOXICS["bandwidth"]["attributes"]["rate"]
            for size in (1024, 65536, 262144, 1048576):
                payload = b"x" * size
                # dynamic timeout: round-trip at rate * 2.5 safety margin + 10s base
                expected_sec = size / (rate * 1024) * 2
                dyn_timeout = max(30, expected_sec * 2.5 + 10)
                t0 = time.monotonic()
                await ws.send(payload)
                echo = await asyncio.wait_for(ws.recv(), timeout=dyn_timeout)
                elapsed = time.monotonic() - t0
                ok = echo == payload
                actual = f"{size/elapsed:.0f} B/s" if elapsed > 0.001 else "instant"
                print(f"  {size:>7} bytes: {elapsed:.3f}s ({actual}, limit {rate} KB/s)")
                if not ok:
                    rc = 1
        elif toxic_name == "slicer":
            for payload in (b"Hello, World!", b"x" * 1000, b"A" * 65536):
                await ws.send(payload)
                echo = await asyncio.wait_for(ws.recv(), timeout=10)
                ok = echo == payload
                print(f"  {len(payload):>7} bytes: {'OK' if ok else 'MISMATCH'}")
                if not ok:
                    rc = 1
        elif toxic_name == "latency":
            t0 = time.monotonic()
            await ws.send(b"ping")
            echo = await asyncio.wait_for(ws.recv(), timeout=15)
            elapsed = time.monotonic() - t0
            ok = echo == b"ping"
            print(f"  ping-pong: {elapsed:.2f}s (latency 5s expected) {'OK' if ok else 'MISMATCH'}")
            if not ok:
                rc = 1
        elif toxic_name in ("timeout", "reset"):
            try:
                await ws.send(b"will this get through?")
                await asyncio.wait_for(ws.recv(), timeout=5)
                print("  unexpected: message received after timeout/reset")
                rc = 1
            except (asyncio.TimeoutError, websockets.exceptions.ConnectionClosed):
                pass
            await asyncio.sleep(1)
            if ws.state == websockets.State.OPEN:
                print(f"  unexpected: connection still open after {toxic_name}")
                rc = 1
            else:
                print(f"  connection closed as expected ({toxic_name})")
        else:
            await ws.send(b"Hello from mid-connection!")
            echo = await asyncio.wait_for(ws.recv(), timeout=10)
            ok = echo == b"Hello from mid-connection!"
            print(f"  echo: {'OK' if ok else 'MISMATCH'}")
            if not ok:
                rc = 1

        await ws.close()

    print(f"mid-connection test exit code: {rc}")
    return rc


def main():
    parser = argparse.ArgumentParser(description="Toxiproxy WS test harness")
    parser.add_argument("toxic", nargs="?", default="slicer", choices=list(TOXICS.keys()),
                        help="toxic to apply")
    parser.add_argument("mode", nargs="?", default="pre", choices=("pre", "mid"),
                        help="pre=apply before connect, mid=apply mid-connection")
    parser.add_argument("test_path", nargs="?", default=None,
                        help="path to echo_test binary (pre mode only)")
    parser.add_argument("port", nargs="?", type=int, default=UPSTREAM_PORT,
                        help="upstream port")
    args = parser.parse_args()

    toxic_name = args.toxic
    up_port = args.port
    proxy_port = PROXY_PORT
    use_mid = args.mode == "mid"

    # Start echo server
    echo = subprocess.Popen([sys.executable, find_echo_server(), str(up_port)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            text=True)
    wait_port(up_port)

    # Start Toxiproxy
    toxiproxy = start_toxiproxy()

    rc = 0
    try:
        if use_mid:
            rc = asyncio.run(run_mid_connection(toxic_name, proxy_port, up_port))
        else:
            echo_test_bin = args.test_path if args.test_path and os.path.exists(args.test_path) else find_echo_test()
            rc = run_pre_connect(toxic_name, echo_test_bin, proxy_port, up_port)
    finally:
        try:
            delete_proxy("ws_test")
        except (OSError, urllib.error.URLError):
            pass
        if toxiproxy:
            stop_process(toxiproxy)
        stop_process(echo)

    sys.exit(rc)

if __name__ == '__main__':
    main()
