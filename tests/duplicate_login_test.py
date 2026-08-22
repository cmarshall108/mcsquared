#!/usr/bin/env python3

import json
import socket
import struct
import subprocess
import sys
import time
import uuid


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


def frame(packet_id: int, payload: bytes = b"") -> bytes:
    body = varint(packet_id) + payload
    return varint(len(body)) + body


def read_exact(connection: socket.socket, size: int) -> bytes:
    output = bytearray()
    while len(output) < size:
        data = connection.recv(size - len(output))
        if not data:
            raise RuntimeError("connection closed")
        output.extend(data)
    return bytes(output)


def read_varint(connection: socket.socket) -> int:
    value = 0
    for index in range(5):
        byte = read_exact(connection, 1)[0]
        value |= (byte & 0x7F) << (index * 7)
        if byte & 0x80 == 0:
            return value
    raise RuntimeError("oversized VarInt")


def read_packet(connection: socket.socket) -> bytes:
    return read_exact(connection, read_varint(connection))


def login_start(port: int) -> bytes:
    handshake = varint(776) + string("localhost") + struct.pack(">H", port) + varint(2)
    hello = string("Duplicate") + uuid.UUID("00112233-4455-6677-8899-aabbccddeeff").bytes
    return frame(0, handshake) + frame(0, hello)


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
    first = second = None
    try:
        deadline = time.monotonic() + 5
        while True:
            try:
                first = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.02)
        first.sendall(login_start(port))
        assert read_packet(first)[0] == 3
        read_packet(first)

        second = socket.create_connection(("127.0.0.1", port), timeout=1)
        second.sendall(login_start(port))
        disconnect = read_packet(second)
        assert disconnect[0] == 0
        reason_length = disconnect[1]
        reason = json.loads(disconnect[2:2 + reason_length])
        assert "already connected" in reason["text"]

        first.close()
        first = None
    finally:
        if first:
            first.close()
        if second:
            second.close()
        process.terminate()
        process.wait(timeout=3)


if __name__ == "__main__":
    main()