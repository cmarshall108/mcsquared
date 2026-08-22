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
        manifest_path = Path(directory) / "content.json"
        recipes_path = Path(directory) / "recipes.mcr"
        registries_path = Path(directory) / "registries.mcr"
        tags_path = Path(directory) / "tags.mcr"
        network_registries_path = Path(directory) / "network-registries.mcr"
        subprocess.run(
            [
                sys.executable,
                str(extractor),
                "--bundle",
                str(bundle),
                "--content-manifest",
                str(manifest_path),
                "--runtime-recipes",
                str(recipes_path),
                "--runtime-registries",
                str(registries_path),
                "--runtime-tags",
                str(tags_path),
                "--runtime-network-registries",
                str(network_registries_path),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        runtime_recipes = recipes_path.read_text(encoding="utf-8").splitlines()
        runtime_registries = registries_path.read_text(encoding="utf-8").splitlines()
        runtime_tags = tags_path.read_text(encoding="utf-8").splitlines()
        network_registries = network_registries_path.read_text(
            encoding="utf-8"
        ).splitlines()

    assert manifest["minecraft"] == "26.2"
    assert manifest["protocol"] == 776
    assert len(manifest["blocks"]) == 1_196
    assert len(manifest["items"]) == 1_538
    assert len(manifest["entities"]) == 158
    assert len(manifest["data_components"]) == 111
    assert len(manifest["feature_flags"]) == 4
    assert len(manifest["command_argument_types"]) == 57
    assert len(manifest["damage_types"]) == 51
    assert len(manifest["fluids"]) == 5
    assert len(manifest["game_events"]) == 61
    assert len(manifest["potions"]) == 46
    assert len(manifest["synchronized_registries"]) == 29
    assert len(manifest["loot_tables"]) == 1_355
    assert len(manifest["tags"]) == 801
    assert len(manifest["dynamic_registries"]["worldgen"]) == 963
    worldgen = manifest["dynamic_registries"]["worldgen"]
    assert sum("/biome/" in f"/{key.split(':', 1)[1]}/" for key in worldgen) == 66
    assert sum("/noise/" in f"/{key.split(':', 1)[1]}/" for key in worldgen) == 63
    assert sum("/density_function/" in f"/{key.split(':', 1)[1]}/" for key in worldgen) == 35
    assert sum("/configured_carver/" in f"/{key.split(':', 1)[1]}/" for key in worldgen) == 4
    assert sum("/configured_feature/" in f"/{key.split(':', 1)[1]}/" for key in worldgen) == 226
    assert sum("/placed_feature/" in f"/{key.split(':', 1)[1]}/" for key in worldgen) == 262
    assert len(manifest["dynamic_registries"]["dimension_type"]) == 4
    assert manifest["feature_flags"][0]["name"] == "minecraft:vanilla"
    assert manifest["command_argument_types"][0]["name"] == "brigadier:bool"
    assert manifest["entities"][0] == {
        "id": 0,
        "name": "minecraft:acacia_boat",
        "field": "net.minecraft.world.entity.EntityTypes.ACACIA_BOAT",
        "java_type": "net.minecraft.world.entity.vehicle.boat.Boat",
    }
    assert manifest["blocks"][:2] == [
        {
            "id": 0,
            "name": "minecraft:air",
            "field": "net.minecraft.world.level.block.Blocks.AIR",
        },
        {
            "id": 1,
            "name": "minecraft:stone",
            "field": "net.minecraft.world.level.block.Blocks.STONE",
        },
    ]
    assert manifest["items"][0]["name"] == "minecraft:air"
    assert manifest["items"][1]["name"] == "minecraft:stone"
    assert len(manifest["recipes"]) == 1_585
    assert len(manifest["block_tags"]) == 265
    assert len(manifest["item_tags"]) == 224
    assert manifest["recipes"]["minecraft:crafting_table"] == {
        "type": "minecraft:crafting_shaped",
        "key": {"#": "#minecraft:planks"},
        "pattern": ["##", "##"],
        "result": {"id": "minecraft:crafting_table"},
        "show_notification": False,
    }
    assert manifest["recipes"]["minecraft:oak_planks"]["ingredients"] == [
        "#minecraft:oak_logs"
    ]
    assert "minecraft:air" in manifest["block_tags"]
    assert "minecraft:planks" in manifest["item_tags"]
    assert runtime_recipes[0] == "MCRECIPES1\tshaped=733\tshapeless=323"
    assert len(runtime_recipes) == 1_057
    crafting_table = next(
        line for line in runtime_recipes if line.startswith("S\tminecraft:crafting_table\t")
    )
    assert crafting_table.startswith("S\tminecraft:crafting_table\tminecraft:crafting_table\t1\t2\t2\t")
    assert runtime_registries[0] == "MCREGISTRIES1\tblocks=1196\titems=1538\tentities=158"
    assert len(runtime_registries) == 2_893
    assert runtime_registries[1] == "B\t0\tminecraft:air"
    assert runtime_registries[1_197] == "I\t0\tminecraft:air"
    assert runtime_registries[2_735].startswith("E\t0\tminecraft:acacia_boat\t")
    assert runtime_tags[0] == "MCTAGS1\tregistries=15\ttags=704"
    assert "R\tminecraft:damage_type\t34" in runtime_tags
    assert "T\tminecraft:is_fire\t0,1,3,4,5,6,39,38" in runtime_tags
    assert network_registries[0] == "MCREGISTRYDATA1\tregistries=29\tentries=397"
    assert network_registries[1] == "R\tminecraft:worldgen/biome\t66"
    assert "R\tminecraft:dimension_type\t4" in network_registries
    assert "R\tminecraft:damage_type\t51" in network_registries
    damage_index = network_registries.index("R\tminecraft:damage_type\t51")
    assert network_registries[damage_index + 1 : damage_index + 8] == [
        "E\tminecraft:in_fire",
        "E\tminecraft:campfire",
        "E\tminecraft:lightning_bolt",
        "E\tminecraft:on_fire",
        "E\tminecraft:lava",
        "E\tminecraft:hot_floor",
        "E\tminecraft:sulfur_cube_hot",
    ]
    assert not any("flat_level_generator_preset" in line for line in runtime_tags)


if __name__ == "__main__":
    main()