#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> None:
    extractor = Path(sys.argv[1])
    bundle = Path(sys.argv[2])
    with tempfile.TemporaryDirectory() as directory:
        manifest_path = Path(directory) / "protocol.json"
        subprocess.run(
            [
                sys.executable,
                str(extractor),
                "--bundle",
                str(bundle),
                "--manifest",
                str(manifest_path),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    assert manifest["minecraft"] == "26.2"
    assert manifest["protocol"] == 776
    states = manifest["states"]
    assert list(states) == ["handshaking", "status", "login", "configuration", "play"]
    assert [packet["name"] for packet in states["login"]["clientbound"]] == [
        "LOGIN_DISCONNECT",
        "HELLO",
        "LOGIN_FINISHED",
        "LOGIN_COMPRESSION",
        "CUSTOM_QUERY",
        "COOKIE_REQUEST",
    ]
    assert [packet["name"] for packet in states["login"]["serverbound"]] == [
        "HELLO",
        "KEY",
        "CUSTOM_QUERY_ANSWER",
        "LOGIN_ACKNOWLEDGED",
        "COOKIE_RESPONSE",
    ]
    assert len(states["configuration"]["clientbound"]) == 20
    assert len(states["configuration"]["serverbound"]) == 10
    assert sum(len(packets) for packets in states["play"].values()) == 210
    packets = [
        packet
        for directions in states.values()
        for direction_packets in directions.values()
        for packet in direction_packets
    ]
    assert len(packets) == 255
    assert sum("codec_class" not in packet for packet in packets) == 1
    assert [packet["name"] for packet in packets if "codec_class" not in packet] == [
        "BUNDLE"
    ]
    assert all("declared_fields" in packet for packet in packets if "codec_class" in packet)
    assert sum(bool(packet.get("declared_fields")) for packet in packets) == 232
    login_finished = states["login"]["clientbound"][2]
    assert login_finished["codec_class"].endswith("ClientboundLoginFinishedPacket")
    assert [field["name"] for field in login_finished["declared_fields"]] == [
        "gameProfile",
        "sessionId",
    ]
    login_hello = states["login"]["serverbound"][0]
    assert login_hello["codec_operations"] == [
        "net.minecraft.network.FriendlyByteBuf.readUtf",
        "net.minecraft.network.FriendlyByteBuf.readUUID",
        "net.minecraft.network.FriendlyByteBuf.writeUtf",
        "net.minecraft.network.FriendlyByteBuf.writeUUID",
    ]
    assert states["login"]["serverbound"][3]["terminal"] is True
    assert states["login"]["clientbound"][2]["terminal"] is True
    assert states["configuration"]["serverbound"][3]["terminal"] is True
    assert states["configuration"]["clientbound"][3]["terminal"] is True
    for directions in states.values():
        for packets in directions.values():
            assert [packet["id"] for packet in packets] == list(range(len(packets)))


if __name__ == "__main__":
    main()