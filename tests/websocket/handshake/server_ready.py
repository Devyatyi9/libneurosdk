"""Non-connecting readiness signal for test servers."""
import os


def mark_server_ready():
    path = os.environ.get("WS_TEST_READY_FILE")
    if path:
        temporary = path + ".tmp"
        with open(temporary, "x", encoding="ascii"):
            pass
        os.replace(temporary, path)
