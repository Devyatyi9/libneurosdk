#!/usr/bin/env python3
"""Run bad_handshake_server through Toxiproxy with slicer toxic.

Tests ws_client's response to fragmented (1-byte) HTTP responses
across all 7 bad handshake modes.

Usage:
    python bad_handshake_toxiproxy_test.py [echo_test_path]
"""
import glob, json, os, subprocess, sys, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
TOXIPROXY_PORT = 8474
UPSTREAM_PORT = 19001
PROXY_PORT = 19002
MODES = ["200", "404", "301", "no_upgrade", "no_conn", "bad_accept", "no_accept"]

def _pe_arch(path):
    try:
        with open(path, 'rb') as f:
            if f.read(2) != b'MZ':
                return None
            f.seek(0x3C)
            pe_off = int.from_bytes(f.read(4), 'little')
            f.seek(pe_off)
            if f.read(4) != b'PE\0\0':
                return None
            machine = int.from_bytes(f.read(2), 'little')
            if machine == 0x14C:
                return 'x86'
            if machine == 0x8664:
                return 'x64'
            return None
    except Exception:
        return None

def build_env(bin_path):
    env = os.environ.copy()
    msvc_root = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC"
    versions = sorted(glob.glob(os.path.join(msvc_root, "*")), reverse=True)
    arch = _pe_arch(bin_path) if os.path.isfile(bin_path) else None
    subdirs = ['x64', 'x86'] if arch is None else [{'x86': 'x86', 'x64': 'x64'}[arch]]
    for v in versions:
        for sd in subdirs:
            dll_dir = os.path.join(v, "bin", "Hostx64", sd)
            asan_name = "clang_rt.asan_dynamic-x86_64.dll" if sd == 'x64' else "clang_rt.asan_dynamic-i386.dll"
            if os.path.isfile(os.path.join(dll_dir, asan_name)):
                env["PATH"] = dll_dir + os.pathsep + env["PATH"]
                return env
    return env

def find_echo_test():
    candidates = [
        os.path.join(ROOT, "build-asan-x86", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build-asan-x64", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build-asan", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build-x86", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build-x64", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build", "Release", "echo_test.exe"),
    ]
    for c in candidates:
        p = os.path.abspath(c)
        if os.path.exists(p):
            return p
    return "echo_test"

def toxiproxy_url(path):
    return f"http://127.0.0.1:{TOXIPROXY_PORT}{path}"

def create_proxy(name, listen, upstream):
    data = json.dumps({"name": name, "listen": f"127.0.0.1:{listen}", "upstream": f"127.0.0.1:{upstream}"}).encode()
    req = urllib.request.Request(toxiproxy_url("/proxies"), data, {"Content-Type": "application/json"})
    urllib.request.urlopen(req)

def add_toxic(proxy_name, toxic_type, attrs):
    body = json.dumps({"type": toxic_type, "attributes": attrs}).encode()
    req = urllib.request.Request(f"{toxiproxy_url('/proxies')}/{proxy_name}/toxics", body, {"Content-Type": "application/json"}, method="POST")
    urllib.request.urlopen(req)

def delete_proxy(name):
    try:
        req = urllib.request.Request(f"{toxiproxy_url('/proxies')}/{name}", method="DELETE")
        urllib.request.urlopen(req)
    except:
        pass

def main():
    echo_test_bin = sys.argv[1] if len(sys.argv) > 1 else find_echo_test()
    print(f"echo_test: {echo_test_bin}")
    print(f"Modes: {', '.join(MODES)}")
    print()

    bad_server = None
    toxiproxy = None
    results = {}

    try:
        # Start Toxiproxy
        toxiproxy_bin = os.environ.get("TOXIPROXY_BIN", "toxiproxy-server")
        toxiproxy = subprocess.Popen([toxiproxy_bin], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2)

        for mode in MODES:
            print(f"--- [{mode}] ---")
            # Kill old bad_handshake_server if any
            if bad_server:
                bad_server.terminate()
                bad_server.wait()

            # Start bad_handshake_server with current mode on UPSTREAM_PORT
            script = os.path.join(HERE, "bad_handshake_server.py")
            bad_server = subprocess.Popen([sys.executable, script, mode, str(UPSTREAM_PORT)],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1)

            # Create proxy with slicer
            delete_proxy("bad_test")
            create_proxy("bad_test", PROXY_PORT, UPSTREAM_PORT)
            add_toxic("bad_test", "slicer", {"average_size": 1, "size_variation": 0})

            # Run echo_test
            url = f"ws://127.0.0.1:{PROXY_PORT}/"
            rc = subprocess.call([echo_test_bin, url], env=build_env(echo_test_bin),
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            ok = rc != 0  # non-zero = expected (connection rejected)
            status = "OK" if ok else "CONNECTED (unexpected)"
            results[mode] = (rc, ok)
            print(f"  exit={rc}  {status}")

            delete_proxy("bad_test")

    finally:
        if bad_server:
            bad_server.terminate()
            bad_server.wait()
        if toxiproxy:
            toxiproxy.terminate()
            toxiproxy.wait()
        delete_proxy("bad_test")

    print()
    print("=== Summary ===")
    all_ok = True
    for mode, (rc, ok) in results.items():
        mark = "✅" if ok else "❌"
        if not ok:
            all_ok = False
        print(f"  {mark} [{mode:12s}] exit={rc}")
    print()
    print(f"All tests passed: {all_ok}")
    sys.exit(0 if all_ok else 1)

if __name__ == '__main__':
    main()
