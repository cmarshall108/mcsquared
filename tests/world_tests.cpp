#include "mc/world/generation.hpp"
#include "mc/world/storage.hpp"
#include "mc/world/ticks.hpp"
#include "mc/world/world.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <thread>
#include <vector>

namespace {

void assert_same_chunk(const mc::world::Chunk& left, const mc::world::Chunk& right) {
    assert(left.position() == right.position());
    for (std::size_t z = 0; z < 16; ++z) {
        for (std::size_t x = 0; x < 16; ++x) {
            assert(left.height(x, z) == right.height(x, z));
            for (auto y = mc::world::min_build_y; y < mc::world::max_build_y; ++y) {
                assert(left.block(x, y, z) == right.block(x, y, z));
            }
        }
    }
}

void test_deterministic_generation() {
    const mc::world::ChunkGenerator generator({0x1234'5678'9ABC'DEF0ULL, 63});
    const auto first = generator.generate({-17, 23});
    const auto second = generator.generate({-17, 23});
    assert_same_chunk(first, second);
}

void test_terrain_invariants() {
    const mc::world::ChunkGenerator generator({42, 63});
    const auto chunk = generator.generate({0, 0});
    for (std::size_t z = 0; z < 16; ++z) {
        for (std::size_t x = 0; x < 16; ++x) {
            const auto height = chunk.height(x, z);
            assert(height >= 63);
            assert(height < mc::world::max_build_y);
            assert(chunk.block(x, mc::world::min_build_y, z) == mc::world::BlockId::bedrock);
            assert(chunk.block(x, height, z) != mc::world::BlockId::air);
            if (height + 1 < mc::world::max_build_y) {
                assert(chunk.block(x, height + 1, z) == mc::world::BlockId::air);
            }
        }
    }
}

void test_negative_coordinate_continuity() {
    const mc::world::ChunkGenerator generator({7, 63});
    const auto west = generator.generate({-1, 0});
    const auto east = generator.generate({0, 0});
    for (std::size_t z = 0; z < 16; ++z) {
        const auto difference = west.height(15, z) - east.height(0, z);
        assert(difference >= -4 && difference <= 4);
    }
}

void test_biomes_and_decorations() {
    const mc::world::ChunkGenerator generator({0, 63});
    bool found_tree = false;
    bool found_plant = false;
    std::array<bool, 5> found_biomes{};
    for (std::int32_t chunk_z = -2; chunk_z <= 2; ++chunk_z) {
        for (std::int32_t chunk_x = -2; chunk_x <= 2; ++chunk_x) {
            const auto chunk = generator.generate({chunk_x, chunk_z});
            for (std::size_t quart_z = 0; quart_z < 4; ++quart_z) {
                for (std::size_t quart_x = 0; quart_x < 4; ++quart_x) {
                    found_biomes[static_cast<std::size_t>(
                        chunk.biome(quart_x, 0, quart_z))] = true;
                }
            }
            for (const auto& section : chunk.sections()) {
                for (const auto block : section.blocks()) {
                    found_tree = found_tree || block == mc::world::BlockId::oak_log ||
                        block == mc::world::BlockId::oak_leaves;
                    found_plant = found_plant || block == mc::world::BlockId::short_grass ||
                        block == mc::world::BlockId::dandelion ||
                        block == mc::world::BlockId::poppy;
                }
            }
        }
    }
    assert(found_tree);
    assert(found_plant);
    assert(std::count(found_biomes.begin(), found_biomes.end(), true) >= 2);
}

void test_chunk_manager_deduplication() {
    mc::world::World world({{99, 63}, 4, std::nullopt});
    std::vector<std::shared_ptr<const mc::world::Chunk>> chunks(8);
    std::vector<std::jthread> workers;
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        workers.emplace_back([&world, &chunks, index] {
            chunks[index] = world.chunk({-3, 7});
        });
    }
    workers.clear();
    for (const auto& chunk : chunks) {
        assert(chunk == chunks.front());
    }
    assert(world.chunks().loaded_count() == 1);
}

void test_chunk_manager_eviction() {
    mc::world::World world({{101, 63}, 2, std::nullopt});
    for (std::int32_t x = 0; x < 8; ++x) {
        static_cast<void>(world.chunk({x, 0}));
    }
    world.chunks().unload_unused();
    assert(world.chunks().loaded_count() <= 2);
}

void test_world_mutation_and_falling_blocks() {
    mc::world::World world({{2026, 63}, 8, std::nullopt});
    const auto surface = world.surface_height(-1, -17);
    assert(world.block({-1, surface, -17}) != mc::world::BlockId::air);

    world.set_block({-1, surface + 1, -17}, mc::world::BlockId::stone);
    world.set_block({-1, surface + 4, -17}, mc::world::BlockId::sand);
    const auto changed = world.settle_falling_column({-1, surface + 1, -17});
    assert(changed.size() == 2);
    assert(world.block({-1, surface + 4, -17}) == mc::world::BlockId::air);
    assert(world.block({-1, surface + 2, -17}) == mc::world::BlockId::sand);
    assert(world.surface_height(-1, -17) == surface + 2);
}

void test_world_biome_lookup() {
    mc::world::World world({{3030, 63}, 8, std::nullopt});
    constexpr std::int32_t x = -17;
    constexpr std::int32_t z = 18;
    const auto y = world.surface_height(x, z);
    const auto chunk = world.chunk({-2, 1});
    assert(world.biome(x, y, z) == chunk->biome(15 / 4, 0, 2 / 4));
}

void test_world_time() {
    mc::world::World world({{3031, 63}, 8, std::nullopt, 100, 6'000, true});
    world.tick_time(20);
    assert(world.game_time() == 120);
    assert(world.day_time() == 6'020);
    world.set_advance_time(false);
    world.tick_time(5);
    assert(world.game_time() == 125);
    assert(world.day_time() == 6'020);
    world.set_advance_time(true);
    world.tick_time(5);
    assert(world.day_time() == 6'025);
    mc::world::World frozen({{3032, 63}, 8, std::nullopt, 10, 20, false});
    frozen.tick_time(5);
    assert(frozen.game_time() == 15);
    assert(frozen.day_time() == 20);

    mc::world::World weather({
        {3033, 63}, 8, std::nullopt, 0, 0, true, 1, false, false});
    auto update = weather.tick_weather();
    assert(weather.raining());
    assert(update.raining == true);
    assert(update.rain_level == 0.01F);
    weather.set_weather(false, false, 10);
    update = weather.tick_weather();
    assert(update.rain_level == 0.0F);
    weather.set_weather(true, true, 1);
    weather.set_weather_cycle(false);
    update = weather.tick_weather();
    assert(weather.raining());
    assert(update.rain_level == 0.01F);
    assert(update.thunder_level == 0.01F);
    weather.set_weather_cycle(true);
    update = weather.tick_weather();
    assert(!weather.raining());
    assert(update.raining == false);
}

void test_world_border() {
    mc::world::World world({{3034, 63}, 8, std::nullopt});
    assert(world.inside_border({29'999'983.7, 0.0, 0.0}, 0.3));
    assert(!world.inside_border({29'999'984.0, 0.0, 0.0}, 0.3));
    world.set_border_center(10.0, -20.0);
    world.set_border_size(100.0);
    assert(world.inside_border({59.7, 0.0, -20.0}, 0.3));
    assert(!world.inside_border({60.0, 0.0, -20.0}, 0.3));
    assert(!world.inside_border({10.0, 0.0, 30.0}, 0.3));
    world.set_border_lerp_size(200.0, 40);
    world.tick_border(20);
    assert(world.border().size == 150.0);
    assert(world.inside_border({84.7, 0.0, -20.0}, 0.3));
    world.tick_border(20);
    assert(world.border().size == 200.0);
    assert(world.border().lerp_remaining_ticks == 0);
    world.set_border_warning_distance(12);
    world.set_border_warning_time(8);
    assert(world.border().warning_distance == 12);
    assert(world.border().warning_time == 8);
    try {
        world.set_border_center(30'000'000.0, 0.0);
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_world_collision_and_line_of_sight() {
    mc::world::World world({{4040, 63}, 8, std::nullopt});
    const auto surface = world.surface_height(0, 0);
    const auto obstacle_y = surface + 10;
    world.set_block({0, obstacle_y, 0}, mc::world::BlockId::stone);
    assert(world.solid({0, obstacle_y, 0}));
    assert(!world.solid({1, obstacle_y, 0}));
    assert(world.collides(
        {-0.25, static_cast<double>(obstacle_y), -0.25},
        {0.25, static_cast<double>(obstacle_y + 1), 0.25}));
    assert(!world.collides(
        {1.1, static_cast<double>(obstacle_y), 0.1},
        {1.9, static_cast<double>(obstacle_y + 1), 0.9}));
    assert(!world.line_of_sight(
        {-2.5, static_cast<double>(obstacle_y) + 0.5, 0.5},
        {2.5, static_cast<double>(obstacle_y) + 0.5, 0.5}));
    assert(world.line_of_sight(
        {-2.5, static_cast<double>(obstacle_y) + 0.5, 0.5},
        {0.5, static_cast<double>(obstacle_y) + 0.5, 0.5},
        mc::core::BlockPosition{0, obstacle_y, 0}));
}

void test_block_tick_system() {
    mc::world::World world({{5050, 63}, 8, std::nullopt});
    mc::world::BlockTickSystem ticks(77);
    const auto surface = world.surface_height(2, 2);
    world.set_block({2, surface + 1, 2}, mc::world::BlockId::stone);
    world.set_block({2, surface + 4, 2}, mc::world::BlockId::gravel);
    ticks.notify_neighbors({2, surface + 1, 2});
    const auto gravity = ticks.tick(world, {});
    assert(gravity.size() == 2);
    assert(world.block({2, surface + 2, 2}) == mc::world::BlockId::gravel);

    world.chunk({0, 0})->set_biome(
        0, static_cast<std::size_t>(surface + 1 - mc::world::min_build_y) / 4,
        0, mc::world::BiomeId::plains);
    world.set_block({3, surface + 1, 3}, mc::world::BlockId::dirt);
    auto changed = ticks.random_tick_at(world, {3, surface + 1, 3}, 1);
    assert(changed.size() == 1);
    assert(world.block({3, surface + 1, 3}) == mc::world::BlockId::grass_block);
    world.set_block({3, surface + 2, 3}, mc::world::BlockId::stone);
    changed = ticks.random_tick_at(world, {3, surface + 1, 3}, 1);
    assert(world.block({3, surface + 1, 3}) == mc::world::BlockId::dirt);

    world.set_block({4, surface + 1, 4}, mc::world::BlockId::grass_block);
    changed = ticks.random_tick_at(world, {4, surface + 1, 4}, 0);
    assert(world.block({4, surface + 2, 4}) == mc::world::BlockId::short_grass);

    const auto fluid_y = surface + 10;
    world.set_block({8, fluid_y, 8}, mc::world::BlockId::water);
    changed = ticks.fluid_tick_at(world, {8, fluid_y, 8}, 0);
    assert(changed == std::vector<mc::core::BlockPosition>({{8, fluid_y - 1, 8}}));
    assert(world.block({8, fluid_y - 1, 8}) == mc::world::BlockId::water);
    world.set_block({8, fluid_y - 1, 8}, mc::world::BlockId::stone);
    changed = ticks.fluid_tick_at(world, {8, fluid_y, 8}, 0);
    assert(changed.size() == 1);
    assert(world.block({9, fluid_y, 8}) == mc::world::BlockId::water);
}

void test_region_storage_round_trip() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("mcsquared-world-test-" + std::to_string(unique));
    try {
        mc::world::LevelStorage storage(root);
        const mc::world::ChunkGenerator generator({2026, 63});
        auto source = generator.generate({-33, 64});
        storage.save_chunk(source);

        const auto region_path = root / "region" / "r.-2.2.mca";
        assert(std::filesystem::exists(region_path));
        assert(std::filesystem::file_size(region_path) >= 12'288);

        const auto loaded = storage.load_chunk({-33, 64});
        assert(loaded.has_value());
        assert_same_chunk(source, *loaded);

        source.set_block(3, 70, 4, mc::world::BlockId::gravel);
        storage.save_chunk(source);
        const auto overwritten = storage.load_chunk({-33, 64});
        assert(overwritten.has_value());
        assert(overwritten->block(3, 70, 4) == mc::world::BlockId::gravel);
        assert(!storage.load_chunk({-32, 64}).has_value());
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void test_world_restart_loads_persisted_chunk() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("mcsquared-restart-test-" + std::to_string(unique));
    try {
        std::shared_ptr<const mc::world::Chunk> original;
        {
            mc::world::World world({{123, 63}, 4, root});
            original = world.chunk({5, -9});
        }
        {
            mc::world::World world({{999, 63}, 4, root});
            const auto restored = world.chunk({5, -9});
            assert_same_chunk(*original, *restored);
        }
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void test_world_restart_restores_all_state() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("mcsquared-state-restart-test-" + std::to_string(unique));
    try {
        {
            mc::world::World world({{8'080, 63}, 4, root, 100, 6'000, true});
            world.set_block({3, 100, 4}, mc::world::BlockId::iron_ore);
            world.tick_time(25);
            world.set_advance_time(false);
            world.set_weather(true, true, 4'000);
            world.set_weather_cycle(false);
            static_cast<void>(world.tick_weather());
            world.set_border_center(12.5, -30.25);
            world.set_border_size(500.0);
            world.set_border_lerp_size(750.0, 200);
            world.tick_border(50);
            world.set_border_warning_distance(17);
            world.set_border_warning_time(9);
            world.set_difficulty(3);
            world.set_spawn({24, 80, -16});
            world.set_game_rule("keepInventory", "true");
            assert(world.save_all() == 1);
        }
        {
            mc::world::World restored({{1, 63}, 4, root});
            assert(restored.block({3, 100, 4}) == mc::world::BlockId::iron_ore);
            assert(restored.game_time() == 125);
            assert(restored.day_time() == 6'025);
            restored.tick_time(5);
            assert(restored.game_time() == 130);
            assert(restored.day_time() == 6'025);
            assert(restored.raining());
            assert(restored.rain_level() == 0.01F);
            assert(restored.thunder_level() == 0.01F);
            assert(restored.border().center_x == 12.5);
            assert(restored.border().center_z == -30.25);
            assert(restored.border().size == 562.5);
            assert(restored.border().lerp_remaining_ticks == 150);
            assert(restored.border().warning_distance == 17);
            assert(restored.border().warning_time == 9);
            assert(restored.difficulty() == 3);
            const mc::core::BlockPosition expected_spawn{24, 80, -16};
            assert(restored.spawn() == expected_spawn);
            assert(restored.game_rule("keepInventory") == "true");
        }
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void test_dirty_chunk_autosave_and_unload() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("mcsquared-dirty-save-test-" + std::to_string(unique));
    try {
        {
            mc::world::World world({{7070, 63}, 4, root});
            auto chunk = world.chunk({2, 3});
            assert(!chunk->dirty());
            chunk->set_block(1, 100, 2, mc::world::BlockId::iron_ore);
            assert(chunk->dirty());
            assert(world.save_dirty() == 1);
            assert(!chunk->dirty());
            assert(world.save_dirty() == 0);
            chunk->set_block(1, 101, 2, mc::world::BlockId::coal_ore);
            chunk.reset();
            assert(world.chunks().unload({2, 3}));
        }
        {
            mc::world::World restored({{0, 63}, 4, root});
            const auto chunk = restored.chunk({2, 3});
            assert(chunk->block(1, 100, 2) == mc::world::BlockId::iron_ore);
            assert(chunk->block(1, 101, 2) == mc::world::BlockId::coal_ore);
            assert(!chunk->dirty());
        }
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void test_level_metadata_round_trip() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("mcsquared-metadata-test-" + std::to_string(unique));
    try {
        mc::world::LevelStorage storage(root);
        assert(!storage.load_metadata().has_value());
        mc::world::LevelMetadata metadata;
        metadata.seed = 0x1234'5678'9ABC'DEF0ULL;
        metadata.spawn = {-120, 72, 345};
        metadata.dimensions = {
            mc::core::ResourceLocation::parse("minecraft:overworld"),
            mc::core::ResourceLocation::parse("minecraft:the_nether"),
            mc::core::ResourceLocation::parse("minecraft:the_end"),
        };
        metadata.game_rules = {{"doDaylightCycle", "true"}, {"keepInventory", "false"}};
        storage.save_metadata(metadata);
        assert(storage.load_metadata() == metadata);

        metadata.game_rules["keepInventory"] = "true";
        storage.save_metadata(metadata);
        assert(storage.load_metadata() == metadata);

        metadata.data_version = mc::world::LevelMetadata::current_data_version + 1;
        try {
            storage.save_metadata(metadata);
            assert(false);
        } catch (const std::invalid_argument&) {
        }
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_deterministic_generation();
    test_terrain_invariants();
    test_negative_coordinate_continuity();
    test_biomes_and_decorations();
    test_chunk_manager_deduplication();
    test_chunk_manager_eviction();
    test_world_mutation_and_falling_blocks();
    test_world_biome_lookup();
    test_world_time();
    test_world_border();
    test_world_collision_and_line_of_sight();
    test_block_tick_system();
    test_region_storage_round_trip();
    test_world_restart_loads_persisted_chunk();
    test_world_restart_restores_all_state();
    test_dirty_chunk_autosave_and_unload();
    test_level_metadata_round_trip();
}