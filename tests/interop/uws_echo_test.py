#!/usr/bin/env python3
"""Build, start uWS echo-server and run echo_test against it."""
import os, platform, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")

sys.path.insert(0, os.path.join(ROOT, "tests"))
from test_utils import build_env, find_echo_test

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19004

    # Build uWS echo-server via CMake if missing
    uws_bin = os.path.join(ROOT, "uws_echo.exe" if sys.platform == "win32" else "uws_echo")
    if not os.path.isfile(uws_bin):
        uws_dir = os.path.join(HERE, "uWebSockets")
        if not os.path.isdir(uws_dir):
            subprocess.check_call(["git", "clone", "--recursive",
                                   "https://github.com/uWebSockets/uWebSockets", uws_dir])
        build_dir = os.path.join(HERE, "build")
        cmake_args = ["cmake", "-S", HERE, "-B", build_dir]
        if sys.platform == "win32":
            vcpkg_root = os.path.dirname(
                subprocess.check_output(["where", "vcpkg"], text=True).splitlines()[0].strip())
            machine = platform.machine().lower()
            arch = "x64" if machine in ("amd64", "x86_64", "arm64") else "x86"
            triplet = f"{arch}-windows"
            subprocess.check_call(["vcpkg", "install", f"libuv:{triplet}"])
            pkg_dir = f"{vcpkg_root}/packages/libuv_{triplet}"
            if os.path.isdir(pkg_dir):
                cmake_args.append(f"-DCMAKE_PREFIX_PATH={pkg_dir}")
            else:
                cmake_args.append(f"-DCMAKE_PREFIX_PATH={vcpkg_root}/installed/{triplet}")
        subprocess.check_call(cmake_args)
        subprocess.check_call(["cmake", "--build", build_dir, "--config", "Release"])
        src = os.path.join(build_dir, "Release" if sys.platform == "win32" else "",
                           "uws_echo.exe" if sys.platform == "win32" else "uws_echo")
        if os.path.isfile(src):
            import shutil
            shutil.copy2(src, uws_bin)
            # Copy uv.dll alongside on Windows (dynamic dependency)
            if sys.platform == "win32":
                dll_dir = os.path.join(build_dir, "Release")
                dll_src = os.path.join(dll_dir, "uv.dll")
                if not os.path.isfile(dll_src):
                    # Try vcpkg installed path
                    machine = platform.machine().lower()
                    arch = "x64" if machine in ("amd64", "x86_64", "arm64") else "x86"
                    vcpkg_root = os.path.dirname(
                        subprocess.check_output(["where", "vcpkg"], text=True).splitlines()[0].strip())
                    dll_src = os.path.join(vcpkg_root, "installed", f"{arch}-windows", "bin", "uv.dll")
                if os.path.isfile(dll_src):
                    shutil.copy2(dll_src, os.path.join(os.path.dirname(uws_bin), "uv.dll"))

    # Add uv.dll path on Windows
    env = os.environ.copy()
    if sys.platform == "win32":
        machine = platform.machine().lower()
        arch = "x64" if machine in ("amd64", "x86_64", "arm64") else "x86"
        vcpkg_root = os.path.dirname(
            subprocess.check_output(["where", "vcpkg"], text=True).splitlines()[0].strip())
        for candidate in [f"{vcpkg_root}/installed/{arch}-windows/bin",
                          f"C:/vcpkg/installed/{arch}-windows/bin"]:
            if os.path.isdir(candidate):
                env["PATH"] = candidate + os.pathsep + env["PATH"]
                break

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
