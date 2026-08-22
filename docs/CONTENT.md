# Blocks, Items, Inventory, and Crafting

## Verified content source

`tools/mc-extract/extract.py` verifies the named 26.2 server JAR digest before producing:

- `docs/generated/content-26.2.json`: source-provenance registries, 1,585 recipes, 265 block tags, and 224 item tags.
- `docs/generated/registries-26.2.mcr`: normalized runtime names for 1,196 blocks and 1,538 items/block-items.
- `docs/generated/recipes-26.2.mcr`: 733 shaped and 323 shapeless recipes with recursively expanded item tags.
- `docs/generated/tags-26.2.mcr`: 704 network-safe tags across 15 client-owned registry keys, selected from 801 extracted tags and resolved to raw IDs.
- `docs/generated/network-registries-26.2.mcr`: 29 synchronized registries and 397 core-pack entry identifiers in official order.

Most registry names come from ordered `Blocks` and `Items` fields. Color and weathering-copper collection fields expand in declaration order. `cut_sandstone_slab` is explicitly marked as derived from its block field because it is recipe-visible but has no standalone `Items` field.

Runtime registries retain official extracted ordinals as protocol IDs separately from internal IDs. Bootstrap entries carry tested properties used by terrain and interactions; generated names without extracted behavior currently receive conservative defaults.

## Item stacks and containers

An item stack contains an internal item ID, count, durability damage, and a component map. Validation enforces registered items, per-item limits, durability bounds, normalized empty stacks, and the rule that air cannot form a non-empty stack.

Containers merge compatible stacks before occupying empty slots and increment their state ID only when contents change. Crafting uses copy-before-commit transactions, so blocked outputs leave both input and output unchanged. Player inventory storage provides 36 main slots, four armor slots, and one offhand slot.

## Crafting

Shaped matching supports placement offsets and mirrored patterns in 2x2 or 3x3 grids. Shapeless matching uses one-to-one ingredient assignment and supports expanded tag alternatives. The runtime loader validates record structure, identifiers, dimensions, counts, and registry references before registration.

The ordinary shaped/shapeless corpus is complete for the extracted 26.2 data. Cooking, stonecutting, smithing, transmute, and special recipe execution remain separate pending systems.

## Block interactions

Placement requires a block item and a replaceable target, mutates the chunk, and consumes one held item. Breaking respects unbreakable hardness, computes a tool-adjusted duration, suppresses tool-required drops for incorrect tools, resolves drops through the item registry, and replaces the block with air.

Full block-state placement, collision/reach validation, drops with loot conditions, enchantments, durability use, fluids, redstone, and protocol prediction acknowledgement remain pending.