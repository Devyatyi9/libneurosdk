#!/usr/bin/env python3
"""Long-run leak detection — many short connections.

Loops echo_test: connect → send → echo → close → destroy.
Each iteration is a full alloc/dealloc cycle. Catches frag_buf,
socket handle, and outstanding allocation leaks under ASan.

Usage:
    python long_run.py [url] [duration_hours] [timeout_sec]

Default: ws://127.0.0.1:9001/  3h  timeout 30s/iter
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

def find_echo_test():
    candidates = [
        os.path.join(ROOT, "build-x86", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build-x64", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build-asan-x86", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build-asan-x64", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build-asan", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build", "Release", "echo_test.exe"),
        os.path.join(ROOT, "build", "echo_test"),
    ]
    for c in candidates:
        p = os.path.abspath(c)
        if os.path.exists(p): return p
    return "echo_test"

def build_env(bin_path):
    """Add MSVC ASan DLL directory to PATH based on binary architecture."""
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

def main():
    url = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:9001/"
    hours = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
    timeout = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    max_fails = 3                              # consecutive HUNG/FAIL before abort
    end = time.time() + hours * 3600
    exe = find_echo_test()

    print(f"Multi-connection long-run: {exe} -> {url}")
    print(f"Duration: {hours}h, timeout: {timeout}s/iter")
    print(f"End: {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(end))}")
    print()

    iterations = 0
    consecutive_fails = 0
    while time.time() < end:
        t0 = time.time()
        try:
            r = subprocess.run([exe, url], timeout=timeout, capture_output=True, env=build_env(exe))
            rc = r.returncode
        except subprocess.TimeoutExpired:
            rc = -1
            status = "HUNG"
        except Exception as e:
            rc = -2
            status = f"EXCEPTION ({e})"
        else:
            status = "OK" if rc == 0 else f"FAIL (rc={rc})"

        elapsed = time.time() - t0
        iterations += 1
        remaining = end - time.time()
        print(f"[{iterations:6d}] {status}  ({elapsed:.1f}s, "
              f"{remaining/3600:.1f}h left)")
        if rc != 0:
            consecutive_fails += 1
            if consecutive_fails >= max_fails:
                print(f"Aborting after {max_fails} consecutive failures")
                sys.exit(1)
        else:
            consecutive_fails = 0

    print(f"\nDone: {iterations} iterations, 0 failures")

if __name__ == '__main__':
    main()
