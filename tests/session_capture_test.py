#!/usr/bin/env python3

import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time


def reserve_port() -> int:
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def main() -> None:
    proxy_path = Path(sys.argv[1])
    backend_port = reserve_port()
    proxy_port = reserve_port()
    received = bytearray()

    def backend() -> None:
        with socket.create_server(("127.0.0.1", backend_port)) as listener:
            with listener.accept()[0] as connection:
                while True:
                    data = connection.recv(3)
                    if not data:
                        break
                    received.extend(data)
                    connection.sendall(data.upper())

    backend_thread = threading.Thread(target=backend)
    backend_thread.start()
    with tempfile.TemporaryDirectory() as temporary_directory:
        capture_path = Path(temporary_directory) / "capture.ndjson"
        proxy = subprocess.Popen([
            sys.executable,
            str(proxy_path),
            "--listen", f"127.0.0.1:{proxy_port}",
            "--target", f"127.0.0.1:{backend_port}",
            "--output", str(capture_path),
        ])
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    connection = socket.create_connection(("127.0.0.1", proxy_port), timeout=1)
                    break
                except ConnectionRefusedError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)
            with connection:
                connection.sendall(b"minecraft-session")
                connection.shutdown(socket.SHUT_WR)
                response = bytearray()
                while data := connection.recv(4):
                    response.extend(data)
            assert response == b"MINECRAFT-SESSION"
            assert proxy.wait(timeout=5) == 0
        finally:
            if proxy.poll() is None:
                proxy.terminate()
                proxy.wait(timeout=3)
        backend_thread.join(timeout=3)
        assert not backend_thread.is_alive()
        assert received == b"minecraft-session"

        records = [json.loads(line) for line in capture_path.read_text().splitlines()]
        assert records[0]["format"] == "mc-session-v1"
        assert records[-1]["complete"] is True
        events = [record for record in records if "direction" in record]
        assert [event["sequence"] for event in events] == list(range(len(events)))
        for direction, expected in (
            ("serverbound", b"minecraft-session"),
            ("clientbound", b"MINECRAFT-SESSION"),
        ):
            directional = [event for event in events if event["direction"] == direction]
            assert [event["offset"] for event in directional] == [
                sum(len(bytes.fromhex(previous["data"])) for previous in directional[:index])
                for index in range(len(directional))
            ]
            assert b"".join(bytes.fromhex(event["data"]) for event in directional) == expected


if __name__ == "__main__":
    main()