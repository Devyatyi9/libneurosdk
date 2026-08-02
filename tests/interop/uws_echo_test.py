#!/usr/bin/env python3
"""Build, start uWS echo-server and run echo_test against it."""
import os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
UWS_COMMIT = "fe7da4cb05622b8d004718ec3ca05101782eb1c2"

sys.path.insert(0, os.path.join(ROOT, "tests"))
from test_utils import build_env, find_echo_test

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19004

    # Build uWS echo-server via CMake if missing
    uws_bin = os.path.join(ROOT, "uws_echo.exe" if sys.platform == "win32" else "uws_echo")
    if not os.path.isfile(uws_bin):
        uws_dir = os.path.join(HERE, "uWebSockets")
        if not os.path.isdir(uws_dir):
            subprocess.check_call([
                "git", "clone", "--depth", "1", "--filter=blob:none", "--no-checkout",
                "https://github.com/uWebSockets/uWebSockets", uws_dir,
            ], timeout=600)
        has_commit = subprocess.run(
            ["git", "-C", uws_dir, "cat-file", "-e", f"{UWS_COMMIT}^{{commit}}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        if not has_commit:
            subprocess.check_call(
                ["git", "-C", uws_dir, "fetch", "--depth", "1", "origin", UWS_COMMIT],
                timeout=600)
        subprocess.check_call(
            ["git", "-C", uws_dir, "checkout", "--detach", UWS_COMMIT], timeout=60)
        # Only uSockets is needed for this no-SSL, no-QUIC interop server.
        subprocess.check_call([
            "git", "-C", uws_dir, "submodule", "update", "--init", "--depth", "1",
            "uSockets",
        ], timeout=600)
        build_name = "build"
        if sys.platform == "win32":
            vcpkg_bin = subprocess.check_output(
                ["where", "vcpkg"], text=True, timeout=30).splitlines()[0].strip()
            vcpkg_root = os.path.dirname(vcpkg_bin)
            target_arch = os.environ.get("VSCMD_ARG_TGT_ARCH", "x64").lower()
            arch = "x86" if target_arch in ("x86", "win32") else "x64"
            build_name = f"build-{arch}"
            triplet = f"{arch}-windows-static"
            subprocess.check_call([vcpkg_bin, "install", f"libuv:{triplet}",
                                   f"zlib:{triplet}"], timeout=600)
        build_dir = os.path.join(HERE, build_name)
        cmake_args = ["cmake", "-S", HERE, "-B", build_dir]
        if sys.platform == "win32":
            cmake_args.extend([
                "-A", "Win32" if arch == "x86" else "x64",
                f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_root}/scripts/buildsystems/vcpkg.cmake",
                f"-DVCPKG_TARGET_TRIPLET={triplet}",
            ])
        subprocess.check_call(cmake_args, timeout=600)
        subprocess.check_call(
            ["cmake", "--build", build_dir, "--config", "Release"], timeout=600)
        src = os.path.join(build_dir, "Release" if sys.platform == "win32" else "",
                           "uws_echo.exe" if sys.platform == "win32" else "uws_echo")
        if os.path.isfile(src):
            import shutil
            shutil.copy2(src, uws_bin)

    # Start uWS echo-server
    uws = subprocess.Popen([uws_bin, str(port)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if uws.poll() is not None:
            print(f"uWS exited during startup ({uws.returncode}): {uws.stderr.read()}")
            return 1
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                break
        except OSError:
            time.sleep(0.1)
    else:
        uws.terminate()
        try:
            uws.wait(timeout=10)
        except subprocess.TimeoutExpired:
            uws.kill()
            uws.wait(timeout=10)
        print("uWS did not become ready within 10s")
        return 1

    # Run echo_test
    echo_bin = find_echo_test()
    url = f"ws://127.0.0.1:{port}/"
    try:
        rc = subprocess.run([echo_bin, url], env=build_env(echo_bin),
                            timeout=30).returncode
    except subprocess.TimeoutExpired:
        print("uWS echo test FAILED (echo_test timed out after 30s)")
        rc = 1
    finally:
        if uws.poll() is None:
            uws.terminate()
        try:
            uws.wait(timeout=10)
        except subprocess.TimeoutExpired:
            uws.kill()
            uws.wait(timeout=10)

    if rc != 0:
        print(f"uWS echo test FAILED (exit={rc})")
        return 1
    print("uWS echo test: OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())
