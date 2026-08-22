# mcsquared

An independent C++20 implementation targeting the Minecraft Java Edition 26.2 wire protocol (protocol 776).

The executable implements status/ping and an offline Login-to-Play protocol path with compression, known-pack negotiation, Play Login, keepalive, and movement-driven chunk streaming. An optional encrypted-offline path exercises the vanilla RSA and AES-CFB8 transport. The world library provides deterministic infinite terrain, biome-aware surfaces, trees, flowers, ores, bounded chunk caching, and persistent region storage. The Play session maintains a moving 5×5 generated chunk window as the player crosses chunk boundaries.

Protocol research is maintained in [docs/PROTOCOL_1_26_2.md](docs/PROTOCOL_1_26_2.md), the world format in [docs/WORLD.md](docs/WORLD.md), content registries/crafting in [docs/CONTENT.md](docs/CONTENT.md), and entity/AI systems in [docs/ENTITIES.md](docs/ENTITIES.md). The implementation ledger is in [TODO.md](TODO.md).

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Developer presets provide `debug`, `release`, `asan`, and `coverage` configure/build/test workflows. Dependencies are locked by `vcpkg.json`; set `CMAKE_TOOLCHAIN_FILE` to a vcpkg checkout to acquire the pinned OpenSSL and zlib versions.

## Run

```sh
./build/mcsquared --port 25565 --motd "mcsquared"
```

Add `--encrypted-offline` to require RSA key exchange and AES-CFB8 without Mojang session authentication. Use `--online-mode` for Mojang `hasJoined` authentication; `--session-server-url` can override the endpoint for testing.

Login Custom Query policy is available through `--login-query-channel`, `--login-query-payload`, and `--require-login-query-response`.

Play keepalives default to a 15-second interval and 30-second timeout. Tests and deployments can override these with `--keepalive-interval-ms` and `--keepalive-timeout-ms`.

Connection admission supports hard global/per-IP quotas plus a rolling per-IP limit through `--connection-rate-limit`. Official wire sessions can be recorded as bounded NDJSON with `tools/session-capture/proxy.py`; the checked-in 26.2 Status/Ping fixture is tied to the verified official server JAR hash.

Configuration supports compact known-pack registries and authoritative inline-NBT fallback. Resource packs can be configured with `--resource-pack-url`, `--resource-pack-hash`, and `--require-resource-pack`. Vanilla recipe property sets and recipe-book initialization are synchronized on entering Play.

`--world PATH` persists dirty chunks and complete scalar world state on the configured autosave interval and on orderly session teardown. Restarts restore the seed, spawn, clocks, weather, difficulty, gamerules, and world border before Play synchronization.

The implementation currently targets POSIX networking APIs and builds on macOS and Unix-like systems.