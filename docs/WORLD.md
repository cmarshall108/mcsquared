# World and Chunk System

## Coordinates

- Chunks are 16 by 16 columns.
- Build height is Y=-64 through Y=319, represented by 24 sections.
- Biomes are stored at quart resolution: 4 by 4 cells per section.
- Region coordinates use mathematical floor division by 32, so chunk `(-33, 64)` maps to region `(-2, 2)` and local slot `(31, 0)`.

## Generation

`ChunkGenerator` is deterministic for a seed and chunk coordinate. It combines coarse continental noise and finer detail noise, then assigns ocean, plains, forest, desert, or mountain biomes from height, temperature, and humidity samples. Columns contain bedrock, stone, deterministic coal and iron ore, dirt or sand surface layers, grass, and water up to sea level.

Generation is infinite in both coordinate axes and does not depend on generation order. It is intentionally a basic independent generator; vanilla 26.2 noise-router, carver, feature, and structure parity remains pending.

## Lifecycle

`ChunkManager` serializes first-load generation so concurrent requests for one coordinate receive the same shared chunk. Loaded chunks track access order. When the configured limit is exceeded, the least-recently-used chunk with no external owner is evicted; externally referenced chunks are never invalidated.

With a storage path configured, loading checks persistent storage before generation. Newly generated chunks are saved immediately.

Autosave and orderly world destruction flush every dirty chunk plus `level.mcd`. The `MCL2` metadata record stores the generation seed, spawn, dimensions, gamerules, game/day clocks, daylight and weather-cycle flags, weather duration and exact fade levels, difficulty, and the complete world-border interpolation/warning state. Existing `MCL1` metadata remains readable and receives defaults for fields it did not contain.

## Region Files

Region files use the standard `.mca` container layout:

- 4 KiB location table
- 4 KiB timestamp table
- 4 KiB-aligned chunk sectors
- Three-byte sector offset and one-byte sector count per location
- Four-byte chunk length, compression type `2`, and zlib payload

The payload currently uses the internal `MCC1` binary schema containing chunk coordinates, 256 height values, 1,536 biome cells, and all 24 block sections. It is bounded to 16 MiB uncompressed and validated for truncation and trailing data. Vanilla NBT payload compatibility, free-sector reclamation, journaling, and recovery remain pending.