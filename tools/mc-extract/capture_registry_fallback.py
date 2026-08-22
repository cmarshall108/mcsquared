#!/usr/bin/env python3

import argparse
import hashlib
from pathlib import Path
import socket
import struct
import uuid
import zlib


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


def compressed_frame(packet_id: int, threshold: int, payload: bytes = b"") -> bytes:
    body = encode_varint(packet_id) + payload
    framed = (encode_varint(len(body)) + zlib.compress(body)
              if len(body) >= threshold else b"\x00" + body)
    return encode_varint(len(framed)) + framed


def read_exact(connection: socket.socket, count: int) -> bytes:
    output = bytearray()
    while len(output) < count:
        data = connection.recv(count - len(output))
        if not data:
            raise RuntimeError("official server closed the connection")
        output.extend(data)
    return bytes(output)


def read_socket_varint(connection: socket.socket) -> int:
    value = 0
    for index in range(5):
        byte = read_exact(connection, 1)[0]
        value |= (byte & 0x7F) << (7 * index)
        if byte & 0x80 == 0:
            return value
    raise RuntimeError("oversized socket VarInt")


def read_packet(connection: socket.socket) -> bytes:
    return read_exact(connection, read_socket_varint(connection))


def read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for index in range(5):
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << (7 * index)
        if byte & 0x80 == 0:
            return value, offset
    raise RuntimeError("oversized packet VarInt")


def read_string(data: bytes, offset: int) -> tuple[str, int]:
    size, offset = read_varint(data, offset)
    end = offset + size
    return data[offset:end].decode("utf-8"), end


def read_compressed(connection: socket.socket, threshold: int) -> bytes:
    framed = read_packet(connection)
    data_length, offset = read_varint(framed, 0)
    if data_length == 0:
        return framed[offset:]
    packet = zlib.decompress(framed[offset:])
    if len(packet) != data_length or data_length < threshold:
        raise RuntimeError("invalid compressed packet from official server")
    return packet


def capture(host: str, port: int) -> list[tuple[str, bytes]]:
    with socket.create_connection((host, port), timeout=10) as connection:
        profile_id = uuid.UUID("00112233-4455-6677-8899-aabbccddeeff")
        handshake = (encode_varint(776) + encode_string(host) + struct.pack(">H", port)
                     + encode_varint(2))
        hello = encode_string("FallbackCapture") + profile_id.bytes
        connection.sendall(frame(0, handshake) + frame(0, hello))

        compression = read_packet(connection)
        packet_id, offset = read_varint(compression, 0)
        if packet_id != 3:
            raise RuntimeError(f"expected Set Compression, got {packet_id:#x}")
        threshold, offset = read_varint(compression, offset)
        if offset != len(compression):
            raise RuntimeError("Set Compression contains trailing data")
        finished = read_compressed(connection, threshold)
        if read_varint(finished, 0)[0] != 2:
            raise RuntimeError("expected Login Finished")
        connection.sendall(compressed_frame(3, threshold))

        client_information = (
            encode_string("en_us") + struct.pack(">b", 10) + encode_varint(0)
            + b"\x01\x7f" + encode_varint(1) + b"\x00\x01" + encode_varint(0)
        )
        connection.sendall(compressed_frame(0, threshold, client_information))

        registries = []
        while True:
            packet = read_compressed(connection, threshold)
            packet_id, offset = read_varint(packet, 0)
            if packet_id == 0x0E:
                connection.sendall(compressed_frame(0x07, threshold, b"\x00"))
            elif packet_id == 0x07:
                registry, _ = read_string(packet, offset)
                registries.append((registry, packet))
            elif packet_id == 0x03:
                break
        if len(registries) != 29:
            raise RuntimeError(f"expected 29 Registry Data packets, got {len(registries)}")
        return registries


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="127.0.0.1:25575")
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    host, port_text = arguments.server.rsplit(":", 1)
    registries = capture(host, int(port_text))
    lines = ["MCREGISTRYFALLBACK1\tprotocol=776\tregistries=29"]
    for registry, packet in registries:
        lines.append(
            f"R\t{registry}\t{hashlib.sha256(packet).hexdigest()}\t{packet.hex()}"
        )
    arguments.output.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()