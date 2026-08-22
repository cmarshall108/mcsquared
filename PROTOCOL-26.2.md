# Minecraft 26.2 Protocol

The maintained specification and generated registration manifest are in `docs/PROTOCOL_1_26_2.md` and `docs/generated/protocol-776.json`.

This document records facts verified from `tools/mc-extract/server.jar` and wire behavior covered by this repository's tests. It is intentionally incomplete until packet-level extraction is performed.

## Verified metadata

| Field | Value |
| --- | --- |
| Minecraft | 26.2 |
| Protocol | 776 |
| World version | 4903 |
| Java runtime | 25 |
| Data pack | 107.1 |
| Resource pack | 88.0 |

Source: the JAR's embedded `version.json`, read on 2026-08-21.

## Wire conventions

- Packet lengths and IDs use Minecraft VarInt encoding.
- Signed 32-bit VarInts use unsigned continuation encoding and occupy at most five bytes.
- Strings are encoded as a VarInt byte length followed by UTF-8 data. `readUtf(n)` accepts at most `n * 3` encoded bytes and at most `n` decoded Java UTF-16 code units.
- Unsigned shorts and signed longs are transmitted in network byte order.
- Packet layout depends on connection state: Handshaking, Status, Login, Configuration, and Play.
- Compression threshold and encryption negotiation are negotiated during Login.
- Exact 26.2 packet IDs, field order, registries, codec payloads, and configuration transitions must be extracted from the bundled classes before implementing a compatible client connection.

## Implemented packets

All IDs in this section are state-local and clientbound/serverbound as shown.

### Handshaking: serverbound `0x00`

| Field | Encoding |
| --- | --- |
| Protocol version | VarInt (`776` for 26.2) |
| Server address | String, at most 255 UTF-16 code units |
| Server port | Unsigned Short |
| Next state | VarInt (`1` for Status) |

The implementation parses but does not require protocol `776` so server-list clients from other versions can read the advertised version. Only next state `1` is currently accepted.

Bytecode source: `ClientIntentionPacket(FriendlyByteBuf)` invokes, in order, `readVarInt`, `readUtf(255)`, `readUnsignedShort`, and `readVarInt`. `ClientIntent.byId` maps `1` to Status, `2` to Login, and `3` to Transfer.

### Status: serverbound `0x00`

The request has no fields.

### Status: clientbound `0x00`

The response contains one JSON value limited to 32,767 bytes by `ByteBufCodecs.lenientJson(32767)`. The implemented response object contains `version`, `players`, `description`, and `enforcesSecureChat` members.

### Status: serverbound/clientbound `0x01`

The Ping Request and Ping Response each contain one signed 64-bit integer. The response echoes the request value exactly.

In the 26.2 JAR these classes live in `network.protocol.ping`, but `StatusProtocols` registers them second in both Status directions, assigning packet ID `0x01`. Their codecs invoke Netty/FriendlyByteBuf `readLong` and `writeLong`.

## Verified behavior

- The server accepts a Handshaking packet with protocol `776` and routes next state `1` (Status).
- Status Request (`0x00`) receives a Status Response (`0x00`) containing a length-prefixed JSON string with version `26.2`, protocol `776`, zero online players, and description `mcsquared`.
- Ping Request (`0x01`) receives Ping Response (`0x01`) with the original eight-byte payload.
- Status connections are handled by a fixed worker pool with a bounded pending queue.
- `status_integration_test` verifies this exchange over a real loopback TCP connection.

## Extraction notes

The supplied outer JAR is a bundler. Its named nested server JAR is `META-INF/versions/26.2/server-26.2.jar`, with SHA-256 `183c0499c5f855570ee487dd38e141a53f0121f83a0b07a3bac2d8b6698823e8`. The `tools/mc-extract/extract.py` utility verifies that digest before extracting it. This repository does not treat an older wiki revision as authoritative.