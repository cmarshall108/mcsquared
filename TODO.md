# Implementation Ledger

Checked items are implemented and tested. Registry-driven tasks are complete only when every entry extracted from the official 26.2 server data is implemented and covered by conformance tests.

## Phase 1: Project skeleton

- [x] C++20 root project configuration and warning policy
- [x] Target-owning CMake files for every source subsystem
- [x] Public include tree and subsystem forward declarations
- [x] Protocol documentation skeleton at `docs/PROTOCOL_1_26_2.md`
- [x] Phase-aligned implementation ledger
- [x] Preserve and validate the existing status protocol implementation
- [x] Add dependency-locking and reproducible third-party acquisition
- [x] Add developer presets for debug, release, sanitizers, and coverage
- [x] Add Linux, macOS, and Windows continuous-integration configurations

## Phase 2: Core networking and protocol

### Extraction and conformance

- [x] Verify release 26.2, protocol 776, world version 4903, and pack versions from the bundled official JAR
- [x] Verify the nested named server JAR by SHA-256 before extraction
- [x] Automate extraction of every packet registration, state-local ID, and direction
- [x] Automate extraction of packet declared fields, codec operations/references, and terminal state transitions
- [x] Extract all dynamic JSON registries, tags, feature flags, component types, recipes, loot tables, and command argument types
- [x] Generate machine-readable protocol manifests with packet IDs, codec classes, and declared-field provenance
- [x] Capture official client/server sessions for differential conformance tests
- [x] Reject packet registrations lacking codec/detail provenance, except the verified synthetic bundle wrapper

### Transport and framing

- [x] Implement bounded VarInt, string, unsigned-short, long, and packet-frame codecs
- [x] Implement exact reads/writes and bounded packet allocation
- [x] Implement TCP accept, bounded dispatch, status request, and ping
- [x] Implement cross-platform socket lifecycle, polling, I/O, timeouts, and Windows Winsock linkage
- [x] Convert accepted sockets to nonblocking event-driven operation on all platforms
- [x] Implement event-driven reads/writes, partial frames, scatter/gather I/O, queues, and backpressure
- [x] Implement socket timeouts plus tested global and per-IP active connection quotas
- [x] Implement rolling rate limits, IP throttling, and protocol-state graceful disconnect packets
- [x] Implement zlib compression negotiation, thresholds, compressed framing, and decompression limits
- [x] Implement AES-CFB8 encryption, RSA key exchange, challenge validation, and secure random generation
- [x] Implement deterministic offline profiles and encrypted-offline transport
- [x] Implement Mojang session-server authentication and authenticated profile properties
- [x] Implement Transfer handshake intent and bounded Login cookie request/response exchange

### Connection states

- [x] Implement Handshaking to Status, Status response, and Ping echo
- [x] Implement Handshaking to Login
- [x] Implement Handshaking to Transfer
- [x] Implement codecs for all five serverbound and six clientbound Login packets
- [x] Implement Mojang-compatible online authentication, signed profile properties, and Custom Query Login policies
- [x] Implement compression setup and login acknowledgement
- [x] Implement active-profile duplicate rejection and structured Login disconnect reasons
- [x] Implement all serverbound and clientbound Configuration packets and legal transitions
- [x] Synchronize known packs, enabled features, 29 core-pack registries, and all 704 network-safe tags from 801 verified definitions
- [x] Synchronize registry fallback data, resource packs, and recipe displays
- [ ] Implement all serverbound and clientbound Play packets
- [x] Implement Play login, keepalive, movement/status, teleport, chunk streaming, and prediction acknowledgement packets
- [x] Implement Play player/world scalar state, HUD/title, border, common cookie/resource-pack/report/link, and game-rule packets
- [x] Implement Play entity lifecycle, adaptive motion, relationships, attack/interact, and block/world update packets
- [x] Implement bounded block-entity data framing and transaction-matched null block/entity tag queries
- [x] Implement Play empty/component-free inventory, container UI/control, Player Info, scoreboard, team, and sound/effect packets
- [ ] Implement remaining Play metadata, particles, advancements, signed chat, complex inventory, and debug/test packet families
- [x] Implement Play attribute snapshots/modifiers, mob-effect updates/removals, and beacon effect selection
- [x] Implement Play entity metadata for bool, VarInt, float, and string serializers used by the entity model
- [x] Implement Play byte, VarLong, rotations, block-position, block-state, optional-unsigned-int, and pose metadata serializers
- [x] Implement component-free ItemStack entity metadata serializer for dropped items
- [x] Implement Play recipe-book removal/settings and place/settings/seen interaction packets
- [x] Implement Play level-particle framing with bounded registry-dispatched option payloads
- [x] Implement Play empty advancement synchronization and open/close tab actions
- [ ] Implement remaining Play metadata serializers, particles, advancements, signed chat, complex inventory, and debug/test packet families
- [x] Implement recurring keepalive timeout and teleport confirmation
- [ ] Implement latency tracking, complete movement validation, and configurable idle timeout
- [x] Track keepalive round-trip latency for active Play sessions
- [x] Implement configurable non-keepalive Play idle timeout and CLI option
- [x] Reject excessive per-packet movement and local solid-block AABB intersections
- [x] Issue stateful correction teleports and require matching acknowledgements
- [x] Send cache center, default spawn, and initial player position after entering Play
- [x] Reach Play state with an unmodified official 26.2 client

### Protocol data types

- [x] Implement bounded scalar, VarInt/VarLong, optional, collection, bitset, identifier, UUID, packed-position, and angle codecs
- [ ] Implement every registry enum and structured text-component codec
- [ ] Implement bounded NBT and registry-aware NBT codecs
- [ ] Implement item-stack data components and patch codecs
- [ ] Implement entity metadata, attributes, particles, sounds, recipes, statistics, advancements, and command-tree codecs
- [x] Implement bounded executable-literal command trees synchronized at Play entry
- [x] Implement single-value and local chunk section/biome palettes, packed storage, batch markers, populated heightmaps, block-entity lists, and light masks
- [ ] Implement global palettes, populated block entities, and incremental light updates
- [x] Implement bounded global block-state and biome palette fallback encoding
- [x] Implement heightmap-derived skylight arrays and incremental light updates after block changes
- [ ] Implement signed chat, message acknowledgement, filtering, reporting metadata, and secure-chat enforcement
- [ ] Fuzz every decoder and verify malformed input cannot exhaust memory or stall workers

## Phase 3: World and chunk system

### Storage and lifecycle

- [x] Implement atomic level metadata with world seed, spawn, dimensions, game rules, and future-version rejection
- [x] Persist and restore clocks, weather fades/cycles, difficulty, gamerules, spawn, and world-border interpolation on autosave and shutdown
- [ ] Implement older data-version migration and vanilla level.dat compatibility
- [x] Implement Anvil location/timestamp headers, sector records, zlib compression, and overwrites
- [x] Implement versioned chunk payload serialization for sections, heightmaps, and biomes
- [ ] Implement vanilla chunk NBT, atomic writes, free-sector reuse, recovery, and corruption tooling
- [x] Implement chunk dirty tracking and dirty-only region persistence
- [x] Save dirty chunks before unload, cache eviction, and world shutdown
- [ ] Implement chunk NBT, entities, points of interest, scheduled ticks, heightmaps, sections, and data-version migration
- [ ] Implement player-data persistence, world borders, maps, raids, scoreboards, advancements, and statistics
- [x] Implement thread-safe on-demand chunk loading, de-duplication, persistence, bounded LRU caching, and unloading
- [x] Implement movement-driven bounded chunk subscriptions, batched loading, cache-center updates, and client unloads
- [ ] Implement chunk tickets, view/simulation distance, asynchronous load/generation/save, and prioritization
- [ ] Implement deterministic server ticks, random ticks, scheduled ticks, block events, and autosave
- [x] Implement deterministic scheduled block ticks and neighbor-trigger queues
- [x] Implement delayed sand/gravel gravity through scheduled neighbor updates
- [x] Implement deterministic active-chunk random block tick sampling
- [x] Implement configurable periodic dirty-chunk autosave
- [ ] Implement crash-safe shutdown, backups, lock files, and read-only recovery tooling

### Infinite Overworld generation

- [x] Extract all 26.2 biome, noise, density-function, carver, configured-feature, and placed-feature registries
- [x] Implement deterministic seeded positional mixing across positive and negative coordinates
- [ ] Match vanilla random sources and positional-randomness sequences
- [x] Implement basic height-noise terrain, biome climate sampling, surface layers, sea fill, and ores
- [x] Implement deterministic biome-aware oak trees, short grass, dandelions, and poppies
- [ ] Implement climate sampling, biome source, noise router, aquifers, surface rules, and terrain shaping
- [ ] Implement caves, ravines, fluid simulation, disks, springs, geodes, lakes, and vanilla-compatible decoration placement
- [x] Implement infinite deterministic generation across positive and negative coordinates
- [ ] Implement vanilla-compatible spawn selection
- [ ] Match official biome, height, block, and feature samples for fixed conformance seeds

## Phase 4: Blocks, items, inventory, and crafting

### Blocks

- [x] Generate all 1,196 canonical block names and field/ordinal provenance from official 26.2 bytecode
- [ ] Extract every block state, property, shape, material, sound, and behavior binding
- [ ] Implement every registered block's properties, shapes, state transitions, placement, use, drops, and destruction behavior
- [ ] Implement block entities, serialization, ticking, menus, synchronization, and update packets
- [ ] Implement neighbor updates, scheduled ticks, random ticks, gravity, fluids, fire, explosions, pistons, and redstone
- [x] Implement random-tick grass spread, covered-grass decay, and sparse short-grass growth
- [x] Implement scheduled downward water flow and deterministic lateral source spread
- [ ] Implement mining speed, tools, enchantments, harvest rules, durability, and block-breaking progress
- [x] Implement selected-tool mining speed and correct-for-drops context in live breaking
- [x] Implement basic replaceability-aware block placement, stack consumption, hardness timing, tool-gated drops, and breaking
- [x] Wire validated Survival block breaking/placement into Play with reach, collision, inventory, updates, and falling sand/gravel
- [ ] Implement reach, collision, permissions, state placement, and prediction acknowledgement
- [x] Implement occlusion-aware reach validation for block breaking and placement
- [x] Reject block placement volumes occupied by the player or living entities

### Items and data components

- [x] Generate all 1,538 canonical item/block-item names with field/derived provenance
- [ ] Extract every item default component map, stack limit, durability, rarity, and behavior binding
- [ ] Implement every registered item's use, use-on, consume, equip, projectile, cooldown, durability, and crafting behavior
- [x] Implement bow use with arrow ammunition consumption and internal durability loss
- [ ] Implement item stack limits, component patches, damage, enchantments, names, lore, attributes, food, containers, and serialization
- [x] Implement bounded internal tool durability mutation and breakage on successful mining
- [x] Implement item nutrition and saturation properties for ordinary food
- [ ] Implement dropped items, pickup rules, merging, despawning, and ownership delays
- [x] Implement component-free dropped-item entities, pickup delay, nearby stack merging, hotbar pickup results, and five-minute despawn
- [x] Implement basic species death loot, ItemEntity metadata, pickup animation, and hotbar insertion

### Inventory and crafting

- [x] Implement transactional containers plus player main, armor, and offhand storage
- [ ] Implement equipment slot restrictions, cursor stack, ender chest, and creative inventory
- [x] Enforce armor-slot item restrictions across direct writes, insertion, and transactions
- [x] Implement authoritative nine-slot hotbar state and selected-slot tracking
- [x] Merge eligible block harvest drops into available hotbar stacks
- [x] Implement selected-stack drop-one/drop-all actions with delayed ItemEntity pickup
- [x] Implement selected-hotbar and offhand swap with synchronized inventory slots
- [ ] Implement every vanilla container/menu type, slot rule, synchronization state, quick move, drag, swap, clone, and drop action
- [ ] Validate transaction state IDs and reject duplication, stale clicks, and invalid stacks
- [x] Decode bounded component-free 26.2 container clicks and reject unsupported state with authoritative resynchronization
- [x] Extract all 1,585 official recipe JSON records and 489 block/item tags
- [x] Expand tags and load all 1,056 ordinary shaped/shapeless recipes, with four bootstrap duplicates skipped
- [ ] Implement cooking, smithing, stonecutting, transmute, special, and display recipe serializers
- [ ] Implement recipe book discovery, placement, filtering, and synchronization

## Phase 5: Entities, mobs, animals, and AI

### Entity framework

- [x] Generate all 158 entity type names, protocol ordinals, and Java implementation provenance
- [x] Implement runtime IDs, deterministic UUIDs, dimensions, metadata, attributes, equipment, effects, passengers, leashes, teams, and snapshots
- [x] Implement velocity, gravity, drag, ground clamping, rotations, AABBs, and spatial queries
- [ ] Implement world collision, fluids, climbing, portals, vehicle physics, projectile physics, and interpolation
- [x] Implement solid-block voxel collision-volume and line-of-sight queries
- [x] Implement bounded entity water drag and buoyancy response
- [x] Implement bounded projectile spawn, gravity flight, lifetime, block/entity impact, and removal
- [x] Convert recoverable arrows to delayed dropped-arrow items after block impact or expiry
- [x] Spawn unmerged block-harvest overflow as delayed ItemEntities
- [ ] Implement tracking ranges, spawn/despawn packets, metadata deltas, velocity, head rotation, and chunk migration
- [x] Implement category-aware entity tracking ranges and visibility enter/leave transitions
- [x] Implement relative movement updates with absolute resync for large deltas
- [x] Implement deduplicated entity rotation, head rotation, and velocity tracking
- [x] Implement living health, healing, direct damage, death removal, and relationship cleanup
- [x] Implement basic living-entity knockback and apply it on successful melee attacks
- [x] Implement typed damage context, melee attacker attribution, and hurt invulnerability ticks
- [x] Implement armor and armor-toughness damage mitigation with explicit armor bypass
- [x] Implement poison, wither, and regeneration active-effect ticking and expiry
- [x] Implement speed and slowness movement modifiers across mob goals
- [x] Implement fire-resistance immunity for typed fire damage
- [x] Ignite and damage sun-sensitive undead in exposed daylight with shade, darkness, and headgear protection
- [x] Implement bounded knockback-resistance attribute scaling
- [x] Implement mob death experience rewards, orb entities, proximity pickup, and player level synchronization
- [ ] Implement typed damage sources, invulnerability, knockback, armor, shields, enchantments, drops, and experience
- [x] Implement main/offhand shield use, release, blockable damage reduction, durability loss, and breakage

### AI and spawning

- [x] Implement bounded A* navigation, typed memories, priority goals, control arbitration, wandering, pursuit, and melee attacks
- [ ] Implement full node types, sensing, schedules, look controls, and species-specific brains
- [x] Implement same-species proximity sensing for passive flock and aquatic school leaders
- [x] Implement official category caps/distances, deterministic candidate spawning, peaceful/light gates, and distance despawning
- [x] Gate live surface hostile spawning to nighttime darkness
- [x] Wire recurring natural animal/monster spawning, AI ticks, terrain grounding, attacks, movement, and despawning into Play
- [x] Implement deterministic biome/habitat-weighted land, hostile, and aquatic spawn tables and live selection
- [ ] Implement eligible chunk, biome, structure, pack, patrol, and phantom spawning rules
- [x] Attach generic passive wandering and hostile melee AI across all 92 classified living entity types
- [ ] Implement registry-complete species-specific passive, hostile, neutral, boss, raider, aquatic, and utility behavior
- [x] Implement generic aging, love mode, breeding, baby growth, cooldowns, taming, ownership, and variants
- [ ] Implement species genetics, food/temptation rules, panic, trust, flocking, schooling, riding, and owner commands
- [x] Implement species food matching and held-food temptation goals
- [x] Implement interaction feeding, item consumption, love mode, and nearby breeding
- [x] Implement damage-triggered passive panic memory and timed fleeing behavior
- [x] Implement same-species passive flocking with preferred spacing
- [x] Implement aquatic schooling with water-mob spacing and movement speed
- [x] Implement bone taming, persistent owner UUID, and taming-item consumption for wolves
- [x] Implement owner-follow behavior plus owner-only sit/stand commands and AI suspension
- [x] Replace owner-follow temptation approximation with explicit owner-position memory and goal arbitration
- [x] Enforce owner-follow start/stop distance, panic priority, and sit suspend/resume behavior
- [ ] Implement villagers, professions, work sites, schedules, gossip, reputation, trading, restocking, breeding, villages, raids, and golems
- [x] Add deterministic lifecycle/AI smoke tests for all 92 classified living entity types

## Phase 6: Survival, Creative, and player systems

- [ ] Implement player spawn, movement modes, collision, sprinting, sneaking, swimming, crawling, gliding, riding, sleeping, and portals
- [x] Implement movement-distance and sprint-rate exhaustion tracking
- [x] Decode authoritative sprint, sneak, jump, and movement input state
- [x] Implement sneaking posture, reduced eye height, movement rate, and exhaustion
- [x] Implement underwater swimming mode and swim-specific eye height/exhaustion
- [x] Implement airborne fall-flying activation, posture, and landing cancellation
- [x] Implement gliding fall-distance suppression and zero horizontal exhaustion
- [x] Apply movement-mode eye height to reach and underwater air checks
- [x] Implement interaction-driven mounting of rideable passive entities
- [x] Synchronize external player passengers on mount, remount, and dismount
- [x] Route directional player input and jump impulses to mounted vehicles
- [x] Implement sneak dismount with safe rider repositioning
- [x] Implement rider position following and forced eject when vehicles disappear
- [x] Implement bounded hunger, saturation, exhaustion conversion, and food-consumption state
- [x] Implement natural regeneration and difficulty-aware starvation damage
- [x] Implement air supply, underwater drowning damage, recovery, and health synchronization
- [x] Implement player fall-distance accumulation, landing damage, and reset
- [x] Implement active player speed, slowness, regeneration, poison, and wither effect ticking
- [x] Implement `/effect give|clear` with protocol add/remove and expiration synchronization
- [x] Implement typed player damage context, hostile attacker attribution, and hurt invulnerability
- [x] Implement player armor/toughness mitigation with explicit armor and invulnerability bypass
- [x] Implement player fire-resistance immunity and typed fall, drowning, poison, and wither damage
- [x] Implement absorption-effect hearts, damage consumption, removal, and natural-expiry cleanup
- [ ] Implement health, absorption, armor, air, freezing, fire, fall distance, effects, regeneration, and all damage sources
- [ ] Implement hunger, saturation, exhaustion, food consumption, and difficulty scaling
- [x] Implement selected-food use, stack consumption, and immediate hunger synchronization
- [x] Prevent sprint activation at food level six or below
- [x] Apply Peaceful rapid regeneration and hunger restoration plus Easy/Normal/Hard starvation floors
- [ ] Implement melee, ranged, critical hits, attack cooldown, blocking, knockback, sweeping, friendly fire, and combat tracking
- [x] Implement nearby hostile attacks against the player with cooldown and shield interaction
- [x] Scale hostile melee damage across Peaceful, Easy, Normal, and Hard
- [x] Synchronize player hurt animation and directional knockback motion on hostile hits
- [x] Preserve authoritative yaw from rotation-only and combined movement packets
- [x] Restrict shield mitigation and durability loss to attacks from the front hemisphere
- [x] Implement ranged arrow launch from player yaw/pitch with projectile damage and knockback feedback
- [x] Implement charged melee damage scaling, attack recharge, and attack exhaustion
- [x] Implement falling critical-hit eligibility and damage multiplier
- [x] Implement grounded full-charge sweeping damage to nearby living entities
- [x] Implement combat target tracking and timeout
- [x] Implement line-of-sight validation for primary and sweeping melee targets
- [x] Synchronize swing, hurt, critical, knockback motion, and player head-rotation feedback
- [ ] Implement death messages, drops, keep-inventory, respawn anchors, beds, respawn, and hardcore rules
- [x] Implement health-zero player death state and suppress gameplay actions while dead
- [x] Broadcast a system death message when the player reaches zero health
- [x] Implement keep-inventory-false hotbar clearing and mounted-player eject on death
- [x] Spawn hotbar and offhand contents as delayed ItemEntities on keep-inventory-false death
- [x] Implement runtime `/gamerule keepInventory` changes and immediate value synchronization
- [x] Preserve owned inventory and experience across death/respawn when keep-inventory is enabled
- [x] Implement protocol 776 respawn request/response, survival/experience reset, and spawn teleport
- [x] Add `--hardcore`, advertise it during Play login, and lock difficulty to Hard
- [x] Transition hardcore deaths into forced-flight Spectator mode instead of Survival respawn
- [x] Require matching teleport acknowledgement after player respawn
- [ ] Implement experience orbs, levels, enchanting, anvils, grindstones, repairing, brewing, and beacons
- [x] Centralize total-experience to level/progress calculation across pickup and respawn
- [x] Implement bounded `/experience set|add` and `/xp set|add` with live synchronization
- [x] Drop capped level-based experience orbs and reset client XP on non-keep-inventory death
- [ ] Implement Survival, Creative, Adventure, and Spectator rules and abilities
- [x] Implement Survival/Creative command switching and authoritative abilities
- [x] Extend `/gamemode` and authoritative abilities to Adventure and Spectator
- [x] Enforce Adventure world-modification restrictions while retaining survival combat and item use
- [x] Enforce Spectator invulnerability, collision bypass, forced flight, dismount, and interaction suppression
- [x] Authorize client flight toggles only in Creative/Spectator and correct denied requests
- [x] Suppress movement exhaustion and fall-distance accumulation during authorized flight
- [x] Implement `/difficulty peaceful|easy|normal|hard` with protocol synchronization
- [ ] Implement Creative inventory actions, instant breaking, flight, pick block, and operator block restrictions
- [x] Implement bounded component-free Creative hotbar/offhand slot mutation with authoritative correction
- [x] Implement Creative instant breaking and non-consuming block placement
- [x] Prevent Creative bow ammunition consumption and durability loss
- [x] Implement authoritative Creative flight enable/disable state
- [x] Decode protocol 776 pick-item-from-block requests with strict payload validation
- [x] Implement Creative-only pick block with existing-slot selection and active-slot replacement
- [x] Decode protocol 776 pick-item-from-entity requests and map living types to spawn eggs
- [x] Implement Creative-only entity pick with reach, line-of-sight, and hotbar synchronization
- [ ] Implement chat, commands, command permissions, suggestions, selectors, text components, titles, boss bars, and system messages
- [x] Implement bounded root command suggestions for every live command handler
- [x] Implement unsigned `/say`, `/time`, `/weather`, and `/gamemode` command handling with system feedback
- [x] Implement `/teleport` and `/tp` with finite bounds, border validation, chunk streaming, and acknowledgement
- [x] Implement `/kill` through the shared death, drop, dismount, statistics, and health-sync pipeline
- [x] Implement bounded `/summon <living-type>` with tracking, AI, and animal-system attachment
- [x] Implement bounded `/give <item> [count]` with stack merging, slot sync, and dropped overflow
- [x] Implement `/clear [item]` across live hotbar/offhand storage with slot synchronization
- [ ] Implement operators, whitelist, bans, permissions, teams, scoreboards, objectives, statistics, and advancements
- [x] Implement live play-time, walking, death, mob-kill, block-mined, item-used/dropped/picked-up counters
- [x] Implement protocol 776 periodic statistic updates and request-driven full snapshots
- [ ] Implement weather, time, sleep voting, world border, difficulty, game rules, and multiplayer pause-independent ticking
- [x] Implement on-demand gamerule value synchronization
- [x] Implement mutable `/gamerule keepInventory`, `doDaylightCycle`, and `doMobSpawning` values
- [x] Implement mutable `/gamerule doWeatherCycle` with independent weather fading
- [x] Freeze and resume daylight independently while preserving monotonic game time
- [x] Gate natural creature, monster, and aquatic spawning while retaining despawn maintenance
- [x] Implement validated world-border center, size, absolute limits, and AABB containment
- [x] Implement `/worldborder center` and `/worldborder set` with protocol synchronization
- [x] Reject player movement beyond the active world border with teleport correction
- [x] Implement timed `/worldborder set <size> <seconds>` interpolation and live enforcement
- [x] Implement `/worldborder warning distance|time` state and protocol synchronization
- [x] Implement deterministic game/day clocks with advance-time control
- [x] Implement protocol 776 WorldClock map synchronization at Play entry and one-second intervals
- [x] Implement deterministic rain/thunder cycles with bounded fade levels
- [x] Implement optional validated durations for `/weather clear|rain|thunder`
- [x] Suppress hostile spawning and attacks in Peaceful and remove existing tracked monsters on transition
- [x] Implement protocol 776 weather game events and initial/live weather synchronization

## Phase 7: Dimensions and structures

### Dimensions

- [ ] Implement dimension registry behavior, coordinate scaling, portals, travel validation, and per-dimension ticking
- [ ] Implement infinite Nether terrain, biomes, lava seas, bedrock, caves, ores, vegetation, features, and mob spawning
- [ ] Implement infinite End terrain, islands, void rules, gateways, return portals, features, and mob spawning
- [ ] Implement the End dragon fight, crystals, podium, death sequence, exits, gateways, and persistent fight state

### Structures

- [ ] Extract every 26.2 structure, structure set, template pool, processor list, placement rule, and template
- [ ] Implement starts, references, bounding boxes, spacing, separation, concentric rings, biome checks, and locate commands
- [ ] Implement jigsaw assembly, template transforms, processors, data markers, terrain adaptation, and post-processing
- [ ] Generate every registered Overworld, Nether, and End structure with vanilla-compatible placement and contents
- [ ] Implement mineshafts, strongholds, monuments, mansions, villages, temples, outposts, ancient cities, trial chambers, fortresses, bastions, End cities, and all remaining registry entries
- [ ] Implement structure mobs, chests, loot, maps, exploration targets, and saved state
- [ ] Match official structure starts and sampled pieces for fixed conformance seeds

## Phase 8: Tale of Kingdoms: A New Conquest

- [ ] Pin the exact mod release, loader, Minecraft target, dependencies, configuration, and content sources
- [ ] Produce a legally usable behavioral specification and asset plan for the pinned release
- [ ] Inventory every mod block, item, entity, NPC, profession, structure, biome, sound, particle, menu, command, recipe, loot table, and localization key
- [ ] Implement kingdom creation, settlement growth, guild systems, contracts, ranks, reputation, influence, currency, taxation, and progression
- [ ] Implement every quest, prerequisite, branch, objective, reward, dialogue, trigger, failure condition, and persistence rule
- [ ] Implement every NPC's role, schedule, navigation, combat, trade, dialogue, recruitment, relationship, and respawn behavior
- [ ] Implement every mod structure's generation, upgrades, ownership, interaction, loot, inhabitants, and saved data
- [ ] Implement every mod block, item, equipment set, weapon, tool, consumable, recipe, effect, and data component
- [ ] Implement every menu, HUD synchronization message, command, permission, configuration option, and multiplayer interaction
- [ ] Implement world migration, save compatibility, dedicated-server behavior, and vanilla client compatibility requirements
- [ ] Add content-manifest coverage tests and behavioral regression tests for every inventoried mod feature

## Phase 9: Integration, optimization, and release

### Threading and performance

- [ ] Define thread ownership for network I/O, simulation, generation, storage, and background work
- [ ] Implement bounded queues, cancellation, priorities, backpressure, shutdown barriers, and thread-safe handoff
- [ ] Profile tick time, allocations, packet throughput, chunk generation, pathfinding, and storage latency
- [ ] Implement memory budgets, chunk/entity limits, cache policies, packet batching, and hot-path optimizations
- [ ] Add overload degradation, watchdogs, tick metrics, traces, and operational health endpoints

### Operations and polish

- [ ] Implement command-line options, server properties, EULA acknowledgement, logging configuration, and environment overrides
- [x] Add command-line world storage path and autosave interval options
- [ ] Implement console commands, remote administration, graceful signals, crash reports, user-facing disconnects, and diagnostics
- [ ] Package release archives, default configuration, service examples, backups, restore, migration, and upgrade documentation
- [ ] Add sanitizer, static-analysis, formatting, unit, integration, fuzz, malformed-client, differential, load, soak, and security jobs
- [ ] Validate Linux, macOS, and Windows builds and supported architectures
- [ ] Validate clean install, first run, restart, crash recovery, world upgrade, and backup restore
- [ ] Complete protocol documentation with every packet and codec table
- [ ] Validate end-to-end multiplayer with unmodified official 26.2 clients
- [ ] Run compatibility and regression suites for all vanilla and Tale of Kingdoms systems
- [ ] Mark every ledger item complete only after its implementation and tests land