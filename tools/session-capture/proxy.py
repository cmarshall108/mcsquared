#!/usr/bin/env python3

import argparse
import json
from pathlib import Path
import selectors
import socket
import time


FORMAT = "mc-session-v1"
MAX_EVENT_BYTES = 64 * 1024


def parse_endpoint(value: str) -> tuple[str, int]:
    host, separator, port_text = value.rpartition(":")
    if not separator or not host:
        raise argparse.ArgumentTypeError("endpoint must use HOST:PORT")
    try:
        port = int(port_text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("endpoint port must be an integer") from error
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("endpoint port is outside 1..65535")
    return host, port


class CaptureWriter:
    def __init__(self, path: Path, listen: tuple[str, int], target: tuple[str, int]):
        self.output = path.open("x", encoding="utf-8")
        self.sequence = 0
        self.offsets = {"serverbound": 0, "clientbound": 0}
        self.write({
            "format": FORMAT,
            "listen": f"{listen[0]}:{listen[1]}",
            "target": f"{target[0]}:{target[1]}",
            "started_unix_ns": time.time_ns(),
        })

    def write(self, value: dict[str, object]) -> None:
        self.output.write(json.dumps(value, separators=(",", ":"), sort_keys=True) + "\n")
        self.output.flush()

    def event(self, direction: str, data: bytes) -> None:
        for start in range(0, len(data), MAX_EVENT_BYTES):
            chunk = data[start:start + MAX_EVENT_BYTES]
            self.write({
                "data": chunk.hex(),
                "direction": direction,
                "offset": self.offsets[direction],
                "sequence": self.sequence,
                "time_ns": time.monotonic_ns(),
            })
            self.offsets[direction] += len(chunk)
            self.sequence += 1

    def close(self) -> None:
        self.write({
            "complete": True,
            "clientbound_bytes": self.offsets["clientbound"],
            "events": self.sequence,
            "serverbound_bytes": self.offsets["serverbound"],
        })
        self.output.close()


def relay(client: socket.socket, server: socket.socket, capture: CaptureWriter) -> None:
    selector = selectors.DefaultSelector()
    peers = {client: (server, "serverbound"), server: (client, "clientbound")}
    for connection in peers:
        connection.setblocking(False)
        selector.register(connection, selectors.EVENT_READ)
    try:
        while selector.get_map():
            for key, _ in selector.select(timeout=30):
                source = key.fileobj
                destination, direction = peers[source]
                data = source.recv(MAX_EVENT_BYTES)
                if not data:
                    selector.unregister(source)
                    destination.shutdown(socket.SHUT_WR)
                    continue
                capture.event(direction, data)
                destination.sendall(data)
    finally:
        selector.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="Capture one Minecraft TCP session")
    parser.add_argument("--listen", type=parse_endpoint, required=True)
    parser.add_argument("--target", type=parse_endpoint, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    listener = socket.create_server(arguments.listen, reuse_port=False)
    try:
        with listener.accept()[0] as client, socket.create_connection(arguments.target) as server:
            capture = CaptureWriter(arguments.output, arguments.listen, arguments.target)
            try:
                relay(client, server, capture)
            finally:
                capture.close()
    finally:
        listener.close()


if __name__ == "__main__":
    main()