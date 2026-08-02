"""Autobahn fuzzingserver runner for ws_client.

Usage:
    python tests/autobahn/run_local_tests.py                   # localhost:9001, skip server start on Win/macOS
    python tests/autobahn/run_local_tests.py 192.168.1.10      # remote host:9001
    python tests/autobahn/run_local_tests.py 192.168.1.10 9002 # remote host:9002

On Linux, starts Autobahn fuzzingserver via Docker automatically.
On Windows/macOS, assumes the server is already running (user starts it manually).
Default host=127.0.0.1 port=9001.
"""
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import time

AGENT = "ws_client"

def get_case_count(host, port, timeout=30):
    """Wait for fuzzingserver and return its enabled case count."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, int(port)), timeout=2) as s:
                key = base64.b64encode(os.urandom(16)).decode()
                req = (
                    f"GET /getCaseCount HTTP/1.1\r\n"
                    f"Host: {host}:{port}\r\n"
                    f"Upgrade: websocket\r\n"
                    f"Connection: Upgrade\r\n"
                    f"Sec-WebSocket-Key: {key}\r\n"
                    f"Sec-WebSocket-Version: 13\r\n\r\n"
                )
                s.sendall(req.encode())
                response = b""
                while b"\r\n\r\n" not in response:
                    response += s.recv(4096)
                headers, payload = response.split(b"\r\n\r\n", 1)
                if not headers.startswith(b"HTTP/1.1 101 "):
                    raise OSError("WebSocket upgrade rejected")

                while len(payload) < 2:
                    payload += s.recv(4096)
                length = payload[1] & 0x7f
                offset = 2
                if length == 126:
                    while len(payload) < 4:
                        payload += s.recv(4096)
                    length = int.from_bytes(payload[2:4], "big")
                    offset = 4
                while len(payload) < offset + length:
                    payload += s.recv(4096)
                return int(payload[offset:offset + length])
        except (OSError, ValueError):
            pass
        time.sleep(0.25)
    return None


def trigger_update_reports(host, port):
    """Connect to /updateReports to finalise Autobahn report files."""
    try:
        s = socket.socket()
        s.settimeout(10)
        s.connect((host, int(port)))
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET /updateReports?agent={AGENT} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        s.send(req.encode())
        resp = s.recv(4096)
        if b"101" not in resp:
            print(f"  updateReports: unexpected response ({resp[:50]})")
        s.close()
    except Exception as e:
        print(f"  updateReports failed: {e}")


def load_reports(reports_dir):
    index = os.path.join(reports_dir, "index.json")
    if not os.path.exists(index):
        return None, []
    with open(index, encoding="utf-8") as f:
        reports = json.load(f).get(AGENT, {})
    bad_behavior = ("FAILED", "NON-COMPLIANT", "UNSTABLE")
    bad_close = ("FAILED", "WRONG CODE", "UNCLEAN")
    failures = [case for case, report in reports.items()
                if report.get("behavior") in bad_behavior
                or report.get("behaviorClose") in bad_close]
    return reports, failures

DOCKER = ["docker"]

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = sys.argv[2] if len(sys.argv) > 2 else "9001"
HERE = os.path.dirname(os.path.abspath(__file__))

# Find fuzzing_client binary in common build directories
BIN = None
for d in ["build-wsl", "build-x86", "build-x64", "build-asan-x86", "build-asan-x64", "build", "build-asan", "build-release"]:
    exe = os.path.join(HERE, "..", "..", d, "Release", "fuzzing_client.exe") if sys.platform == "win32" else os.path.join(HERE, "..", "..", d, "fuzzing_client")
    if os.path.exists(exe):
        BIN = exe
        break
if not BIN:
    BIN = os.path.join(HERE, "..", "..", "build-wsl", "fuzzing_client") if sys.platform == "linux" else os.path.join(HERE, "..", "..", "build", "Release", "fuzzing_client.exe")
    print(f"WARNING: binary not found, expected {BIN}")

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from test_utils import build_env

if not os.path.exists(BIN):
    print(f"ERROR: binary not found at {BIN}")
    if sys.platform == "linux":
        print("Build it first: CC=clang CXX=clang++ cmake -S . -B build-wsl -DNEURO_BUILD_TESTS=ON -DNEURO_SANITIZE=ON && cmake --build build-wsl -j")
    sys.exit(1)

started_server = False
reports_dir = os.path.expanduser("~/autobahn-reports")
try:
    # Start server on Linux (Docker available), skip on Win/macOS.
    if sys.platform == "linux":
        shutil.rmtree(reports_dir, ignore_errors=True)
        os.makedirs(reports_dir)
        subprocess.run(DOCKER + ["rm", "-f", "autobahn"], capture_output=True,
                       timeout=30)
        subprocess.run(DOCKER + [
            "run", "--rm", "-d",
            "--user", f"{os.getuid()}:{os.getgid()}",
            "-p", f"{PORT}:{PORT}",
            "-v", f"{HERE}:/config",
            "-v", f"{os.path.expanduser('~')}/autobahn-reports:/reports",
            "--name", "autobahn",
            "crossbario/autobahn-testsuite",
            "wstest", "-m", "fuzzingserver", "-s", "/config/fuzzingserver.json"
        ], check=True, timeout=30)
        started_server = True
        print("Autobahn fuzzingserver started via Docker")
        print("Mount ~/autobahn-reports:/reports to capture reports")
    else:
        print("Assuming Autobahn fuzzingserver is already running")

    url = f"ws://{HOST}:{PORT}"
    print(f"Waiting for WebSocket server at {url} ...", flush=True)
    total_cases = get_case_count(HOST, PORT)
    if total_cases is None:
        print("ERROR: Autobahn fuzzingserver did not become ready within 30s")
        sys.exit(1)

    print(f"Running {total_cases} test cases ...", flush=True)
    run_started = time.monotonic()
    for case in range(1, total_cases + 1):
        case_started = time.monotonic()
        try:
            rc = subprocess.run([BIN, str(case), url], env=build_env(BIN),
                                timeout=600).returncode
        except subprocess.TimeoutExpired:
            rc = 124
            print(f"  case {case}: TIMEOUT after 600s", flush=True)
        case_elapsed = time.monotonic() - case_started
        if rc != 0:
            print(f"  case {case}: FAIL (exit {rc})", flush=True)
        if case % 50 == 0 or case > 247 or case == total_cases:
            elapsed = time.monotonic() - run_started
            eta = elapsed / case * (total_cases - case)
            print(f"  {case}/{total_cases} done | case {case_elapsed:.1f}s | "
                  f"elapsed {elapsed / 60:.1f}m | ETA ~{eta / 60:.1f}m",
                  flush=True)

    print("Done. Triggering /updateReports to finalise report files ...")
    trigger_update_reports(HOST, PORT)
    reports, failures = load_reports(reports_dir)
    if failures:
        print(f"Retrying {len(failures)} failed report cases once: {failures}", flush=True)
        for case_id in failures:
            report_path = os.path.join(reports_dir, reports[case_id]["reportfile"])
            with open(report_path, encoding="utf-8") as f:
                case_number = json.load(f)["case"]
            try:
                subprocess.run([BIN, str(case_number), url], env=build_env(BIN),
                               check=False, timeout=600)
            except subprocess.TimeoutExpired:
                print(f"  retry case {case_number}: TIMEOUT after 600s", flush=True)
        trigger_update_reports(HOST, PORT)
except KeyboardInterrupt:
    print("\nInterrupted.")
    sys.exit(130)
finally:
    if started_server:
        subprocess.run(DOCKER + ["stop", "autobahn"], capture_output=True,
                       timeout=30)

index = os.path.join(reports_dir, "index.json")
if os.path.exists(index):
    reports, failures = load_reports(reports_dir)
    if len(reports) != total_cases:
        print(f"ERROR: expected {total_cases} reports, found {len(reports)}")
        sys.exit(1)
    if failures:
        print(f"ERROR: {len(failures)} Autobahn failures: {failures}")
        sys.exit(1)
    print(f"Reports saved to {reports_dir}/; {len(reports)} cases passed")
else:
    print(f"WARNING: {index} not found -- reports may be incomplete")
    print(f"  Check {reports_dir}/ manually")
