#pragma once

#include "mc/world/generation.hpp"
#include "mc/world/storage.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mc::world {

struct WorldPoint final {
	double x;
	double y;
	double z;
};

struct WorldBorder final {
	double center_x{0.0};
	double center_z{0.0};
	double size{59'999'968.0};
	double lerp_start_size{59'999'968.0};
	double lerp_target_size{59'999'968.0};
	std::uint64_t lerp_total_ticks{0};
	std::uint64_t lerp_remaining_ticks{0};
	std::int32_t warning_distance{5};
	std::int32_t warning_time{15};
};

struct WorldSettings final {
	GenerationSettings generation;
	std::size_t max_loaded_chunks{1024};
	std::optional<std::filesystem::path> storage_path;
	std::uint64_t game_time{0};
	std::uint64_t day_time{0};
	bool advance_time{true};
	std::uint32_t weather_duration{12'000};
	bool raining{false};
	bool thundering{false};
	bool weather_cycle{true};
	std::uint8_t difficulty{2};
	float rain_level{-1.0F};
	float thunder_level{-1.0F};
};

struct WeatherUpdate final {
	std::optional<bool> raining;
	std::optional<float> rain_level;
	std::optional<float> thunder_level;
};

class ChunkManager final {
public:
	explicit ChunkManager(ChunkGenerator generator,
						  std::size_t max_loaded_chunks,
						  std::shared_ptr<LevelStorage> storage = nullptr);
	~ChunkManager();

	ChunkManager(const ChunkManager&) = delete;
	ChunkManager& operator=(const ChunkManager&) = delete;

	[[nodiscard]] std::shared_ptr<Chunk> load(ChunkPosition position);
	[[nodiscard]] bool unload(ChunkPosition position);
	void unload_unused();
	[[nodiscard]] std::size_t save_dirty();
	[[nodiscard]] std::size_t loaded_count() const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

class World final {
public:
	explicit World(WorldSettings settings);
	~World();

	[[nodiscard]] std::shared_ptr<Chunk> chunk(ChunkPosition position);
	[[nodiscard]] BlockId block(mc::core::BlockPosition position);
	void set_block(mc::core::BlockPosition position, BlockId block);
	[[nodiscard]] std::int32_t surface_height(std::int32_t x, std::int32_t z);
	[[nodiscard]] BiomeId biome(std::int32_t x, std::int32_t y, std::int32_t z);
	[[nodiscard]] bool solid(mc::core::BlockPosition position);
	[[nodiscard]] bool line_of_sight(WorldPoint from,
									 WorldPoint to,
									 std::optional<mc::core::BlockPosition> allowed = std::nullopt);
	[[nodiscard]] bool collides(WorldPoint minimum, WorldPoint maximum);
	[[nodiscard]] std::vector<mc::core::BlockPosition> settle_falling_column(
		mc::core::BlockPosition position);
	[[nodiscard]] std::size_t save_dirty();
	[[nodiscard]] std::size_t save_all();
	void tick_time(std::uint64_t ticks = 1) noexcept;
	[[nodiscard]] std::uint64_t game_time() const noexcept;
	[[nodiscard]] std::uint64_t day_time() const noexcept;
	void set_day_time(std::uint64_t day_time) noexcept;
	void set_advance_time(bool advance) noexcept;
	[[nodiscard]] WeatherUpdate tick_weather(std::uint32_t ticks = 1) noexcept;
	void set_weather(bool raining, bool thundering, std::uint32_t duration) noexcept;
	void set_weather_cycle(bool advance) noexcept;
	[[nodiscard]] bool raining() const noexcept;
	[[nodiscard]] float rain_level() const noexcept;
	[[nodiscard]] float thunder_level() const noexcept;
	[[nodiscard]] const WorldBorder& border() const noexcept;
	void set_border_center(double x, double z);
	void set_border_size(double size);
	void set_border_lerp_size(double size, std::uint64_t duration_ticks);
	void tick_border(std::uint64_t ticks = 1) noexcept;
	void set_border_warning_distance(std::int32_t blocks);
	void set_border_warning_time(std::int32_t seconds);
	[[nodiscard]] bool inside_border(WorldPoint point, double margin = 0.0) const noexcept;
	[[nodiscard]] std::uint8_t difficulty() const noexcept;
	void set_difficulty(std::uint8_t difficulty);
	[[nodiscard]] bool has_persisted_state() const noexcept;
	[[nodiscard]] mc::core::BlockPosition spawn() const noexcept;
	void set_spawn(mc::core::BlockPosition spawn);
	[[nodiscard]] std::optional<std::string> game_rule(const std::string& name) const;
	void set_game_rule(std::string name, std::string value);
	[[nodiscard]] ChunkManager& chunks() noexcept;
	[[nodiscard]] const ChunkManager& chunks() const noexcept;

private:
	World(WorldSettings settings, std::nullptr_t);
	[[nodiscard]] static WorldSettings load_persisted_settings(WorldSettings settings);

	std::shared_ptr<LevelStorage> storage_;
	ChunkManager chunks_;
	LevelMetadata metadata_;
	std::uint64_t game_time_;
	std::uint64_t day_time_;
	bool advance_time_;
	std::uint64_t weather_seed_;
	std::uint32_t weather_duration_;
	bool raining_;
	bool thundering_;
	bool weather_cycle_{true};
	float rain_level_{0.0F};
	float thunder_level_{0.0F};
	WorldBorder border_;
	std::uint8_t difficulty_{2};
	std::map<std::string, std::string> game_rules_;
	bool has_persisted_state_{false};
};

class WorldManager;

} // namespace mc::world