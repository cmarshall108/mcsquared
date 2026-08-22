#pragma once

#include "mc/entity/entity.hpp"
#include "mc/item/item.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace mc::entity {

struct ItemPickup final {
	EntityId entity_id;
	mc::item::ItemStack stack;
};

struct DroppedItemTick final {
	std::vector<ItemPickup> pickups;
	std::vector<EntityId> changed;
};

class DroppedItemSystem final {
public:
	[[nodiscard]] EntityId spawn(EntityManager& entities,
								 mc::item::ItemStack stack,
								 Vec3 position,
								 std::uint32_t pickup_delay_ticks = 20,
								 std::uint32_t lifetime_ticks = 6'000);
	[[nodiscard]] DroppedItemTick tick(EntityManager& entities,
									 const mc::item::ItemRegistry& registry,
									 std::optional<Vec3> collector);
	[[nodiscard]] const mc::item::ItemStack* stack(EntityId entity) const noexcept;
	[[nodiscard]] std::size_t size() const noexcept;

private:
	struct State final {
		mc::item::ItemStack stack;
		std::uint32_t pickup_delay_ticks;
		std::uint32_t lifetime_ticks;
	};

	std::map<EntityId, State> items_;
};

} // namespace mc::entity