#include "mc/world/generation.hpp"

#include <algorithm>
#include <cstdint>

namespace mc::world {
namespace {

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::int64_t floor_div(const std::int64_t value,
                                     const std::int64_t divisor) noexcept {
    const auto quotient = value / divisor;
    const auto remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

[[nodiscard]] std::int32_t lattice(const std::uint64_t seed,
                                   const std::int64_t x,
                                   const std::int64_t z) noexcept {
    const auto x_bits = static_cast<std::uint64_t>(x);
    const auto z_bits = static_cast<std::uint64_t>(z);
    const auto value = mix(seed ^ mix(x_bits) ^ (mix(z_bits) << 1U));
    return static_cast<std::int32_t>(value & 0xFFFFU) - 32'768;
}

[[nodiscard]] std::int32_t noise(const std::uint64_t seed,
                                 const std::int64_t x,
                                 const std::int64_t z,
                                 const std::int64_t scale) noexcept {
    const auto cell_x = floor_div(x, scale);
    const auto cell_z = floor_div(z, scale);
    const auto local_x = x - cell_x * scale;
    const auto local_z = z - cell_z * scale;
    const auto top = lattice(seed, cell_x, cell_z) * (scale - local_x) +
        lattice(seed, cell_x + 1, cell_z) * local_x;
    const auto bottom = lattice(seed, cell_x, cell_z + 1) * (scale - local_x) +
        lattice(seed, cell_x + 1, cell_z + 1) * local_x;
    return static_cast<std::int32_t>(
        (top * (scale - local_z) + bottom * local_z) / (scale * scale));
}

[[nodiscard]] bool ore_at(const std::uint64_t seed,
                          const std::int64_t x,
                          const std::int32_t y,
                          const std::int64_t z,
                          const std::uint64_t modulus) noexcept {
    const auto value = mix(seed ^ mix(static_cast<std::uint64_t>(x)) ^
                           mix(static_cast<std::uint64_t>(z)) ^
                           mix(static_cast<std::uint64_t>(y)));
    return value % modulus == 0;
}

[[nodiscard]] std::uint64_t feature_value(const std::uint64_t seed,
                                          const std::int64_t x,
                                          const std::int64_t z,
                                          const std::uint64_t salt) noexcept {
    return mix(seed ^ salt ^ mix(static_cast<std::uint64_t>(x)) ^
               (mix(static_cast<std::uint64_t>(z)) << 1U));
}

} // namespace

BiomeSource::BiomeSource(const std::uint64_t seed) noexcept : seed_(seed) {}

BiomeId BiomeSource::sample(const std::int64_t block_x,
                            const std::int32_t surface_y,
                            const std::int64_t block_z) const noexcept {
    if (surface_y < 60) {
        return BiomeId::ocean;
    }
    if (surface_y > 86) {
        return BiomeId::mountains;
    }
    const auto temperature = noise(seed_ ^ 0xA24BAED4963EE407ULL, block_x, block_z, 192);
    const auto humidity = noise(seed_ ^ 0x9FB21C651E98DF25ULL, block_x, block_z, 160);
    if (temperature > 8'000 && humidity < -2'000) {
        return BiomeId::desert;
    }
    if (humidity > 4'000) {
        return BiomeId::forest;
    }
    return BiomeId::plains;
}

ChunkGenerator::ChunkGenerator(const GenerationSettings settings)
    : settings_(settings), biomes_(settings.seed) {
    if (settings_.sea_level < min_build_y || settings_.sea_level >= max_build_y) {
        throw std::invalid_argument("sea level is outside build height");
    }
}

std::int32_t ChunkGenerator::surface_height(const std::int64_t block_x,
                                            const std::int64_t block_z) const noexcept {
    const auto continental = noise(settings_.seed, block_x, block_z, 128);
    const auto detail = noise(settings_.seed ^ 0xD1B54A32D192ED03ULL, block_x, block_z, 32);
    const auto height = settings_.sea_level + continental * 30 / 32'768 + detail * 9 / 32'768;
    return std::clamp(height, min_build_y + 5, max_build_y - 2);
}

Chunk ChunkGenerator::generate(const ChunkPosition position) const {
    Chunk chunk(position);
    for (std::size_t z = 0; z < 16; ++z) {
        for (std::size_t x = 0; x < 16; ++x) {
            const auto block_x = static_cast<std::int64_t>(position.x) * 16 +
                static_cast<std::int64_t>(x);
            const auto block_z = static_cast<std::int64_t>(position.z) * 16 +
                static_cast<std::int64_t>(z);
            const auto surface = surface_height(block_x, block_z);
            const auto biome = biomes_.sample(block_x, surface, block_z);
            chunk.set_height(x, z, std::max(surface, settings_.sea_level));

            for (auto y = min_build_y; y <= std::max(surface, settings_.sea_level); ++y) {
                BlockId block = BlockId::air;
                if (y == min_build_y) {
                    block = BlockId::bedrock;
                } else if (y <= surface - 4) {
                    if (y < 32 && ore_at(settings_.seed ^ 0xC0A1ULL, block_x, y, block_z, 97)) {
                        block = BlockId::iron_ore;
                    } else if (y < 96 && ore_at(settings_.seed ^ 0xC0A2ULL, block_x, y, block_z, 61)) {
                        block = BlockId::coal_ore;
                    } else {
                        block = BlockId::stone;
                    }
                } else if (y < surface) {
                    block = biome == BiomeId::desert ? BlockId::sand : BlockId::dirt;
                } else if (y == surface) {
                    block = biome == BiomeId::desert || surface <= settings_.sea_level
                        ? BlockId::sand
                        : BlockId::grass_block;
                } else if (y <= settings_.sea_level) {
                    block = BlockId::water;
                }
                chunk.set_block(x, y, z, block);
            }

            for (std::size_t quart_y = 0; quart_y < section_count * 4; ++quart_y) {
                if (x % 4 == 0 && z % 4 == 0) {
                    chunk.set_biome(x / 4, quart_y, z / 4, biome);
                }
            }
        }
    }

    for (std::size_t candidate = 0; candidate < 4; ++candidate) {
        const auto chunk_seed = feature_value(
            settings_.seed, position.x, position.z, 0x54524545ULL + candidate);
        const auto x = static_cast<std::size_t>(2 + chunk_seed % 12U);
        const auto z = static_cast<std::size_t>(2 + (chunk_seed >> 8U) % 12U);
        const auto surface = chunk.height(x, z);
        const auto biome = chunk.biome(x / 4, 0, z / 4);
        const auto tree_allowed = biome == BiomeId::forest ||
            (biome == BiomeId::plains && candidate == 0 && chunk_seed % 3U == 0);
        if (!tree_allowed || surface + 8 >= max_build_y ||
            chunk.block(x, surface, z) != BlockId::grass_block) {
            continue;
        }

        const auto trunk_height = static_cast<std::int32_t>(4 + (chunk_seed >> 16U) % 3U);
        for (std::int32_t y = surface + 1; y <= surface + trunk_height; ++y) {
            chunk.set_block(x, y, z, BlockId::oak_log);
        }
        const auto crown_y = surface + trunk_height;
        for (std::int32_t y = crown_y - 2; y <= crown_y + 1; ++y) {
            const auto radius = y == crown_y + 1 ? 1 : 2;
            for (std::int32_t offset_z = -radius; offset_z <= radius; ++offset_z) {
                for (std::int32_t offset_x = -radius; offset_x <= radius; ++offset_x) {
                    if (y == crown_y + 1 && std::abs(offset_x) == 1 &&
                        std::abs(offset_z) == 1) {
                        continue;
                    }
                    const auto leaf_x = static_cast<std::size_t>(
                        static_cast<std::int32_t>(x) + offset_x);
                    const auto leaf_z = static_cast<std::size_t>(
                        static_cast<std::int32_t>(z) + offset_z);
                    if (chunk.block(leaf_x, y, leaf_z) == BlockId::air) {
                        chunk.set_block(leaf_x, y, leaf_z, BlockId::oak_leaves);
                        chunk.set_height(leaf_x, leaf_z,
                                         std::max(chunk.height(leaf_x, leaf_z), y));
                    }
                }
            }
        }
        chunk.set_height(x, z, crown_y + 1);
    }

    for (std::size_t z = 0; z < 16; ++z) {
        for (std::size_t x = 0; x < 16; ++x) {
            const auto surface = chunk.height(x, z);
            if (surface + 1 >= max_build_y ||
                chunk.block(x, surface, z) != BlockId::grass_block) {
                continue;
            }
            const auto block_x = static_cast<std::int64_t>(position.x) * 16 +
                static_cast<std::int64_t>(x);
            const auto block_z = static_cast<std::int64_t>(position.z) * 16 +
                static_cast<std::int64_t>(z);
            const auto value = feature_value(settings_.seed, block_x, block_z, 0x504C414E54ULL);
            BlockId plant = BlockId::air;
            if (value % 131U == 0) {
                plant = BlockId::poppy;
            } else if (value % 97U == 0) {
                plant = BlockId::dandelion;
            } else if (value % 13U == 0) {
                plant = BlockId::short_grass;
            }
            if (plant != BlockId::air) {
                chunk.set_block(x, surface + 1, z, plant);
                chunk.set_height(x, z, surface + 1);
            }
        }
    }
    return chunk;
}

} // namespace mc::world