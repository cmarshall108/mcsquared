#pragma once

#include "mc/core/types.hpp"
#include "mc/entity/entity.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <variant>
#include <vector>

namespace mc::entity {

enum class MemoryKey {
	target_entity,
	home,
	panic,
	tempted,
	owner_position,
	flock_leader,
	last_seen_position,
};

using MemoryValue = std::variant<EntityId, mc::core::BlockPosition, Vec3, bool, double>;

class Brain final {
public:
	void set(MemoryKey key, MemoryValue value);
	[[nodiscard]] const MemoryValue* get(MemoryKey key) const noexcept;
	[[nodiscard]] bool contains(MemoryKey key) const noexcept;
	void erase(MemoryKey key) noexcept;
	void clear() noexcept;

private:
	std::map<MemoryKey, MemoryValue> memories_;
};

enum class GoalControl : std::uint8_t {
	none = 0,
	move = 1U << 0U,
	look = 1U << 1U,
	jump = 1U << 2U,
	target = 1U << 3U,
};

[[nodiscard]] GoalControl operator|(GoalControl left, GoalControl right) noexcept;
[[nodiscard]] bool controls_overlap(GoalControl left, GoalControl right) noexcept;

class Goal {
public:
	Goal(std::uint32_t priority, GoalControl controls);
	virtual ~Goal() = default;

	Goal(const Goal&) = delete;
	Goal& operator=(const Goal&) = delete;

	[[nodiscard]] std::uint32_t priority() const noexcept;
	[[nodiscard]] GoalControl controls() const noexcept;
	[[nodiscard]] bool running() const noexcept;

	[[nodiscard]] virtual bool can_start(Entity& entity, Brain& brain) = 0;
	[[nodiscard]] virtual bool can_continue(Entity& entity, Brain& brain);
	virtual void start(Entity& entity, Brain& brain);
	virtual void tick(Entity& entity, Brain& brain, double delta_seconds) = 0;
	virtual void stop(Entity& entity, Brain& brain);

private:
	friend class GoalSelector;
	std::uint32_t priority_;
	GoalControl controls_;
	bool running_{false};
};

class GoalSelector final {
public:
	void add(std::unique_ptr<Goal> goal);
	void tick(Entity& entity, Brain& brain, double delta_seconds);
	void stop_all(Entity& entity, Brain& brain);
	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] std::size_t running_count() const noexcept;

private:
	std::vector<std::unique_ptr<Goal>> goals_;
};

class Navigation final {
public:
	using Passable = std::function<bool(mc::core::BlockPosition)>;

	[[nodiscard]] std::vector<mc::core::BlockPosition> find_path(
		mc::core::BlockPosition start,
		mc::core::BlockPosition destination,
		const Passable& passable,
		std::size_t max_visited = 4096) const;
	[[nodiscard]] bool steer(Entity& entity,
							 const std::vector<mc::core::BlockPosition>& path,
							 double speed) const;
};

class WanderGoal final : public Goal {
public:
	WanderGoal(std::uint32_t priority,
			   std::uint64_t seed,
			   std::uint32_t interval_ticks = 40);

	[[nodiscard]] bool can_start(Entity& entity, Brain& brain) override;
	[[nodiscard]] bool can_continue(Entity& entity, Brain& brain) override;
	void start(Entity& entity, Brain& brain) override;
	void tick(Entity& entity, Brain& brain, double delta_seconds) override;
	void stop(Entity& entity, Brain& brain) override;

private:
	std::uint64_t seed_;
	std::uint32_t interval_ticks_;
	std::uint32_t remaining_ticks_{0};
	Vec3 direction_{};
};

class MeleeAttackGoal final : public Goal {
public:
	MeleeAttackGoal(std::uint32_t priority,
					EntityManager& entities,
					float damage,
					double speed);

	[[nodiscard]] bool can_start(Entity& entity, Brain& brain) override;
	[[nodiscard]] bool can_continue(Entity& entity, Brain& brain) override;
	void tick(Entity& entity, Brain& brain, double delta_seconds) override;
	void stop(Entity& entity, Brain& brain) override;

private:
	[[nodiscard]] LivingEntity* target(Brain& brain) const noexcept;

	EntityManager* entities_;
	float damage_;
	double speed_;
	std::uint32_t cooldown_{0};
};

class PanicGoal final : public Goal {
public:
	PanicGoal(std::uint32_t priority, double speed, std::uint32_t duration_ticks = 40);

	[[nodiscard]] bool can_start(Entity& entity, Brain& brain) override;
	[[nodiscard]] bool can_continue(Entity& entity, Brain& brain) override;
	void start(Entity& entity, Brain& brain) override;
	void tick(Entity& entity, Brain& brain, double delta_seconds) override;
	void stop(Entity& entity, Brain& brain) override;

private:
	double speed_;
	std::uint32_t duration_ticks_;
	std::uint32_t remaining_ticks_{0};
};

class FlockGoal final : public Goal {
public:
	FlockGoal(std::uint32_t priority,
			  EntityManager& entities,
			  double speed,
			  double range,
			  double preferred_distance);

	[[nodiscard]] bool can_start(Entity& entity, Brain& brain) override;
	[[nodiscard]] bool can_continue(Entity& entity, Brain& brain) override;
	void tick(Entity& entity, Brain& brain, double delta_seconds) override;
	void stop(Entity& entity, Brain& brain) override;

private:
	[[nodiscard]] Entity* leader(Entity& entity, Brain& brain) const noexcept;

	EntityManager* entities_;
	double speed_;
	double range_squared_;
	double preferred_distance_squared_;
};

class TemptGoal final : public Goal {
public:
	TemptGoal(std::uint32_t priority, double speed, double stop_distance = 1.5);

	[[nodiscard]] bool can_start(Entity& entity, Brain& brain) override;
	[[nodiscard]] bool can_continue(Entity& entity, Brain& brain) override;
	void tick(Entity& entity, Brain& brain, double delta_seconds) override;
	void stop(Entity& entity, Brain& brain) override;

private:
	double speed_;
	double stop_distance_squared_;
};

class OwnerFollowGoal final : public Goal {
public:
	OwnerFollowGoal(std::uint32_t priority,
					double speed,
					double start_distance = 3.0,
					double stop_distance = 2.0);

	[[nodiscard]] bool can_start(Entity& entity, Brain& brain) override;
	[[nodiscard]] bool can_continue(Entity& entity, Brain& brain) override;
	void tick(Entity& entity, Brain& brain, double delta_seconds) override;
	void stop(Entity& entity, Brain& brain) override;

private:
	double speed_;
	double start_distance_squared_;
	double stop_distance_squared_;
};

class MobAiSystem final {
public:
	MobAiSystem(EntityManager& entities, std::uint64_t seed);

	[[nodiscard]] bool attach(EntityId entity);
	[[nodiscard]] Brain* brain(EntityId entity) noexcept;
	[[nodiscard]] bool set_target(EntityId entity, EntityId target);
	[[nodiscard]] bool notify_damage(EntityId entity, Vec3 threat);
	[[nodiscard]] bool tempt(EntityId entity, Vec3 position);
	[[nodiscard]] bool follow_owner(EntityId entity, Vec3 position);
	[[nodiscard]] bool set_suspended(EntityId entity, bool suspended);
	void tick(double delta_seconds);
	[[nodiscard]] std::size_t size() const noexcept;

private:
	struct State final {
		Brain brain;
		GoalSelector goals;
		bool suspended{false};
	};

	EntityManager* entities_;
	std::uint64_t seed_;
	std::map<EntityId, State> states_;
};

} // namespace mc::entity