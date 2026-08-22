#!/usr/bin/env python3

import json
import socket
import struct
import subprocess
import sys
import time


def varint(value: int) -> bytes:
    output = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        output.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(output)


def string(value: str) -> bytes:
    data = value.encode()
    return varint(len(data)) + data


def frame(packet_id: int, payload: bytes) -> bytes:
    body = varint(packet_id) + payload
    return varint(len(body)) + body


def read_varint(connection: socket.socket) -> int:
    value = 0
    for index in range(5):
        byte = connection.recv(1)[0]
        value |= (byte & 0x7F) << (index * 7)
        if byte & 0x80 == 0:
            return value
    raise RuntimeError("oversized VarInt")


def main() -> None:
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen(
        [sys.argv[1], "--port", str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        deadline = time.monotonic() + 5
        while True:
            try:
                connection = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.02)
        with connection:
            handshake = varint(775) + string("localhost") + struct.pack(">H", port) + varint(2)
            connection.sendall(frame(0, handshake))
            size = read_varint(connection)
            packet = connection.recv(size)
            assert packet[0] == 0
            length = packet[1]
            reason = json.loads(packet[2:2 + length])
            assert reason == {"text": "This server requires Minecraft 26.2"}
    finally:
        process.terminate()
        process.wait(timeout=3)


if __name__ == "__main__":
    main()