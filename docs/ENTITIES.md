# Entities, Mobs, Animals, and AI

## Registry

The verified content extractor reads `EntityTypes` from the named 26.2 server JAR and emits 158 ordered entries with canonical names, protocol ordinals, source fields, and Java implementation classes. The normalized `MCREGISTRIES1` stream loads those entries into `EntityTypeRegistry` while retaining protocol IDs separately from internal runtime IDs.

Package and name rules classify 92 entries as living monsters, creatures, ambient mobs, axolotls, or aquatic entities. Generated types receive category defaults; exact per-species dimensions, attributes, tracking ranges, immunity flags, and serializers remain pending extraction.

## Lifecycle

`EntityManager` owns entities by monotonic runtime ID and deterministic version-4 UUID. Entities track transforms, velocity, rotations, AABBs, metadata, teams, passengers, vehicles, and leashes. Living entities add health, attributes, equipment, and timed status effects.

Manager removal cleans mounts and leashes. Snapshot restoration is two-pass: all entities are restored first, then relationship IDs are validated and reconstructed. Snapshots preserve identity, type, position, velocity, rotation, health, metadata, teams, vehicles, and leash holders.

## Movement and queries

Base ticking applies gravity, drag, velocity integration, pitch bounds, and ground clamping. AABB intersection supports bounded spatial queries. World block collision, fluid movement, climbing, portals, projectiles, and vehicle-specific physics remain pending.

## AI

Brains store typed memories such as target entities, homes, panic state, temptation, and last-seen positions. Goal selectors enforce numeric priority and mutually exclusive move/look/jump/target controls. Higher-priority goals preempt conflicting lower-priority goals; stopped goals release controls before replacements start.

Navigation uses bounded four-neighbor A* and reconstructs deterministic paths. Built-in generic goals provide wandering and melee pursuit. `MobAiSystem` attaches wandering to living entities and melee targeting to monsters.

## Spawning

Category settings match the extracted 26.2 values:

| Category | Cap | Friendly | Persistent | No-despawn | Despawn |
| --- | ---: | --- | --- | ---: | ---: |
| Monster | 70 | No | No | 32 | 128 |
| Creature | 10 | Yes | Yes | 32 | 128 |
| Ambient | 15 | Yes | No | 32 | 128 |
| Axolotls | 5 | Yes | No | 32 | 128 |
| Underground water | 5 | Yes | No | 32 | 128 |
| Water creature | 5 | Yes | No | 32 | 128 |
| Water ambient | 20 | Yes | No | 32 | 64 |
| Misc | Unlimited | Yes | Yes | 32 | 128 |

Natural spawning deterministically selects eligible registered types, enforces category caps, blocks monsters in peaceful or bright conditions, and requires sufficient light for creatures. Distance despawning preserves persistent categories.

## Animals

`AnimalSystem` supplies shared adult/baby ages, breeding cooldowns, love duration, same-type offspring, inherited variants, taming, and owner UUIDs. Species-specific foods, genetics, trust, flocking, schooling, ride controls, schedules, villages, and trades remain pending specializations.