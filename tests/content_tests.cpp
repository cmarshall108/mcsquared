#include "mc/block/block.hpp"
#include "mc/core/types.hpp"
#include "mc/item/item.hpp"
#include "mc/inventory/inventory.hpp"
#include "mc/world/generation.hpp"

#include <cassert>
#include <fstream>
#include <stdexcept>

namespace {

void test_resource_locations() {
    const auto stone = mc::core::ResourceLocation::parse("stone");
    assert(stone.to_string() == "minecraft:stone");
    assert(mc::core::ResourceLocation::parse("example:machine/part").name_space() == "example");
    try {
        static_cast<void>(mc::core::ResourceLocation::parse("Minecraft:stone"));
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_block_registry() {
    mc::block::BlockRegistry blocks;
    assert(blocks.size() == 15);
    assert(blocks.by_id(2).name().to_string() == "minecraft:stone");
    assert(blocks.by_id(10).name().to_string() == "minecraft:oak_log");
    assert(blocks.by_id(14).name().to_string() == "minecraft:poppy");
    assert(blocks.by_name("water").properties().replaceable);
    try {
        blocks.register_block(mc::core::ResourceLocation::parse("minecraft:stone"), {});
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_item_stacks() {
    mc::item::ItemRegistry items;
    const auto stone_id = items.by_name("stone").id();
    mc::item::ItemStack destination(stone_id, 60);
    mc::item::ItemStack source(stone_id, 10);
    assert(destination.insert_from(source, items) == 6);
    assert(destination.count() == 64);
    assert(source.count() == 6);
    destination.validate(items);

    const auto removed = destination.take(5);
    assert(removed.count() == 5);
    assert(destination.count() == 59);

    const auto pickaxe_id = items.by_name("wooden_pickaxe").id();
    mc::item::ItemStack pickaxe(pickaxe_id, 1, 59);
    pickaxe.validate(items);
    mc::item::ItemStack working_pickaxe(pickaxe_id, 1);
    assert(!working_pickaxe.apply_damage(58, items));
    assert(working_pickaxe.damage() == 58);
    assert(working_pickaxe.apply_damage(1, items));
    assert(working_pickaxe.empty());
    const auto& bread = items.by_name("bread");
    assert(bread.properties().nutrition == 5);
    assert(bread.properties().saturation_modifier == 0.6F);
    assert(items.by_name("shield").properties().max_damage == 336);
    try {
        mc::item::ItemStack invalid(pickaxe_id, 2);
        invalid.validate(items);
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_container_insertion() {
    mc::item::ItemRegistry items;
    mc::inventory::Container container(items, 2);
    const auto stone = items.by_name("stone").id();
    container.set_slot(0, mc::item::ItemStack(stone, 60));
    const auto remainder = container.insert(mc::item::ItemStack(stone, 10));
    assert(remainder.empty());
    assert(container.slot(0).count() == 64);
    assert(container.slot(1).count() == 6);
    assert(container.state_id() == 2);

    const auto dirt = items.by_name("dirt").id();
    container.set_slot(1, mc::item::ItemStack(dirt, 64));
    const auto before_failed_insert = container.state_id();
    const auto rejected = container.insert(mc::item::ItemStack(stone, 1));
    assert(rejected.count() == 1);
    assert(container.state_id() == before_failed_insert);

    const auto current_state = container.state_id();
    assert(!container.apply_transaction(
        current_state - 1, {{0, mc::item::ItemStack(dirt, 1)}}));
    assert(container.slot(0).item_id() == stone);
    assert(!container.apply_transaction(
        current_state, {{9, mc::item::ItemStack(dirt, 1)}}));
    assert(container.state_id() == current_state);
    assert(container.apply_transaction(
        current_state,
        {{0, mc::item::ItemStack(dirt, 2)}, {1, mc::item::ItemStack(stone, 3)}}));
    assert(container.state_id() == current_state + 1);
    assert(container.slot(0).item_id() == dirt);

    mc::inventory::Inventory inventory(items);
    inventory.set_cursor(mc::item::ItemStack(stone, 1));
    assert(inventory.cursor().count() == 1);
    inventory.ender_chest().set_slot(0, mc::item::ItemStack(dirt, 4));
    assert(inventory.ender_chest().slot_count() == 27);

    std::ifstream registries(MC_RUNTIME_REGISTRIES_PATH);
    assert(registries);
    static_cast<void>(items.load_normalized(registries));
    const auto boots = items.by_name("iron_boots").id();
    const auto chestplate = items.by_name("iron_chestplate").id();
    inventory.armor().set_slot(0, mc::item::ItemStack(boots, 1));
    inventory.armor().set_slot(2, mc::item::ItemStack(chestplate, 1));
    assert(inventory.armor().slot(0).item_id() == boots);
    try {
        inventory.armor().set_slot(3, mc::item::ItemStack(stone, 1));
        assert(false);
    } catch (const std::invalid_argument&) {
    }
    const auto armor_state = inventory.armor().state_id();
    assert(!inventory.armor().apply_transaction(
        armor_state, {{3, mc::item::ItemStack(boots, 1)}}));
    assert(inventory.armor().state_id() == armor_state);
    assert(inventory.armor().insert(mc::item::ItemStack(stone, 1)).count() == 1);
}

void test_crafting_transactions() {
    mc::item::ItemRegistry items;
    mc::inventory::CraftingManager crafting(items);
    assert(crafting.size() == 4);

    mc::inventory::Container grid(items, 9);
    mc::inventory::Container output(items, 1);
    const auto planks = items.by_name("oak_planks").id();
    const auto table = items.by_name("crafting_table").id();
    grid.set_slot(4, mc::item::ItemStack(planks, 1));
    grid.set_slot(5, mc::item::ItemStack(planks, 1));
    grid.set_slot(7, mc::item::ItemStack(planks, 1));
    grid.set_slot(8, mc::item::ItemStack(planks, 1));
    assert(crafting.craft_once(grid, 3, 3, output));
    assert(output.slot(0).item_id() == table);
    assert(output.slot(0).count() == 1);
    assert(grid.slot(4).empty());

    mc::inventory::Container blocked_grid(items, 4);
    mc::inventory::Container blocked_output(items, 1);
    const auto log = items.by_name("oak_log").id();
    const auto stone = items.by_name("stone").id();
    blocked_grid.set_slot(0, mc::item::ItemStack(log, 1));
    blocked_output.set_slot(0, mc::item::ItemStack(stone, 64));
    assert(!crafting.craft_once(blocked_grid, 2, 2, blocked_output));
    assert(blocked_grid.slot(0).count() == 1);
    assert(blocked_output.slot(0).count() == 64);
}

void test_official_recipe_loading() {
    mc::item::ItemRegistry items;
    std::ifstream registries(MC_RUNTIME_REGISTRIES_PATH);
    assert(registries);
    const auto item_report = items.load_normalized(registries);
    assert(item_report.encountered == 1'538);
    assert(items.size() == 1'538);
    assert(items.by_protocol_id(1).name().to_string() == "minecraft:stone");

    mc::inventory::CraftingManager crafting(items);
    std::ifstream input(MC_RUNTIME_RECIPES_PATH);
    assert(input);
    const auto report = crafting.load_normalized(input);
    assert(report.encountered == 1'056);
    assert(report.loaded == 1'052);
    assert(report.loaded + report.skipped == report.encountered);

    mc::inventory::Container grid(items, 4);
    mc::inventory::Container output(items, 1);
    grid.set_slot(0, mc::item::ItemStack(items.by_name("coal").id(), 1));
    grid.set_slot(2, mc::item::ItemStack(items.by_name("stick").id(), 1));
    assert(crafting.craft_once(grid, 2, 2, output));
    assert(output.slot(0).item_id() == items.by_name("torch").id());
    assert(output.slot(0).count() == 4);
}

void test_official_block_registry_loading() {
    mc::block::BlockRegistry blocks;
    std::ifstream registries(MC_RUNTIME_REGISTRIES_PATH);
    assert(registries);
    const auto report = blocks.load_normalized(registries);
    assert(report.encountered == 1'196);
    assert(blocks.size() == 1'196);
    assert(blocks.by_protocol_id(0).name().to_string() == "minecraft:air");
    assert(blocks.by_protocol_id(1).name().to_string() == "minecraft:stone");
}

void test_block_interactions() {
    mc::block::BlockRegistry blocks;
    mc::item::ItemRegistry items;
    const mc::block::BlockInteraction interaction(blocks, items);
    const mc::world::ChunkGenerator generator({88, 63});
    auto chunk = generator.generate({0, 0});
    const auto y = chunk.height(1, 1) + 1;

    mc::item::ItemStack dirt(items.by_name("dirt").id(), 2);
    assert(interaction.place(chunk, 1, y, 1, dirt));
    assert(chunk.block(1, y, 1) == mc::world::BlockId::dirt);
    assert(dirt.count() == 1);
    assert(!interaction.place(chunk, 1, y, 1, dirt));
    assert(dirt.count() == 1);

    const auto dirt_result = interaction.break_block(chunk, 1, y, 1, {});
    assert(dirt_result.broken);
    assert(dirt_result.drops.size() == 1);
    assert(dirt_result.drops.front().item_id() == items.by_name("dirt").id());
    assert(chunk.block(1, y, 1) == mc::world::BlockId::air);

    chunk.set_block(2, y, 2, mc::world::BlockId::stone);
    const auto wrong_tool = interaction.break_block(chunk, 2, y, 2, {1.0F, false});
    assert(wrong_tool.broken);
    assert(wrong_tool.drops.empty());
    chunk.set_block(2, y, 2, mc::world::BlockId::stone);
    const auto pickaxe = interaction.break_block(chunk, 2, y, 2, {2.0F, true});
    assert(pickaxe.broken);
    assert(pickaxe.drops.front().item_id() == items.by_name("cobblestone").id());

    const auto bedrock = interaction.break_block(
        chunk, 0, mc::world::min_build_y, 0, {100.0F, true});
    assert(!bedrock.broken);
}

} // namespace

int main() {
    test_resource_locations();
    test_block_registry();
    test_item_stacks();
    test_container_insertion();
    test_crafting_transactions();
    test_official_recipe_loading();
    test_official_block_registry_loading();
    test_block_interactions();
}