#!/usr/bin/env python3
"""Long-run leak detection — single long-lived connection.

Runs long_session binary (compiled from long_session.c) which keeps
one WS connection open for hours, periodically sending messages.

Usage:
    python long_session.py [url] [interval_sec] [duration_hours]

Default: ws://127.0.0.1:9001/  60s  3h
"""
import subprocess, sys, time, os, glob

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")

def _pe_arch(path):
    """Detect PE machine type: 'x86' or 'x64', or None."""
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

def find_bin():
    candidates = [
        os.path.join(ROOT, "build-x86", "Release", "long_session.exe"),
        os.path.join(ROOT, "build-x64", "Release", "long_session.exe"),
        os.path.join(ROOT, "build-asan-x86", "Release", "long_session.exe"),
        os.path.join(ROOT, "build-asan-x64", "Release", "long_session.exe"),
        os.path.join(ROOT, "build-asan", "Release", "long_session.exe"),
        os.path.join(ROOT, "build", "Release", "long_session.exe"),
        os.path.join(ROOT, "build", "long_session"),
    ]
    for c in candidates:
        p = os.path.abspath(c)
        if os.path.exists(p): return p
    return "long_session"

def build_env(bin_path):
    """Add MSVC ASan DLL directory to PATH based on binary architecture."""
    env = os.environ.copy()
    msvc_root = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC"
    versions = sorted(glob.glob(os.path.join(msvc_root, "*")), reverse=True)

    arch = _pe_arch(bin_path) if os.path.isfile(bin_path) else None
    # default to x86 — if unknown, try both (x64 preferred, then x86 fallback)
    subdirs = ['x64', 'x86'] if arch is None else [{'x86': 'x86', 'x64': 'x64'}[arch]]

    for v in versions:
        for sd in subdirs:
            dll_dir = os.path.join(v, "bin", "Hostx64", sd)
            asan_name = "clang_rt.asan_dynamic-x86_64.dll" if sd == 'x64' else "clang_rt.asan_dynamic-i386.dll"
            if os.path.isfile(os.path.join(dll_dir, asan_name)):
                env["PATH"] = dll_dir + os.pathsep + env["PATH"]
                return env
    return env

def main():
    url = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:9001/"
    interval = sys.argv[2] if len(sys.argv) > 2 else "60"
    hours = sys.argv[3] if len(sys.argv) > 3 else "3"
    bin_path = find_bin()

    # Timeout = hours + 5min safety margin
    timeout = int(hours) * 3600 + 300

    print(f"Single-session long-run: {bin_path} → {url}")
    print(f"Interval: {interval}s, duration: {hours}h, timeout: {timeout}s")
    sys.stdout.flush()

    try:
        r = subprocess.run([bin_path, url, interval, hours], timeout=timeout, env=build_env(bin_path))
        if r.returncode == 0:
            print("Session completed successfully.")
        else:
            print(f"Session FAILED (rc={r.returncode})")
        sys.exit(r.returncode)
    except subprocess.TimeoutExpired:
        print("Session HUNG (timeout expired)")
        sys.exit(1)
    except FileNotFoundError:
        print(f"ERROR: {bin_path} not found. Compile long_session.c first.")
        sys.exit(1)

if __name__ == '__main__':
    main()
