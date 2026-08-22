# Minecraft 26.2 Protocol Specification

## Scope

This document is the implementation specification for the Minecraft Java Edition 26.2 dedicated-server protocol. The requested filename retains the `1_26_2` spelling; metadata extracted from the supplied official server JAR identifies the release as `26.2`.

Packet facts must be traceable to the named server JAR, generated reports, or captured official-client conformance tests. Unknown fields remain marked `TBD` rather than inferred from older protocol revisions.

## Verified release metadata

| Field | Value |
| --- | --- |
| Release name | 26.2 |
| Protocol version | 776 |
| World version | 4903 |
| Required Java runtime | 25 |
| Data pack version | 107.1 |
| Resource pack version | 88.0 |
| Named server JAR SHA-256 | `183c0499c5f855570ee487dd38e141a53f0121f83a0b07a3bac2d8b6698823e8` |

## Connection lifecycle

```text
TCP Connected
  -> Handshaking
      -> Status -> Closed
      -> Login -> Configuration -> Play -> Closed
      -> Transfer -> Login -> Configuration -> Play -> Closed
```

Each packet ID is scoped by connection state and direction. State changes take effect only at the transition defined by the corresponding packet handler.

## Framing

### Uncompressed

| Field | Encoding |
| --- | --- |
| Packet length | VarInt; byte length of packet ID and payload |
| Packet ID | VarInt |
| Payload | Packet-specific fields |

### Compressed

| Field | Encoding |
| --- | --- |
| Packet length | VarInt; byte length of data length and following data |
| Data length | VarInt; zero for an uncompressed packet below threshold |
| Data | Packet ID and payload, optionally zlib-compressed |

Compression limits, threshold validation, and exact 26.2 behavior: **TBD by extraction and conformance test**.

### Encryption

Login encryption uses the Java protocol's negotiated encrypted stream. Key sizes, digest construction, AES mode, authentication flow, and failure behavior: **TBD by 26.2 extraction and conformance test**.

## Primitive codecs

| Type | Wire representation | Status |
| --- | --- | --- |
| VarInt | Signed 32-bit value in at most five continuation bytes | Verified and tested |
| String | VarInt byte length followed by UTF-8 | Verified and tested |
| Unsigned Short | Two bytes, network byte order | Verified and tested |
| Long | Signed 64-bit integer, network byte order | Verified and tested |
| Boolean | TBD | Extraction pending |
| UUID | TBD | Extraction pending |
| Identifier | TBD | Extraction pending |
| Position | TBD | Extraction pending |
| NBT | TBD | Extraction pending |
| Text component | TBD | Extraction pending |
| Item stack | TBD | Extraction pending |

## Handshaking packets

### Serverbound `0x00`: Client Intention

| Field | Encoding |
| --- | --- |
| Protocol version | VarInt; `776` for release 26.2 |
| Server address | String; at most 255 Java UTF-16 code units |
| Server port | Unsigned Short |
| Intent | VarInt; `1` Status, `2` Login, `3` Transfer |

## Status packets

| Direction | ID | Packet | Fields | Status |
| --- | ---: | --- | --- | --- |
| Serverbound | `0x00` | Status Request | None | Implemented and tested |
| Clientbound | `0x00` | Status Response | JSON string, maximum 32,767 bytes | Implemented and tested |
| Serverbound | `0x01` | Ping Request | Long timestamp/payload | Implemented and tested |
| Clientbound | `0x01` | Ping Response | Echoed Long | Implemented and tested |

The implemented status JSON includes `version`, `players`, `description`, and `enforcesSecureChat`.

## Login packets

Registration order is generated from `LoginProtocols` into `docs/generated/protocol-776.json`.

| Direction | ID | Packet | Fields | Status |
| --- | ---: | --- | --- | --- |
| Serverbound | `0x00` | Hello | String(16), UUID | Implemented and tested |
| Serverbound | `0x01` | Key | ByteArray encrypted secret, ByteArray encrypted challenge | Implemented and tested |
| Serverbound | `0x02` | Custom Query Answer | Transaction and nullable payload | Implemented and tested |
| Serverbound | `0x03` | Login Acknowledged | None; terminal transition | Implemented and tested |
| Serverbound | `0x04` | Cookie Response | Identifier and optional payload | Implemented and tested |
| Clientbound | `0x00` | Login Disconnect | Text component | Implemented and tested |
| Clientbound | `0x01` | Hello | String(20), ByteArray public key, ByteArray challenge, Boolean authenticate | Implemented and tested |
| Clientbound | `0x02` | Login Finished | GameProfile, session UUID; terminal transition | Implemented and tested |
| Clientbound | `0x03` | Login Compression | VarInt threshold | Implemented and tested |
| Clientbound | `0x04` | Custom Query | Transaction, channel, and payload | Implemented and tested |
| Clientbound | `0x05` | Cookie Request | Identifier | Implemented and tested |

The tested offline sequence is Hello, optional RSA/AES negotiation, optional Custom Query, Set Compression, Login Finished, and Login Acknowledged. Offline profiles use Java-compatible version-3 UUIDs derived from `OfflinePlayer:<name>`.

Online mode sends Encryption Request with authentication enabled, validates the RSA challenge, enables AES-CFB8, computes the Java signed SHA-1 server hash, calls the Mojang-compatible `hasJoined` endpoint through verified TLS, validates the returned UUID/name/property bounds, and includes signed and unsigned profile properties in Login Finished. A local mock-session integration test covers this flow without contacting Mojang.

## Configuration packets

`ConfigurationProtocols` registers 20 clientbound and 10 serverbound packet types. Their exact IDs and type fields are generated in `docs/generated/protocol-776.json`.

The implemented core-pack path sends, in order:

1. Clientbound Custom Payload `0x01`, channel `minecraft:brand`.
2. Clientbound Update Enabled Features `0x0C`, containing `minecraft:vanilla`.
3. Clientbound Select Known Packs `0x0E`, requesting `minecraft:core:26.2`.
4. Serverbound Select Known Packs `0x07`, validated against that request.
5. Twenty-nine Clientbound Registry Data `0x07` packets, containing 397 entry identifiers in official order. Entry NBT is absent because the client accepted `minecraft:core:26.2` and loads matching payloads locally.
6. Clientbound Update Tags `0x0D`, containing the 704 network-safe tags across 15 registries the client owns. Nested references and optional members are resolved to raw IDs; damage types use official bootstrap order. Tags for non-synchronized registries such as `worldgen/flat_level_generator_preset` are intentionally omitted.
7. Clientbound Finish Configuration `0x03`, with no payload.
8. Serverbound Finish Configuration `0x03`, with no payload and a terminal transition to Play.

Accepting a known pack suppresses each registry entry's NBT payload, not the Registry Data packet itself. Entry identifiers are still required to create client registries such as `minecraft:damage_type`. Complete tags are then applied to those registries. Full NBT payload fallback for clients that reject the known pack remains pending.

## Play packets

The generated manifest contains all 210 Play registrations with state-local IDs and official packet-type fields. Implemented entry packets are:

| Direction | ID | Packet | Fields | Status |
| --- | ---: | --- | --- | --- |
| Clientbound | `0x31` | Login | Entity ID, worlds, distances, flags, spawn info, security flags | Implemented and tested |
| Clientbound | `0x2C` | Keep Alive | Long ID | Implemented and tested |
| Serverbound | `0x1C` | Keep Alive | Echoed Long ID | Implemented and tested |
| Serverbound | `0x1E`/`0x1F` | Move Player Pos/PosRot | Position, optional rotation, packed flags | Implemented and tested |
| Clientbound | `0x25` | Forget Level Chunk | Packed ChunkPos Long | Implemented and tested |

Initial spawn entry also sends Set Chunk Cache Center `0x5E`, Set Default Spawn Position `0x61`, and Player Position `0x48`. The client must acknowledge teleport ID `1` with serverbound Accept Teleportation `0x00`. The server then remains in a recurring keepalive/dispatch loop instead of closing after one response. The first keepalive uses the normal 15-second cadence, avoiding a race while the client installs its outbound Play protocol; responses time out after 30 seconds.

The initial terrain stream begins with Game Event `0x26`, event `13` (`LEVEL_CHUNKS_LOAD_START`), then sends Chunk Batch Start `0x0C`, a 5×5 neighborhood of Level Chunk With Light `0x2D` packets centered on `(0,0)`, and Chunk Batch Finished `0x0B`. Spawn Y is selected from the generated center column. Each chunk contains 24 sections for Y=-64..319, 26.2 local palettes with authoritative default block-state IDs, generated biome palettes, three populated heightmaps, no block entities, and full skylight across all 26 light sections. Position packets update a bounded 5×5 subscription: the server sends Set Chunk Cache Center `0x5E`, packed Forget Level Chunk `0x25` packets, and a batch containing only newly visible chunks. Serverbound Chunk Batch Received `0x0B` and other legal Play traffic may arrive before teleport acknowledgement; the acknowledgement is enforced by a 10-second deadline rather than packet ordering.

The Play Login describes `minecraft:overworld`, dimension-type registry ID zero, Survival mode, sea level 63, and offline security flags. Command, recipe, entity, inventory, and broader gameplay packet codecs remain for later phases.

### Configuration coverage and network NBT

All 20 clientbound and 10 serverbound Configuration registrations have bounded codecs. The server validates legal ancillary packets during known-pack negotiation and after clientbound Finish Configuration, and enters Play only after an empty serverbound Finish Configuration acknowledgement. Illegal packets receive a Configuration Disconnect before closure.

Minecraft 26.2 context-free Components and direct Dialogs are codec values serialized through `NbtOps` and `NbtIo.writeAnyTag`; they are not JSON strings. Network any-tags begin with the tag type and omit a root name. Custom Click Action wraps its optional any-tag in a VarInt byte length capped at 65,536 bytes; its untrusted NBT accounting is capped at 32 KiB and depth 16, with `TAG_End` representing absence. The implementation supports every NBT tag family with explicit byte, depth, string, and collection limits and Java modified-UTF encoding.

Clients that decline `minecraft:core:26.2` receive all 29 Registry Data packets with inline NBT. These packets are captured byte-for-byte from the SHA-256-verified official 26.2 server, individually hashed, validated at startup, and covered by a zero-known-pack Login-to-Play integration test. Accepting the known pack retains the compact identifier-only path.

Optional Configuration resource-pack pushes support URL, SHA-1, required policy, UUID-correlated progress responses, and graceful rejection on required-pack failure. Initial Play synchronization replays the official protocol-776 `RECIPE_BOOK_ADD` and `UPDATE_RECIPES` payloads, also individually SHA-256-verified at startup.

### Play entity and interaction codecs

Protocol 776 entity motion uses the adaptive `LpVec3` codec, not the legacy three-short/8000 representation. Zero is a one-byte sentinel; nonzero vectors encode three normalized 15-bit components in a 48-bit payload with a two-bit scale and optional continuation VarInt. Entity rotation bytes use `floor(degrees * 256 / 360)`. Relative move packets retain signed-short deltas at 1/4096 block resolution. Remove Entities is a bounded count followed by absolute VarInt IDs, not delta-coded IDs.

Player Action, Use Item On, and Use Item packets are decoded with strict action, hand, direction, hit-offset, and sequence bounds. Every accepted prediction sequence receives Block Changed Ack. Entity lifecycle codecs cover spawn, absolute and relative synchronization, adaptive motion, animation/events, head rotation, links, passengers, pickup, and removal; attack/interact inputs are validated for later gameplay dispatch.

Protocol 776 optional ItemStacks begin with a count VarInt; zero is the complete empty-stack encoding. Non-empty component-free stacks contain the item registry VarInt followed by DataComponentPatch counts `added=0` and `removed=0`. Container IDs are VarInts. The implemented inventory family covers container close/content/data/slot, open book/screen/sign editor, cursor item, player inventory, and simple serverbound container controls. Play entry synchronizes the 46-slot player container and cursor as empty; arbitrary component patches remain assigned to the dedicated data-component work.

Scoreboard coverage includes objective add/change/remove with literal Components and absent custom number formats, score set/reset, all 19 display slots, and all five team methods with bounded players and literal display/prefix/suffix Components. Player Info initialization emits the fixed eight-action bitset, profile properties, absent chat session/display name, Survival mode, listing, latency, order, and hat state; the connected profile is added to the tab list immediately after Play Login.

Sound packets use SoundEvent holder references encoded as `registry_id + 1`; zero remains the direct-holder discriminator. Sound sources are enum VarInts 0–9, positioned sounds use signed fixed-point integers at eight units per block, and Stop Sound supports all source/name flag combinations. Hurt animation, difficulty, player rotation, advancement-tab selection, tick state/steps, projectile power, and game-test position packets are also covered. Play initialization sends Normal difficulty and an unfrozen 20 TPS state.

Attribute and mob-effect holders also use `registry_id + 1`. Attribute modifiers use Identifier keys, double amounts, and operation IDs 0–2; updates are bounded to 128 attributes and bounded modifier collections. Effect updates cover ambient, visible, icon, and blend flags, and beacon selections decode optional effect holders.

Entity metadata packets support the serializers used by the current entity model: VarInt ID 1, float ID 3, string ID 4, and Boolean ID 8, followed by the `0xFF` terminator. Metadata indices are unique and index 255 is reserved. Recipe-book interaction covers display removal, all eight settings Booleans, place recipe, per-book setting changes, and seen display IDs.

Level Particles (`0x2F`) writes `overrideLimiter` and `alwaysShow` before its position, offset/speed, fixed-width count, and registry-dispatched particle options. Protocol 776 has 125 particle types; option payloads are bounded and follow the particle registry ID unchanged.

Update Advancements (`0x82`) orders reset, added list, removed set, progress map, and `showAdvancements`. Empty reset synchronization is supported. Seen Advancements (`0x32`) has two actions: OPENED_TAB (ordinal 0 plus Identifier) and CLOSED_SCREEN (ordinal 1 with no tab field).

## Registry and world codecs

The following sections are populated from extracted 26.2 registries and codecs:

- Dynamic registry synchronization and holder/reference encoding
- Tags and known-pack negotiation
- Data components and item-stack patches
- NBT limits and canonical forms
- Chunk sections, palettes, heightmaps, biomes, light, and block entities
- Entity metadata serializers and attribute modifiers
- Command trees and argument serializers
- Recipes, particles, sounds, statistics, advancements, and loot-related payloads

## Security and resource limits

Every variable-length field must have a protocol-derived bound before allocation. The implementation specification must define limits for packet frames, decompression ratio and output, strings, arrays, maps, NBT depth and size, command trees, registry payloads, chunk data, pending writes, connection rate, and per-client work.

## Extraction provenance

The supplied outer JAR is a bundler. The verified named server JAR is `META-INF/versions/26.2/server-26.2.jar`. `tools/mc-extract/extract.py` verifies its digest before extraction. Generated packet rows must record the source class and codec or registration method used to derive each fact.

## Conformance matrix

| Capability | Unit | Loopback | Official client | Differential capture |
| --- | --- | --- | --- | --- |
| Primitive codecs | Passing | N/A | N/A | Pending |
| Status and ping | Passing | Passing | Pending | Pending |
| Login, all registered packet types | Passing | Passing | Pending | Pending |
| Compression | Passing | Passing | Pending | Pending |
| RSA/AES encrypted offline Login | Passing | Passing | Pending | Pending |
| Configuration core-pack path | Passing | Passing | Pending | Pending |
| Play spawn entry, teleport, and keepalive | Passing | Passing | Pending | Pending |
| Play registration corpus | Generated | N/A | Pending | Pending |