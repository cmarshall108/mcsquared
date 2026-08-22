#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::world {

inline constexpr std::int32_t chunk_width = 16;
inline constexpr std::int32_t section_height = 16;
inline constexpr std::int32_t min_build_y = -64;
inline constexpr std::int32_t max_build_y = 320;
inline constexpr std::size_t section_count = 24;

struct ChunkPosition final {
	std::int32_t x;
	std::int32_t z;

	bool operator==(const ChunkPosition&) const = default;
};

enum class BlockId : std::uint16_t {
	air,
	bedrock,
	stone,
	dirt,
	grass_block,
	water,
	sand,
	gravel,
	coal_ore,
	iron_ore,
	oak_log,
	oak_leaves,
	short_grass,
	dandelion,
	poppy,
};

enum class BiomeId : std::uint8_t {
	plains,
	forest,
	desert,
	ocean,
	mountains,
};

class ChunkSection final {
public:
	[[nodiscard]] BlockId block(std::size_t x, std::size_t y, std::size_t z) const;
	void set_block(std::size_t x, std::size_t y, std::size_t z, BlockId block);
	[[nodiscard]] std::size_t non_air_count() const noexcept;
	[[nodiscard]] const std::array<BlockId, 4096>& blocks() const noexcept;

private:
	std::array<BlockId, 4096> blocks_{};
	std::size_t non_air_count_{0};
};

class Chunk final {
public:
	explicit Chunk(ChunkPosition position);

	[[nodiscard]] ChunkPosition position() const noexcept;
	[[nodiscard]] BlockId block(std::size_t x, std::int32_t y, std::size_t z) const;
	void set_block(std::size_t x, std::int32_t y, std::size_t z, BlockId block);
	[[nodiscard]] std::int32_t height(std::size_t x, std::size_t z) const;
	void set_height(std::size_t x, std::size_t z, std::int32_t height);
	[[nodiscard]] BiomeId biome(std::size_t quart_x,
								std::size_t quart_y,
								std::size_t quart_z) const;
	void set_biome(std::size_t quart_x,
				   std::size_t quart_y,
				   std::size_t quart_z,
				   BiomeId biome);
	[[nodiscard]] const std::array<ChunkSection, section_count>& sections() const noexcept;
	[[nodiscard]] bool dirty() const noexcept;
	void mark_saved() noexcept;

private:
	ChunkPosition position_;
	std::array<ChunkSection, section_count> sections_{};
	std::array<std::int32_t, 256> heightmap_{};
	std::array<BiomeId, section_count * 64> biomes_{};
	bool dirty_{false};
};

class ChunkManager;

} // namespace mc::world