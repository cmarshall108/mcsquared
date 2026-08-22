#!/usr/bin/env python3

import json
from pathlib import Path
import struct
import sys


EXPECTED_JAR_SHA256 = "183c0499c5f855570ee487dd38e141a53f0121f83a0b07a3bac2d8b6698823e8"


def read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for index in range(5):
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << (7 * index)
        if byte & 0x80 == 0:
            return value, offset
    raise AssertionError("fixture contains an oversized VarInt")


def read_string(data: bytes, offset: int) -> tuple[str, int]:
    size, offset = read_varint(data, offset)
    end = offset + size
    return data[offset:end].decode("utf-8"), end


def split_frames(data: bytes) -> list[bytes]:
    frames = []
    offset = 0
    while offset < len(data):
        size, offset = read_varint(data, offset)
        end = offset + size
        assert end <= len(data)
        frames.append(data[offset:end])
        offset = end
    return frames


def main() -> None:
    records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
    assert records[0] == {
        "format": "mc-session-v1",
        "server": "official-minecraft-26.2",
        "server_jar_sha256": EXPECTED_JAR_SHA256,
    }
    footer = records[-1]
    assert footer == {
        "clientbound_bytes": 117,
        "complete": True,
        "events": 4,
        "serverbound_bytes": 29,
    }
    events = records[1:-1]
    assert [event["sequence"] for event in events] == list(range(len(events)))

    streams = {}
    for direction in ("serverbound", "clientbound"):
        directional = [event for event in events if event["direction"] == direction]
        expected_offset = 0
        stream = bytearray()
        for event in directional:
            assert event["offset"] == expected_offset
            chunk = bytes.fromhex(event["data"])
            stream.extend(chunk)
            expected_offset += len(chunk)
        streams[direction] = bytes(stream)

    handshake, status_request, ping_request = split_frames(streams["serverbound"])
    packet_id, offset = read_varint(handshake, 0)
    protocol, offset = read_varint(handshake, offset)
    address, offset = read_string(handshake, offset)
    port = struct.unpack(">H", handshake[offset:offset + 2])[0]
    offset += 2
    intention, offset = read_varint(handshake, offset)
    assert (packet_id, protocol, address, port, intention, offset) == (
        0, 776, "localhost", 25576, 1, len(handshake)
    )
    assert status_request == b"\x00"
    assert ping_request[0] == 1

    status_response, ping_response = split_frames(streams["clientbound"])
    packet_id, offset = read_varint(status_response, 0)
    status_text, offset = read_string(status_response, offset)
    status = json.loads(status_text)
    assert packet_id == 0
    assert offset == len(status_response)
    assert status == {
        "description": "Official 26.2",
        "players": {"max": 20, "online": 0},
        "version": {"name": "26.2", "protocol": 776},
    }
    assert ping_response == ping_request


if __name__ == "__main__":
    main()