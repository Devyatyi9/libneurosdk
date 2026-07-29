#!/usr/bin/env python3
"""Long-run leak detection -- single long-lived connection.

Runs long_session binary (compiled from long_session.c) which keeps
one WS connection open for hours, periodically sending messages.

Usage:
    python long_session.py [url] [interval_sec] [duration_hours]

Default: ws://127.0.0.1:9001/  60s  3h
"""
import subprocess, sys, time, os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from test_utils import build_env, find_tool

def find_bin():
    return find_tool("long_session")

def main():
    url = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:9001/"
    interval = sys.argv[2] if len(sys.argv) > 2 else "60"
    hours = sys.argv[3] if len(sys.argv) > 3 else "3"
    bin_path = find_bin()

    # Timeout = hours + 5min safety margin
    timeout = int(hours) * 3600 + 300

    print(f"Single-session long-run: {bin_path} -> {url}")
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
