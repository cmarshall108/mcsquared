#pragma once

#include "mc/world/chunk.hpp"

#include <cstdint>

namespace mc::world {

struct GenerationSettings final {
	std::uint64_t seed{0};
	std::int32_t sea_level{63};
};

class BiomeSource final {
public:
	explicit BiomeSource(std::uint64_t seed) noexcept;
	[[nodiscard]] BiomeId sample(std::int64_t block_x,
								 std::int32_t surface_y,
								 std::int64_t block_z) const noexcept;

private:
	std::uint64_t seed_;
};

class ChunkGenerator final {
public:
	explicit ChunkGenerator(GenerationSettings settings);
	[[nodiscard]] Chunk generate(ChunkPosition position) const;
	[[nodiscard]] std::int32_t surface_height(std::int64_t block_x,
											  std::int64_t block_z) const noexcept;

private:
	GenerationSettings settings_;
	BiomeSource biomes_;
};

class NoiseRouter;

} // namespace mc::world