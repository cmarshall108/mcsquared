#pragma once

#include "mc/core/types.hpp"
#include "mc/item/item.hpp"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mc::entity {

using EntityId = std::uint32_t;
using MetadataValue = std::variant<bool, std::int32_t, float, std::string>;

struct Vec3 final {
	double x{0.0};
	double y{0.0};
	double z{0.0};

	Vec3& operator+=(const Vec3& other) noexcept;
	Vec3& operator*=(double scale) noexcept;
	[[nodiscard]] double length_squared() const noexcept;
	auto operator<=>(const Vec3&) const = default;
};

[[nodiscard]] Vec3 operator+(Vec3 left, const Vec3& right) noexcept;
[[nodiscard]] Vec3 operator-(Vec3 left, const Vec3& right) noexcept;
[[nodiscard]] Vec3 operator*(Vec3 value, double scale) noexcept;

struct Aabb final {
	Vec3 minimum;
	Vec3 maximum;

	[[nodiscard]] bool intersects(const Aabb& other) const noexcept;
	[[nodiscard]] bool contains(Vec3 point) const noexcept;
};

enum class EntityCategory {
	monster,
	creature,
	ambient,
	axolotls,
	underground_water_creature,
	water_creature,
	water_ambient,
	misc,
};

enum class DamageType {
	generic,
	melee,
	projectile,
	fire,
	fall,
	drowning,
	poison,
	wither,
	void_damage,
};

struct DamageSource final {
	DamageType type{DamageType::generic};
	std::optional<EntityId> attacker;
	bool bypasses_invulnerability{false};
	bool bypasses_armor{false};
};

struct EntityTypeProperties final {
	EntityCategory category{EntityCategory::misc};
	float width{0.6F};
	float height{1.8F};
	float max_health{0.0F};
	float movement_speed{0.1F};
	float follow_range{16.0F};
	bool affected_by_gravity{true};
	bool persistent{false};
	bool aquatic{false};
	std::uint16_t experience_reward{0};
};

struct CategoryProperties final {
	std::int32_t maximum;
	bool friendly;
	bool persistent;
	std::int32_t no_despawn_distance;
	std::int32_t despawn_distance;
};

[[nodiscard]] CategoryProperties category_properties(EntityCategory category) noexcept;

class EntityType final {
public:
	EntityType(std::uint32_t id,
			   mc::core::ResourceLocation name,
			   std::string implementation,
			   EntityTypeProperties properties);

	[[nodiscard]] std::uint32_t id() const noexcept;
	[[nodiscard]] const mc::core::ResourceLocation& name() const noexcept;
	[[nodiscard]] const std::string& implementation() const noexcept;
	[[nodiscard]] const EntityTypeProperties& properties() const noexcept;
	[[nodiscard]] std::optional<std::uint32_t> protocol_id() const noexcept;

private:
	friend class EntityTypeRegistry;
	std::uint32_t id_;
	mc::core::ResourceLocation name_;
	std::string implementation_;
	EntityTypeProperties properties_;
	std::optional<std::uint32_t> protocol_id_;
};

struct EntityRegistryLoadReport final {
	std::size_t encountered{0};
	std::size_t loaded{0};
	std::size_t existing{0};
};

class EntityTypeRegistry final {
public:
	EntityTypeRegistry();

	[[nodiscard]] const EntityType& by_id(std::uint32_t id) const;
	[[nodiscard]] const EntityType& by_name(std::string_view name) const;
	[[nodiscard]] const EntityType& by_protocol_id(std::uint32_t id) const;
	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] const std::vector<EntityType>& types() const noexcept;
	[[nodiscard]] EntityRegistryLoadReport load_normalized(std::istream& input);
	const EntityType& register_type(mc::core::ResourceLocation name,
									std::string implementation,
									EntityTypeProperties properties);

private:
	std::vector<EntityType> types_;
};

class Entity {
public:
	Entity(EntityId id, mc::core::Uuid uuid, const EntityType& type, Vec3 position);
	virtual ~Entity() = default;

	Entity(const Entity&) = delete;
	Entity& operator=(const Entity&) = delete;
	virtual void tick(double delta_seconds);

	[[nodiscard]] EntityId id() const noexcept;
	[[nodiscard]] const mc::core::Uuid& uuid() const noexcept;
	[[nodiscard]] const EntityType& type() const noexcept;
	[[nodiscard]] Vec3 position() const noexcept;
	[[nodiscard]] Vec3 velocity() const noexcept;
	[[nodiscard]] float yaw() const noexcept;
	[[nodiscard]] float pitch() const noexcept;
	[[nodiscard]] bool on_ground() const noexcept;
	[[nodiscard]] bool removed() const noexcept;
	[[nodiscard]] std::uint64_t tick_count() const noexcept;
	[[nodiscard]] Aabb bounding_box() const noexcept;
	[[nodiscard]] const std::map<std::uint8_t, MetadataValue>& metadata() const noexcept;
	[[nodiscard]] std::optional<EntityId> vehicle() const noexcept;
	[[nodiscard]] const std::vector<EntityId>& passengers() const noexcept;
	[[nodiscard]] std::optional<EntityId> leash_holder() const noexcept;
	[[nodiscard]] const std::string& team() const noexcept;

	void set_position(Vec3 position) noexcept;
	void set_velocity(Vec3 velocity) noexcept;
	void set_rotation(float yaw, float pitch) noexcept;
	void apply_water_physics(double drag, double buoyancy) noexcept;
	void set_metadata(std::uint8_t index, MetadataValue value);
	void set_team(std::string team);
	void remove() noexcept;

protected:
	const EntityType* type_;

private:
	friend class EntityManager;
	EntityId id_;
	mc::core::Uuid uuid_;
	Vec3 position_;
	Vec3 velocity_;
	float yaw_{0.0F};
	float pitch_{0.0F};
	bool on_ground_{false};
	bool removed_{false};
	std::uint64_t tick_count_{0};
	std::map<std::uint8_t, MetadataValue> metadata_;
	std::optional<EntityId> vehicle_;
	std::vector<EntityId> passengers_;
	std::optional<EntityId> leash_holder_;
	std::string team_;
};

class LivingEntity : public Entity {
public:
	LivingEntity(EntityId id,
				 mc::core::Uuid uuid,
				 const EntityType& type,
				 Vec3 position);

	enum class EquipmentSlot {
		main_hand,
		off_hand,
		feet,
		legs,
		chest,
		head,
		body,
	};

	struct StatusEffect final {
		std::uint8_t amplifier{0};
		std::uint32_t duration_ticks{0};
		bool ambient{false};
		bool visible{true};
	};

	void tick(double delta_seconds) override;

	[[nodiscard]] float health() const noexcept;
	[[nodiscard]] float max_health() const noexcept;
	[[nodiscard]] bool alive() const noexcept;
	[[nodiscard]] double attribute(std::string_view name) const;
	[[nodiscard]] const mc::item::ItemStack& equipment(EquipmentSlot slot) const;
	[[nodiscard]] const std::map<mc::core::ResourceLocation, StatusEffect>& effects() const noexcept;
	[[nodiscard]] const std::optional<DamageSource>& last_damage_source() const noexcept;
	[[nodiscard]] std::uint32_t hurt_invulnerability_ticks() const noexcept;
	[[nodiscard]] double movement_speed() const;
	[[nodiscard]] bool damage(float amount);
	[[nodiscard]] bool damage(float amount, DamageSource source);
	[[nodiscard]] bool knockback(Vec3 source, double strength) noexcept;
	void heal(float amount) noexcept;
	void set_attribute(mc::core::ResourceLocation name, double value);
	void equip(EquipmentSlot slot,
			   mc::item::ItemStack stack,
			   const mc::item::ItemRegistry& registry);
	void apply_effect(mc::core::ResourceLocation effect, StatusEffect instance);
	[[nodiscard]] bool remove_effect(const mc::core::ResourceLocation& effect) noexcept;

private:
	friend class EntityManager;
	float health_;
	std::map<mc::core::ResourceLocation, double> attributes_;
	std::map<EquipmentSlot, mc::item::ItemStack> equipment_;
	std::map<mc::core::ResourceLocation, StatusEffect> effects_;
	std::optional<DamageSource> last_damage_source_;
	std::uint32_t hurt_invulnerability_ticks_{0};
};

struct EntitySnapshot final {
	EntityId id;
	mc::core::Uuid uuid;
	std::string type;
	Vec3 position;
	Vec3 velocity;
	float yaw;
	float pitch;
	std::optional<float> health;
	std::map<std::uint8_t, MetadataValue> metadata;
	std::string team;
	std::optional<EntityId> vehicle;
	std::optional<EntityId> leash_holder;
};

struct DeathEvent final {
	EntityId entity_id;
	Vec3 position;
	std::string type;
	std::uint16_t experience;
};

class EntityManager final {
public:
	explicit EntityManager(const EntityTypeRegistry& registry,
						   std::uint64_t uuid_seed = 0,
						   EntityId first_entity_id = 1);

	Entity& spawn(std::string_view type, Vec3 position);
	[[nodiscard]] Entity* find(EntityId id) noexcept;
	[[nodiscard]] const Entity* find(EntityId id) const noexcept;
	[[nodiscard]] bool remove(EntityId id);
	[[nodiscard]] bool mount(EntityId passenger, EntityId vehicle);
	[[nodiscard]] bool dismount(EntityId passenger);
	[[nodiscard]] bool add_external_passenger(EntityId vehicle, EntityId passenger);
	[[nodiscard]] bool remove_external_passenger(EntityId vehicle, EntityId passenger);
	[[nodiscard]] bool set_leash(EntityId entity, std::optional<EntityId> holder);
	void tick(double delta_seconds);
	[[nodiscard]] std::vector<EntityId> query(Aabb bounds) const;
	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] std::size_t count(EntityCategory category) const noexcept;
	[[nodiscard]] std::vector<EntityId> ids() const;
	[[nodiscard]] std::vector<DeathEvent> drain_deaths();
	[[nodiscard]] std::vector<EntitySnapshot> snapshots() const;
	void restore(const std::vector<EntitySnapshot>& snapshots);

private:
	[[nodiscard]] mc::core::Uuid next_uuid();

	const EntityTypeRegistry* registry_;
	EntityId next_id_;
	std::uint64_t uuid_seed_;
	std::uint64_t uuid_counter_{0};
	std::map<EntityId, std::unique_ptr<Entity>> entities_;
	std::vector<DeathEvent> deaths_;
};

} // namespace mc::entity