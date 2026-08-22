#pragma once

#include "mc/entity/entity.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace mc::player {

enum class Difficulty {
	peaceful,
	easy,
	normal,
	hard,
};

[[nodiscard]] float scale_hostile_damage(float normal_damage,
										 Difficulty difficulty) noexcept;

enum class MovementMode {
	standing,
	sneaking,
	swimming,
	gliding,
	flying,
};

struct StatisticValue final {
	std::int32_t type_id;
	std::int32_t value_id;
	std::int32_t value;
};

struct ExperienceState final {
	std::int32_t total;
	std::int32_t level;
	float progress;
};

[[nodiscard]] ExperienceState experience_from_total(std::int32_t total) noexcept;
[[nodiscard]] std::int32_t experience_drop_on_death(std::int32_t total) noexcept;

class Statistics final {
public:
	void increment(std::int32_t type_id,
				   std::int32_t value_id,
				   std::int32_t amount = 1);
	[[nodiscard]] std::int32_t value(std::int32_t type_id,
									  std::int32_t value_id) const noexcept;
	[[nodiscard]] std::vector<StatisticValue> snapshot() const;
	[[nodiscard]] std::vector<StatisticValue> drain_updates();

private:
	std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t> values_;
	std::map<std::pair<std::int32_t, std::int32_t>, bool> dirty_;
};

class SurvivalState final {
public:
	struct Effect final {
		std::uint8_t amplifier;
		std::uint32_t duration_ticks;
		bool ambient;
		bool visible;
	};
	struct AttackResult final {
		float damage_multiplier;
		bool critical;
		bool sweeping;
	};
	struct DamageResult final {
		bool applied;
		float blocked;
	};

	[[nodiscard]] float health() const noexcept;
	[[nodiscard]] float absorption() const noexcept;
	[[nodiscard]] std::int32_t food_level() const noexcept;
	[[nodiscard]] float saturation() const noexcept;
	[[nodiscard]] float exhaustion() const noexcept;
	[[nodiscard]] std::int32_t air_ticks() const noexcept;
	[[nodiscard]] float fall_distance() const noexcept;
	[[nodiscard]] bool sprinting() const noexcept;
	[[nodiscard]] bool sneaking() const noexcept;
	[[nodiscard]] bool jumping() const noexcept;
	[[nodiscard]] bool blocking() const noexcept;
	[[nodiscard]] bool flying() const noexcept;
	[[nodiscard]] MovementMode movement_mode() const noexcept;
	[[nodiscard]] float eye_height() const noexcept;
	[[nodiscard]] float body_height() const noexcept;
	[[nodiscard]] float movement_speed_multiplier() const noexcept;
	[[nodiscard]] float attack_charge() const noexcept;
	[[nodiscard]] std::optional<entity::EntityId> combat_target() const noexcept;
	[[nodiscard]] std::uint32_t combat_ticks() const noexcept;
	[[nodiscard]] std::uint32_t hurt_invulnerability_ticks() const noexcept;
	[[nodiscard]] const std::optional<entity::DamageSource>& last_damage_source() const noexcept;
	[[nodiscard]] const std::map<std::int32_t, Effect>& effects() const noexcept;

	void tick(bool underwater, Difficulty difficulty);
	void record_movement(entity::Vec3 from,
						 entity::Vec3 to,
						 bool on_ground,
						 bool sprinting);
	void add_exhaustion(float amount);
	void set_input(bool sprinting, bool sneaking, bool jumping) noexcept;
	[[nodiscard]] bool start_gliding() noexcept;
	void set_flying(bool flying) noexcept;
	[[nodiscard]] AttackResult attack(entity::EntityId target);
	[[nodiscard]] bool apply_effect(std::int32_t effect_id, Effect effect);
	[[nodiscard]] bool remove_effect(std::int32_t effect_id) noexcept;
	[[nodiscard]] std::vector<std::int32_t> drain_expired_effects();
	void set_blocking(bool blocking) noexcept;
	[[nodiscard]] bool blocks_attack(entity::Vec3 source,
									 entity::Vec3 player_position,
									 float yaw) const noexcept;
	[[nodiscard]] bool consume_food(std::int32_t nutrition, float saturation_modifier);
	void set_armor(float armor, float toughness) noexcept;
	[[nodiscard]] bool damage(float amount) noexcept;
	[[nodiscard]] DamageResult damage(float amount, bool blockable) noexcept;
	[[nodiscard]] DamageResult damage(float amount,
								  entity::DamageSource source,
								  bool blockable = false) noexcept;
	void heal(float amount) noexcept;
	void reset() noexcept;
	[[nodiscard]] bool take_dirty() noexcept;

private:
	void apply_exhaustion();

	float health_{20.0F};
	float absorption_{0.0F};
	std::int32_t food_level_{20};
	float saturation_{5.0F};
	float exhaustion_{0.0F};
	std::int32_t air_ticks_{300};
	float fall_distance_{0.0F};
	std::uint32_t regeneration_ticks_{0};
	std::uint32_t starvation_ticks_{0};
	std::uint32_t attack_recharge_ticks_{5};
	std::uint32_t combat_ticks_{0};
	std::uint32_t hurt_invulnerability_ticks_{0};
	std::optional<entity::EntityId> combat_target_;
	std::optional<entity::DamageSource> last_damage_source_;
	std::map<std::int32_t, Effect> effects_;
	std::vector<std::int32_t> expired_effects_;
	float armor_{0.0F};
	float armor_toughness_{0.0F};
	bool was_on_ground_{true};
	bool sprinting_{false};
	bool sneaking_{false};
	bool jumping_{false};
	bool blocking_{false};
	bool underwater_{false};
	bool gliding_{false};
	bool flying_{false};
	bool dirty_{true};
};

class PlayerManager;
class PlayerSession;

} // namespace mc::player