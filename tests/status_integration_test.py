#!/usr/bin/env python3

import json
import socket
import struct
import subprocess
import sys
import time


def encode_varint(value: int) -> bytes:
    value &= 0xFFFFFFFF
    output = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            byte |= 0x80
        output.append(byte)
        if not value:
            return bytes(output)


def encode_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return encode_varint(len(encoded)) + encoded


def frame(packet_id: int, payload: bytes = b"") -> bytes:
    body = encode_varint(packet_id) + payload
    return encode_varint(len(body)) + body


def read_exact(connection: socket.socket, count: int) -> bytes:
    output = bytearray()
    while len(output) < count:
        chunk = connection.recv(count - len(output))
        if not chunk:
            raise RuntimeError("server closed the connection")
        output.extend(chunk)
    return bytes(output)


def read_varint(connection: socket.socket) -> int:
    value = 0
    for index in range(5):
        byte = read_exact(connection, 1)[0]
        value |= (byte & 0x7F) << (7 * index)
        if byte & 0x80 == 0:
            return value
    raise RuntimeError("server sent an oversized VarInt")


def read_packet(connection: socket.socket) -> bytes:
    return read_exact(connection, read_varint(connection))


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


def main() -> None:
    port = reserve_port()
    process = subprocess.Popen(
        [sys.argv[1], "--port", str(port), "--motd", 'C++ "status"'],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        with connect_when_ready(port) as connection:
            handshake = (
                encode_varint(776)
                + encode_string("localhost")
                + struct.pack(">H", port)
                + encode_varint(1)
            )
            request = frame(0, handshake) + frame(0)
            for byte in request:
                connection.sendall(bytes([byte]))

            response = read_packet(connection)
            assert response[0] == 0
            offset = 1
            length = 0
            for index in range(5):
                byte = response[offset]
                offset += 1
                length |= (byte & 0x7F) << (7 * index)
                if byte & 0x80 == 0:
                    break
            status = json.loads(response[offset:offset + length])
            assert status["version"] == {"name": "26.2", "protocol": 776}
            assert status["description"] == {"text": 'C++ "status"'}
            assert status["players"]["online"] == 0

            timestamp = -123_456_789_012_345
            connection.sendall(frame(1, struct.pack(">q", timestamp)))
            assert read_packet(connection) == b"\x01" + struct.pack(">q", timestamp)
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


if __name__ == "__main__":
    main()