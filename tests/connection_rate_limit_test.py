#!/usr/bin/env python3

import json
import socket
import struct
import subprocess
import sys
import time


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


def handshake(port: int, state: int) -> bytes:
    address = b"localhost"
    return frame(0, encode_varint(776) + encode_varint(len(address)) + address +
                 struct.pack(">H", port) + encode_varint(state))


def read_varint(connection: socket.socket) -> int:
    value = 0
    for index in range(5):
        byte = connection.recv(1)[0]
        value |= (byte & 0x7F) << (7 * index)
        if byte & 0x80 == 0:
            return value
    raise AssertionError("oversized VarInt")


def reserve_port() -> int:
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def main() -> None:
    port = reserve_port()
    process = subprocess.Popen([
        sys.argv[1], "--port", str(port), "--connection-rate-limit", "1",
    ])
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
        with first:
            first.sendall(handshake(port, 1) + frame(0))
            status = first.recv(read_varint(first))
            assert status[0] == 0
            first.sendall(frame(1, struct.pack(">q", 7)))
            assert first.recv(read_varint(first)) == b"\x01" + struct.pack(">q", 7)

        with socket.create_connection(("127.0.0.1", port), timeout=1) as limited:
            limited.sendall(handshake(port, 2))
            disconnect = limited.recv(read_varint(limited))
            assert disconnect[0] == 0
            text_size = disconnect[1]
            reason = json.loads(disconnect[2:2 + text_size])
            assert reason == {"text": "Connection rate limit exceeded"}
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


if __name__ == "__main__":
    main()