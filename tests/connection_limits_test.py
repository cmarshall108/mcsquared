#!/usr/bin/env python3

import socket
import subprocess
import sys
import time


def reserve_port() -> int:
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def connect_when_ready(port: int) -> socket.socket:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1)
        except ConnectionRefusedError:
            time.sleep(0.02)
    raise RuntimeError("server did not begin listening")


def encode_varint(value: int) -> bytes:
    output = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            byte |= 0x80
        output.append(byte)
        if not value:
            return bytes(output)


def frame(packet_id: int, payload: bytes = b"") -> bytes:
    body = encode_varint(packet_id) + payload
    return encode_varint(len(body)) + body


def status_request(port: int) -> bytes:
    address = b"localhost"
    handshake = (
        encode_varint(776)
        + encode_varint(len(address))
        + address
        + port.to_bytes(2, "big")
        + encode_varint(1)
    )
    return frame(0, handshake) + frame(0)


def main() -> None:
    port = reserve_port()
    process = subprocess.Popen(
        [
            sys.argv[1],
            "--port",
            str(port),
            "--max-connections",
            "1",
            "--max-connections-per-ip",
            "1",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    first = None
    try:
        first = connect_when_ready(port)
        with socket.create_connection(("127.0.0.1", port), timeout=1) as rejected:
            rejected.settimeout(1)
            assert rejected.recv(1) == b""
        first.close()
        first = None

        deadline = time.monotonic() + 3
        while True:
            replacement = socket.create_connection(("127.0.0.1", port), timeout=1)
            replacement.sendall(status_request(port))
            try:
                data = replacement.recv(1)
                replacement.close()
                if data == b"":
                    if time.monotonic() >= deadline:
                        raise RuntimeError("connection capacity was not released")
                    time.sleep(0.02)
                    continue
                break
            except ConnectionResetError:
                replacement.close()
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.02)
    finally:
        if first is not None:
            first.close()
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


if __name__ == "__main__":
    main()