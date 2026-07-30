#!/usr/bin/env python3
"""nginx WebSocket reverse proxy test.

Starts uWS echo-server behind nginx, runs echo_test client through the proxy,
then does the same via the ws:// nginx endpoint.
"""
import os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")

sys.path.insert(0, os.path.join(ROOT, "tests"))
from test_utils import build_env, find_echo_test


def find_nginx():
    """Locate nginx binary."""
    if sys.platform == "win32":
        for root in ("C:\\", "C:\\Program Files", "C:\\Program Files (x86)"):
            for d in os.listdir(root):
                if d.startswith("nginx-"):
                    p = os.path.join(root, d, "nginx.exe")
                    if os.path.isfile(p):
                        return p
    else:
        for p in ("/usr/sbin/nginx", "/usr/local/bin/nginx", "/opt/homebrew/bin/nginx"):
            if os.path.isfile(p):
                return p
    which = "where" if sys.platform == "win32" else "which"
    try:
        return subprocess.check_output([which, "nginx"], text=True).strip().splitlines()[0]
    except subprocess.CalledProcessError:
        return None


NGINX_CONF = """
daemon off;
master_process off;
worker_processes 1;
error_log logs/error.log;
pid logs/nginx.pid;

events {{
    worker_connections 1024;
}}

http {{
    access_log off;
    server {{
        listen 127.0.0.1:{proxy_port};
        location / {{
            proxy_pass http://127.0.0.1:{upstream_port};
            proxy_http_version 1.1;
            proxy_set_header Upgrade $http_upgrade;
            proxy_set_header Connection "upgrade";
        }}
    }}
}}
"""


def main():
    upstream_port = int(sys.argv[1]) if len(sys.argv) > 1 else 19009
    proxy_port = int(sys.argv[2]) if len(sys.argv) > 2 else 19010

    nginx_bin = find_nginx()
    if not nginx_bin:
        print("nginx not found, skipping")
        return 77  # skip code

    echo_bin = find_echo_test()
    if not echo_bin or not os.path.isfile(echo_bin):
        print("echo_test not found, skipping")
        return 77

    # Start uWS echo-server
    uws_bin = None
    for p in [
        os.path.join(ROOT, "uws_echo.exe" if sys.platform == "win32" else "uws_echo"),
        os.path.join(ROOT, "tests", "interop", "build", "Release",
                     "uws_echo.exe" if sys.platform == "win32" else "uws_echo"),
        os.path.join(ROOT, "tests", "interop", "build",
                     "uws_echo.exe" if sys.platform == "win32" else "uws_echo"),
    ]:
        if os.path.isfile(p):
            uws_bin = p
            break

    if not uws_bin:
        print("uWS echo-server not found, skipping")
        return 77

    print(f"  uWS: {uws_bin}")
    print(f"  nginx: {nginx_bin}")

    # Start uWS
    uws = subprocess.Popen([uws_bin, str(upstream_port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)

    conf_data = NGINX_CONF.format(proxy_port=proxy_port,
                                  upstream_port=upstream_port)

    if sys.platform == "win32":
        nginx_prefix = os.path.dirname(nginx_bin)
        for d in ("conf", "logs", "temp"):
            os.makedirs(os.path.join(nginx_prefix, d), exist_ok=True)
        with open(os.path.join(nginx_prefix, "conf", "nginx.conf"), "w") as f:
            f.write(conf_data)
        nginx = subprocess.Popen(
            [nginx_bin, "-p", nginx_prefix],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    else:
        nginx_prefix = os.path.join(tempfile.gettempdir(), f"nginx-proxy-test-{proxy_port}")
        for d in ("", "logs", "temp", "conf"):
            os.makedirs(os.path.join(nginx_prefix, d), exist_ok=True)
        conf_path = os.path.join(nginx_prefix, "conf", "nginx.conf")
        with open(conf_path, "w") as f:
            f.write(conf_data)
        nginx = subprocess.Popen(
            [nginx_bin, "-p", nginx_prefix, "-c", conf_path],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    time.sleep(2)

    # Ensure proxy port is reachable
    import socket
    ok = False
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            s = socket.socket()
            s.settimeout(1)
            s.connect(("127.0.0.1", proxy_port))
            s.close()
            ok = True
            break
        except (ConnectionRefusedError, OSError):
            time.sleep(0.3)

    if not ok:
        err = nginx.stderr.read().decode(errors="replace") if nginx.stderr else ""
        print(f"nginx did not start on proxy port")
        if err:
            print(f"  nginx stderr: {err.strip()}")
        nginx.terminate(); nginx.wait()
        uws.terminate(); uws.wait()
        return 1

    # Debug: probe both nginx and upstream
    import http.client

    def probe(host, port, label):
        try:
            conn = http.client.HTTPConnection(host, port, timeout=5)
            conn.request("GET", "/")
            resp = conn.getresponse()
            print(f"  {label}: {resp.status} {resp.reason}")
            body = resp.read(200)
            conn.close()
        except Exception as e:
            print(f"  {label}: probe failed: {e}")

    probe("127.0.0.1", proxy_port, "nginx")
    probe("127.0.0.1", upstream_port, "uWS")

    # Also check nginx error log
    err_log = os.path.join(nginx_prefix, "logs", "error.log")
    if os.path.isfile(err_log):
        with open(err_log) as f:
            err_content = f.read().strip()
            if err_content:
                print(f"  nginx error log: {err_content[:300]}")

    # Run echo_test through nginx proxy
    url = f"ws://127.0.0.1:{proxy_port}/"
    env = build_env(echo_bin)
    rc = subprocess.call([echo_bin, url], env=env)

    nginx.terminate(); nginx.wait()
    uws.terminate(); uws.wait()

    if rc != 0:
        print(f"nginx proxy test FAILED (exit={rc})")
        sys.exit(1)

    print("nginx proxy test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
