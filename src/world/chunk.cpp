#include "mc/world/chunk.hpp"

#include <stdexcept>

namespace mc::world {
namespace {

[[nodiscard]] std::size_t block_index(const std::size_t x,
                                      const std::size_t y,
                                      const std::size_t z) {
    if (x >= 16 || y >= 16 || z >= 16) {
        throw std::out_of_range("chunk-section block coordinate is out of range");
    }
    return (y * 16 + z) * 16 + x;
}

[[nodiscard]] std::size_t column_index(const std::size_t x, const std::size_t z) {
    if (x >= 16 || z >= 16) {
        throw std::out_of_range("chunk column coordinate is out of range");
    }
    return z * 16 + x;
}

} // namespace

BlockId ChunkSection::block(const std::size_t x,
                            const std::size_t y,
                            const std::size_t z) const {
    return blocks_[block_index(x, y, z)];
}

void ChunkSection::set_block(const std::size_t x,
                             const std::size_t y,
                             const std::size_t z,
                             const BlockId block_value) {
    auto& current = blocks_[block_index(x, y, z)];
    if (current == BlockId::air && block_value != BlockId::air) {
        ++non_air_count_;
    } else if (current != BlockId::air && block_value == BlockId::air) {
        --non_air_count_;
    }
    current = block_value;
}

std::size_t ChunkSection::non_air_count() const noexcept {
    return non_air_count_;
}

const std::array<BlockId, 4096>& ChunkSection::blocks() const noexcept {
    return blocks_;
}

Chunk::Chunk(const ChunkPosition position) : position_(position) {
    heightmap_.fill(min_build_y);
    biomes_.fill(BiomeId::plains);
}

ChunkPosition Chunk::position() const noexcept {
    return position_;
}

BlockId Chunk::block(const std::size_t x, const std::int32_t y, const std::size_t z) const {
    if (y < min_build_y || y >= max_build_y) {
        throw std::out_of_range("block Y coordinate is outside build height");
    }
    const auto offset = static_cast<std::size_t>(y - min_build_y);
    return sections_[offset / section_height].block(x, offset % section_height, z);
}

void Chunk::set_block(const std::size_t x,
                      const std::int32_t y,
                      const std::size_t z,
                      const BlockId block_value) {
    if (y < min_build_y || y >= max_build_y) {
        throw std::out_of_range("block Y coordinate is outside build height");
    }
    const auto offset = static_cast<std::size_t>(y - min_build_y);
    const auto previous = sections_[offset / section_height].block(
        x, offset % section_height, z);
    sections_[offset / section_height].set_block(x, offset % section_height, z, block_value);
    dirty_ = dirty_ || previous != block_value;
    auto& height = heightmap_[column_index(x, z)];
    if (block_value != BlockId::air && y > height) {
        height = y;
    } else if (previous != BlockId::air && block_value == BlockId::air && y == height) {
        while (height > min_build_y && block(x, height, z) == BlockId::air) {
            --height;
        }
    }
}

std::int32_t Chunk::height(const std::size_t x, const std::size_t z) const {
    return heightmap_[column_index(x, z)];
}

void Chunk::set_height(const std::size_t x,
                       const std::size_t z,
                       const std::int32_t height_value) {
    if (height_value < min_build_y || height_value >= max_build_y) {
        throw std::out_of_range("heightmap value is outside build height");
    }
    heightmap_[column_index(x, z)] = height_value;
    dirty_ = true;
}

BiomeId Chunk::biome(const std::size_t quart_x,
                     const std::size_t quart_y,
                     const std::size_t quart_z) const {
    if (quart_x >= 4 || quart_z >= 4 || quart_y >= section_count * 4) {
        throw std::out_of_range("quart biome coordinate is out of range");
    }
    return biomes_[(quart_y * 4 + quart_z) * 4 + quart_x];
}

void Chunk::set_biome(const std::size_t quart_x,
                      const std::size_t quart_y,
                      const std::size_t quart_z,
                      const BiomeId biome_value) {
    if (quart_x >= 4 || quart_z >= 4 || quart_y >= section_count * 4) {
        throw std::out_of_range("quart biome coordinate is out of range");
    }
    biomes_[(quart_y * 4 + quart_z) * 4 + quart_x] = biome_value;
    dirty_ = true;
}

bool Chunk::dirty() const noexcept { return dirty_; }
void Chunk::mark_saved() noexcept { dirty_ = false; }

const std::array<ChunkSection, section_count>& Chunk::sections() const noexcept {
    return sections_;
}

} // namespace mc::world