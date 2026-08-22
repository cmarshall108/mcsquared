#pragma once

#include "mc/core/types.hpp"
#include "mc/world/chunk.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace mc::world {

class World;

class BlockTickSystem final {
public:
	explicit BlockTickSystem(std::uint64_t seed);

	void notify_neighbors(mc::core::BlockPosition position, std::uint32_t delay_ticks = 1);
	[[nodiscard]] std::vector<mc::core::BlockPosition> tick(
		World& world, std::span<const ChunkPosition> active_chunks);
	[[nodiscard]] std::vector<mc::core::BlockPosition> random_tick_at(
		World& world, mc::core::BlockPosition position, std::uint64_t random_value);
	[[nodiscard]] std::vector<mc::core::BlockPosition> fluid_tick_at(
		World& world, mc::core::BlockPosition position, std::uint64_t random_value);

private:
	std::uint64_t seed_;
	std::uint64_t tick_{0};
	std::multimap<std::uint64_t, mc::core::BlockPosition> scheduled_;
};

} // namespace mc::world