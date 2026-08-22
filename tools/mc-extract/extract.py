#!/usr/bin/env python3

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile


PROTOCOL_CLASSES = {
    "handshaking": "net.minecraft.network.protocol.handshake.HandshakeProtocols",
    "status": "net.minecraft.network.protocol.status.StatusProtocols",
    "login": "net.minecraft.network.protocol.login.LoginProtocols",
    "configuration": "net.minecraft.network.protocol.configuration.ConfigurationProtocols",
    "play": "net.minecraft.network.protocol.game.GameProtocols",
}

PACKET_FIELD = re.compile(
    r"// Field (?P<owner>[\w/$]+PacketTypes)\."
    r"(?P<name>(?:CLIENTBOUND|SERVERBOUND)_[A-Z0-9_]+):"
)
CODEC_FIELD = re.compile(
    r"// Field (?P<owner>[\w/$]+)\.(?P<name>[A-Z0-9_]*STREAM_CODEC):"
)
CLASS_DECLARATION = re.compile(
    r"^(?:public |protected |private )?(?:final )?"
    r"(?:class|interface) (?P<name>[\w.$]+)"
)
INSTANCE_FIELD = re.compile(r"^  private final (?P<type>.+) (?P<name>[\w$]+);$")
BYTECODE_METHOD = re.compile(
    r"// (?:InterfaceMethod|Method) (?P<owner>[\w/$]+)\."
    r"(?P<method>[\w$<>]+):"
)
BYTECODE_FIELD = re.compile(
    r"// Field (?P<owner>[\w/$]+)\.(?P<field>[A-Z0-9_]+):"
)


def extract_registry_fields(
    jar_path: Path, class_name: str, java_type: str
) -> list[dict[str, object]]:
    result = subprocess.run(
        ["javap", "-classpath", str(jar_path), "-p", class_name],
        check=True,
        capture_output=True,
        text=True,
    )
    pattern = re.compile(
        r"^  public static final (?P<type>[^ ]+) (?P<field>[A-Z0-9_]+);$"
    )
    colors = [
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink",
        "gray", "light_gray", "cyan", "purple", "blue", "brown", "green", "red",
        "black",
    ]
    entries = []

    def append(name: str, field: str) -> None:
        entries.append(
            {
                "id": len(entries),
                "name": f"minecraft:{name}",
                "field": f"{class_name}.{field}",
            }
        )

    logical_lines = []
    pending = ""
    for line in result.stdout.splitlines():
        if line.startswith("  public static final "):
            pending = line
        elif pending:
            pending += line.strip()
        if pending.endswith(";"):
            logical_lines.append(pending)
            pending = ""
    for line in logical_lines:
        match = pattern.match(line)
        if not match:
            continue
        field = match.group("field")
        field_type = match.group("type")
        if field_type == java_type:
            aliases = {
                "POTTED_AZALEA": "potted_azalea_bush",
                "POTTED_FLOWERING_AZALEA": "potted_flowering_azalea_bush",
            }
            append(aliases.get(field, field.lower()), field)
        elif field_type == f"net.minecraft.world.level.block.ColorCollection<{java_type}>":
            base = field.removeprefix("DYED_").lower()
            for color in colors:
                append(f"{color}_{base}", f"{field}[{color}]")
        elif field_type == f"net.minecraft.world.level.block.WeatheringCopperCollection<{java_type}>":
            base = field.lower()
            variants = [base]
            for stage in ("exposed", "weathered", "oxidized"):
                variants.append(f"{stage}_copper" if base == "copper_block" else f"{stage}_{base}")
            for variant in [*variants, *(f"waxed_{name}" for name in variants)]:
                append(variant, f"{field}[{variant}]")
    return entries


def extract_entity_fields(jar_path: Path) -> list[dict[str, object]]:
    class_name = "net.minecraft.world.entity.EntityTypes"
    result = subprocess.run(
        ["javap", "-classpath", str(jar_path), "-p", class_name],
        check=True,
        capture_output=True,
        text=True,
    )
    pattern = re.compile(
        r"^  public static final net\.minecraft\.world\.entity\.EntityType<"
        r"(?P<java_type>[^>]+)> (?P<field>[A-Z0-9_]+);$"
    )
    entries = []
    for line in result.stdout.splitlines():
        match = pattern.match(line)
        if match:
            field = match.group("field")
            entries.append(
                {
                    "id": len(entries),
                    "name": f"minecraft:{field.lower()}",
                    "field": f"{class_name}.{field}",
                    "java_type": match.group("java_type"),
                }
            )
    return entries


def extract_static_field_names(
    jar_path: Path, class_name: str, field_type_prefix: str
) -> list[dict[str, object]]:
    result = subprocess.run(
        ["javap", "-classpath", str(jar_path), "-p", class_name],
        check=True,
        capture_output=True,
        text=True,
    )
    pattern = re.compile(
        rf"^  public static final {re.escape(field_type_prefix)}(?:<[^;]+>)? "
        r"(?P<field>[A-Z0-9_]+);$"
    )
    entries = []
    for line in result.stdout.splitlines():
        match = pattern.match(line)
        if match:
            field = match.group("field")
            entries.append(
                {
                    "id": len(entries),
                    "name": f"minecraft:{field.lower()}",
                    "field": f"{class_name}.{field}",
                }
            )
    return entries


def extract_named_fields(
    jar_path: Path,
    class_name: str,
    field_pattern: str,
    aliases: dict[str, str] | None = None,
) -> list[dict[str, object]]:
    result = subprocess.run(
        ["javap", "-classpath", str(jar_path), "-p", class_name],
        check=True,
        capture_output=True,
        text=True,
    )
    pattern = re.compile(field_pattern)
    aliases = aliases or {}
    entries = []
    logical_lines = []
    pending = ""
    for line in result.stdout.splitlines():
        if line.startswith("  public static final "):
            pending = line
        elif pending:
            pending += line.strip()
        if pending.endswith(";"):
            logical_lines.append(pending)
            pending = ""
    for line in logical_lines:
        match = pattern.match(line)
        if not match:
            continue
        field = match.group("field")
        entries.append(
            {
                "id": len(entries),
                "name": f"minecraft:{aliases.get(field, field.lower())}",
                "field": f"{class_name}.{field}",
            }
        )
    return entries


def extract_command_argument_types(jar_path: Path) -> list[dict[str, object]]:
    class_name = "net.minecraft.commands.synchronization.ArgumentTypeInfos"
    result = subprocess.run(
        ["javap", "-classpath", str(jar_path), "-c", "-p", class_name],
        check=True,
        capture_output=True,
        text=True,
    )
    pattern = re.compile(r"// String (?P<name>[a-z0-9_:]+)$")
    entries = []
    for line in result.stdout.splitlines():
        match = pattern.search(line)
        if not match or match.group("name").startswith("Unrecognized"):
            continue
        name = match.group("name")
        if ":" not in name:
            name = f"minecraft:{name}"
        if name not in {entry["name"] for entry in entries}:
            entries.append({"id": len(entries), "name": name})
    return entries


def extract_synchronized_registries(jar_path: Path) -> list[str]:
    result = subprocess.run(
        [
            "javap",
            "-classpath",
            str(jar_path),
            "-c",
            "-p",
            "net.minecraft.resources.RegistryDataLoader",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    field_pattern = re.compile(
        r"// Field net/minecraft/core/registries/Registries\.([A-Z0-9_]+):"
    )
    synchronized = []
    collecting = False
    for line in result.stdout.splitlines():
        if "putstatic" in line and "DIMENSION_REGISTRIES" in line:
            collecting = True
            continue
        if collecting and "putstatic" in line and "SYNCHRONIZED_REGISTRIES" in line:
            break
        if collecting:
            match = field_pattern.search(line)
            if match:
                synchronized.append(match.group(1).lower())
    if not synchronized:
        raise RuntimeError("failed to extract synchronized registry list")
    return synchronized


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify and extract the nested Minecraft server JAR."
    )
    parser.add_argument(
        "--bundle",
        type=Path,
        default=Path(__file__).with_name("server.jar"),
        help="path to Mojang's bundled server JAR",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="optional destination for the verified nested server JAR",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        help="optional destination for the extracted packet registration manifest",
    )
    parser.add_argument(
        "--content-manifest",
        type=Path,
        help="optional destination for extracted recipes and block/item tags",
    )
    parser.add_argument(
        "--runtime-recipes",
        type=Path,
        help="optional destination for normalized shaped and shapeless recipes",
    )
    parser.add_argument(
        "--runtime-registries",
        type=Path,
        help="optional destination for normalized block and item registry names",
    )
    parser.add_argument(
        "--runtime-tags",
        type=Path,
        help="optional destination for normalized network registry tags",
    )
    parser.add_argument(
        "--runtime-network-registries",
        type=Path,
        help="optional destination for known-pack synchronized registry entries",
    )
    return parser.parse_args()


def extract_json_entries(
    archive: zipfile.ZipFile, marker: str
) -> dict[str, object]:
    entries: dict[str, object] = {}
    segment = f"/{marker}/"
    for name in sorted(archive.namelist()):
        if not name.startswith("data/") or segment not in name or not name.endswith(".json"):
            continue
        parts = name.split("/")
        namespace = parts[1]
        marker_index = parts.index(marker)
        path = "/".join(parts[marker_index + 1 :])[:-5]
        entries[f"{namespace}:{path}"] = json.loads(archive.read(name))
    return entries


def extract_all_tags(archive: zipfile.ZipFile) -> dict[str, object]:
    entries = {}
    for name in sorted(archive.namelist()):
        if not name.startswith("data/") or "/tags/" not in name or not name.endswith(".json"):
            continue
        parts = name.split("/")
        namespace = parts[1]
        tags_index = parts.index("tags")
        path = "/".join(parts[tags_index + 1 :])[:-5]
        entries[f"{namespace}:{path}"] = json.loads(archive.read(name))
    return entries


def extract_dynamic_json(archive: zipfile.ZipFile) -> dict[str, dict[str, object]]:
    excluded = {"advancement", "recipe", "loot_table", "tags", "datapacks"}
    roots: dict[str, dict[str, object]] = {}
    for name in sorted(archive.namelist()):
        if not name.startswith("data/") or not name.endswith(".json"):
            continue
        parts = name.split("/")
        if len(parts) < 4 or parts[2] in excluded:
            continue
        root = parts[2]
        path = "/".join(parts[3:])[:-5]
        roots.setdefault(root, {})[f"{parts[1]}:{path}"] = json.loads(archive.read(name))
    return roots


def extract_content_manifest(
    nested_bytes: bytes, jar_path: Path, version: dict[str, object]
) -> dict[str, object]:
    with zipfile.ZipFile(io.BytesIO(nested_bytes)) as archive:
        recipes = extract_json_entries(archive, "recipe")
        block_tags = extract_json_entries(archive, "block")
        item_tags = extract_json_entries(archive, "item")
        tags = extract_all_tags(archive)
        loot_tables = extract_json_entries(archive, "loot_table")
        dynamic_registries = extract_dynamic_json(archive)
    blocks = extract_registry_fields(
        jar_path,
        "net.minecraft.world.level.block.Blocks",
        "net.minecraft.world.level.block.Block",
    )
    items = extract_registry_fields(
        jar_path,
        "net.minecraft.world.item.Items",
        "net.minecraft.world.item.Item",
    )
    entities = extract_entity_fields(jar_path)
    data_components = extract_static_field_names(
        jar_path,
        "net.minecraft.core.component.DataComponents",
        "net.minecraft.core.component.DataComponentType",
    )
    feature_flags = extract_static_field_names(
        jar_path,
        "net.minecraft.world.flag.FeatureFlags",
        "net.minecraft.world.flag.FeatureFlag",
    )
    command_argument_types = extract_command_argument_types(jar_path)
    damage_types = extract_named_fields(
        jar_path,
        "net.minecraft.world.damagesource.DamageTypes",
        r"^  public static final net\.minecraft\.resources\.ResourceKey<"
        r"net\.minecraft\.world\.damagesource\.DamageType> (?P<field>[A-Z0-9_]+);$",
        {"FELL_OUT_OF_WORLD": "out_of_world"},
    )
    fluids = extract_named_fields(
        jar_path,
        "net.minecraft.world.level.material.Fluids",
        r"^  public static final net\.minecraft\.world\.level\.material\."
        r"(?:FlowingFluid|Fluid) (?P<field>[A-Z0-9_]+);$",
    )
    game_events = extract_named_fields(
        jar_path,
        "net.minecraft.world.level.gameevent.GameEvent",
        r"^  public static final net\.minecraft\.core\.Holder\$Reference<"
        r"net\.minecraft\.world\.level\.gameevent\.GameEvent> (?P<field>[A-Z0-9_]+);$",
    )
    potions = extract_named_fields(
        jar_path,
        "net.minecraft.world.item.alchemy.Potions",
        r"^  public static final net\.minecraft\.core\.Holder<"
        r"net\.minecraft\.world\.item\.alchemy\.Potion> (?P<field>[A-Z0-9_]+);$",
    )
    point_of_interest_types = extract_named_fields(
        jar_path,
        "net.minecraft.world.entity.ai.village.poi.PoiTypes",
        r"^  public static final net\.minecraft\.resources\.ResourceKey<"
        r"net\.minecraft\.world\.entity\.ai\.village\.poi\.PoiType> "
        r"(?P<field>[A-Z0-9_]+);$",
    )
    dimension_types = extract_named_fields(
        jar_path,
        "net.minecraft.world.level.dimension.BuiltinDimensionTypes",
        r"^  public static final net\.minecraft\.resources\.ResourceKey<"
        r"net\.minecraft\.world\.level\.dimension\.DimensionType> "
        r"(?P<field>[A-Z0-9_]+);$",
        {"NETHER": "the_nether", "END": "the_end"},
    )
    synchronized_registries = extract_synchronized_registries(jar_path)
    item_names = {entry["name"] for entry in items}
    block_fields = {entry["name"]: entry["field"] for entry in blocks}
    for recipe in recipes.values():
        result_name = recipe.get("result", {}).get("id")
        if result_name and result_name not in item_names and result_name in block_fields:
            items.append(
                {
                    "id": len(items),
                    "name": result_name,
                    "field": f"derived-from:{block_fields[result_name]}",
                }
            )
            item_names.add(result_name)
    return {
        "minecraft": version["id"],
        "protocol": version["protocol_version"],
        "blocks": blocks,
        "items": items,
        "entities": entities,
        "data_components": data_components,
        "feature_flags": feature_flags,
        "command_argument_types": command_argument_types,
        "damage_types": damage_types,
        "fluids": fluids,
        "game_events": game_events,
        "potions": potions,
        "point_of_interest_types": point_of_interest_types,
        "dimension_types": dimension_types,
        "synchronized_registries": synchronized_registries,
        "recipes": recipes,
        "loot_tables": loot_tables,
        "dynamic_registries": dynamic_registries,
        "tags": tags,
        "block_tags": block_tags,
        "item_tags": item_tags,
    }


def normalize_runtime_recipes(content: dict[str, object]) -> str:
    item_tags = content["item_tags"]
    recipes = content["recipes"]
    resolved_tags: dict[str, set[str]] = {}

    def resolve_tag(tag: str, resolving: set[str]) -> set[str]:
        if tag in resolved_tags:
            return resolved_tags[tag]
        if tag in resolving:
            raise RuntimeError(f"cyclic item tag: {tag}")
        resolving.add(tag)
        values = item_tags.get(tag, {}).get("values", [])
        resolved: set[str] = set()
        for value in values:
            identifier = value["id"] if isinstance(value, dict) else value
            if identifier.startswith("#"):
                resolved.update(resolve_tag(identifier[1:], resolving))
            else:
                resolved.add(identifier)
        resolving.remove(tag)
        resolved_tags[tag] = resolved
        return resolved

    def resolve_ingredient(value: object) -> list[str]:
        values = value if isinstance(value, list) else [value]
        resolved: set[str] = set()
        for entry in values:
            identifier = entry["item"] if isinstance(entry, dict) else entry
            if identifier.startswith("#"):
                resolved.update(resolve_tag(identifier[1:], set()))
            else:
                resolved.add(identifier)
        if not resolved:
            raise RuntimeError(f"ingredient resolves to no items: {value}")
        return sorted(resolved)

    lines = ["MCRECIPES1\tshaped=733\tshapeless=323"]
    for recipe_id, recipe in recipes.items():
        recipe_type = recipe.get("type")
        if recipe_type not in {"minecraft:crafting_shaped", "minecraft:crafting_shapeless"}:
            continue
        result = recipe["result"]
        result_id = result["id"]
        result_count = int(result.get("count", 1))
        if recipe_type == "minecraft:crafting_shaped":
            pattern = recipe["pattern"]
            width = len(pattern[0])
            if any(len(row) != width for row in pattern):
                raise RuntimeError(f"recipe has uneven pattern rows: {recipe_id}")
            key = recipe["key"]
            cells = []
            for row in pattern:
                for symbol in row:
                    cells.append(
                        "" if symbol == " " else ",".join(resolve_ingredient(key[symbol]))
                    )
            lines.append(
                "\t".join(
                    [
                        "S",
                        recipe_id,
                        result_id,
                        str(result_count),
                        str(width),
                        str(len(pattern)),
                        "|".join(cells),
                    ]
                )
            )
        else:
            ingredients = [
                ",".join(resolve_ingredient(ingredient))
                for ingredient in recipe["ingredients"]
            ]
            lines.append(
                "\t".join(
                    ["L", recipe_id, result_id, str(result_count), "|".join(ingredients)]
                )
            )
    return "\n".join(lines) + "\n"


def normalize_runtime_registries(content: dict[str, object]) -> str:
    blocks = content["blocks"]
    items = content["items"]
    entities = content["entities"]
    lines = [
        f"MCREGISTRIES1\tblocks={len(blocks)}\titems={len(items)}\tentities={len(entities)}"
    ]
    lines.extend(f"B\t{entry['id']}\t{entry['name']}" for entry in blocks)
    lines.extend(f"I\t{entry['id']}\t{entry['name']}" for entry in items)
    lines.extend(
        f"E\t{entry['id']}\t{entry['name']}\t{entry['java_type']}" for entry in entities
    )
    return "\n".join(lines) + "\n"


def normalize_runtime_tags(content: dict[str, object]) -> str:
    registry_entries: dict[str, list[dict[str, object]]] = {
        "block": content["blocks"],
        "item": content["items"],
        "entity_type": content["entities"],
        "damage_type": content["damage_types"],
        "fluid": content["fluids"],
        "game_event": content["game_events"],
        "potion": content["potions"],
        "point_of_interest_type": content["point_of_interest_types"],
    }
    for root, entries in content["dynamic_registries"].items():
        if root == "damage_type":
            continue
        if root != "worldgen":
            registry_entries.setdefault(root, [])
            registry_entries[root].extend(
                {"id": index, "name": name}
                for index, name in enumerate(entries)
            )
            continue
        grouped: dict[str, list[str]] = {}
        for name in entries:
            namespace, path = name.split(":", 1)
            family, entry_path = path.split("/", 1)
            grouped.setdefault(f"worldgen/{family}", []).append(
                f"{namespace}:{entry_path}"
            )
        for family, names in grouped.items():
            registry_entries[family] = [
                {"id": index, "name": name} for index, name in enumerate(names)
            ]

    raw_ids = {
        registry: {entry["name"]: int(entry["id"]) for entry in entries}
        for registry, entries in registry_entries.items()
    }
    tags = content["tags"]
    registry_aliases = {"biome": "worldgen/biome"}
    network_safe = {
        "block",
        "item",
        "entity_type",
        "fluid",
        "game_event",
        "point_of_interest_type",
        "potion",
        *(registry_aliases.get(name, name) for name in content["synchronized_registries"]),
    }
    families = sorted(
        registry
        for registry in raw_ids
        if registry in network_safe
        and any(key.split(":", 1)[1].startswith(f"{registry}/") for key in tags)
    )
    resolved: dict[tuple[str, str], list[int]] = {}

    def resolve(registry: str, tag_key: str, active: set[tuple[str, str]]) -> list[int]:
        cache_key = (registry, tag_key)
        if cache_key in resolved:
            return resolved[cache_key]
        if cache_key in active:
            raise RuntimeError(f"cyclic registry tag: {tag_key}")
        active.add(cache_key)
        namespace, tag_path = tag_key.split(":", 1)
        source_key = f"{namespace}:{registry}/{tag_path}"
        source = tags.get(source_key)
        if source is None:
            raise RuntimeError(f"missing referenced registry tag: {source_key}")
        members: list[int] = []
        for value in source.get("values", []):
            required = True
            identifier = value
            if isinstance(value, dict):
                identifier = value["id"]
                required = bool(value.get("required", True))
            if identifier.startswith("#"):
                members.extend(resolve(registry, identifier[1:], active))
                continue
            raw_id = raw_ids[registry].get(identifier)
            if raw_id is None:
                if required:
                    raise RuntimeError(
                        f"required tag member {identifier} is absent from {registry}"
                    )
                continue
            members.append(raw_id)
        active.remove(cache_key)
        resolved[cache_key] = list(dict.fromkeys(members))
        return resolved[cache_key]

    records: list[tuple[str, list[tuple[str, list[int]]]]] = []
    for registry in families:
        prefix = f"{registry}/"
        registry_tags = []
        for source_key in sorted(tags):
            namespace, path = source_key.split(":", 1)
            if not path.startswith(prefix):
                continue
            tag_name = f"{namespace}:{path[len(prefix):]}"
            registry_tags.append((tag_name, resolve(registry, tag_name, set())))
        if registry_tags:
            records.append((f"minecraft:{registry}", registry_tags))

    tag_count = sum(len(registry_tags) for _, registry_tags in records)
    lines = [f"MCTAGS1\tregistries={len(records)}\ttags={tag_count}"]
    for registry, registry_tags in records:
        lines.append(f"R\t{registry}\t{len(registry_tags)}")
        lines.extend(
            f"T\t{name}\t{','.join(str(raw_id) for raw_id in members)}"
            for name, members in registry_tags
        )
    return "\n".join(lines) + "\n"


def normalize_runtime_network_registries(content: dict[str, object]) -> str:
    special = {
        "damage_type": [entry["name"] for entry in content["damage_types"]],
        "dimension_type": [entry["name"] for entry in content["dimension_types"]],
    }
    dynamic = content["dynamic_registries"]
    records: list[tuple[str, list[str]]] = []
    for registry in content["synchronized_registries"]:
        if registry in special:
            entries = special[registry]
        elif registry == "biome":
            entries = []
            for name in dynamic["worldgen"]:
                namespace, path = name.split(":", 1)
                if path.startswith("biome/"):
                    entries.append(f"{namespace}:{path.removeprefix('biome/')}")
        else:
            entries = list(dynamic[registry])
        if not entries:
            raise RuntimeError(f"synchronized registry {registry} has no entries")
        registry_identifier = "worldgen/biome" if registry == "biome" else registry
        records.append((f"minecraft:{registry_identifier}", entries))
    entry_count = sum(len(entries) for _, entries in records)
    lines = [f"MCREGISTRYDATA1\tregistries={len(records)}\tentries={entry_count}"]
    for registry, entries in records:
        lines.append(f"R\t{registry}\t{len(entries)}")
        lines.extend(f"E\t{entry}" for entry in entries)
    return "\n".join(lines) + "\n"


def extract_packet_details(
    jar_path: Path, codec_classes: set[str]
) -> dict[str, dict[str, object]]:
    if not codec_classes:
        return {}
    result = subprocess.run(
        ["javap", "-classpath", str(jar_path), "-c", "-p", *sorted(codec_classes)],
        check=True,
        capture_output=True,
        text=True,
    )
    lines_by_class: dict[str, list[str]] = {}
    current_class = None
    for line in result.stdout.splitlines():
        class_match = CLASS_DECLARATION.match(line)
        if class_match:
            current_class = class_match.group("name")
            lines_by_class[current_class] = [line]
        elif current_class:
            lines_by_class[current_class].append(line)

    details = {}
    for class_name, lines in lines_by_class.items():
        fields = []
        operations = []
        references = []
        terminal = False
        in_terminal_method = False
        for line in lines:
            field_match = INSTANCE_FIELD.match(line)
            if field_match:
                fields.append(
                    {
                        "name": field_match.group("name"),
                        "java_type": field_match.group("type"),
                    }
                )
            stripped = line.strip()
            if stripped == "public boolean isTerminal();":
                in_terminal_method = True
            elif in_terminal_method and stripped.endswith(");"):
                in_terminal_method = False
            elif in_terminal_method and "iconst_1" in stripped:
                terminal = True

            method_match = BYTECODE_METHOD.search(line)
            if method_match and (
                method_match.group("method").startswith("read")
                or method_match.group("method").startswith("write")
            ):
                operation = (
                    f"{method_match.group('owner').replace('/', '.')}"
                    f".{method_match.group('method')}"
                )
                if operation not in operations:
                    operations.append(operation)

            reference_match = BYTECODE_FIELD.search(line)
            if reference_match and (
                "codec" in reference_match.group("owner").lower()
                or reference_match.group("field").endswith("STREAM_CODEC")
            ):
                reference = (
                    f"{reference_match.group('owner').replace('/', '.')}"
                    f".{reference_match.group('field')}"
                )
                if reference not in references:
                    references.append(reference)
        details[class_name] = {
            "declared_fields": fields,
            "codec_operations": operations,
            "codec_references": references,
            "terminal": terminal,
        }
    return details


def extract_protocol_manifest(
    jar_path: Path, version: dict[str, object]
) -> dict[str, object]:
    states: dict[str, dict[str, list[dict[str, object]]]] = {}
    all_packets: list[dict[str, object]] = []
    for state, class_name in PROTOCOL_CLASSES.items():
        result = subprocess.run(
            ["javap", "-classpath", str(jar_path), "-c", "-p", class_name],
            check=True,
            capture_output=True,
            text=True,
        )
        directions: dict[str, list[dict[str, object]]] = {
            "clientbound": [],
            "serverbound": [],
        }
        pending_packet = None
        for line in result.stdout.splitlines():
            packet_match = PACKET_FIELD.search(line)
            if packet_match:
                field_name = packet_match.group("name")
                direction = (
                    "clientbound"
                    if field_name.startswith("CLIENTBOUND_")
                    else "serverbound"
                )
                packet_id = len(directions[direction])
                pending_packet = {
                    "id": packet_id,
                    "hex_id": f"0x{packet_id:02X}",
                    "name": field_name.removeprefix(f"{direction.upper()}_"),
                    "field": (
                        f"{packet_match.group('owner').replace('/', '.')}"
                        f".{field_name}"
                    ),
                }
                directions[direction].append(pending_packet)
                all_packets.append(pending_packet)
                continue

            codec_match = CODEC_FIELD.search(line)
            if pending_packet is not None and codec_match:
                pending_packet["codec_class"] = codec_match.group("owner").replace(
                    "/", "."
                )
                pending_packet["codec_field"] = codec_match.group("name")
                pending_packet = None
        states[state] = directions

    codec_classes = {
        str(packet["codec_class"])
        for packet in all_packets
        if "codec_class" in packet
    }
    details_by_class = extract_packet_details(jar_path, codec_classes)
    for packet in all_packets:
        codec_class = packet.get("codec_class")
        if codec_class:
            packet.update(details_by_class.get(str(codec_class), {}))

    missing_codecs = [
        packet["field"]
        for packet in all_packets
        if "codec_class" not in packet and packet["name"] != "BUNDLE"
    ]
    missing_details = [
        packet["field"]
        for packet in all_packets
        if "codec_class" in packet and "declared_fields" not in packet
    ]
    if missing_codecs or missing_details:
        raise RuntimeError(
            "protocol extraction contains unverified packet registrations: "
            + ", ".join([*missing_codecs, *missing_details])
        )

    return {
        "minecraft": version["id"],
        "protocol": version["protocol_version"],
        "source": {
            "nested_path": version.get("name", version["id"]),
            "registration_classes": PROTOCOL_CLASSES,
        },
        "states": states,
    }


def main() -> int:
    arguments = parse_arguments()
    with zipfile.ZipFile(arguments.bundle) as bundle:
        version = json.loads(bundle.read("version.json"))
        records = bundle.read("META-INF/versions.list").decode("utf-8").splitlines()
        if len(records) != 1:
            raise RuntimeError(f"expected one nested server version, found {len(records)}")
        expected_hash, listed_version, nested_path = records[0].split("\t")
        if listed_version != version["id"]:
            raise RuntimeError("version.json and versions.list disagree")

        nested_bytes = bundle.read(f"META-INF/versions/{nested_path}")
        actual_hash = hashlib.sha256(nested_bytes).hexdigest()
        if actual_hash != expected_hash:
            raise RuntimeError(
                f"nested server digest mismatch: expected {expected_hash}, got {actual_hash}"
            )

    temporary_directory = None
    jar_path = arguments.output
    if jar_path is None and (
        arguments.manifest
        or arguments.content_manifest
        or arguments.runtime_recipes
        or arguments.runtime_registries
        or arguments.runtime_tags
        or arguments.runtime_network_registries
    ):
        temporary_directory = tempfile.TemporaryDirectory()
        jar_path = Path(temporary_directory.name) / "server.jar"
    if jar_path:
        jar_path.parent.mkdir(parents=True, exist_ok=True)
        jar_path.write_bytes(nested_bytes)

    if arguments.manifest:
        manifest = extract_protocol_manifest(jar_path, version)
        manifest["source"]["nested_path"] = nested_path
        manifest["source"]["sha256"] = actual_hash
        arguments.manifest.parent.mkdir(parents=True, exist_ok=True)
        arguments.manifest.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )

    content_manifest = None
    if (
        arguments.content_manifest
        or arguments.runtime_recipes
        or arguments.runtime_registries
        or arguments.runtime_tags
        or arguments.runtime_network_registries
    ):
        content_manifest = extract_content_manifest(nested_bytes, jar_path, version)

    if arguments.content_manifest:
        content_manifest["source"] = {
            "nested_path": nested_path,
            "sha256": actual_hash,
        }
        arguments.content_manifest.parent.mkdir(parents=True, exist_ok=True)
        arguments.content_manifest.write_text(
            json.dumps(content_manifest, indent=2) + "\n", encoding="utf-8"
        )

    if arguments.runtime_recipes:
        arguments.runtime_recipes.parent.mkdir(parents=True, exist_ok=True)
        arguments.runtime_recipes.write_text(
            normalize_runtime_recipes(content_manifest), encoding="utf-8"
        )

    if arguments.runtime_registries:
        arguments.runtime_registries.parent.mkdir(parents=True, exist_ok=True)
        arguments.runtime_registries.write_text(
            normalize_runtime_registries(content_manifest), encoding="utf-8"
        )

    if arguments.runtime_tags:
        arguments.runtime_tags.parent.mkdir(parents=True, exist_ok=True)
        arguments.runtime_tags.write_text(
            normalize_runtime_tags(content_manifest), encoding="utf-8"
        )

    if arguments.runtime_network_registries:
        arguments.runtime_network_registries.parent.mkdir(parents=True, exist_ok=True)
        arguments.runtime_network_registries.write_text(
            normalize_runtime_network_registries(content_manifest), encoding="utf-8"
        )

    if temporary_directory:
        temporary_directory.cleanup()

    print(
        json.dumps(
            {
                "minecraft": version["id"],
                "protocol": version["protocol_version"],
                "sha256": actual_hash,
                "nested_path": nested_path,
                "output": str(arguments.output) if arguments.output else None,
                "manifest": str(arguments.manifest) if arguments.manifest else None,
                "content_manifest": (
                    str(arguments.content_manifest) if arguments.content_manifest else None
                ),
                "runtime_recipes": (
                    str(arguments.runtime_recipes) if arguments.runtime_recipes else None
                ),
                "runtime_registries": (
                    str(arguments.runtime_registries) if arguments.runtime_registries else None
                ),
                "runtime_tags": (
                    str(arguments.runtime_tags) if arguments.runtime_tags else None
                ),
                "runtime_network_registries": (
                    str(arguments.runtime_network_registries)
                    if arguments.runtime_network_registries
                    else None
                ),
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (
        KeyError,
        OSError,
        RuntimeError,
        subprocess.CalledProcessError,
        ValueError,
        zipfile.BadZipFile,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)