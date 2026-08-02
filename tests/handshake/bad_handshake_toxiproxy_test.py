#!/usr/bin/env python3
"""Run bad_handshake_server through Toxiproxy with slicer toxic.

Tests ws_client's response to fragmented (1-byte) HTTP responses
across all 7 bad handshake modes.

Usage:
    python bad_handshake_toxiproxy_test.py [echo_test_path]
"""
import json, os, subprocess, sys, tempfile, time
import urllib.error, urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from test_utils import build_env, find_echo_test, stop_process

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
TOXIPROXY_PORT = 8474
UPSTREAM_PORT = 19001
PROXY_PORT = 19002
MODES = ["200", "404", "301", "no_upgrade", "no_conn", "bad_accept", "no_accept"]

def toxiproxy_url(path):
    return f"http://127.0.0.1:{TOXIPROXY_PORT}{path}"

def create_proxy(name, listen, upstream):
    data = json.dumps({"name": name, "listen": f"127.0.0.1:{listen}", "upstream": f"127.0.0.1:{upstream}"}).encode()
    req = urllib.request.Request(toxiproxy_url("/proxies"), data, {"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=10)

def add_toxic(proxy_name, toxic_type, attrs):
    body = json.dumps({"type": toxic_type, "attributes": attrs}).encode()
    req = urllib.request.Request(f"{toxiproxy_url('/proxies')}/{proxy_name}/toxics", body, {"Content-Type": "application/json"}, method="POST")
    urllib.request.urlopen(req, timeout=10)

def delete_proxy(name, missing_ok=True):
    try:
        req = urllib.request.Request(f"{toxiproxy_url('/proxies')}/{name}", method="DELETE")
        urllib.request.urlopen(req, timeout=10)
    except urllib.error.HTTPError as exc:
        if not (missing_ok and exc.code == 404):
            raise

def wait_ready(proc, ready_file, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(ready_file):
            return
        if proc.poll() is not None:
            raise RuntimeError(
                f"server exited during startup ({proc.returncode}): "
                f"{proc.stderr.read()}")
        time.sleep(0.1)
    raise RuntimeError("server did not become ready within 10s")

def main():
    echo_test_bin = sys.argv[1] if len(sys.argv) > 1 else find_echo_test()
    print(f"echo_test: {echo_test_bin}")
    print(f"Modes: {', '.join(MODES)}")
    print()

    bad_server = None
    toxiproxy = None
    results = {}

    try:
        # Start Toxiproxy (or use already running one)
        toxiproxy = None
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{TOXIPROXY_PORT}/version", timeout=2)
        except (OSError, urllib.error.URLError):
            toxiproxy_bin = os.environ.get("TOXIPROXY_BIN")
            if not toxiproxy_bin:
                candidates = ["toxiproxy-server",
                              "/tmp/toxiproxy-server",
                              r"C:\ProgramData\toxiproxy\toxiproxy-server-windows-amd64.exe",
                              r"C:\ProgramData\toxiproxy\toxiproxy-server.exe"]
                for c in candidates:
                    if os.path.isfile(c):
                        toxiproxy_bin = c
                        break
                if not toxiproxy_bin:
                    import shutil
                    toxiproxy_bin = shutil.which("toxiproxy-server") or "toxiproxy-server"
            toxiproxy = subprocess.Popen(
                [toxiproxy_bin], stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if toxiproxy.poll() is not None:
                    raise RuntimeError(
                        f"toxiproxy exited during startup ({toxiproxy.returncode}): "
                        f"{toxiproxy.stderr.read()}")
                try:
                    urllib.request.urlopen(
                        f"http://127.0.0.1:{TOXIPROXY_PORT}/version", timeout=1)
                    break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.1)
            else:
                raise RuntimeError("toxiproxy did not become ready within 10s")

        for mode in MODES:
            print(f"--- [{mode}] ---")
            # Kill old bad_handshake_server if any
            if bad_server:
                stop_process(bad_server)

            # Start bad_handshake_server with current mode on UPSTREAM_PORT
            script = os.path.join(HERE, "bad_handshake_server.py")
            with tempfile.TemporaryDirectory() as marker_dir:
                ready_file = os.path.join(marker_dir, "ready")
                served_file = os.path.join(marker_dir, "served")
                server_env = os.environ.copy()
                server_env["WS_TEST_READY_FILE"] = ready_file
                server_env["WS_TEST_SERVED_FILE"] = served_file
                bad_server = subprocess.Popen(
                    [sys.executable, script, mode, str(UPSTREAM_PORT), "--once"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                    env=server_env)
                wait_ready(bad_server, ready_file)

                # Create proxy with slicer
                delete_proxy("bad_test")
                create_proxy("bad_test", PROXY_PORT, UPSTREAM_PORT)
                add_toxic("bad_test", "slicer", {"average_size": 1, "size_variation": 0})

                # Run echo_test
                url = f"ws://127.0.0.1:{PROXY_PORT}/"
                try:
                    rc = subprocess.run(
                        [echo_test_bin, url], env=build_env(echo_test_bin), timeout=30,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL).returncode
                except subprocess.TimeoutExpired:
                    print("  echo_test exceeded 30s")
                    results[mode] = (124, False)
                    continue

                try:
                    server_rc = bad_server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server_rc = None
                served = os.path.exists(served_file)
                ok = rc != 0 and server_rc == 0 and served
                status = "OK" if ok else "FAILED validation"
                results[mode] = (rc, ok)
                print(f"  exit={rc} server_exit={server_rc} served={served}  {status}")

                delete_proxy("bad_test")

    finally:
        if bad_server:
            stop_process(bad_server)
        try:
            delete_proxy("bad_test")
        except (OSError, urllib.error.URLError):
            pass
        if toxiproxy:
            stop_process(toxiproxy)

    print()
    print("=== Summary ===")
    all_ok = True
    for mode, (rc, ok) in results.items():
        mark = "[PASS]" if ok else "[FAIL]"
        if not ok:
            all_ok = False
        print(f"  {mark} [{mode:12s}] exit={rc}")
    print()
    print(f"All tests passed: {all_ok}")
    sys.exit(0 if all_ok else 1)

if __name__ == '__main__':
    main()
