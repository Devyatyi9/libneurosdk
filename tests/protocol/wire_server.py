"""Deterministic Neuro protocol server profile using only the Python stdlib."""

import json
import socket
import sys

sys.path.insert(0, str(__file__).rsplit("tests", 1)[0] + "tests/websocket/network/servers")
from ws_frame import (  # type: ignore[import-not-found]  # noqa: E402
    OPCODE_CLOSE,
    OPCODE_PING,
    OPCODE_TEXT,
    build_close,
    build_pong,
    build_text,
    make_101,
    read_client_frame,
    read_http_upgrade,
)


def receive_json(connection):
    while True:
        final, opcode, payload = read_client_frame(connection)
        if not final:
            raise AssertionError("protocol client fragmented a small JSON message")
        if opcode == OPCODE_PING:
            connection.sendall(build_pong(payload))
            continue
        if opcode == OPCODE_CLOSE:
            raise AssertionError("protocol client closed before completing profile")
        if opcode != OPCODE_TEXT:
            raise AssertionError(f"expected text frame, got opcode {opcode}")
        return json.loads(payload.decode("utf-8"))


def check_registration(message):
    assert message["command"] == "actions/register"
    assert message["game"] == 'Protocol Interop "UTF-8"'
    actions = message["data"]["actions"]
    assert len(actions) == 1
    assert actions[0]["name"] == "echo_text"
    assert actions[0]["description"] == "Echo text supplied by Neuro."
    assert actions[0]["schema"] == {
        "type": "object",
        "properties": {"text": {"type": "string"}},
        "required": ["text"],
    }


def run(port):
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", port))
        listener.listen(1)
        print("READY", flush=True)
        connection, _ = listener.accept()
        with connection:
            _, key = read_http_upgrade(connection)
            connection.sendall(make_101(key))

            startup = receive_json(connection)
            assert startup == {
                "command": "startup",
                "game": 'Protocol Interop "UTF-8"',
            }
            check_registration(receive_json(connection))

            connection.sendall(build_text(json.dumps({
                "command": "startup",
                "data": {"session": {
                    "sessionId": "interop-session",
                    "characterId": "neuro",
                    "displayName": "Neuro-sama",
                }},
            }, separators=(",", ":"))))
            connection.sendall(build_text('{"command":"actions/reregister_all"}'))
            check_registration(receive_json(connection))

            action_id = 'opaque-id-"-\\-fire'
            connection.sendall(build_text(json.dumps({
                "command": "action",
                "data": {
                    "id": action_id,
                    "name": "echo_text",
                    "data": '{"text":"hello"}',
                },
            }, separators=(",", ":"))))
            result = receive_json(connection)
            assert result == {
                "command": "action/result",
                "game": 'Protocol Interop "UTF-8"',
                "data": {
                    "id": action_id,
                    "success": True,
                    "message": "Echoed: hello",
                },
            }
            connection.sendall(build_close())


if __name__ == "__main__":
    run(int(sys.argv[1]))
