#!/usr/bin/env python3
"""Long-run leak detection -- many short connections.

Loops echo_test: connect -> send -> echo -> close -> destroy.
Each iteration is a full alloc/dealloc cycle. Catches frag_buf,
socket handle, and outstanding allocation leaks under ASan.

Usage:
    python long_run.py [url] [duration_hours] [timeout_sec]

Default: ws://127.0.0.1:9001/  3h  timeout 30s/iter
"""
import subprocess, sys, time, os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from test_utils import build_env, find_echo_test

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
