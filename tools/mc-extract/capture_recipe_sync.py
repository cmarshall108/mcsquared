#!/usr/bin/env python3

import argparse
import hashlib
from pathlib import Path
import socket
import struct
import uuid

from capture_registry_fallback import (
    compressed_frame,
    encode_string,
    encode_varint,
    frame,
    read_compressed,
    read_packet,
    read_varint,
)


def capture(host: str, port: int) -> list[tuple[str, bytes]]:
    with socket.create_connection((host, port), timeout=10) as connection:
        profile_id = uuid.UUID("00112233-4455-6677-8899-aabbccddeeff")
        handshake = (encode_varint(776) + encode_string(host) + struct.pack(">H", port)
                     + encode_varint(2))
        connection.sendall(
            frame(0, handshake)
            + frame(0, encode_string("RecipeCapture") + profile_id.bytes)
        )
        compression = read_packet(connection)
        packet_id, offset = read_varint(compression, 0)
        if packet_id != 3:
            raise RuntimeError("expected Set Compression")
        threshold, _ = read_varint(compression, offset)
        if read_varint(read_compressed(connection, threshold), 0)[0] != 2:
            raise RuntimeError("expected Login Finished")
        connection.sendall(compressed_frame(3, threshold))

        client_information = (
            encode_string("en_us") + struct.pack(">b", 10) + encode_varint(0)
            + b"\x01\x7f" + encode_varint(1) + b"\x00\x01" + encode_varint(0)
        )
        connection.sendall(compressed_frame(0, threshold, client_information))
        while True:
            packet = read_compressed(connection, threshold)
            packet_id, _ = read_varint(packet, 0)
            if packet_id == 0x0E:
                known_pack = (
                    encode_varint(1) + encode_string("minecraft")
                    + encode_string("core") + encode_string("26.2")
                )
                connection.sendall(compressed_frame(0x07, threshold, known_pack))
            elif packet_id == 0x03:
                connection.sendall(compressed_frame(0x03, threshold))
                break

        wanted = {0x4A: "recipe_book_add", 0x85: "update_recipes"}
        captured = {}
        connection.settimeout(10)
        while len(captured) < len(wanted):
            packet = read_compressed(connection, threshold)
            packet_id, _ = read_varint(packet, 0)
            if packet_id in wanted and packet_id not in captured:
                captured[packet_id] = packet
        return [(wanted[packet_id], captured[packet_id]) for packet_id in sorted(captured)]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="127.0.0.1:25575")
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    host, port_text = arguments.server.rsplit(":", 1)
    packets = capture(host, int(port_text))
    lines = [f"MCRECIPESYNC1\tprotocol=776\tpackets={len(packets)}"]
    for name, packet in packets:
        lines.append(f"P\t{name}\t{hashlib.sha256(packet).hexdigest()}\t{packet.hex()}")
    arguments.output.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()