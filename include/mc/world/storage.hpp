#pragma once

#include "mc/world/chunk.hpp"
#include "mc/core/types.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace mc::world {

using ChunkPayload = std::vector<std::uint8_t>;

struct LevelMetadata final {
	static constexpr std::uint32_t current_data_version = 4903;

	std::uint32_t data_version{current_data_version};
	std::uint64_t seed{0};
	mc::core::BlockPosition spawn{0, 64, 0};
	std::vector<mc::core::ResourceLocation> dimensions{
		mc::core::ResourceLocation::parse("minecraft:overworld")};
	std::map<std::string, std::string> game_rules;
	std::map<std::string, std::string> world_state;

	bool operator==(const LevelMetadata&) const = default;
};

struct RegionPosition final {
	std::int32_t x;
	std::int32_t z;

	bool operator==(const RegionPosition&) const = default;
};

[[nodiscard]] RegionPosition region_position(ChunkPosition chunk) noexcept;
[[nodiscard]] std::uint8_t local_region_x(ChunkPosition chunk) noexcept;
[[nodiscard]] std::uint8_t local_region_z(ChunkPosition chunk) noexcept;
[[nodiscard]] ChunkPayload serialize_chunk(const Chunk& chunk);
[[nodiscard]] Chunk deserialize_chunk(std::span<const std::uint8_t> payload);

class RegionFile final {
public:
	explicit RegionFile(std::filesystem::path path);
	~RegionFile();

	RegionFile(const RegionFile&) = delete;
	RegionFile& operator=(const RegionFile&) = delete;

	[[nodiscard]] std::optional<ChunkPayload> read(std::uint8_t local_x,
												   std::uint8_t local_z) const;
	void write(std::uint8_t local_x,
			   std::uint8_t local_z,
			   std::span<const std::uint8_t> payload);

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

class LevelStorage final {
public:
	explicit LevelStorage(std::filesystem::path root);
	~LevelStorage();

	LevelStorage(const LevelStorage&) = delete;
	LevelStorage& operator=(const LevelStorage&) = delete;

	[[nodiscard]] std::optional<Chunk> load_chunk(ChunkPosition position);
	void save_chunk(const Chunk& chunk);
	[[nodiscard]] std::optional<LevelMetadata> load_metadata() const;
	void save_metadata(const LevelMetadata& metadata);

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

class PlayerStorage;

} // namespace mc::world