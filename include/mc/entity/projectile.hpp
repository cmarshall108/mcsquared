#pragma once

#include "mc/entity/entity.hpp"
#include "mc/world/world.hpp"

#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

namespace mc::entity {

struct ProjectileImpact final {
	EntityId projectile;
	std::optional<EntityId> target;
	Vec3 position;
	std::optional<mc::item::ItemStack> recovery;
};

class ProjectileSystem final {
public:
	[[nodiscard]] EntityId spawn(EntityManager& entities,
								 std::string_view type,
								 Vec3 position,
								 Vec3 velocity,
								 EntityId owner,
								 float damage,
								std::uint32_t lifetime_ticks = 1'200,
								std::optional<mc::item::ItemStack> recovery = std::nullopt);
	[[nodiscard]] std::vector<ProjectileImpact> tick(
		EntityManager& entities, world::World& world);
	[[nodiscard]] std::size_t size() const noexcept;

private:
	struct State final {
		EntityId owner;
		float damage;
		std::uint32_t lifetime_ticks;
		std::optional<mc::item::ItemStack> recovery;
	};

	std::map<EntityId, State> projectiles_;
};

} // namespace mc::entity