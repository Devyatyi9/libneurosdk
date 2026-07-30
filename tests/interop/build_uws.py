#!/usr/bin/env python3
"""Clone (if missing) and build uWS echo-server for the current platform."""
import glob, os, platform, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
UWS_DIR = os.path.join(ROOT, "uWebSockets")
UWS_REPO = "https://github.com/uNetworking/uWebSockets.git"
USRC_SRC = os.path.join(UWS_DIR, "uSockets", "src")

USOCKET_FILES = ["bsd.c", "context.c", "loop.c", "socket.c", "udp.c"]

DEFINES = ["/DUWS_NO_SSL", "/DUWS_NO_ZLIB", "/DLIBUS_NO_SSL"] if sys.platform == "win32" \
    else ["-DUWS_NO_SSL", "-DUWS_NO_ZLIB", "-DLIBUS_NO_SSL"]

def main():
    if not os.path.isdir(UWS_DIR):
        subprocess.check_call(["git", "clone", "--depth", "1", UWS_REPO, UWS_DIR])
        subprocess.check_call(["git", "submodule", "update", "--init", "--depth", "1", "uSockets"],
                              cwd=UWS_DIR)

    machine = platform.machine().lower()
    arch = "x64" if machine in ("amd64", "x86_64", "arm64") else "x86"

    sources = [os.path.join(USRC_SRC, f) for f in USOCKET_FILES]
    includes = [f"-I{UWS_DIR}/src", f"-I{USRC_SRC}"]
    cxx_flags = ["-std=c++20"] + DEFINES

    if sys.platform == "win32":
        triplet = f"{arch}-windows"
        subprocess.check_call(["vcpkg", "install", f"libuv:{triplet}"])
        vcpkg_root = os.environ.get("VCPKG_ROOT") or os.path.dirname(
            subprocess.check_output(["where", "vcpkg"], text=True).strip())
        sources.append(os.path.join(USRC_SRC, "eventing", "libuv.c"))
        includes.append(f"-I{vcpkg_root}/installed/{triplet}/include")
        out_flag = f"/OUT:{ROOT}/uws_echo.exe"
        link = [f"{vcpkg_root}/installed/{triplet}/lib/uv.lib"]
        subprocess.check_call(
            ["cl.exe", "/std:c++20", "/EHsc"] + DEFINES +
            [f"/I{UWS_DIR}/src", f"/I{USRC_SRC}", f"/I{vcpkg_root}/installed/{triplet}/include",
             os.path.join(HERE, "uws_echo.cpp")] + sources +
            ["/link"] + link + [out_flag])
    else:
        sources.append(os.path.join(USRC_SRC, "eventing", "epoll_kqueue.c"))
        subprocess.check_call(
            ["clang++"] + cxx_flags + includes +
            ["-o", os.path.join(ROOT, "uws_echo"),
             os.path.join(HERE, "uws_echo.cpp")] + sources)

if __name__ == "__main__":
    main()
