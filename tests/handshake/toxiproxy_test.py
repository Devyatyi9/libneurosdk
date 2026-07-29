#!/usr/bin/env python3
"""Orchestrate Toxiproxy + echo server + echo_test.

Starts Toxiproxy, creates an upstream proxy, applies a toxic,
then runs echo_test against the proxy endpoint.

Usage:
    python toxiproxy_test.py <toxic> [test_path] [port]

Toxics:
    slicer     - slice TCP stream into 1-byte chunks
    latency    - add 5s latency with 1s jitter
    timeout    - close connection after 500ms
    reset      - TCP RST instead of FIN
    bandwidth  - limit to 1 KB/s

Requires toxiproxy-server in PATH or TOXIPROXY_BIN env var.
"""
import json, os, socket, subprocess, sys, time, urllib.request

TOXIPROXY_PORT = 8474
UPSTREAM_PORT = 19001
PROXY_PORT = 19002

TOXICS = {
    "slicer":  {"type": "slicer",  "attributes": {"average_size": 1, "size_variation": 0}},
    "latency": {"type": "latency", "attributes": {"latency": 5000, "jitter": 1000}},
    "timeout": {"type": "timeout", "attributes": {"timeout": 500}},
    "reset":   {"type": "reset_peer", "attributes": {}},
    "bandwidth": {"type": "bandwidth", "attributes": {"rate": 1024}},
}

def find_echo_server():
    """Find echo_server.py in tests/integration/."""
    d = os.path.dirname(os.path.abspath(__file__))
    p = os.path.join(d, "..", "integration", "echo_server.py")
    if not os.path.exists(p):
        raise RuntimeError(f"echo_server.py not found at {p}")
    return p

def find_echo_test():
    """Find echo_test binary."""
    root = os.path.join(os.path.dirname(__file__), "..", "..")
    candidates = [
        os.path.join(root, "build", "Release", "echo_test.exe"),
        os.path.join(root, "build", "echo_test"),
        os.path.join(root, "build-release", "Release", "echo_test.exe"),
        os.path.join(root, "build-release", "echo_test"),
    ]
    for c in candidates:
        p = os.path.abspath(c)
        if os.path.exists(p):
            return p
    # Maybe just "echo_test" in PATH
    return "echo_test"

def toxiproxy_url(path):
    return f"http://127.0.0.1:{TOXIPROXY_PORT}{path}"

def start_toxiproxy():
    bin_path = os.environ.get("TOXIPROXY_BIN", "toxiproxy-server")
    proc = subprocess.Popen([bin_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)  # wait for startup
    return proc

def create_proxy(name, listen, upstream):
    data = json.dumps({"name": name, "listen": f"127.0.0.1:{listen}", "upstream": f"127.0.0.1:{upstream}"}).encode()
    req = urllib.request.Request(toxiproxy_url("/proxies"), data, {"Content-Type": "application/json"})
    urllib.request.urlopen(req)

def add_toxic(proxy_name, toxic):
    data = json.dumps(toxic).encode()
    req = urllib.request.Request(f"{toxiproxy_url('/proxies')}/{proxy_name}/toxics", data, {"Content-Type": "application/json"}, method="POST")
    urllib.request.urlopen(req)

def delete_proxy(name):
    req = urllib.request.Request(f"{toxiproxy_url('/proxies')}/{name}", method="DELETE")
    try:
        urllib.request.urlopen(req)
    except:
        pass

def main():
    toxic_name = sys.argv[1] if len(sys.argv) > 1 else "slicer"
    test_path = sys.argv[2] if len(sys.argv) > 2 else find_echo_test()
    up_port = int(sys.argv[3]) if len(sys.argv) > 3 else UPSTREAM_PORT
    proxy_port = PROXY_PORT

    if toxic_name not in TOXICS:
        print(f"Unknown toxic: {toxic_name}. Available: {', '.join(TOXICS.keys())}")
        sys.exit(1)

    echo_script = find_echo_server()
    echo_test_bin = test_path if os.path.exists(test_path) else find_echo_test()

    # Start echo server as upstream
    echo = subprocess.Popen([sys.executable, echo_script, str(up_port)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)

    # Start Toxiproxy
    toxiproxy = start_toxiproxy()

    try:
        # Create proxy
        delete_proxy("ws_test")
        create_proxy("ws_test", proxy_port, up_port)
        # Add toxic
        add_toxic("ws_test", TOXICS[toxic_name])
        print(f"Toxiproxy: {toxic_name} on port {proxy_port} → {up_port}")

        # Run echo_test
        url = f"ws://127.0.0.1:{proxy_port}/"
        print(f"Running: {echo_test_bin} {url}")
        rc = subprocess.call([echo_test_bin, url])
        print(f"echo_test exit code: {rc}")
        sys.exit(rc)
    finally:
        delete_proxy("ws_test")
        toxiproxy.terminate()
        toxiproxy.wait()
        echo.terminate()
        echo.wait()

if __name__ == '__main__':
    main()
