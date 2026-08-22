#pragma once

#include "mc/entity/entity.hpp"

#include <map>

namespace mc::entity {

struct TrackingUpdate final {
	bool entered{false};
	bool left{false};
	bool absolute_position{false};
	bool position{false};
	bool rotation{false};
	bool velocity{false};
	bool head_rotation{false};
	Vec3 delta{};
};

class EntityTracker final {
public:
	[[nodiscard]] TrackingUpdate update(const Entity& entity, Vec3 viewer);
	[[nodiscard]] bool forget(EntityId entity) noexcept;
	[[nodiscard]] bool visible(EntityId entity) const noexcept;

private:
	struct State final {
		Vec3 position;
		Vec3 velocity;
		float yaw;
		float pitch;
		float head_yaw;
	};

	[[nodiscard]] static double tracking_range(const Entity& entity) noexcept;

	std::map<EntityId, State> states_;
};

} // namespace mc::entity