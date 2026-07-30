#!/usr/bin/env python3
"""Build, start uWS echo-server and run echo_test against it."""
import os, platform, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")

sys.path.insert(0, os.path.join(ROOT, "tests"))
from test_utils import build_env, find_echo_test

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19004

    # Build uWS echo-server if missing
    uws_bin = os.path.join(ROOT, "uws_echo.exe" if sys.platform == "win32" else "uws_echo")
    if not os.path.isfile(uws_bin):
        subprocess.check_call([sys.executable, os.path.join(HERE, "build_uws.py")])

    # Add uv.dll path on Windows
    env = os.environ.copy()
    if sys.platform == "win32":
        machine = platform.machine().lower()
        arch = "x64" if machine in ("amd64", "x86_64", "arm64") else "x86"
        vcpkg_root = env.get("VCPKG_ROOT") or os.path.dirname(
            subprocess.check_output(["where", "vcpkg"], text=True).strip())
        dll_dir = f"{vcpkg_root}/installed/{arch}-windows/bin"
        if os.path.isdir(dll_dir):
            env["PATH"] = dll_dir + os.pathsep + env["PATH"]

    # Start uWS echo-server
    uws = subprocess.Popen([uws_bin, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           env=env)
    time.sleep(2)

    # Run echo_test
    echo_bin = find_echo_test()
    url = f"ws://127.0.0.1:{port}/"
    rc = subprocess.call([echo_bin, url], env=build_env(echo_bin))

    uws.terminate()
    uws.wait()

    if rc != 0:
        print(f"uWS echo test FAILED (exit={rc})")
        sys.exit(1)
    print("uWS echo test: OK")

if __name__ == "__main__":
    main()
