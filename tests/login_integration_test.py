#!/usr/bin/env python3

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
import uuid
import zlib
from threading import Thread
from urllib.parse import parse_qs, urlparse


AUTHENTICATED_PROFILE_ID = uuid.UUID("12345678-1234-5678-1234-567812345678")


class SessionHandler(BaseHTTPRequestHandler):
    request_parameters = None

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        parameters = parse_qs(parsed.query)
        SessionHandler.request_parameters = parameters
        if (
            parsed.path != "/session/minecraft/hasJoined"
            or parameters.get("username") != ["CppPlayer"]
            or not parameters.get("serverId", [""])[0]
        ):
            self.send_response(400)
            self.end_headers()
            return
        body = json.dumps(
            {
                "id": AUTHENTICATED_PROFILE_ID.hex,
                "name": "CppPlayer",
                "properties": [
                    {
                        "name": "textures",
                        "value": "texture-value",
                        "signature": "texture-signature",
                    }
                ],
            }
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args) -> None:
        pass


class ProtocolConnection:
    def __init__(self, connection: socket.socket) -> None:
        self.connection = connection
        self.encryptor = None
        self.decryptor = None

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception, traceback):
        self.connection.close()

    def sendall(self, data: bytes) -> None:
        if self.encryptor:
            data = self.encryptor.update(data)
        self.connection.sendall(data)

    def recv(self, count: int) -> bytes:
        data = self.connection.recv(count)
        if self.decryptor and data:
            data = self.decryptor.update(data)
        return data

    def enable_encryption(self, shared_secret: bytes) -> None:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
        try:
            from cryptography.hazmat.decrepit.ciphers.modes import CFB8
        except ImportError:
            from cryptography.hazmat.primitives.ciphers.modes import CFB8

        self.encryptor = Cipher(
            algorithms.AES(shared_secret), CFB8(shared_secret)
        ).encryptor()
        self.decryptor = Cipher(
            algorithms.AES(shared_secret), CFB8(shared_secret)
        ).decryptor()


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


def encode_byte_array(value: bytes) -> bytes:
    return encode_varint(len(value)) + value


def encode_position(x: int, y: int, z: int) -> bytes:
    packed = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
    return struct.pack(">Q", packed)


def frame(packet_id: int, payload: bytes = b"") -> bytes:
    body = encode_varint(packet_id) + payload
    return encode_varint(len(body)) + body


def compressed_frame(packet_id: int, threshold: int, payload: bytes = b"") -> bytes:
    body = encode_varint(packet_id) + payload
    if len(body) >= threshold:
        framed_payload = encode_varint(len(body)) + zlib.compress(body)
    else:
        framed_payload = b"\x00" + body
    return encode_varint(len(framed_payload)) + framed_payload


def read_exact(connection: ProtocolConnection, count: int) -> bytes:
    output = bytearray()
    while len(output) < count:
        chunk = connection.recv(count - len(output))
        if not chunk:
            raise RuntimeError("server closed the connection")
        output.extend(chunk)
    return bytes(output)


def read_varint_from_socket(connection: ProtocolConnection) -> int:
    value = 0
    for index in range(5):
        byte = read_exact(connection, 1)[0]
        value |= (byte & 0x7F) << (7 * index)
        if byte & 0x80 == 0:
            return value
    raise RuntimeError("server sent an oversized VarInt")


def read_packet(connection: ProtocolConnection) -> bytes:
    return read_exact(connection, read_varint_from_socket(connection))


def read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for index in range(5):
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << (7 * index)
        if byte & 0x80 == 0:
            return value, offset
    raise RuntimeError("packet contains an oversized VarInt")


def read_compressed_packet(connection: ProtocolConnection, threshold: int) -> bytes:
    framed_payload = read_packet(connection)
    data_length, offset = read_varint(framed_payload, 0)
    if data_length == 0:
        assert len(framed_payload) - offset < threshold
        return framed_payload[offset:]
    assert data_length >= threshold
    packet = zlib.decompress(framed_payload[offset:])
    assert len(packet) == data_length
    return packet


def read_string(data: bytes, offset: int) -> tuple[str, int]:
    length, offset = read_varint(data, offset)
    end = offset + length
    return data[offset:end].decode("utf-8"), end


def read_byte_array(data: bytes, offset: int) -> tuple[bytes, int]:
    length, offset = read_varint(data, offset)
    end = offset + length
    return data[offset:end], end


def read_paletted_container(
    data: bytes, offset: int, entry_count: int
) -> tuple[list[int], list[int], int]:
    bits = data[offset]
    offset += 1
    if bits == 0:
        value, offset = read_varint(data, offset)
        return [value] * entry_count, [value], offset

    palette_size, offset = read_varint(data, offset)
    palette = []
    for _ in range(palette_size):
        entry, offset = read_varint(data, offset)
        palette.append(entry)
    values_per_word = 64 // bits
    word_count = (entry_count + values_per_word - 1) // values_per_word
    values = []
    mask = (1 << bits) - 1
    for _ in range(word_count):
        word = struct.unpack(">Q", data[offset:offset + 8])[0]
        offset += 8
        for value_index in range(values_per_word):
            if len(values) == entry_count:
                break
            palette_index = (word >> (value_index * bits)) & mask
            assert palette_index < len(palette)
            values.append(palette[palette_index])
    return values, palette, offset


def encode_known_pack() -> bytes:
    return encode_string("minecraft") + encode_string("core") + encode_string("26.2")


def reserve_port() -> int:
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def connect_when_ready(port: int) -> ProtocolConnection:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            return ProtocolConnection(
                socket.create_connection(("127.0.0.1", port), timeout=1)
            )
        except ConnectionRefusedError:
            time.sleep(0.02)
    raise RuntimeError("server did not begin listening")


def main() -> None:
    port = reserve_port()
    options = set(sys.argv[2:])
    encrypted_offline = "--encrypted-offline" in options
    online_mode = "--online-mode" in options
    hardcore = "--hardcore" in options
    custom_query = "--custom-query" in options
    verify_delayed_keepalive = "--verify-delayed-keepalive" in options
    transfer = "--transfer" in options
    reject_known_pack = "--reject-known-pack" in options
    required_resource_pack = "--required-resource-pack" in options
    fallback_manifest = None
    if "--fallback-manifest" in sys.argv:
        fallback_manifest = Path(sys.argv[sys.argv.index("--fallback-manifest") + 1])
    command = [sys.argv[1], "--port", str(port)]
    if not verify_delayed_keepalive:
        command.extend(
            [
                "--keepalive-interval-ms",
                "10",
                "--keepalive-timeout-ms",
                "5000",
            ]
        )
    if encrypted_offline:
        command.append("--encrypted-offline")
    if hardcore:
        command.append("--hardcore")
    session_server = None
    session_thread = None
    if online_mode:
        session_server = ThreadingHTTPServer(("127.0.0.1", 0), SessionHandler)
        session_thread = Thread(target=session_server.serve_forever, daemon=True)
        session_thread.start()
        session_port = session_server.server_address[1]
        command.extend(
            [
                "--online-mode",
                "--session-server-url",
                f"http://127.0.0.1:{session_port}/session/minecraft/hasJoined",
            ]
        )
    if custom_query:
        command.extend(
            [
                "--login-query-channel",
                "mcsquared:test",
                "--login-query-payload",
                "challenge",
                "--require-login-query-response",
            ]
        )
    if required_resource_pack:
        command.extend(
            [
                "--resource-pack-url",
                "https://example.test/pack.zip",
                "--resource-pack-hash",
                "a" * 40,
                "--require-resource-pack",
            ]
        )
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        with connect_when_ready(port) as connection:
            profile_id = uuid.UUID("00112233-4455-6677-8899-aabbccddeeff")
            handshake = (
                encode_varint(776)
                + encode_string("localhost")
                + struct.pack(">H", port)
                + encode_varint(3 if transfer else 2)
            )
            hello = encode_string("CppPlayer") + profile_id.bytes
            connection.sendall(frame(0, handshake) + frame(0, hello))

            if transfer:
                cookie_request = read_packet(connection)
                packet_id, offset = read_varint(cookie_request, 0)
                key, offset = read_string(cookie_request, offset)
                assert (packet_id, key, offset) == (5, "mcsquared:transfer", len(cookie_request))
                connection.sendall(frame(4, encode_string(key) + b"\x00"))

            if encrypted_offline or online_mode:
                from cryptography.hazmat.primitives import serialization
                from cryptography.hazmat.primitives.asymmetric import padding

                encryption_request = read_packet(connection)
                packet_id, offset = read_varint(encryption_request, 0)
                server_id, offset = read_string(encryption_request, offset)
                public_key_der, offset = read_byte_array(encryption_request, offset)
                challenge, offset = read_byte_array(encryption_request, offset)
                should_authenticate = encryption_request[offset]
                assert (packet_id, server_id, should_authenticate, offset + 1) == (
                    1, "", int(online_mode), len(encryption_request)
                )
                public_key = serialization.load_der_public_key(public_key_der)
                shared_secret = bytes(range(16))
                encrypted_secret = public_key.encrypt(shared_secret, padding.PKCS1v15())
                encrypted_challenge = public_key.encrypt(challenge, padding.PKCS1v15())
                connection.sendall(
                    frame(
                        1,
                        encode_byte_array(encrypted_secret)
                        + encode_byte_array(encrypted_challenge),
                    )
                )
                connection.enable_encryption(shared_secret)

            if custom_query:
                query = read_packet(connection)
                packet_id, offset = read_varint(query, 0)
                transaction_id, offset = read_varint(query, offset)
                channel, offset = read_string(query, offset)
                assert (packet_id, transaction_id, channel) == (4, 0, "mcsquared:test")
                assert query[offset:] == b"challenge"
                connection.sendall(frame(2, encode_varint(transaction_id) + b"\x01response"))

            compression = read_packet(connection)
            packet_id, offset = read_varint(compression, 0)
            assert packet_id == 3
            threshold, offset = read_varint(compression, offset)
            assert threshold == 256
            assert offset == len(compression)

            finished = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(finished, 0)
            assert packet_id == 2
            expected_profile_id = (
                AUTHENTICATED_PROFILE_ID
                if online_mode
                else uuid.UUID("c71adf29-5a8a-32a1-95a9-93381f0058be")
            )
            assert uuid.UUID(bytes=finished[offset:offset + 16]) == expected_profile_id
            offset += 16
            name, offset = read_string(finished, offset)
            assert name == "CppPlayer"
            property_count, offset = read_varint(finished, offset)
            assert property_count == int(online_mode)
            if online_mode:
                property_name, offset = read_string(finished, offset)
                property_value, offset = read_string(finished, offset)
                signed = finished[offset]
                offset += 1
                signature, offset = read_string(finished, offset)
                assert (property_name, property_value, signed, signature) == (
                    "textures",
                    "texture-value",
                    1,
                    "texture-signature",
                )
            session_id = uuid.UUID(bytes=finished[offset:offset + 16])
            assert session_id.version == 4
            assert session_id.variant == uuid.RFC_4122
            assert offset + 16 == len(finished)

            connection.sendall(compressed_frame(3, threshold))

            brand = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(brand, 0)
            assert packet_id == 1
            channel, offset = read_string(brand, offset)
            value, offset = read_string(brand, offset)
            assert (channel, value, offset) == ("minecraft:brand", "mcsquared", len(brand))

            features = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(features, 0)
            count, offset = read_varint(features, offset)
            feature, offset = read_string(features, offset)
            assert (packet_id, count, feature, offset) == (12, 1, "minecraft:vanilla", len(features))

            known_packs = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(known_packs, 0)
            count, offset = read_varint(known_packs, offset)
            namespace, offset = read_string(known_packs, offset)
            pack_id, offset = read_string(known_packs, offset)
            version, offset = read_string(known_packs, offset)
            assert (packet_id, count, namespace, pack_id, version, offset) == (
                14, 1, "minecraft", "core", "26.2", len(known_packs)
            )
            client_information = (
                encode_string("en_us")
                + struct.pack(">b", 10)
                + encode_varint(0)
                + b"\x01\x7f"
                + encode_varint(1)
                + b"\x00\x01"
                + encode_varint(0)
            )
            connection.sendall(compressed_frame(0, threshold, client_information))
            selected_packs = (
                encode_varint(0)
                if reject_known_pack
                else encode_varint(1) + encode_known_pack()
            )
            connection.sendall(compressed_frame(7, threshold, selected_packs))

            if required_resource_pack:
                resource_pack = read_compressed_packet(connection, threshold)
                packet_id, offset = read_varint(resource_pack, 0)
                resource_pack_id = resource_pack[offset:offset + 16]
                offset += 16
                resource_pack_url, offset = read_string(resource_pack, offset)
                resource_pack_hash, offset = read_string(resource_pack, offset)
                required = resource_pack[offset]
                prompt_present = resource_pack[offset + 1]
                offset += 2
                assert (
                    packet_id,
                    resource_pack_url,
                    resource_pack_hash,
                    required,
                    prompt_present,
                    offset,
                ) == (
                    0x09,
                    "https://example.test/pack.zip",
                    "a" * 40,
                    1,
                    0,
                    len(resource_pack),
                )
                connection.sendall(compressed_frame(
                    0x06, threshold, resource_pack_id + encode_varint(3)
                ))
                connection.sendall(compressed_frame(
                    0x06, threshold, resource_pack_id + encode_varint(0)
                ))

            decoded_registries = {}
            expected_fallback_packets = []
            if reject_known_pack:
                assert fallback_manifest is not None
                for line in fallback_manifest.read_text().splitlines()[1:]:
                    fields = line.split("\t")
                    assert len(fields) == 4 and fields[0] == "R"
                    expected_fallback_packets.append(bytes.fromhex(fields[3]))
                assert len(expected_fallback_packets) == 29
            for registry_index in range(29):
                registry_packet = read_compressed_packet(connection, threshold)
                if reject_known_pack:
                    assert registry_packet == expected_fallback_packets[registry_index]
                    continue
                packet_id, offset = read_varint(registry_packet, 0)
                registry, offset = read_string(registry_packet, offset)
                entry_count, offset = read_varint(registry_packet, offset)
                entries = []
                for _ in range(entry_count):
                    entry, offset = read_string(registry_packet, offset)
                    has_data = registry_packet[offset]
                    offset += 1
                    assert has_data == 0
                    entries.append(entry)
                assert packet_id == 0x07
                assert offset == len(registry_packet)
                decoded_registries[registry] = entries
            if not reject_known_pack:
                assert decoded_registries["minecraft:dimension_type"] == [
                    "minecraft:overworld",
                    "minecraft:the_nether",
                    "minecraft:the_end",
                    "minecraft:overworld_caves",
                ]
                assert decoded_registries["minecraft:damage_type"][:7] == [
                    "minecraft:in_fire",
                    "minecraft:campfire",
                    "minecraft:lightning_bolt",
                    "minecraft:on_fire",
                    "minecraft:lava",
                    "minecraft:hot_floor",
                    "minecraft:sulfur_cube_hot",
                ]

            tags = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(tags, 0)
            registry_count, offset = read_varint(tags, offset)
            decoded_tags = {}
            for _ in range(registry_count):
                registry, offset = read_string(tags, offset)
                tag_count, offset = read_varint(tags, offset)
                registry_tags = {}
                for _ in range(tag_count):
                    tag, offset = read_string(tags, offset)
                    member_count, offset = read_varint(tags, offset)
                    members = []
                    for _ in range(member_count):
                        member, offset = read_varint(tags, offset)
                        members.append(member)
                    registry_tags[tag] = members
                decoded_tags[registry] = registry_tags
            assert packet_id == 0x0D
            assert offset == len(tags)
            assert decoded_tags["minecraft:damage_type"]["minecraft:is_fire"] == [
                0, 1, 3, 4, 5, 6, 39, 38
            ]
            assert read_compressed_packet(connection, threshold) == b"\x03"
            connection.sendall(compressed_frame(3, threshold))

            play_login = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(play_login, 0)
            assert packet_id == 0x31
            assert struct.unpack(">i", play_login[offset:offset + 4])[0] == 1
            offset += 4
            assert play_login[offset] == int(hardcore)
            offset += 1
            level_count, offset = read_varint(play_login, offset)
            level, offset = read_string(play_login, offset)
            assert (level_count, level) == (1, "minecraft:overworld")
            max_players, offset = read_varint(play_login, offset)
            chunk_radius, offset = read_varint(play_login, offset)
            simulation_distance, offset = read_varint(play_login, offset)
            assert (max_players, chunk_radius, simulation_distance) == (20, 10, 10)
            assert play_login[offset:offset + 3] == b"\x00\x01\x00"
            offset += 3
            dimension_type, offset = read_varint(play_login, offset)
            dimension, offset = read_string(play_login, offset)
            assert (dimension_type, dimension) == (0, "minecraft:overworld")
            assert struct.unpack(">q", play_login[offset:offset + 8])[0] == 0
            offset += 8
            assert play_login[offset:offset + 5] == b"\x00\xff\x00\x00\x00"
            offset += 5
            portal_cooldown, offset = read_varint(play_login, offset)
            sea_level, offset = read_varint(play_login, offset)
            assert (portal_cooldown, sea_level) == (0, 63)
            assert play_login[offset:offset + 2] == b"\x00\x00"
            assert offset + 2 == len(play_login)

            player_info = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(player_info, 0)
            actions = player_info[offset]
            offset += 1
            entry_count, offset = read_varint(player_info, offset)
            player_id = uuid.UUID(bytes=player_info[offset:offset + 16])
            offset += 16
            player_name, offset = read_string(player_info, offset)
            property_count, offset = read_varint(player_info, offset)
            for _ in range(property_count):
                _, offset = read_string(player_info, offset)
                _, offset = read_string(player_info, offset)
                signed = player_info[offset]
                offset += 1
                if signed:
                    _, offset = read_string(player_info, offset)
            no_chat_session = player_info[offset]
            offset += 1
            game_mode, offset = read_varint(player_info, offset)
            listed = player_info[offset]
            offset += 1
            latency, offset = read_varint(player_info, offset)
            no_display_name = player_info[offset]
            offset += 1
            list_order, offset = read_varint(player_info, offset)
            show_hat = player_info[offset]
            offset += 1
            assert (packet_id, actions, entry_count, player_id, player_name,
                    no_chat_session, game_mode, listed, latency,
                    no_display_name, list_order, show_hat, offset) == (
                0x46, 0xFF, 1, expected_profile_id, "CppPlayer",
                0, 0, 1, 0, 0, 0, 1, len(player_info)
            )

            recipe_book_add = read_compressed_packet(connection, threshold)
            assert recipe_book_add == b"\x4a\x00\x01"
            update_recipes = read_compressed_packet(connection, threshold)
            packet_id, _ = read_varint(update_recipes, 0)
            assert packet_id == 0x85
            assert len(update_recipes) > 100

            inventory = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(inventory, 0)
            container_id, offset = read_varint(inventory, offset)
            state_id, offset = read_varint(inventory, offset)
            slot_count, offset = read_varint(inventory, offset)
            expected_hotbar = {
                36: (64, 55),
                37: (16, 980),
                38: (1, 941),
                39: (16, 981),
                40: (1, 922),
                41: (32, 923),
                42: (1, 1325),
                43: (16, 1112),
            }
            for slot in range(slot_count):
                count, offset = read_varint(inventory, offset)
                if slot in expected_hotbar:
                    item_id, offset = read_varint(inventory, offset)
                    added_components, offset = read_varint(inventory, offset)
                    removed_components, offset = read_varint(inventory, offset)
                    expected_count, expected_item_id = expected_hotbar[slot]
                    assert (count, item_id, added_components, removed_components) == (
                        expected_count, expected_item_id, 0, 0
                    )
                else:
                    assert count == 0
            carried_count, offset = read_varint(inventory, offset)
            assert (packet_id, container_id, state_id, slot_count,
                    carried_count, offset) == (
                0x12, 0, 0, 46, 0, len(inventory)
            )

            difficulty = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(difficulty, 0)
            difficulty_id, offset = read_varint(difficulty, offset)
            locked = difficulty[offset]
            offset += 1
            assert (packet_id, difficulty_id, locked, offset) == (
                0x0A, 3 if hardcore else 2, int(hardcore), len(difficulty)
            )

            ticking = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(ticking, 0)
            tick_rate = struct.unpack(">f", ticking[offset:offset + 4])[0]
            frozen = ticking[offset + 4]
            offset += 5
            assert (packet_id, tick_rate, frozen, offset) == (
                0x7F, 20.0, 0, len(ticking)
            )

            game_rules = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(game_rules, 0)
            game_rule_count, offset = read_varint(game_rules, offset)
            decoded_game_rules = {}
            for _ in range(game_rule_count):
                game_rule, offset = read_string(game_rules, offset)
                value, offset = read_string(game_rules, offset)
                decoded_game_rules[game_rule] = value
            assert packet_id == 0x27
            assert decoded_game_rules == {
                "minecraft:advance_time": "true",
                "minecraft:advance_weather": "true",
                "minecraft:keep_inventory": "false",
                "minecraft:spawn_mobs": "true",
            }
            assert offset == len(game_rules)

            state_packet_ids = []
            for _ in range(10):
                state_packet = read_compressed_packet(connection, threshold)
                state_packet_id, _ = read_varint(state_packet, 0)
                state_packet_ids.append(state_packet_id)
            assert state_packet_ids == [
                0x2B, 0x40, 0x5F, 0x67, 0x68, 0x69, 0x71, 0x26, 0x26, 0x26
            ]

            cache_center = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(cache_center, 0)
            chunk_x, offset = read_varint(cache_center, offset)
            chunk_z, offset = read_varint(cache_center, offset)
            assert (packet_id, chunk_x, chunk_z, offset) == (
                0x5E, 0, 0, len(cache_center)
            )

            default_spawn = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(default_spawn, 0)
            dimension, offset = read_string(default_spawn, offset)
            packed_position = struct.unpack(">q", default_spawn[offset:offset + 8])[0]
            offset += 8
            yaw, pitch = struct.unpack(">ff", default_spawn[offset:offset + 8])
            offset += 8
            spawn_y = packed_position & 0xFFF
            if spawn_y >= 0x800:
                spawn_y -= 0x1000
            assert (packet_id, dimension, packed_position >> 38, (packed_position >> 12) & 0x3FFFFFF,
                    yaw, pitch, offset) == (
                0x61,
                "minecraft:overworld",
                8,
                8,
                0.0,
                0.0,
                len(default_spawn),
            )
            assert 40 <= spawn_y <= 100

            position = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(position, 0)
            teleport_id, offset = read_varint(position, offset)
            values = struct.unpack(">ddddddffI", position[offset:offset + 60])
            offset += 60
            assert packet_id == 0x48
            assert teleport_id == 1
            assert values == (8.5, float(spawn_y), 8.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0)
            assert offset == len(position)
            load_start = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(load_start, 0)
            event_id = load_start[offset]
            parameter = struct.unpack(">f", load_start[offset + 1:offset + 5])[0]
            offset += 5
            assert (packet_id, event_id, parameter, offset) == (
                0x26, 13, 0.0, len(load_start)
            )
            assert read_compressed_packet(connection, threshold) == b"\x0c"

            chunk = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(chunk, 0)
            chunk_x, chunk_z = struct.unpack(">ii", chunk[offset:offset + 8])
            offset += 8
            heightmap_count, offset = read_varint(chunk, offset)
            heightmaps = {}
            for _ in range(heightmap_count):
                heightmap_type, offset = read_varint(chunk, offset)
                word_count, offset = read_varint(chunk, offset)
                words = []
                for _ in range(word_count):
                    words.append(struct.unpack(">Q", chunk[offset:offset + 8])[0])
                    offset += 8
                heightmaps[heightmap_type] = words
            section_data_size, offset = read_varint(chunk, offset)
            section_end = offset + section_data_size
            assert (packet_id, chunk_x, chunk_z, heightmap_count) == (0x2D, 0, 0, 3)
            assert set(heightmaps) == {1, 4, 5}
            decoded_heightmaps = []
            for words in heightmaps.values():
                assert len(words) == 37
                decoded_heights = []
                for word in words:
                    for value_index in range(7):
                        decoded_heights.append((word >> (value_index * 9)) & 0x1FF)
                decoded_heightmaps.append(decoded_heights[:256])
            assert decoded_heightmaps[0] == decoded_heightmaps[1] == decoded_heightmaps[2]
            assert len(set(decoded_heightmaps[0])) > 1
            assert decoded_heightmaps[0][8 * 16 + 8] == spawn_y + 64
            mixed_section_count = 0
            decoded_block_states = set()
            decoded_biomes = set()
            for section in range(24):
                non_empty, fluid_count = struct.unpack(">hh", chunk[offset:offset + 4])
                offset += 4
                block_values, block_palette, offset = read_paletted_container(
                    chunk, offset, 4096
                )
                decoded_block_states.update(block_palette)
                assert sum(value != 0 for value in block_values) == non_empty
                assert block_values.count(86) == fluid_count
                if len(block_palette) > 1:
                    mixed_section_count += 1
                biome_values, biome_palette, offset = read_paletted_container(
                    chunk, offset, 64
                )
                assert len(biome_values) == 64
                decoded_biomes.update(biome_palette)
            assert mixed_section_count >= 1
            assert {0, 1, 9, 10, 85}.issubset(decoded_block_states)
            assert decoded_block_states.issubset(
                {0, 1, 9, 10, 85, 86, 118, 124, 131, 133,
                 137, 279, 2248, 2321, 2324}
            )
            assert decoded_biomes.issubset({14, 21, 35, 40, 63})
            assert offset == section_end
            block_entity_count, offset = read_varint(chunk, offset)
            assert block_entity_count == 0

            bitsets = []
            for _ in range(4):
                word_count, offset = read_varint(chunk, offset)
                words = []
                for _ in range(word_count):
                    words.append(struct.unpack(">Q", chunk[offset:offset + 8])[0])
                    offset += 8
                bitsets.append(words)
            sky_update_count, offset = read_varint(chunk, offset)
            sky_updates = []
            for _ in range(sky_update_count):
                data_size, offset = read_varint(chunk, offset)
                data = chunk[offset:offset + data_size]
                offset += data_size
                sky_updates.append(data)
            block_update_count, offset = read_varint(chunk, offset)
            assert bitsets == [[(1 << 26) - 1], [], [], [(1 << 26) - 1]]
            assert sky_update_count == 26
            assert all(data == b"\xff" * 2048 for data in sky_updates)
            assert block_update_count == 0
            assert offset == len(chunk)

            neighbor_positions = set()
            for _ in range(24):
                neighbor = read_compressed_packet(connection, threshold)
                packet_id, offset = read_varint(neighbor, 0)
                chunk_x, chunk_z = struct.unpack(">ii", neighbor[offset:offset + 8])
                assert packet_id == 0x2D
                neighbor_positions.add((chunk_x, chunk_z))
            assert neighbor_positions == {
                (x, z)
                for z in range(-2, 3)
                for x in range(-2, 3)
                if (x, z) != (0, 0)
            }

            batch_finished = read_compressed_packet(connection, threshold)
            packet_id, offset = read_varint(batch_finished, 0)
            batch_size, offset = read_varint(batch_finished, offset)
            assert (packet_id, batch_size, offset) == (0x0B, 25, len(batch_finished))

            connection.sendall(
                compressed_frame(0x0B, threshold, struct.pack(">f", 4.0))
            )
            connection.sendall(
                compressed_frame(0, threshold, encode_varint(teleport_id))
            )

            if verify_delayed_keepalive:
                deadline = time.monotonic() + 0.5
                try:
                    while time.monotonic() < deadline:
                        connection.connection.settimeout(
                            max(0.01, deadline - time.monotonic())
                        )
                        transition_packet = read_compressed_packet(connection, threshold)
                        transition_id, _ = read_varint(transition_packet, 0)
                        if transition_id == 0x2C:
                            raise AssertionError("server sent an early Play keepalive")
                        assert transition_id in (0x01, 0x23, 0x35, 0x36, 0x38, 0x53, 0x65, 0x71)
                except socket.timeout:
                    pass
                return

            spawned_entity_ids = set()
            break_start_sequence = 6
            break_sequence = 7
            break_position = (8, spawn_y - 1, 8)
            connection.sendall(
                compressed_frame(
                    0x29,
                    threshold,
                    encode_varint(0)
                    + encode_position(*break_position)
                    + encode_varint(1)
                    + encode_varint(break_start_sequence),
                )
            )
            time.sleep(0.06)
            connection.sendall(
                compressed_frame(
                    0x29,
                    threshold,
                    encode_varint(2)
                    + encode_position(*break_position)
                    + encode_varint(1)
                    + encode_varint(break_sequence),
                )
            )
            break_update_seen = False
            break_ack_seen = False
            harvest_seen = False
            while not (break_update_seen and break_ack_seen and harvest_seen):
                gameplay = read_compressed_packet(connection, threshold)
                gameplay_id, offset = read_varint(gameplay, 0)
                if gameplay_id == 0x01:
                    entity_id, _ = read_varint(gameplay, offset)
                    spawned_entity_ids.add(entity_id)
                elif gameplay_id in (0x05, 0x23, 0x35, 0x36, 0x38, 0x53, 0x65, 0x71):
                    pass
                elif gameplay_id == 0x08:
                    packed_position = struct.unpack(">Q", gameplay[offset:offset + 8])[0]
                    offset += 8
                    state_id, offset = read_varint(gameplay, offset)
                    assert packed_position == struct.unpack(
                        ">Q", encode_position(*break_position)
                    )[0]
                    assert state_id == 0
                    break_update_seen = True
                elif gameplay_id == 0x04:
                    sequence, offset = read_varint(gameplay, offset)
                    assert sequence in (break_start_sequence, break_sequence)
                    if sequence == break_sequence:
                        break_ack_seen = True
                elif gameplay_id == 0x6C:
                    slot, offset = read_varint(gameplay, offset)
                    count, offset = read_varint(gameplay, offset)
                    item_id, offset = read_varint(gameplay, offset)
                    added_components, offset = read_varint(gameplay, offset)
                    removed_components, offset = read_varint(gameplay, offset)
                    assert (slot, count, item_id, added_components, removed_components) == (
                        44, 1, 55, 0, 0
                    )
                    harvest_seen = True
                elif gameplay_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", gameplay[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    raise AssertionError(f"unexpected block interaction packet {gameplay_id:#x}")

            place_sequence = 8
            support_position = (8, spawn_y - 2, 8)
            connection.sendall(
                compressed_frame(
                    0x42,
                    threshold,
                    encode_varint(0)
                    + encode_position(*support_position)
                    + encode_varint(1)
                    + struct.pack(">fffBB", 0.5, 1.0, 0.5, 0, 0)
                    + encode_varint(place_sequence),
                )
            )
            place_update_seen = False
            place_ack_seen = False
            inventory_update_seen = False
            while not (place_update_seen and place_ack_seen and inventory_update_seen):
                gameplay = read_compressed_packet(connection, threshold)
                gameplay_id, offset = read_varint(gameplay, 0)
                if gameplay_id == 0x01:
                    entity_id, _ = read_varint(gameplay, offset)
                    spawned_entity_ids.add(entity_id)
                elif gameplay_id in (0x23, 0x35, 0x36, 0x38, 0x53, 0x65, 0x71):
                    pass
                elif gameplay_id == 0x08:
                    packed_position = struct.unpack(">Q", gameplay[offset:offset + 8])[0]
                    offset += 8
                    state_id, offset = read_varint(gameplay, offset)
                    assert packed_position == struct.unpack(
                        ">Q", encode_position(*break_position)
                    )[0]
                    assert state_id == 10
                    place_update_seen = True
                elif gameplay_id == 0x6C:
                    slot, offset = read_varint(gameplay, offset)
                    count, offset = read_varint(gameplay, offset)
                    item_id, offset = read_varint(gameplay, offset)
                    added_components, offset = read_varint(gameplay, offset)
                    removed_components, offset = read_varint(gameplay, offset)
                    assert (slot, count, item_id, added_components, removed_components) == (
                        36, 63, 55, 0, 0
                    )
                    inventory_update_seen = True
                elif gameplay_id == 0x04:
                    sequence, offset = read_varint(gameplay, offset)
                    assert sequence == place_sequence
                    place_ack_seen = True
                elif gameplay_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", gameplay[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    raise AssertionError(f"unexpected block placement packet {gameplay_id:#x}")

            connection.sendall(compressed_frame(0x2B, threshold, b"\x61"))
            connection.sendall(
                compressed_frame(
                    0x2A,
                    threshold,
                    encode_varint(1) + encode_varint(1) + encode_varint(0),
                )
            )
            connection.sendall(compressed_frame(0x3F, threshold, encode_varint(0)))

            connection.sendall(
                compressed_frame(
                    0x1E,
                    threshold,
                    struct.pack(">dddB", 40.5, float(spawn_y), 8.5, 1),
                )
            )
            cache_center_seen = False
            forgotten_positions = set()
            streamed_positions = set()
            batch_started = False
            batch_finished_seen = False
            while not batch_finished_seen:
                streamed = read_compressed_packet(connection, threshold)
                streamed_id, offset = read_varint(streamed, 0)
                if streamed_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", streamed[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                elif streamed_id == 0x5E:
                    center_x, offset = read_varint(streamed, offset)
                    center_z, offset = read_varint(streamed, offset)
                    assert (center_x, center_z, offset) == (2, 0, len(streamed))
                    cache_center_seen = True
                elif streamed_id == 0x25:
                    packed = struct.unpack(">Q", streamed[offset:offset + 8])[0]
                    chunk_x = struct.unpack(">i", struct.pack(">I", packed & 0xFFFFFFFF))[0]
                    chunk_z = struct.unpack(">i", struct.pack(">I", packed >> 32))[0]
                    forgotten_positions.add((chunk_x, chunk_z))
                elif streamed_id == 0x0C:
                    assert offset == len(streamed)
                    batch_started = True
                elif streamed_id == 0x2D:
                    chunk_x, chunk_z = struct.unpack(">ii", streamed[offset:offset + 8])
                    streamed_positions.add((chunk_x, chunk_z))
                elif streamed_id in (0x01, 0x23, 0x35, 0x36, 0x38, 0x53, 0x65, 0x71):
                    if streamed_id == 0x01:
                        entity_id, _ = read_varint(streamed, offset)
                        spawned_entity_ids.add(entity_id)
                elif streamed_id == 0x0B:
                    streamed_count, offset = read_varint(streamed, offset)
                    assert streamed_count == 10
                    assert offset == len(streamed)
                    batch_finished_seen = True
                else:
                    raise AssertionError(f"unexpected streaming packet {streamed_id:#x}")
            assert cache_center_seen
            assert batch_started
            assert forgotten_positions == {
                (x, z) for z in range(-2, 3) for x in (-2, -1)
            }
            assert streamed_positions == {
                (x, z) for z in range(-2, 3) for x in (3, 4)
            }
            assert len(spawned_entity_ids) == 4

            connection.sendall(compressed_frame(0x35, threshold, struct.pack(">h", 4)))
            arrow_sequence = 10
            connection.sendall(
                compressed_frame(
                    0x43,
                    threshold,
                    encode_varint(0)
                    + encode_varint(arrow_sequence)
                    + struct.pack(">ff", 0.0, 0.0),
                )
            )
            arrow_seen = False
            arrow_count_seen = False
            arrow_ack_seen = False
            while not (arrow_seen and arrow_count_seen and arrow_ack_seen):
                ranged = read_compressed_packet(connection, threshold)
                ranged_id, offset = read_varint(ranged, 0)
                if ranged_id == 0x01:
                    _, offset = read_varint(ranged, offset)
                    offset += 16
                    entity_type, offset = read_varint(ranged, offset)
                    arrow_seen = entity_type == 6
                elif ranged_id == 0x6C:
                    slot, offset = read_varint(ranged, offset)
                    count, offset = read_varint(ranged, offset)
                    item_id, offset = read_varint(ranged, offset)
                    _, offset = read_varint(ranged, offset)
                    _, offset = read_varint(ranged, offset)
                    if slot == 41:
                        assert (count, item_id) == (31, 923)
                        arrow_count_seen = True
                elif ranged_id == 0x04:
                    sequence, offset = read_varint(ranged, offset)
                    arrow_ack_seen = sequence == arrow_sequence
                elif ranged_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", ranged[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert ranged_id in (0x08, 0x23, 0x35, 0x36, 0x38, 0x4D, 0x53, 0x65, 0x71), hex(ranged_id)

            connection.sendall(compressed_frame(0x35, threshold, struct.pack(">h", 3)))
            drop_sequence = 11
            connection.sendall(
                compressed_frame(
                    0x29,
                    threshold,
                    encode_varint(4)
                    + encode_position(0, 0, 0)
                    + encode_varint(0)
                    + encode_varint(drop_sequence),
                )
            )
            dropped_entity_id = None
            drop_metadata_seen = False
            drop_slot_seen = False
            drop_ack_seen = False
            while not (
                dropped_entity_id is not None
                and drop_metadata_seen
                and drop_slot_seen
                and drop_ack_seen
            ):
                dropped = read_compressed_packet(connection, threshold)
                dropped_id, offset = read_varint(dropped, 0)
                if dropped_id == 0x01:
                    entity_id, offset = read_varint(dropped, offset)
                    offset += 16
                    entity_type, offset = read_varint(dropped, offset)
                    if entity_type == 71:
                        dropped_entity_id = entity_id
                elif dropped_id == 0x63:
                    entity_id, offset = read_varint(dropped, offset)
                    index = dropped[offset]
                    offset += 1
                    serializer, offset = read_varint(dropped, offset)
                    count, offset = read_varint(dropped, offset)
                    item_id, offset = read_varint(dropped, offset)
                    _, offset = read_varint(dropped, offset)
                    _, offset = read_varint(dropped, offset)
                    assert (entity_id, index, serializer, count, item_id) == (
                        dropped_entity_id, 8, 7, 1, 981
                    )
                    drop_metadata_seen = True
                elif dropped_id == 0x6C:
                    slot, offset = read_varint(dropped, offset)
                    count, offset = read_varint(dropped, offset)
                    item_id, offset = read_varint(dropped, offset)
                    _, offset = read_varint(dropped, offset)
                    _, offset = read_varint(dropped, offset)
                    if slot == 39:
                        assert (count, item_id) == (15, 981)
                        drop_slot_seen = True
                elif dropped_id == 0x04:
                    sequence, offset = read_varint(dropped, offset)
                    drop_ack_seen = sequence == drop_sequence
                elif dropped_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", dropped[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert dropped_id in (0x08, 0x23, 0x35, 0x36, 0x38, 0x4D, 0x53, 0x65, 0x71)

            swap_sequence = 12
            connection.sendall(
                compressed_frame(
                    0x29,
                    threshold,
                    encode_varint(6)
                    + encode_position(0, 0, 0)
                    + encode_varint(0)
                    + encode_varint(swap_sequence),
                )
            )
            swapped_slots = set()
            swap_ack_seen = False
            while not (swapped_slots == {39, 45} and swap_ack_seen):
                swapped = read_compressed_packet(connection, threshold)
                swapped_id, offset = read_varint(swapped, 0)
                if swapped_id == 0x6C:
                    slot, offset = read_varint(swapped, offset)
                    count, offset = read_varint(swapped, offset)
                    if slot == 39:
                        assert count == 0
                        swapped_slots.add(slot)
                    elif slot == 45:
                        item_id, offset = read_varint(swapped, offset)
                        assert (count, item_id) == (15, 981)
                        swapped_slots.add(slot)
                elif swapped_id == 0x04:
                    sequence, offset = read_varint(swapped, offset)
                    swap_ack_seen = sequence == swap_sequence
                elif swapped_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", swapped[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert swapped_id in (0x08, 0x23, 0x35, 0x36, 0x38, 0x4D, 0x53, 0x65, 0x71)

            while True:
                keep_alive = read_compressed_packet(connection, threshold)
                packet_id, offset = read_varint(keep_alive, 0)
                if packet_id == 0x2C:
                    break
                assert packet_id in (0x01, 0x08, 0x23, 0x35, 0x36, 0x38, 0x4D, 0x53, 0x63, 0x65, 0x71), hex(packet_id)
            assert packet_id == 0x2C
            keep_alive_id = struct.unpack(">q", keep_alive[offset:offset + 8])[0]
            assert offset + 8 == len(keep_alive)
            connection.sendall(
                compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
            )

            connection.sendall(compressed_frame(0x0C, threshold, encode_varint(1)))
            while True:
                stats_packet = read_compressed_packet(connection, threshold)
                stats_id, offset = read_varint(stats_packet, 0)
                if stats_id == 0x03:
                    stat_count, offset = read_varint(stats_packet, offset)
                    assert stat_count > 0
                    break
                assert stats_id in (
                    0x01, 0x08, 0x23, 0x2A, 0x35, 0x36, 0x38,
                    0x4D, 0x53, 0x63, 0x65, 0x71
                ), hex(stats_id)

            connection.sendall(
                compressed_frame(
                    0x1E,
                    threshold,
                    struct.pack(">dddB", 200.5, float(spawn_y), 8.5, 1),
                )
            )
            while True:
                correction = read_compressed_packet(connection, threshold)
                correction_id, offset = read_varint(correction, 0)
                if correction_id == 0x48:
                    correction_teleport_id, offset = read_varint(correction, offset)
                    assert correction_teleport_id > teleport_id
                    connection.sendall(
                        compressed_frame(
                            0, threshold, encode_varint(correction_teleport_id)
                        )
                    )
                    break
                assert correction_id in (0x23, 0x35, 0x36, 0x38, 0x53, 0x65, 0x71)

            connection.sendall(
                compressed_frame(
                    0x1E,
                    threshold,
                    struct.pack(">dddB", 40.5, float(spawn_y + 50), 8.5, 0),
                )
            )
            connection.sendall(
                compressed_frame(
                    0x1E,
                    threshold,
                    struct.pack(">dddB", 40.5, float(spawn_y), 8.5, 1),
                )
            )
            death_health_seen = False
            death_message_seen = False
            cleared_slots = set()
            while not (death_health_seen and death_message_seen and len(cleared_slots) == 10):
                death_packet = read_compressed_packet(connection, threshold)
                death_id, offset = read_varint(death_packet, 0)
                if death_id == 0x68:
                    health = struct.unpack(">f", death_packet[offset:offset + 4])[0]
                    death_health_seen = health == 0.0
                elif death_id == 0x67:
                    offset += 4
                    total, offset = read_varint(death_packet, offset)
                    level, offset = read_varint(death_packet, offset)
                    assert (total, level) == (0, 0)
                elif death_id == 0x6C:
                    slot, offset = read_varint(death_packet, offset)
                    count, offset = read_varint(death_packet, offset)
                    if 36 <= slot <= 45 and count == 0:
                        cleared_slots.add(slot)
                elif death_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", death_packet[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                elif death_id == 0x79:
                    death_message_seen = True
                else:
                    assert death_id in (
                        0x01, 0x08, 0x23, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x65, 0x71
                    )

            connection.sendall(compressed_frame(0x0C, threshold, encode_varint(0)))
            respawn_seen = False
            reset_health_seen = False
            reset_experience_seen = False
            hardcore_abilities_seen = not hardcore
            respawn_teleport_id = None
            while not (
                respawn_seen
                and reset_health_seen
                and reset_experience_seen
                and hardcore_abilities_seen
                and respawn_teleport_id is not None
            ):
                respawn_packet = read_compressed_packet(connection, threshold)
                respawn_id, offset = read_varint(respawn_packet, 0)
                if respawn_id == 0x52:
                    _, offset = read_varint(respawn_packet, offset)
                    _, offset = read_string(respawn_packet, offset)
                    offset += 8
                    respawn_mode = respawn_packet[offset]
                    previous_mode = respawn_packet[offset + 1]
                    assert (respawn_mode, previous_mode) == (
                        (3, 0) if hardcore else (0, 255)
                    )
                    respawn_seen = True
                elif respawn_id == 0x68:
                    health = struct.unpack(">f", respawn_packet[offset:offset + 4])[0]
                    reset_health_seen = health == 20.0
                elif respawn_id == 0x67:
                    reset_experience_seen = True
                elif respawn_id == 0x48:
                    respawn_teleport_id, offset = read_varint(respawn_packet, offset)
                elif respawn_id == 0x40:
                    assert respawn_packet[offset] == 0x07
                    hardcore_abilities_seen = True
                elif respawn_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", respawn_packet[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert respawn_id in (
                        0x0B, 0x0C, 0x25, 0x2D, 0x5E,
                        0x01, 0x23, 0x35, 0x36, 0x38, 0x4D, 0x53, 0x63, 0x65, 0x71
                    ), hex(respawn_id)
            connection.sendall(
                compressed_frame(0, threshold, encode_varint(respawn_teleport_id))
            )

            connection.sendall(compressed_frame(0x0C, threshold, encode_varint(2)))
            while True:
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x27:
                    break
                if response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                    continue
                assert response_id in (0x03, 0x08, 0x23, 0x35, 0x36, 0x38, 0x4D, 0x53, 0x63, 0x65, 0x71), hex(response_id)

            connection.sendall(
                compressed_frame(0x07, threshold, encode_string("say hello"))
            )
            while True:
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x79:
                    break
                if response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                    continue
                assert response_id in (0x03, 0x08, 0x23, 0x35, 0x36, 0x38, 0x4D, 0x53, 0x63, 0x65, 0x71)

            for rule_name, registry_name, rule_value in (
                ("keepInventory", "minecraft:keep_inventory", "true"),
                ("keepInventory", "minecraft:keep_inventory", "false"),
                ("doDaylightCycle", "minecraft:advance_time", "false"),
                ("doDaylightCycle", "minecraft:advance_time", "true"),
                ("doMobSpawning", "minecraft:spawn_mobs", "false"),
                ("doMobSpawning", "minecraft:spawn_mobs", "true"),
                ("doWeatherCycle", "minecraft:advance_weather", "false"),
                ("doWeatherCycle", "minecraft:advance_weather", "true"),
            ):
                connection.sendall(
                    compressed_frame(
                        0x07,
                        threshold,
                        encode_string(f"gamerule {rule_name} {rule_value}"),
                    )
                )
                gamerule_seen = False
                gamerule_feedback_seen = False
                while not (gamerule_seen and gamerule_feedback_seen):
                    response = read_compressed_packet(connection, threshold)
                    response_id, offset = read_varint(response, 0)
                    if response_id == 0x27:
                        count, offset = read_varint(response, offset)
                        rules = {}
                        for _ in range(count):
                            name, offset = read_string(response, offset)
                            value, offset = read_string(response, offset)
                            rules[name] = value
                        assert rules[registry_name] == rule_value
                        gamerule_seen = True
                    elif response_id == 0x79:
                        gamerule_feedback_seen = True
                    elif response_id == 0x2C:
                        keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                        connection.sendall(
                            compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                        )
                    else:
                        assert response_id in (
                            0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                            0x4D, 0x53, 0x63, 0x65, 0x71,
                        )

            connection.sendall(
                compressed_frame(0x07, threshold, encode_string("weather thunder 30"))
            )
            thunder_event_seen = False
            weather_feedback_seen = False
            while not (thunder_event_seen and weather_feedback_seen):
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x26:
                    event = response[offset]
                    if event == 1:
                        thunder_event_seen = True
                    else:
                        assert event in (7, 8)
                elif response_id == 0x79:
                    weather_feedback_seen = True
                elif response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert response_id in (
                        0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x65, 0x71,
                    ), hex(response_id)

            difficulty_cases = () if hardcore else (("peaceful", 0), ("normal", 2))
            for difficulty_name, difficulty_value in difficulty_cases:
                connection.sendall(
                    compressed_frame(
                        0x07, threshold, encode_string(f"difficulty {difficulty_name}")
                    )
                )
                difficulty_seen = False
                difficulty_feedback_seen = False
                while not (difficulty_seen and difficulty_feedback_seen):
                    response = read_compressed_packet(connection, threshold)
                    response_id, offset = read_varint(response, 0)
                    if response_id == 0x0A:
                        received_difficulty, offset = read_varint(response, offset)
                        locked = response[offset]
                        assert (received_difficulty, locked) == (difficulty_value, 0)
                        difficulty_seen = True
                    elif response_id == 0x79:
                        difficulty_feedback_seen = True
                    elif response_id == 0x2C:
                        keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                        connection.sendall(
                            compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                        )
                    else:
                        assert response_id in (
                            0x03, 0x08, 0x23, 0x35, 0x36, 0x38, 0x46,
                            0x4D, 0x53, 0x63, 0x65, 0x71,
                        )

            if hardcore:
                connection.sendall(
                    compressed_frame(0x07, threshold, encode_string("difficulty peaceful"))
                )
                while True:
                    response = read_compressed_packet(connection, threshold)
                    response_id, offset = read_varint(response, 0)
                    assert response_id != 0x0A
                    if response_id == 0x79:
                        break
                    if response_id == 0x2C:
                        keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                        connection.sendall(
                            compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                        )
                        continue
                    assert response_id in (
                        0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x65, 0x71,
                    )

            for mode_name, mode_value, ability_flags in (
                ("adventure", 2.0, 0x00),
                ("spectator", 3.0, 0x07),
                ("survival", 0.0, 0x00),
                ("creative", 1.0, 0x0D),
            ):
                connection.sendall(
                    compressed_frame(0x07, threshold, encode_string(f"gamemode {mode_name}"))
                )
                mode_event_seen = False
                mode_abilities_seen = False
                mode_feedback_seen = False
                while not (mode_event_seen and mode_abilities_seen and mode_feedback_seen):
                    response = read_compressed_packet(connection, threshold)
                    response_id, offset = read_varint(response, 0)
                    if response_id == 0x26:
                        event = response[offset]
                        parameter = struct.unpack(">f", response[offset + 1:offset + 5])[0]
                        if event == 3:
                            assert parameter == mode_value
                            mode_event_seen = True
                    elif response_id == 0x40:
                        assert response[offset] == ability_flags
                        mode_abilities_seen = True
                    elif response_id == 0x79:
                        mode_feedback_seen = True
                    elif response_id == 0x2C:
                        keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                        connection.sendall(
                            compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                        )
                    else:
                        assert response_id in (
                            0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                            0x4D, 0x53, 0x63, 0x65, 0x71,
                        )

                connection.sendall(
                    compressed_frame(0x28, threshold, bytes((0x02,)))
                )
                expected_flight_flags = (
                    ability_flags | 0x02
                    if mode_name in ("creative", "spectator")
                    else ability_flags
                )
                while True:
                    response = read_compressed_packet(connection, threshold)
                    response_id, offset = read_varint(response, 0)
                    if response_id == 0x40:
                        assert response[offset] == expected_flight_flags
                        break
                    if response_id == 0x2C:
                        keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                        connection.sendall(
                            compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                        )
                        continue
                    assert response_id in (
                        0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x65, 0x71,
                    )

            connection.sendall(
                compressed_frame(
                    0x38,
                    threshold,
                    struct.pack(">h", 36)
                    + encode_varint(5)
                    + encode_varint(1)
                    + encode_varint(0)
                    + encode_varint(0),
                )
            )
            while True:
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x6C:
                    slot, offset = read_varint(response, offset)
                    count, offset = read_varint(response, offset)
                    item_id, offset = read_varint(response, offset)
                    added_components, offset = read_varint(response, offset)
                    removed_components, offset = read_varint(response, offset)
                    assert (slot, count, item_id, added_components, removed_components) == (
                        36, 5, 1, 0, 0
                    )
                    break
                if response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                    continue
                assert response_id in (
                    0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                    0x4D, 0x53, 0x63, 0x65, 0x71,
                )

            connection.sendall(
                compressed_frame(
                    0x38, threshold, struct.pack(">h", 36) + encode_varint(0)
                )
            )
            while True:
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x6C:
                    slot, offset = read_varint(response, offset)
                    count, offset = read_varint(response, offset)
                    assert (slot, count, offset) == (36, 0, len(response))
                    break
                if response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                    continue
                assert response_id in (
                    0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                    0x4D, 0x53, 0x63, 0x65, 0x71,
                )

            connection.sendall(
                compressed_frame(
                    0x12,
                    threshold,
                    encode_varint(0)
                    + encode_varint(99)
                    + struct.pack(">h", 36)
                    + b"\x00"
                    + encode_varint(0)
                    + encode_varint(1)
                    + struct.pack(">h", 36)
                    + b"\x01"
                    + encode_varint(1)
                    + encode_varint(5)
                    + encode_varint(0)
                    + encode_varint(0)
                    + b"\x00",
                )
            )
            while True:
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x12:
                    container_id, offset = read_varint(response, offset)
                    state_id, offset = read_varint(response, offset)
                    slot_count, offset = read_varint(response, offset)
                    assert (container_id, state_id, slot_count) == (0, 0, 46)
                    slot_36_count = None
                    for slot in range(slot_count):
                        count, offset = read_varint(response, offset)
                        if slot == 36:
                            slot_36_count = count
                        if count:
                            _, offset = read_varint(response, offset)
                            added_components, offset = read_varint(response, offset)
                            removed_components, offset = read_varint(response, offset)
                            assert (added_components, removed_components) == (0, 0)
                    carried_count, offset = read_varint(response, offset)
                    assert (slot_36_count, carried_count, offset) == (
                        0, 0, len(response)
                    )
                    break
                if response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                    continue
                assert response_id in (
                    0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                    0x4D, 0x53, 0x63, 0x65, 0x71,
                )

            for border_command, border_packet_id, border_values in (
                ("worldborder center 10 -20", 0x58, (10.0, -20.0)),
                ("worldborder set 100", 0x5A, (100.0,)),
            ):
                connection.sendall(
                    compressed_frame(0x07, threshold, encode_string(border_command))
                )
                border_seen = False
                border_feedback_seen = False
                while not (border_seen and border_feedback_seen):
                    response = read_compressed_packet(connection, threshold)
                    response_id, offset = read_varint(response, 0)
                    if response_id == border_packet_id:
                        values = struct.unpack(">" + "d" * len(border_values), response[offset:])
                        assert values == border_values
                        border_seen = True
                    elif response_id == 0x79:
                        border_feedback_seen = True
                    elif response_id == 0x2C:
                        keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                        connection.sendall(
                            compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                        )
                    else:
                        assert response_id in (
                            0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                            0x4D, 0x53, 0x63, 0x65, 0x71,
                        )

            for border_command, border_packet_id, expected_value in (
                ("worldborder set 200 2", 0x59, (100.0, 200.0, 2000)),
                ("worldborder warning distance 12", 0x5C, 12),
                ("worldborder warning time 8", 0x5B, 8),
            ):
                connection.sendall(
                    compressed_frame(0x07, threshold, encode_string(border_command))
                )
                border_seen = False
                border_feedback_seen = False
                while not (border_seen and border_feedback_seen):
                    response = read_compressed_packet(connection, threshold)
                    response_id, offset = read_varint(response, 0)
                    if response_id == border_packet_id:
                        if response_id == 0x59:
                            old_size, new_size = struct.unpack(">dd", response[offset:offset + 16])
                            duration, offset = read_varint(response, offset + 16)
                            assert (old_size, new_size, duration) == expected_value
                        else:
                            value, offset = read_varint(response, offset)
                            assert value == expected_value
                        border_seen = True
                    elif response_id == 0x79:
                        border_feedback_seen = True
                    elif response_id == 0x2C:
                        keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                        connection.sendall(
                            compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                        )
                    else:
                        assert response_id in (
                            0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                            0x4D, 0x53, 0x63, 0x65, 0x71,
                        )

            connection.sendall(
                compressed_frame(
                    0x24,
                    threshold,
                    encode_position(8, spawn_y - 1, 8) + b"\x00",
                )
            )
            while True:
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x6C:
                    slot, offset = read_varint(response, offset)
                    count, offset = read_varint(response, offset)
                    item_id, offset = read_varint(response, offset)
                    added_components, offset = read_varint(response, offset)
                    removed_components, offset = read_varint(response, offset)
                    assert (slot, count, item_id, added_components, removed_components) == (
                        39, 1, 55, 0, 0
                    )
                    break
                if response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                    continue
                assert response_id in (
                    0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                    0x4D, 0x53, 0x63, 0x65, 0x71,
                )

            connection.sendall(
                compressed_frame(0x07, threshold, encode_string("effect give speed 30 1"))
            )
            effect_seen = False
            effect_feedback_seen = False
            while not (effect_seen and effect_feedback_seen):
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x84:
                    entity_id, offset = read_varint(response, offset)
                    effect_id, offset = read_varint(response, offset)
                    amplifier, offset = read_varint(response, offset)
                    duration, offset = read_varint(response, offset)
                    assert (entity_id, effect_id, amplifier, duration) == (1, 1, 1, 600)
                    effect_seen = True
                elif response_id == 0x79:
                    effect_feedback_seen = True
                elif response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert response_id in (
                        0x01, 0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x65, 0x71,
                    ), hex(response_id)

            connection.sendall(
                compressed_frame(
                    0x07, threshold,
                    encode_string(f"teleport 9 {spawn_y} 9"),
                )
            )
            command_teleport_id = None
            teleport_feedback_seen = False
            while command_teleport_id is None or not teleport_feedback_seen:
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x48:
                    command_teleport_id, offset = read_varint(response, offset)
                    x, y, z = struct.unpack(">ddd", response[offset:offset + 24])
                    assert (x, y, z) == (9.0, float(spawn_y), 9.0)
                elif response_id == 0x79:
                    teleport_feedback_seen = True
                elif response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert response_id in (
                        0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x65, 0x71,
                    )
            connection.sendall(
                compressed_frame(0x00, threshold, encode_varint(command_teleport_id))
            )

            connection.sendall(
                compressed_frame(0x07, threshold, encode_string("effect clear speed"))
            )
            removal_seen = False
            removal_feedback_seen = False
            while not (removal_seen and removal_feedback_seen):
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x4E:
                    entity_id, offset = read_varint(response, offset)
                    effect_id, offset = read_varint(response, offset)
                    assert (entity_id, effect_id) == (1, 1)
                    removal_seen = True
                elif response_id == 0x79:
                    removal_feedback_seen = True
                elif response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert response_id in (
                        0x01, 0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x65, 0x71,
                    ), hex(response_id)

            connection.sendall(
                compressed_frame(0x07, threshold, encode_string("summon cow"))
            )
            summoned_cow_id = None
            summon_feedback_seen = False
            while summoned_cow_id is None or not summon_feedback_seen:
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x01:
                    summoned_cow_id, offset = read_varint(response, offset)
                    offset += 16
                    entity_type, offset = read_varint(response, offset)
                    assert entity_type == 30
                elif response_id == 0x79:
                    summon_feedback_seen = True
                elif response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert response_id in (
                        0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x65, 0x71,
                    )

            connection.sendall(
                compressed_frame(
                    0x25, threshold, encode_varint(summoned_cow_id) + b"\x00"
                )
            )
            while True:
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x6C:
                    slot, offset = read_varint(response, offset)
                    count, offset = read_varint(response, offset)
                    item_id, offset = read_varint(response, offset)
                    added_components, offset = read_varint(response, offset)
                    removed_components, offset = read_varint(response, offset)
                    assert (slot, count, item_id, added_components, removed_components) == (
                        39, 1, 1160, 0, 0
                    )
                    break
                if response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                    continue
                assert response_id in (
                    0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                    0x4D, 0x53, 0x63, 0x65, 0x71,
                )

            for inventory_command, expected_count in (
                ("give minecraft:diamond 16", 16),
                ("clear minecraft:diamond", 0),
            ):
                connection.sendall(
                    compressed_frame(0x07, threshold, encode_string(inventory_command))
                )
                inventory_command_update_seen = False
                inventory_command_feedback_seen = False
                while not (
                    inventory_command_update_seen and inventory_command_feedback_seen
                ):
                    response = read_compressed_packet(connection, threshold)
                    response_id, offset = read_varint(response, 0)
                    if response_id == 0x6C:
                        slot, offset = read_varint(response, offset)
                        count, offset = read_varint(response, offset)
                        assert slot == 36
                        assert count == expected_count
                        if count:
                            item_id, offset = read_varint(response, offset)
                            added_components, offset = read_varint(response, offset)
                            removed_components, offset = read_varint(response, offset)
                            assert (item_id, added_components, removed_components) == (
                                926, 0, 0
                            )
                        inventory_command_update_seen = True
                    elif response_id == 0x79:
                        inventory_command_feedback_seen = True
                    elif response_id == 0x2C:
                        keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                        connection.sendall(
                            compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                        )
                    else:
                        assert response_id in (
                            0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                            0x4D, 0x53, 0x63, 0x65, 0x71,
                        )

            for experience_command, expected_total, expected_level, expected_progress in (
                ("experience set 50", 50, 4, 2.0 / 3.0),
                ("xp add 10", 60, 5, 5.0 / 17.0),
            ):
                connection.sendall(
                    compressed_frame(0x07, threshold, encode_string(experience_command))
                )
                experience_update_seen = False
                experience_feedback_seen = False
                while not (experience_update_seen and experience_feedback_seen):
                    response = read_compressed_packet(connection, threshold)
                    response_id, offset = read_varint(response, 0)
                    if response_id == 0x67:
                        progress = struct.unpack(">f", response[offset:offset + 4])[0]
                        offset += 4
                        total, offset = read_varint(response, offset)
                        level, offset = read_varint(response, offset)
                        assert (total, level) == (expected_total, expected_level)
                        assert abs(progress - expected_progress) < 0.0001
                        experience_update_seen = True
                    elif response_id == 0x79:
                        experience_feedback_seen = True
                    elif response_id == 0x2C:
                        keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                        connection.sendall(
                            compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                        )
                    else:
                        assert response_id in (
                            0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                            0x4D, 0x53, 0x63, 0x65, 0x71,
                        )

            connection.sendall(
                compressed_frame(0x07, threshold, encode_string("gamemode survival"))
            )
            survival_mode_event_seen = False
            survival_mode_abilities_seen = False
            survival_mode_feedback_seen = False
            while not (
                survival_mode_event_seen
                and survival_mode_abilities_seen
                and survival_mode_feedback_seen
            ):
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x26:
                    event = response[offset]
                    parameter = struct.unpack(">f", response[offset + 1:offset + 5])[0]
                    if event == 3:
                        assert parameter == 0.0
                        survival_mode_event_seen = True
                elif response_id == 0x40:
                    assert response[offset] == 0
                    survival_mode_abilities_seen = True
                elif response_id == 0x79:
                    survival_mode_feedback_seen = True
                elif response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert response_id in (
                        0x03, 0x08, 0x23, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x65, 0x71,
                    )

            connection.sendall(
                compressed_frame(0x07, threshold, encode_string("summon zombie"))
            )
            zombie_spawn_seen = False
            zombie_feedback_seen = False
            hostile_health_seen = False
            player_hurt_seen = False
            player_knockback_seen = False
            while not (
                zombie_spawn_seen and zombie_feedback_seen and hostile_health_seen
                and player_hurt_seen and player_knockback_seen
            ):
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x01:
                    _, offset = read_varint(response, offset)
                    offset += 16
                    entity_type, offset = read_varint(response, offset)
                    if entity_type == 151:
                        zombie_spawn_seen = True
                elif response_id == 0x79:
                    zombie_feedback_seen = True
                elif response_id == 0x68:
                    health = struct.unpack(">f", response[offset:offset + 4])[0]
                    assert health == (17.0 if hardcore else 18.0)
                    hostile_health_seen = True
                elif response_id == 0x2A:
                    entity_id, offset = read_varint(response, offset)
                    player_hurt_seen = entity_id == 1
                elif response_id == 0x65:
                    entity_id, offset = read_varint(response, offset)
                    player_knockback_seen = entity_id == 1
                elif response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert response_id in (
                        0x02, 0x03, 0x08, 0x23, 0x26, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x71,
                    ), hex(response_id)

            connection.sendall(
                compressed_frame(0x07, threshold, encode_string("kill"))
            )
            killed_health_seen = False
            kill_messages_seen = 0
            death_experience_reset_seen = False
            death_experience_orb_seen = False
            while (
                not killed_health_seen or kill_messages_seen < 2
                or not death_experience_reset_seen or not death_experience_orb_seen
            ):
                response = read_compressed_packet(connection, threshold)
                response_id, offset = read_varint(response, 0)
                if response_id == 0x68:
                    health = struct.unpack(">f", response[offset:offset + 4])[0]
                    killed_health_seen = health == 0.0
                elif response_id == 0x67:
                    progress = struct.unpack(">f", response[offset:offset + 4])[0]
                    offset += 4
                    total, offset = read_varint(response, offset)
                    level, offset = read_varint(response, offset)
                    assert (progress, total, level) == (0.0, 0, 0)
                    death_experience_reset_seen = True
                elif response_id == 0x01:
                    _, offset = read_varint(response, offset)
                    offset += 16
                    entity_type, offset = read_varint(response, offset)
                    if entity_type == 49:
                        death_experience_orb_seen = True
                elif response_id == 0x79:
                    kill_messages_seen += 1
                elif response_id == 0x6C:
                    pass
                elif response_id == 0x2C:
                    keep_alive_id = struct.unpack(">q", response[offset:offset + 8])[0]
                    connection.sendall(
                        compressed_frame(0x1C, threshold, struct.pack(">q", keep_alive_id))
                    )
                else:
                    assert response_id in (
                        0x03, 0x08, 0x23, 0x26, 0x35, 0x36, 0x38,
                        0x4D, 0x53, 0x63, 0x65, 0x71,
                    ), hex(response_id)
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
        if session_server:
            session_server.shutdown()
            session_server.server_close()
        if session_thread:
            session_thread.join(timeout=3)
        if online_mode:
            assert SessionHandler.request_parameters is not None


if __name__ == "__main__":
    main()