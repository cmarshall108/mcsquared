#include "mc/entity/entity.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace mc::entity {
namespace {

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] EntityCategory infer_category(const std::string_view implementation,
                                            const std::string_view name) noexcept {
    if (implementation.find(".monster.") != std::string_view::npos ||
        implementation.find(".boss.") != std::string_view::npos ||
        name == "minecraft:creeper" || name == "minecraft:slime" ||
        name == "minecraft:ghast" || name == "minecraft:phantom") {
        return EntityCategory::monster;
    }
    if (implementation.find(".ambient.") != std::string_view::npos) {
        return EntityCategory::ambient;
    }
    if (name == "minecraft:axolotl") {
        return EntityCategory::axolotls;
    }
    if (implementation.find(".fish.") != std::string_view::npos ||
        name == "minecraft:squid" || name == "minecraft:glow_squid") {
        return EntityCategory::water_ambient;
    }
    if (name == "minecraft:dolphin" || name == "minecraft:turtle") {
        return EntityCategory::water_creature;
    }
    if (implementation.find(".animal.") != std::string_view::npos ||
        implementation.find(".npc.") != std::string_view::npos ||
        implementation.find(".player.") != std::string_view::npos) {
        return EntityCategory::creature;
    }
    return EntityCategory::misc;
}

[[nodiscard]] EntityTypeProperties default_properties(
    const EntityCategory category, const std::string_view name = {}) noexcept {
    EntityTypeProperties properties;
    properties.category = category;
    if (category == EntityCategory::monster) {
        properties.max_health = 20.0F;
        properties.movement_speed = 0.23F;
        properties.follow_range = 35.0F;
    } else if (category != EntityCategory::misc) {
        properties.max_health = 10.0F;
        properties.movement_speed = 0.20F;
        properties.persistent = category == EntityCategory::creature;
    }
    properties.aquatic = category == EntityCategory::water_creature ||
        category == EntityCategory::water_ambient ||
        category == EntityCategory::underground_water_creature ||
        category == EntityCategory::axolotls || name == "minecraft:drowned" ||
        name == "minecraft:guardian" || name == "minecraft:elder_guardian";
    if (properties.aquatic || category == EntityCategory::ambient) {
        properties.affected_by_gravity = false;
    }
    if (category == EntityCategory::monster) {
        properties.experience_reward = 5;
    } else if (category == EntityCategory::creature) {
        properties.experience_reward = 3;
    } else if (properties.aquatic || category == EntityCategory::ambient) {
        properties.experience_reward = 1;
    }
    return properties;
}

} // namespace

CategoryProperties category_properties(const EntityCategory category) noexcept {
    switch (category) {
    case EntityCategory::monster: return {70, false, false, 32, 128};
    case EntityCategory::creature: return {10, true, true, 32, 128};
    case EntityCategory::ambient: return {15, true, false, 32, 128};
    case EntityCategory::axolotls: return {5, true, false, 32, 128};
    case EntityCategory::underground_water_creature: return {5, true, false, 32, 128};
    case EntityCategory::water_creature: return {5, true, false, 32, 128};
    case EntityCategory::water_ambient: return {20, true, false, 32, 64};
    case EntityCategory::misc: return {-1, true, true, 32, 128};
    }
    return {-1, true, true, 32, 128};
}

Vec3& Vec3::operator+=(const Vec3& other) noexcept {
    x += other.x;
    y += other.y;
    z += other.z;
    return *this;
}
Vec3& Vec3::operator*=(const double scale) noexcept {
    x *= scale;
    y *= scale;
    z *= scale;
    return *this;
}
double Vec3::length_squared() const noexcept { return x * x + y * y + z * z; }
Vec3 operator+(Vec3 left, const Vec3& right) noexcept { return left += right; }
Vec3 operator-(Vec3 left, const Vec3& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}
Vec3 operator*(Vec3 value, const double scale) noexcept { return value *= scale; }

bool Aabb::intersects(const Aabb& other) const noexcept {
    return maximum.x >= other.minimum.x && minimum.x <= other.maximum.x &&
        maximum.y >= other.minimum.y && minimum.y <= other.maximum.y &&
        maximum.z >= other.minimum.z && minimum.z <= other.maximum.z;
}
bool Aabb::contains(const Vec3 point) const noexcept {
    return point.x >= minimum.x && point.x <= maximum.x &&
        point.y >= minimum.y && point.y <= maximum.y &&
        point.z >= minimum.z && point.z <= maximum.z;
}

EntityType::EntityType(const std::uint32_t id,
                       mc::core::ResourceLocation name,
                       std::string implementation,
                       const EntityTypeProperties properties)
    : id_(id), name_(std::move(name)), implementation_(std::move(implementation)),
      properties_(properties), protocol_id_(std::nullopt) {
    if (properties_.width <= 0.0F || properties_.height <= 0.0F ||
        properties_.max_health < 0.0F || properties_.movement_speed < 0.0F) {
        throw std::invalid_argument("entity type properties are invalid");
    }
}
std::uint32_t EntityType::id() const noexcept { return id_; }
const mc::core::ResourceLocation& EntityType::name() const noexcept { return name_; }
const std::string& EntityType::implementation() const noexcept { return implementation_; }
const EntityTypeProperties& EntityType::properties() const noexcept { return properties_; }
std::optional<std::uint32_t> EntityType::protocol_id() const noexcept { return protocol_id_; }

EntityTypeRegistry::EntityTypeRegistry() {
    register_type(mc::core::ResourceLocation::parse("minecraft:cow"),
                  "net.minecraft.world.entity.animal.cow.Cow",
                  default_properties(EntityCategory::creature));
    register_type(mc::core::ResourceLocation::parse("minecraft:zombie"),
                  "net.minecraft.world.entity.monster.zombie.Zombie",
                  default_properties(EntityCategory::monster));
    register_type(mc::core::ResourceLocation::parse("minecraft:bat"),
                  "net.minecraft.world.entity.ambient.Bat",
                  default_properties(EntityCategory::ambient));
    register_type(mc::core::ResourceLocation::parse("minecraft:cod"),
                  "net.minecraft.world.entity.animal.fish.Cod",
                  default_properties(EntityCategory::water_ambient));
}

const EntityType& EntityTypeRegistry::by_id(const std::uint32_t id) const {
    if (id >= types_.size()) throw std::out_of_range("entity type ID is not registered");
    return types_[id];
}
const EntityType& EntityTypeRegistry::by_name(const std::string_view name) const {
    const auto identifier = mc::core::ResourceLocation::parse(name);
    const auto found = std::find_if(types_.begin(), types_.end(), [&](const auto& type) {
        return type.name() == identifier;
    });
    if (found == types_.end()) throw std::out_of_range("entity type name is not registered");
    return *found;
}
const EntityType& EntityTypeRegistry::by_protocol_id(const std::uint32_t id) const {
    const auto found = std::find_if(types_.begin(), types_.end(), [&](const auto& type) {
        return type.protocol_id() == id;
    });
    if (found == types_.end()) throw std::out_of_range("entity protocol ID is not registered");
    return *found;
}
std::size_t EntityTypeRegistry::size() const noexcept { return types_.size(); }
const std::vector<EntityType>& EntityTypeRegistry::types() const noexcept { return types_; }

const EntityType& EntityTypeRegistry::register_type(mc::core::ResourceLocation name,
                                                    std::string implementation,
                                                    const EntityTypeProperties properties) {
    if (std::any_of(types_.begin(), types_.end(), [&](const auto& type) {
            return type.name() == name;
        })) {
        throw std::invalid_argument("entity type name is already registered");
    }
    const auto id = static_cast<std::uint32_t>(types_.size());
    types_.emplace_back(id, std::move(name), std::move(implementation), properties);
    return types_.back();
}

EntityRegistryLoadReport EntityTypeRegistry::load_normalized(std::istream& input) {
    std::string line;
    if (!std::getline(input, line) || !line.starts_with("MCREGISTRIES1\t")) {
        throw std::runtime_error("normalized registry stream has an invalid header");
    }
    EntityRegistryLoadReport report;
    while (std::getline(input, line)) {
        if (!line.starts_with("E\t")) continue;
        const auto first = line.find('\t', 2);
        const auto second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            throw std::runtime_error("normalized entity registry record is invalid");
        }
        std::uint32_t protocol_id = 0;
        const auto id_text = std::string_view(line).substr(2, first - 2);
        const auto [end, error] = std::from_chars(id_text.data(), id_text.data() + id_text.size(), protocol_id);
        if (error != std::errc{} || end != id_text.data() + id_text.size() ||
            protocol_id != report.encountered) {
            throw std::runtime_error("normalized entity protocol ID is invalid");
        }
        ++report.encountered;
        const auto name_text = std::string_view(line).substr(first + 1, second - first - 1);
        const auto implementation = std::string_view(line).substr(second + 1);
        const auto name = mc::core::ResourceLocation::parse(name_text);
        const auto existing = std::find_if(types_.begin(), types_.end(), [&](const auto& type) {
            return type.name() == name;
        });
        if (existing != types_.end()) {
            existing->protocol_id_ = protocol_id;
            ++report.existing;
        } else {
            static_cast<void>(register_type(name, std::string(implementation),
                default_properties(infer_category(implementation, name_text), name_text)));
            types_.back().protocol_id_ = protocol_id;
            ++report.loaded;
        }
    }
    return report;
}

Entity::Entity(const EntityId id, const mc::core::Uuid uuid,
               const EntityType& type, const Vec3 position)
    : type_(&type), id_(id), uuid_(uuid), position_(position) {}

void Entity::tick(const double delta_seconds) {
    if (removed_ || delta_seconds <= 0.0) return;
    if (type_->properties().affected_by_gravity && !on_ground_) {
        velocity_.y -= 0.08 * delta_seconds * 20.0;
    }
    position_ += velocity_ * (delta_seconds * 20.0);
    velocity_ *= std::pow(0.98, delta_seconds * 20.0);
    if (position_.y <= 0.0) {
        position_.y = 0.0;
        velocity_.y = 0.0;
        on_ground_ = true;
    } else {
        on_ground_ = false;
    }
    ++tick_count_;
}

EntityId Entity::id() const noexcept { return id_; }
const mc::core::Uuid& Entity::uuid() const noexcept { return uuid_; }
const EntityType& Entity::type() const noexcept { return *type_; }
Vec3 Entity::position() const noexcept { return position_; }
Vec3 Entity::velocity() const noexcept { return velocity_; }
float Entity::yaw() const noexcept { return yaw_; }
float Entity::pitch() const noexcept { return pitch_; }
bool Entity::on_ground() const noexcept { return on_ground_; }
bool Entity::removed() const noexcept { return removed_; }
std::uint64_t Entity::tick_count() const noexcept { return tick_count_; }
Aabb Entity::bounding_box() const noexcept {
    const auto half = static_cast<double>(type_->properties().width) / 2.0;
    return {{position_.x - half, position_.y, position_.z - half},
            {position_.x + half, position_.y + type_->properties().height, position_.z + half}};
}
const std::map<std::uint8_t, MetadataValue>& Entity::metadata() const noexcept { return metadata_; }
std::optional<EntityId> Entity::vehicle() const noexcept { return vehicle_; }
const std::vector<EntityId>& Entity::passengers() const noexcept { return passengers_; }
std::optional<EntityId> Entity::leash_holder() const noexcept { return leash_holder_; }
const std::string& Entity::team() const noexcept { return team_; }
void Entity::set_position(const Vec3 value) noexcept { position_ = value; }
void Entity::set_velocity(const Vec3 value) noexcept { velocity_ = value; }
void Entity::set_rotation(const float yaw, const float pitch) noexcept {
    yaw_ = yaw;
    pitch_ = std::clamp(pitch, -90.0F, 90.0F);
}
void Entity::apply_water_physics(const double drag, const double buoyancy) noexcept {
    if (!std::isfinite(drag) || !std::isfinite(buoyancy) || drag < 0.0 || drag > 1.0) return;
    velocity_.x *= drag;
    velocity_.z *= drag;
    velocity_.y = velocity_.y * drag + buoyancy;
}
void Entity::set_metadata(const std::uint8_t index, MetadataValue value) {
    metadata_[index] = std::move(value);
}
void Entity::set_team(std::string team) { team_ = std::move(team); }
void Entity::remove() noexcept { removed_ = true; }

LivingEntity::LivingEntity(const EntityId id, const mc::core::Uuid uuid,
                           const EntityType& type, const Vec3 position)
    : Entity(id, uuid, type, position), health_(type.properties().max_health) {
    attributes_.emplace(mc::core::ResourceLocation::parse("minecraft:max_health"), health_);
    attributes_.emplace(mc::core::ResourceLocation::parse("minecraft:movement_speed"),
                        type.properties().movement_speed);
    attributes_.emplace(mc::core::ResourceLocation::parse("minecraft:follow_range"),
                        type.properties().follow_range);
    attributes_.emplace(mc::core::ResourceLocation::parse("minecraft:armor"), 0.0);
    attributes_.emplace(mc::core::ResourceLocation::parse("minecraft:armor_toughness"), 0.0);
    attributes_.emplace(mc::core::ResourceLocation::parse("minecraft:knockback_resistance"), 0.0);
}
void LivingEntity::tick(const double delta_seconds) {
    Entity::tick(delta_seconds);
    const auto elapsed = static_cast<std::uint32_t>(
        std::max(0.0, std::ceil(delta_seconds * 20.0)));
    hurt_invulnerability_ticks_ = elapsed >= hurt_invulnerability_ticks_
        ? 0
        : hurt_invulnerability_ticks_ - elapsed;
    for (const auto& [effect, instance] : effects_) {
        const auto interval = [&](const std::uint32_t base) {
            return std::max<std::uint32_t>(1, base >> std::min<std::uint8_t>(instance.amplifier, 6));
        };
        if (effect == mc::core::ResourceLocation::parse("minecraft:regeneration") &&
            tick_count() % interval(50) == 0) {
            heal(1.0F);
        } else if (effect == mc::core::ResourceLocation::parse("minecraft:poison") &&
                   tick_count() % interval(25) == 0 && health_ > 1.0F) {
            static_cast<void>(damage(
                std::min(1.0F, health_ - 1.0F),
                {DamageType::generic, std::nullopt, true, true}));
        } else if (effect == mc::core::ResourceLocation::parse("minecraft:wither") &&
                   tick_count() % interval(40) == 0) {
            static_cast<void>(damage(
                1.0F, {DamageType::generic, std::nullopt, true, true}));
        }
    }
    std::erase_if(effects_, [&](auto& entry) {
        auto& duration = entry.second.duration_ticks;
        duration = elapsed >= duration ? 0 : duration - elapsed;
        return duration == 0;
    });
}
float LivingEntity::health() const noexcept { return health_; }
float LivingEntity::max_health() const noexcept { return type_->properties().max_health; }
bool LivingEntity::alive() const noexcept { return health_ > 0.0F && !removed(); }
double LivingEntity::attribute(const std::string_view name) const {
    const auto found = attributes_.find(mc::core::ResourceLocation::parse(name));
    if (found == attributes_.end()) throw std::out_of_range("entity attribute is not present");
    return found->second;
}
const mc::item::ItemStack& LivingEntity::equipment(const EquipmentSlot slot) const {
    static const mc::item::ItemStack empty;
    const auto found = equipment_.find(slot);
    return found == equipment_.end() ? empty : found->second;
}
const std::map<mc::core::ResourceLocation, LivingEntity::StatusEffect>&
LivingEntity::effects() const noexcept {
    return effects_;
}
const std::optional<DamageSource>& LivingEntity::last_damage_source() const noexcept {
    return last_damage_source_;
}
std::uint32_t LivingEntity::hurt_invulnerability_ticks() const noexcept {
    return hurt_invulnerability_ticks_;
}
double LivingEntity::movement_speed() const {
    auto speed = attribute("minecraft:movement_speed");
    if (const auto found = effects_.find(mc::core::ResourceLocation::parse("minecraft:speed"));
        found != effects_.end()) {
        speed *= 1.0 + 0.2 * static_cast<double>(found->second.amplifier + 1U);
    }
    if (const auto found = effects_.find(mc::core::ResourceLocation::parse("minecraft:slowness"));
        found != effects_.end()) {
        speed *= std::max(0.0, 1.0 - 0.15 * static_cast<double>(found->second.amplifier + 1U));
    }
    return speed;
}
bool LivingEntity::damage(const float amount) {
    return damage(amount, {});
}
bool LivingEntity::damage(const float amount, DamageSource source) {
    if (!std::isfinite(amount) || amount <= 0.0F || !alive()) return false;
    if (source.type == DamageType::fire &&
        effects_.contains(mc::core::ResourceLocation::parse("minecraft:fire_resistance"))) {
        return false;
    }
    if (hurt_invulnerability_ticks_ > 0 && !source.bypasses_invulnerability) return false;
    auto applied_amount = amount;
    if (!source.bypasses_armor) {
        const auto armor = attribute("minecraft:armor");
        const auto toughness = attribute("minecraft:armor_toughness");
        const auto effective_armor = std::min(
            20.0,
            std::max(armor / 5.0,
                     armor - static_cast<double>(amount) / (2.0 + toughness / 4.0)));
        applied_amount = static_cast<float>(
            static_cast<double>(amount) * (1.0 - effective_armor / 25.0));
    }
    health_ = std::max(0.0F, health_ - applied_amount);
    last_damage_source_ = std::move(source);
    if (!last_damage_source_->bypasses_invulnerability) {
        hurt_invulnerability_ticks_ = 10;
    }
    if (health_ == 0.0F) remove();
    return true;
}
bool LivingEntity::knockback(const Vec3 source, const double strength) noexcept {
    if (!alive() || !std::isfinite(source.x) || !std::isfinite(source.y) ||
        !std::isfinite(source.z) || !std::isfinite(strength) || strength <= 0.0) {
        return false;
    }
    const auto resistance = std::clamp(
        attributes_.at(mc::core::ResourceLocation::parse("minecraft:knockback_resistance")),
        0.0, 1.0);
    const auto applied_strength = strength * (1.0 - resistance);
    if (applied_strength <= 0.0) return false;
    const auto offset_x = position().x - source.x;
    const auto offset_z = position().z - source.z;
    const auto horizontal_squared = offset_x * offset_x + offset_z * offset_z;
    if (horizontal_squared <= 1.0e-12) return false;

    const auto horizontal = std::sqrt(horizontal_squared);
    auto movement = velocity();
    movement.x = movement.x * 0.5 + offset_x / horizontal * applied_strength;
    movement.z = movement.z * 0.5 + offset_z / horizontal * applied_strength;
    movement.y = std::min(0.4, movement.y * 0.5 + applied_strength);
    set_velocity(movement);
    return true;
}
void LivingEntity::heal(const float amount) noexcept {
    if (amount > 0.0F && alive()) health_ = std::min(max_health(), health_ + amount);
}
void LivingEntity::set_attribute(mc::core::ResourceLocation name, const double value) {
    if (!std::isfinite(value) || value < 0.0) throw std::invalid_argument("invalid attribute");
    attributes_[std::move(name)] = value;
}
void LivingEntity::equip(const EquipmentSlot slot,
                         mc::item::ItemStack stack,
                         const mc::item::ItemRegistry& registry) {
    stack.validate(registry);
    equipment_[slot] = std::move(stack);
}
void LivingEntity::apply_effect(mc::core::ResourceLocation effect, const StatusEffect instance) {
    if (instance.duration_ticks == 0) {
        effects_.erase(effect);
        return;
    }
    const auto found = effects_.find(effect);
    if (found == effects_.end() || instance.amplifier > found->second.amplifier ||
        (instance.amplifier == found->second.amplifier &&
         instance.duration_ticks >= found->second.duration_ticks)) {
        effects_[std::move(effect)] = instance;
    }
}
bool LivingEntity::remove_effect(const mc::core::ResourceLocation& effect) noexcept {
    return effects_.erase(effect) != 0;
}

EntityManager::EntityManager(const EntityTypeRegistry& registry,
                             const std::uint64_t seed,
                             const EntityId first_entity_id)
    : registry_(&registry), next_id_(first_entity_id), uuid_seed_(seed) {
    if (first_entity_id == 0) throw std::invalid_argument("first entity ID must be positive");
}
Entity& EntityManager::spawn(const std::string_view name, const Vec3 position) {
    const auto& type = registry_->by_name(name);
    std::unique_ptr<Entity> entity;
    if (type.properties().max_health > 0.0F) {
        entity = std::make_unique<LivingEntity>(next_id_, next_uuid(), type, position);
    } else {
        entity = std::make_unique<Entity>(next_id_, next_uuid(), type, position);
    }
    auto& result = *entity;
    entities_.emplace(next_id_++, std::move(entity));
    return result;
}
Entity* EntityManager::find(const EntityId id) noexcept {
    const auto found = entities_.find(id);
    return found == entities_.end() ? nullptr : found->second.get();
}
const Entity* EntityManager::find(const EntityId id) const noexcept {
    const auto found = entities_.find(id);
    return found == entities_.end() ? nullptr : found->second.get();
}
bool EntityManager::remove(const EntityId id) {
    auto* entity = find(id);
    if (entity == nullptr) return false;
    static_cast<void>(dismount(id));
    for (const auto passenger : std::vector(entity->passengers_)) {
        static_cast<void>(dismount(passenger));
    }
    for (auto& [other_id, other] : entities_) {
        static_cast<void>(other_id);
        if (other->leash_holder_ == id) other->leash_holder_.reset();
    }
    return entities_.erase(id) != 0;
}

bool EntityManager::mount(const EntityId passenger_id, const EntityId vehicle_id) {
    auto* passenger = find(passenger_id);
    auto* vehicle = find(vehicle_id);
    if (passenger == nullptr || vehicle == nullptr || passenger_id == vehicle_id) return false;
    for (auto* cursor = vehicle; cursor != nullptr && cursor->vehicle_;
         cursor = find(*cursor->vehicle_)) {
        if (*cursor->vehicle_ == passenger_id) return false;
    }
    static_cast<void>(dismount(passenger_id));
    passenger->vehicle_ = vehicle_id;
    vehicle->passengers_.push_back(passenger_id);
    return true;
}

bool EntityManager::dismount(const EntityId passenger_id) {
    auto* passenger = find(passenger_id);
    if (passenger == nullptr || !passenger->vehicle_) return false;
    if (auto* vehicle = find(*passenger->vehicle_)) {
        std::erase(vehicle->passengers_, passenger_id);
    }
    passenger->vehicle_.reset();
    return true;
}

bool EntityManager::add_external_passenger(const EntityId vehicle_id,
                                           const EntityId passenger_id) {
    auto* vehicle = find(vehicle_id);
    if (!vehicle || passenger_id == 0 || find(passenger_id) != nullptr ||
        vehicle_id == passenger_id) {
        return false;
    }
    for (auto& [id, entity] : entities_) {
        static_cast<void>(id);
        std::erase(entity->passengers_, passenger_id);
    }
    vehicle->passengers_.push_back(passenger_id);
    return true;
}

bool EntityManager::remove_external_passenger(const EntityId vehicle_id,
                                              const EntityId passenger_id) {
    auto* vehicle = find(vehicle_id);
    if (!vehicle || find(passenger_id) != nullptr) return false;
    return std::erase(vehicle->passengers_, passenger_id) != 0;
}

bool EntityManager::set_leash(const EntityId id, const std::optional<EntityId> holder) {
    auto* entity = find(id);
    if (entity == nullptr || (holder && (*holder == id || find(*holder) == nullptr))) return false;
    entity->leash_holder_ = holder;
    return true;
}
void EntityManager::tick(const double delta_seconds) {
    for (auto& [id, entity] : entities_) {
        static_cast<void>(id);
        entity->tick(delta_seconds);
    }
    std::vector<EntityId> removed;
    for (const auto& [id, entity] : entities_) {
        if (!entity->removed()) continue;
        if (const auto* living = dynamic_cast<const LivingEntity*>(entity.get());
            living && !living->alive() && entity->type().properties().experience_reward > 0) {
            deaths_.push_back({id, entity->position(), entity->type().name().path(),
                               entity->type().properties().experience_reward});
        }
        removed.push_back(id);
    }
    for (const auto id : removed) static_cast<void>(remove(id));
}
std::vector<EntityId> EntityManager::query(const Aabb bounds) const {
    std::vector<EntityId> result;
    for (const auto& [id, entity] : entities_) {
        if (entity->bounding_box().intersects(bounds)) result.push_back(id);
    }
    return result;
}
std::size_t EntityManager::size() const noexcept { return entities_.size(); }
std::size_t EntityManager::count(const EntityCategory category) const noexcept {
    return static_cast<std::size_t>(std::count_if(entities_.begin(), entities_.end(),
        [&](const auto& entry) { return entry.second->type().properties().category == category; }));
}
std::vector<EntityId> EntityManager::ids() const {
    std::vector<EntityId> result;
    result.reserve(entities_.size());
    for (const auto& [id, entity] : entities_) {
        static_cast<void>(entity);
        result.push_back(id);
    }
    return result;
}

std::vector<DeathEvent> EntityManager::drain_deaths() {
    auto result = std::move(deaths_);
    deaths_.clear();
    return result;
}

std::vector<EntitySnapshot> EntityManager::snapshots() const {
    std::vector<EntitySnapshot> result;
    result.reserve(entities_.size());
    for (const auto& [id, entity] : entities_) {
        std::optional<float> health;
        if (const auto* living = dynamic_cast<const LivingEntity*>(entity.get())) {
            health = living->health();
        }
        result.push_back({
            id,
            entity->uuid(),
            entity->type().name().to_string(),
            entity->position(),
            entity->velocity(),
            entity->yaw(),
            entity->pitch(),
            health,
            entity->metadata(),
            entity->team(),
            entity->vehicle(),
            entity->leash_holder(),
        });
    }
    return result;
}

void EntityManager::restore(const std::vector<EntitySnapshot>& snapshots_value) {
    if (!entities_.empty()) {
        throw std::logic_error("entity snapshots require an empty manager");
    }
    for (const auto& snapshot : snapshots_value) {
        if (snapshot.id == 0 || entities_.contains(snapshot.id)) {
            throw std::invalid_argument("entity snapshot ID is invalid or duplicated");
        }
        const auto& type = registry_->by_name(snapshot.type);
        std::unique_ptr<Entity> entity;
        if (type.properties().max_health > 0.0F) {
            auto living = std::make_unique<LivingEntity>(snapshot.id, snapshot.uuid, type,
                                                         snapshot.position);
            if (!snapshot.health || *snapshot.health < 0.0F ||
                *snapshot.health > living->max_health()) {
                throw std::invalid_argument("living entity snapshot health is invalid");
            }
            living->health_ = *snapshot.health;
            entity = std::move(living);
        } else {
            if (snapshot.health) {
                throw std::invalid_argument("non-living entity snapshot contains health");
            }
            entity = std::make_unique<Entity>(snapshot.id, snapshot.uuid, type, snapshot.position);
        }
        entity->set_velocity(snapshot.velocity);
        entity->set_rotation(snapshot.yaw, snapshot.pitch);
        entity->metadata_ = snapshot.metadata;
        entity->team_ = snapshot.team;
        entities_.emplace(snapshot.id, std::move(entity));
        next_id_ = std::max(next_id_, static_cast<EntityId>(snapshot.id + 1));
    }
    for (const auto& snapshot : snapshots_value) {
        if (snapshot.vehicle && !mount(snapshot.id, *snapshot.vehicle)) {
            throw std::invalid_argument("entity snapshot vehicle relationship is invalid");
        }
        if (snapshot.leash_holder && !set_leash(snapshot.id, snapshot.leash_holder)) {
            throw std::invalid_argument("entity snapshot leash relationship is invalid");
        }
    }
}
mc::core::Uuid EntityManager::next_uuid() {
    mc::core::Uuid uuid{};
    const auto first = mix(uuid_seed_ ^ uuid_counter_++);
    const auto second = mix(first);
    for (std::size_t index = 0; index < 8; ++index) {
        uuid[index] = static_cast<std::uint8_t>(first >> (56U - index * 8U));
        uuid[index + 8] = static_cast<std::uint8_t>(second >> (56U - index * 8U));
    }
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0FU) | 0x40U);
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3FU) | 0x80U);
    return uuid;
}

} // namespace mc::entity