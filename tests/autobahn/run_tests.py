"""Autobahn fuzzingserver runner for ws_client.

Usage:
    python tests/autobahn/run_tests.py                   # localhost:9001, skip server start on Win/macOS
    python tests/autobahn/run_tests.py 192.168.1.10      # remote host:9001
    python tests/autobahn/run_tests.py 192.168.1.10 9002 # remote host:9002

On Linux, starts Autobahn fuzzingserver via Docker automatically.
On Windows/macOS, assumes the server is already running (user starts it manually).
Default host=127.0.0.1 port=9001.
"""
import subprocess, sys, time, os, socket, base64

AGENT = "ws_client"

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

DOCKER = ["sudo", "docker"] if sys.platform == "linux" else ["docker"]

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = sys.argv[2] if len(sys.argv) > 2 else "9001"
HERE = os.path.dirname(os.path.abspath(__file__))

# Find fuzzing_client binary in common build directories
BIN = None
for d in ["build-x86", "build-x64", "build-asan-x86", "build-asan-x64", "build", "build-asan", "build-release"]:
    exe = os.path.join(HERE, "..", "..", d, "Release", "fuzzing_client.exe") if sys.platform == "win32" else os.path.join(HERE, "..", "..", d, "fuzzing_client")
    if os.path.exists(exe):
        BIN = exe
        break
if not BIN:
    BIN = os.path.join(HERE, "..", "..", "build", "fuzzing_client") if sys.platform != "win32" else os.path.join(HERE, "..", "..", "build", "Release", "fuzzing_client.exe")
    print(f"WARNING: binary not found, will try {BIN}")

TOTAL_CASES = 247

# Start server on Linux (Docker available), skip on Win/macOS
if sys.platform == "linux":
    subprocess.run(DOCKER + ["kill", "autobahn"], capture_output=True)
    subprocess.run(DOCKER + ["rm", "autobahn"], capture_output=True)
    subprocess.Popen(DOCKER + [
        "run", "--rm", "-d",
        "-p", f"{PORT}:{PORT}",
        "-v", f"{HERE}:/config",
        "-v", f"{os.path.expanduser('~')}/autobahn-reports:/reports",
        "--name", "autobahn",
        "crossbario/autobahn-testsuite",
        "wstest", "-m", "fuzzingserver", "-s", "/config/fuzzingserver.json"
    ])
    print("Autobahn fuzzingserver started via Docker")
    print("Mount ~/autobahn-reports:/reports to capture reports")
else:
    print("Assuming Autobahn fuzzingserver is already running")

url = f"ws://{HOST}:{PORT}"
print(f"Connecting to {url}, waiting 5s ...")
time.sleep(5)

if not os.path.exists(BIN):
    print(f"ERROR: binary not found at {BIN}")
    sys.exit(1)

print(f"Running {TOTAL_CASES} test cases ...")
for case in range(1, TOTAL_CASES + 1):
    rc = subprocess.run([BIN, str(case), url]).returncode
    if rc != 0:
        print(f"  case {case}: FAIL (exit {rc})")
    if case % 50 == 0:
        print(f"  {case}/{TOTAL_CASES} done")

print("Done. Triggering /updateReports to finalise report files ...")
trigger_update_reports(HOST, PORT)
if sys.platform == "linux":
    subprocess.run(DOCKER + ["stop", "autobahn"], capture_output=True)
reports_dir = os.path.expanduser("~/autobahn-reports")
index = os.path.join(reports_dir, "index.json")
if os.path.exists(index):
    print(f"Reports saved to {reports_dir}/")
else:
    print(f"WARNING: {index} not found — reports may be incomplete")
    print(f"  Check {reports_dir}/ manually")
