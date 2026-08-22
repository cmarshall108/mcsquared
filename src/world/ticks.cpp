#include "mc/world/ticks.hpp"

#include "mc/world/world.hpp"

#include <array>

namespace mc::world {
namespace {

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

} // namespace

BlockTickSystem::BlockTickSystem(const std::uint64_t seed) : seed_(seed) {}

void BlockTickSystem::notify_neighbors(const mc::core::BlockPosition position,
                                       const std::uint32_t delay_ticks) {
    constexpr std::array<mc::core::BlockPosition, 7> offsets{{
        {0, 0, 0}, {0, 1, 0}, {0, -1, 0}, {1, 0, 0},
        {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}}};
    for (const auto offset : offsets) {
        scheduled_.emplace(
            tick_ + std::max<std::uint32_t>(1, delay_ticks),
            mc::core::BlockPosition{
                position.x + offset.x, position.y + offset.y, position.z + offset.z});
    }
}

std::vector<mc::core::BlockPosition> BlockTickSystem::tick(
    World& world, const std::span<const ChunkPosition> active_chunks) {
    ++tick_;
    std::vector<mc::core::BlockPosition> changed;
    for (auto iterator = scheduled_.begin();
         iterator != scheduled_.end() && iterator->first <= tick_;) {
        const auto position = iterator->second;
        iterator = scheduled_.erase(iterator);
        if (position.y > min_build_y && position.y < max_build_y) {
            const auto gravity = world.settle_falling_column(position);
            changed.insert(changed.end(), gravity.begin(), gravity.end());
            const auto fluid = fluid_tick_at(world, position, mix(seed_ ^ tick_));
            changed.insert(changed.end(), fluid.begin(), fluid.end());
        }
    }

    for (const auto chunk : active_chunks) {
        for (std::uint64_t attempt = 0; attempt < 3; ++attempt) {
            const auto random = mix(seed_ ^ mix(tick_) ^
                mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.x)) << 32U |
                    static_cast<std::uint32_t>(chunk.z)) ^ attempt);
            const auto x = chunk.x * chunk_width + static_cast<std::int32_t>(random & 15U);
            const auto z = chunk.z * chunk_width +
                static_cast<std::int32_t>((random >> 4U) & 15U);
            const auto y = world.surface_height(x, z);
            const auto random_changes = random_tick_at(world, {x, y, z}, random);
            changed.insert(changed.end(), random_changes.begin(), random_changes.end());
        }
    }
    return changed;
}

std::vector<mc::core::BlockPosition> BlockTickSystem::random_tick_at(
    World& world, const mc::core::BlockPosition position, const std::uint64_t random_value) {
    if (position.y < min_build_y || position.y + 1 >= max_build_y) return {};
    const auto block = world.block(position);
    const mc::core::BlockPosition above{position.x, position.y + 1, position.z};
    if (block == BlockId::water) {
        return fluid_tick_at(world, position, random_value);
    }
    if (block == BlockId::dirt && world.block(above) == BlockId::air) {
        const auto biome = world.biome(position.x, position.y, position.z);
        if (biome != BiomeId::desert && biome != BiomeId::ocean) {
            world.set_block(position, BlockId::grass_block);
            return {position};
        }
    } else if (block == BlockId::grass_block && world.solid(above)) {
        world.set_block(position, BlockId::dirt);
        return {position};
    } else if (block == BlockId::grass_block && world.block(above) == BlockId::air &&
               random_value % 32U == 0) {
        world.set_block(above, BlockId::short_grass);
        return {above};
    }
    return {};
}

std::vector<mc::core::BlockPosition> BlockTickSystem::fluid_tick_at(
    World& world, const mc::core::BlockPosition position, const std::uint64_t random_value) {
    if (position.y <= min_build_y || position.y >= max_build_y ||
        world.block(position) != BlockId::water) {
        return {};
    }
    const mc::core::BlockPosition below{position.x, position.y - 1, position.z};
    if (world.block(below) == BlockId::air) {
        world.set_block(below, BlockId::water);
        scheduled_.emplace(tick_ + 5, below);
        return {below};
    }

    constexpr std::array<mc::core::BlockPosition, 4> offsets{{
        {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}}};
    for (std::size_t attempt = 0; attempt < offsets.size(); ++attempt) {
        const auto& offset = offsets[(static_cast<std::size_t>(random_value) + attempt) %
            offsets.size()];
        const mc::core::BlockPosition adjacent{
            position.x + offset.x, position.y, position.z + offset.z};
        if (world.block(adjacent) == BlockId::air) {
            world.set_block(adjacent, BlockId::water);
            return {adjacent};
        }
    }
    return {};
}

} // namespace mc::world