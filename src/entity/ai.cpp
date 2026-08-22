#include "mc/entity/ai.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
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

[[nodiscard]] std::int32_t distance(const mc::core::BlockPosition left,
                                    const mc::core::BlockPosition right) noexcept {
    return std::abs(left.x - right.x) + std::abs(left.y - right.y) +
        std::abs(left.z - right.z);
}

[[nodiscard]] double adjusted_speed(Entity& entity, const double configured) {
    const auto* living = dynamic_cast<const LivingEntity*>(&entity);
    const auto base = static_cast<double>(entity.type().properties().movement_speed);
    return living && base > 0.0 ? configured * living->movement_speed() / base : configured;
}

} // namespace

void Brain::set(const MemoryKey key, MemoryValue value) {
    memories_[key] = std::move(value);
}
const MemoryValue* Brain::get(const MemoryKey key) const noexcept {
    const auto found = memories_.find(key);
    return found == memories_.end() ? nullptr : &found->second;
}
bool Brain::contains(const MemoryKey key) const noexcept { return memories_.contains(key); }
void Brain::erase(const MemoryKey key) noexcept { memories_.erase(key); }
void Brain::clear() noexcept { memories_.clear(); }

GoalControl operator|(const GoalControl left, const GoalControl right) noexcept {
    return static_cast<GoalControl>(
        static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}
bool controls_overlap(const GoalControl left, const GoalControl right) noexcept {
    return (static_cast<std::uint8_t>(left) & static_cast<std::uint8_t>(right)) != 0;
}

Goal::Goal(const std::uint32_t priority, const GoalControl controls)
    : priority_(priority), controls_(controls) {}
std::uint32_t Goal::priority() const noexcept { return priority_; }
GoalControl Goal::controls() const noexcept { return controls_; }
bool Goal::running() const noexcept { return running_; }
bool Goal::can_continue(Entity& entity, Brain& brain) { return can_start(entity, brain); }
void Goal::start(Entity&, Brain&) {}
void Goal::stop(Entity&, Brain&) {}

void GoalSelector::add(std::unique_ptr<Goal> goal) {
    if (!goal) throw std::invalid_argument("goal must not be null");
    goals_.push_back(std::move(goal));
    std::stable_sort(goals_.begin(), goals_.end(), [](const auto& left, const auto& right) {
        return left->priority() < right->priority();
    });
}

void GoalSelector::tick(Entity& entity, Brain& brain, const double delta_seconds) {
    for (auto& goal : goals_) {
        if (goal->running_ && !goal->can_continue(entity, brain)) {
            goal->stop(entity, brain);
            goal->running_ = false;
        }
    }
    for (auto& candidate : goals_) {
        if (candidate->running_ || !candidate->can_start(entity, brain)) continue;
        auto blocked = false;
        for (const auto& active : goals_) {
            if (active->running_ && controls_overlap(active->controls(), candidate->controls()) &&
                active->priority() <= candidate->priority()) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;
        for (auto& active : goals_) {
            if (active->running_ && controls_overlap(active->controls(), candidate->controls())) {
                active->stop(entity, brain);
                active->running_ = false;
            }
        }
        candidate->start(entity, brain);
        candidate->running_ = true;
    }
    for (auto& goal : goals_) {
        if (goal->running_) goal->tick(entity, brain, delta_seconds);
    }
}

void GoalSelector::stop_all(Entity& entity, Brain& brain) {
    for (auto& goal : goals_) {
        if (goal->running_) {
            goal->stop(entity, brain);
            goal->running_ = false;
        }
    }
}
std::size_t GoalSelector::size() const noexcept { return goals_.size(); }
std::size_t GoalSelector::running_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        goals_.begin(), goals_.end(), [](const auto& goal) { return goal->running(); }));
}

std::vector<mc::core::BlockPosition> Navigation::find_path(
    const mc::core::BlockPosition start,
    const mc::core::BlockPosition destination,
    const Passable& passable,
    const std::size_t max_visited) const {
    if (!passable || max_visited == 0 || !passable(start) || !passable(destination)) return {};
    struct OpenNode final {
        mc::core::BlockPosition position;
        std::int32_t score;
        bool operator>(const OpenNode& other) const noexcept { return score > other.score; }
    };
    std::priority_queue<OpenNode, std::vector<OpenNode>, std::greater<>> open;
    std::map<mc::core::BlockPosition, std::int32_t> cost;
    std::map<mc::core::BlockPosition, mc::core::BlockPosition> previous;
    open.push({start, distance(start, destination)});
    cost[start] = 0;
    std::size_t visited = 0;
    constexpr std::array<std::pair<std::int32_t, std::int32_t>, 4> directions{
        std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}};
    while (!open.empty() && visited++ < max_visited) {
        const auto current = open.top().position;
        open.pop();
        if (current == destination) {
            std::vector<mc::core::BlockPosition> path{current};
            auto cursor = current;
            while (cursor != start) {
                cursor = previous.at(cursor);
                path.push_back(cursor);
            }
            std::reverse(path.begin(), path.end());
            return path;
        }
        for (const auto [dx, dz] : directions) {
            const mc::core::BlockPosition next{current.x + dx, current.y, current.z + dz};
            if (!passable(next)) continue;
            const auto next_cost = cost[current] + 1;
            const auto old = cost.find(next);
            if (old == cost.end() || next_cost < old->second) {
                cost[next] = next_cost;
                previous[next] = current;
                open.push({next, next_cost + distance(next, destination)});
            }
        }
    }
    return {};
}

bool Navigation::steer(Entity& entity,
                       const std::vector<mc::core::BlockPosition>& path,
                       const double speed) const {
    if (path.size() < 2 || speed <= 0.0) return false;
    const auto target = path[1];
    const Vec3 delta{static_cast<double>(target.x) + 0.5 - entity.position().x,
                     0.0,
                     static_cast<double>(target.z) + 0.5 - entity.position().z};
    const auto length = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    if (length <= std::numeric_limits<double>::epsilon()) return false;
    auto velocity = entity.velocity();
    velocity.x = delta.x / length * speed;
    velocity.z = delta.z / length * speed;
    entity.set_velocity(velocity);
    return true;
}

WanderGoal::WanderGoal(const std::uint32_t priority,
                       const std::uint64_t seed,
                       const std::uint32_t interval_ticks)
    : Goal(priority, GoalControl::move), seed_(seed), interval_ticks_(interval_ticks) {
    if (interval_ticks_ == 0) throw std::invalid_argument("wander interval must be positive");
}
bool WanderGoal::can_start(Entity& entity, Brain&) {
    return entity.tick_count() % interval_ticks_ == 0;
}
bool WanderGoal::can_continue(Entity&, Brain&) { return remaining_ticks_ > 0; }
void WanderGoal::start(Entity& entity, Brain&) {
    const auto value = mix(seed_ ^ entity.tick_count());
    const auto dx = static_cast<double>(static_cast<std::int32_t>(value & 0xFFFFU) - 32'768);
    const auto dz = static_cast<double>(static_cast<std::int32_t>((value >> 16U) & 0xFFFFU) - 32'768);
    const auto length = std::max(1.0, std::sqrt(dx * dx + dz * dz));
    direction_ = {dx / length, 0.0, dz / length};
    remaining_ticks_ = 20;
}
void WanderGoal::tick(Entity& entity, Brain&, double) {
    auto velocity = entity.velocity();
    const auto speed = adjusted_speed(entity, entity.type().properties().movement_speed);
    velocity.x = direction_.x * speed;
    velocity.z = direction_.z * speed;
    entity.set_velocity(velocity);
    if (remaining_ticks_ > 0) --remaining_ticks_;
}
void WanderGoal::stop(Entity& entity, Brain&) {
    auto velocity = entity.velocity();
    velocity.x = 0.0;
    velocity.z = 0.0;
    entity.set_velocity(velocity);
    remaining_ticks_ = 0;
}

MeleeAttackGoal::MeleeAttackGoal(const std::uint32_t priority,
                                 EntityManager& entities,
                                 const float damage,
                                 const double speed)
    : Goal(priority, GoalControl::move | GoalControl::look | GoalControl::target),
      entities_(&entities), damage_(damage), speed_(speed) {
    if (damage_ <= 0.0F || speed_ <= 0.0) throw std::invalid_argument("invalid melee goal");
}
LivingEntity* MeleeAttackGoal::target(Brain& brain) const noexcept {
    const auto* memory = brain.get(MemoryKey::target_entity);
    const auto* id = memory ? std::get_if<EntityId>(memory) : nullptr;
    return id ? dynamic_cast<LivingEntity*>(entities_->find(*id)) : nullptr;
}
bool MeleeAttackGoal::can_start(Entity&, Brain& brain) {
    const auto* value = target(brain);
    return value != nullptr && value->alive();
}
bool MeleeAttackGoal::can_continue(Entity& entity, Brain& brain) {
    const auto* value = target(brain);
    return value != nullptr && value->alive() &&
        (value->position() - entity.position()).length_squared() <=
            entity.type().properties().follow_range * entity.type().properties().follow_range;
}
void MeleeAttackGoal::tick(Entity& entity, Brain& brain, double) {
    auto* value = target(brain);
    if (!value) return;
    const auto delta = value->position() - entity.position();
    const auto horizontal = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    if (horizontal > 0.001) {
        auto velocity = entity.velocity();
        const auto speed = adjusted_speed(entity, speed_);
        velocity.x = delta.x / horizontal * speed;
        velocity.z = delta.z / horizontal * speed;
        entity.set_velocity(velocity);
        entity.set_rotation(static_cast<float>(std::atan2(-delta.x, delta.z) * 180.0 / 3.141592653589793),
                            entity.pitch());
    }
    if (cooldown_ > 0) --cooldown_;
    if (delta.length_squared() <= 2.25 && cooldown_ == 0) {
        if (value->damage(damage_, {DamageType::melee, entity.id(), false})) {
            static_cast<void>(value->knockback(entity.position(), 0.4));
        }
        cooldown_ = 20;
    }
}
void MeleeAttackGoal::stop(Entity& entity, Brain&) {
    auto velocity = entity.velocity();
    velocity.x = 0.0;
    velocity.z = 0.0;
    entity.set_velocity(velocity);
}

PanicGoal::PanicGoal(const std::uint32_t priority,
                     const double speed,
                     const std::uint32_t duration_ticks)
    : Goal(priority, GoalControl::move | GoalControl::look),
      speed_(speed), duration_ticks_(duration_ticks) {
    if (speed_ <= 0.0 || duration_ticks_ == 0) {
        throw std::invalid_argument("invalid panic goal");
    }
}
bool PanicGoal::can_start(Entity&, Brain& brain) {
    return brain.contains(MemoryKey::panic) && brain.contains(MemoryKey::last_seen_position);
}
bool PanicGoal::can_continue(Entity&, Brain& brain) {
    return remaining_ticks_ > 0 && brain.contains(MemoryKey::last_seen_position);
}
void PanicGoal::start(Entity&, Brain&) { remaining_ticks_ = duration_ticks_; }
void PanicGoal::tick(Entity& entity, Brain& brain, double) {
    const auto* memory = brain.get(MemoryKey::last_seen_position);
    const auto* threat = memory ? std::get_if<Vec3>(memory) : nullptr;
    if (!threat) return;
    const auto delta = entity.position() - *threat;
    const auto horizontal = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    if (horizontal > 0.001) {
        auto velocity = entity.velocity();
        const auto speed = adjusted_speed(entity, speed_);
        velocity.x = delta.x / horizontal * speed;
        velocity.z = delta.z / horizontal * speed;
        entity.set_velocity(velocity);
        entity.set_rotation(static_cast<float>(
            std::atan2(-velocity.x, velocity.z) * 180.0 / 3.141592653589793),
            entity.pitch());
    }
    if (remaining_ticks_ > 0) --remaining_ticks_;
}
void PanicGoal::stop(Entity& entity, Brain& brain) {
    auto velocity = entity.velocity();
    velocity.x = 0.0;
    velocity.z = 0.0;
    entity.set_velocity(velocity);
    remaining_ticks_ = 0;
    brain.erase(MemoryKey::panic);
    brain.erase(MemoryKey::last_seen_position);
}

FlockGoal::FlockGoal(const std::uint32_t priority,
                     EntityManager& entities,
                     const double speed,
                     const double range,
                     const double preferred_distance)
    : Goal(priority, GoalControl::move), entities_(&entities), speed_(speed),
      range_squared_(range * range),
      preferred_distance_squared_(preferred_distance * preferred_distance) {
    if (speed_ <= 0.0 || range <= preferred_distance || preferred_distance <= 0.0) {
        throw std::invalid_argument("invalid flock goal");
    }
}
Entity* FlockGoal::leader(Entity& entity, Brain& brain) const noexcept {
    const auto* memory = brain.get(MemoryKey::flock_leader);
    const auto* id = memory ? std::get_if<EntityId>(memory) : nullptr;
    auto* candidate = id ? entities_->find(*id) : nullptr;
    return candidate && candidate->type().id() == entity.type().id() ? candidate : nullptr;
}
bool FlockGoal::can_start(Entity& entity, Brain& brain) {
    Entity* nearest = nullptr;
    auto nearest_distance = range_squared_;
    for (const auto id : entities_->ids()) {
        auto* candidate = entities_->find(id);
        if (!candidate || candidate->id() == entity.id() ||
            candidate->type().id() != entity.type().id()) continue;
        const auto distance_squared =
            (candidate->position() - entity.position()).length_squared();
        if (distance_squared > preferred_distance_squared_ &&
            distance_squared < nearest_distance) {
            nearest = candidate;
            nearest_distance = distance_squared;
        }
    }
    if (!nearest) return false;
    brain.set(MemoryKey::flock_leader, nearest->id());
    return true;
}
bool FlockGoal::can_continue(Entity& entity, Brain& brain) {
    const auto* value = leader(entity, brain);
    if (!value) return false;
    const auto distance_squared = (value->position() - entity.position()).length_squared();
    return distance_squared > preferred_distance_squared_ && distance_squared <= range_squared_;
}
void FlockGoal::tick(Entity& entity, Brain& brain, double) {
    const auto* value = leader(entity, brain);
    if (!value) return;
    const auto delta = value->position() - entity.position();
    const auto horizontal = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    if (horizontal <= 0.001) return;
    auto velocity = entity.velocity();
    const auto speed = adjusted_speed(entity, speed_);
    velocity.x = delta.x / horizontal * speed;
    velocity.z = delta.z / horizontal * speed;
    entity.set_velocity(velocity);
}
void FlockGoal::stop(Entity& entity, Brain& brain) {
    auto velocity = entity.velocity();
    velocity.x = 0.0;
    velocity.z = 0.0;
    entity.set_velocity(velocity);
    brain.erase(MemoryKey::flock_leader);
}

TemptGoal::TemptGoal(const std::uint32_t priority,
                     const double speed,
                     const double stop_distance)
    : Goal(priority, GoalControl::move | GoalControl::look),
      speed_(speed), stop_distance_squared_(stop_distance * stop_distance) {
    if (speed_ <= 0.0 || stop_distance <= 0.0) {
        throw std::invalid_argument("invalid temptation goal");
    }
}
bool TemptGoal::can_start(Entity& entity, Brain& brain) {
    const auto* memory = brain.get(MemoryKey::tempted);
    const auto* position = memory ? std::get_if<Vec3>(memory) : nullptr;
    return position && (*position - entity.position()).length_squared() >
        stop_distance_squared_;
}
bool TemptGoal::can_continue(Entity& entity, Brain& brain) {
    return can_start(entity, brain);
}
void TemptGoal::tick(Entity& entity, Brain& brain, double) {
    const auto* position = std::get_if<Vec3>(brain.get(MemoryKey::tempted));
    if (!position) return;
    const auto delta = *position - entity.position();
    const auto horizontal = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    if (horizontal <= 0.001) return;
    const auto speed = adjusted_speed(entity, speed_);
    auto velocity = entity.velocity();
    velocity.x = delta.x / horizontal * speed;
    velocity.z = delta.z / horizontal * speed;
    entity.set_velocity(velocity);
}
void TemptGoal::stop(Entity& entity, Brain& brain) {
    auto velocity = entity.velocity();
    velocity.x = 0.0;
    velocity.z = 0.0;
    entity.set_velocity(velocity);
    brain.erase(MemoryKey::tempted);
}

OwnerFollowGoal::OwnerFollowGoal(const std::uint32_t priority,
                                 const double speed,
                                 const double start_distance,
                                 const double stop_distance)
    : Goal(priority, GoalControl::move | GoalControl::look), speed_(speed),
      start_distance_squared_(start_distance * start_distance),
      stop_distance_squared_(stop_distance * stop_distance) {
    if (speed_ <= 0.0 || stop_distance <= 0.0 || start_distance <= stop_distance) {
        throw std::invalid_argument("invalid owner follow goal");
    }
}

bool OwnerFollowGoal::can_start(Entity& entity, Brain& brain) {
    const auto* memory = brain.get(MemoryKey::owner_position);
    const auto* owner = memory ? std::get_if<Vec3>(memory) : nullptr;
    return owner && (*owner - entity.position()).length_squared() > start_distance_squared_;
}

bool OwnerFollowGoal::can_continue(Entity& entity, Brain& brain) {
    const auto* memory = brain.get(MemoryKey::owner_position);
    const auto* owner = memory ? std::get_if<Vec3>(memory) : nullptr;
    return owner && (*owner - entity.position()).length_squared() > stop_distance_squared_;
}

void OwnerFollowGoal::tick(Entity& entity, Brain& brain, double) {
    const auto* owner = std::get_if<Vec3>(brain.get(MemoryKey::owner_position));
    if (!owner) return;
    const auto delta = *owner - entity.position();
    const auto horizontal = std::hypot(delta.x, delta.z);
    if (horizontal <= 0.001) return;
    auto velocity = entity.velocity();
    const auto speed = adjusted_speed(entity, speed_);
    velocity.x = delta.x / horizontal * speed;
    velocity.z = delta.z / horizontal * speed;
    entity.set_velocity(velocity);
    entity.set_rotation(static_cast<float>(
        std::atan2(-velocity.x, velocity.z) * 180.0 / 3.141592653589793),
        entity.pitch());
}

void OwnerFollowGoal::stop(Entity& entity, Brain&) {
    auto velocity = entity.velocity();
    velocity.x = 0.0;
    velocity.z = 0.0;
    entity.set_velocity(velocity);
}

MobAiSystem::MobAiSystem(EntityManager& entities, const std::uint64_t seed)
    : entities_(&entities), seed_(seed) {}

bool MobAiSystem::attach(const EntityId id) {
    auto* entity = entities_->find(id);
    if (entity == nullptr || dynamic_cast<LivingEntity*>(entity) == nullptr || states_.contains(id)) {
        return false;
    }
    auto [iterator, inserted] = states_.try_emplace(id);
    if (!inserted) return false;
    if (entity->type().properties().category == EntityCategory::monster) {
        iterator->second.goals.add(
            std::make_unique<MeleeAttackGoal>(1, *entities_, 2.0F,
                                              entity->type().properties().movement_speed));
    }
    const auto category = entity->type().properties().category;
    if (category == EntityCategory::creature || category == EntityCategory::water_creature ||
        category == EntityCategory::water_ambient || category == EntityCategory::axolotls) {
        iterator->second.goals.add(std::make_unique<PanicGoal>(
            0, static_cast<double>(entity->type().properties().movement_speed) * 1.5));
        iterator->second.goals.add(std::make_unique<OwnerFollowGoal>(
            1, entity->type().properties().movement_speed));
        iterator->second.goals.add(std::make_unique<TemptGoal>(
            2, entity->type().properties().movement_speed));
        iterator->second.goals.add(std::make_unique<FlockGoal>(
            3, *entities_, entity->type().properties().movement_speed,
            entity->type().properties().aquatic ? 12.0 : 10.0,
            entity->type().properties().aquatic ? 2.0 : 3.0));
    }
    iterator->second.goals.add(std::make_unique<WanderGoal>(
        5, seed_ ^ id, entity->type().properties().category == EntityCategory::monster ? 60 : 40));
    return true;
}

Brain* MobAiSystem::brain(const EntityId id) noexcept {
    const auto found = states_.find(id);
    return found == states_.end() ? nullptr : &found->second.brain;
}

bool MobAiSystem::set_target(const EntityId id, const EntityId target_id) {
    auto* state = brain(id);
    if (state == nullptr || entities_->find(target_id) == nullptr) return false;
    state->set(MemoryKey::target_entity, target_id);
    return true;
}

bool MobAiSystem::notify_damage(const EntityId id, const Vec3 threat) {
    auto* state = brain(id);
    auto* entity = entities_->find(id);
    if (!state || !entity || entity->type().properties().category == EntityCategory::monster) {
        return false;
    }
    state->set(MemoryKey::panic, true);
    state->set(MemoryKey::last_seen_position, threat);
    return true;
}

bool MobAiSystem::tempt(const EntityId id, const Vec3 position) {
    auto* state = brain(id);
    auto* entity = entities_->find(id);
    if (!state || !entity || entity->type().properties().category == EntityCategory::monster) {
        return false;
    }
    state->set(MemoryKey::tempted, position);
    return true;
}

bool MobAiSystem::follow_owner(const EntityId id, const Vec3 position) {
    auto* state = brain(id);
    auto* entity = entities_->find(id);
    if (!state || !entity || entity->type().properties().category != EntityCategory::creature) {
        return false;
    }
    state->set(MemoryKey::owner_position, position);
    return true;
}

bool MobAiSystem::set_suspended(const EntityId id, const bool suspended) {
    const auto found = states_.find(id);
    auto* entity = entities_->find(id);
    if (found == states_.end() || !entity) return false;
    found->second.suspended = suspended;
    if (suspended) {
        found->second.goals.stop_all(*entity, found->second.brain);
        entity->set_velocity({});
    }
    return true;
}

void MobAiSystem::tick(const double delta_seconds) {
    std::erase_if(states_, [&](auto& entry) {
        auto* entity = entities_->find(entry.first);
        if (entity == nullptr || entity->removed()) return true;
        if (entry.second.suspended) return false;
        entry.second.goals.tick(*entity, entry.second.brain, delta_seconds);
        return false;
    });
    for (auto& [id, state] : states_) {
        if (state.suspended) continue;
        auto* living = dynamic_cast<LivingEntity*>(entities_->find(id));
        if (!living || living->hurt_invulnerability_ticks() != 10 ||
            !living->last_damage_source() || !living->last_damage_source()->attacker) {
            continue;
        }
        const auto* attacker = entities_->find(*living->last_damage_source()->attacker);
        if (attacker && living->type().properties().category != EntityCategory::monster) {
            state.brain.set(MemoryKey::panic, true);
            state.brain.set(MemoryKey::last_seen_position, attacker->position());
        }
    }
}

std::size_t MobAiSystem::size() const noexcept { return states_.size(); }

} // namespace mc::entity