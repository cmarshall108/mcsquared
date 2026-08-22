#include "mc/entity/animal.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mc::entity {

bool AnimalState::baby() const noexcept { return age_ticks < 0; }
bool AnimalState::adult() const noexcept { return age_ticks == 0; }
bool AnimalState::in_love() const noexcept { return love_ticks > 0 && adult(); }

bool AnimalSystem::attach(const Entity& entity) {
    const auto category = entity.type().properties().category;
    const auto animal = category == EntityCategory::creature ||
        category == EntityCategory::axolotls ||
        category == EntityCategory::water_creature ||
        category == EntityCategory::water_ambient;
    if (!animal || dynamic_cast<const LivingEntity*>(&entity) == nullptr) return false;
    return animals_.try_emplace(entity.id()).second;
}

AnimalState* AnimalSystem::state(const EntityId id) noexcept {
    const auto found = animals_.find(id);
    return found == animals_.end() ? nullptr : &found->second;
}
const AnimalState* AnimalSystem::state(const EntityId id) const noexcept {
    const auto found = animals_.find(id);
    return found == animals_.end() ? nullptr : &found->second;
}

bool AnimalSystem::set_in_love(const EntityId id, const std::uint32_t duration) {
    auto* animal = state(id);
    if (animal == nullptr || !animal->adult() || duration == 0) return false;
    animal->love_ticks = duration;
    return true;
}

bool AnimalSystem::tame(const EntityId id, const mc::core::Uuid owner) {
    auto* animal = state(id);
    if (animal == nullptr) return false;
    animal->tamed = true;
    animal->owner = owner;
    animal->sitting = false;
    return true;
}

bool AnimalSystem::toggle_sitting(const EntityId id, const mc::core::Uuid owner) {
    auto* animal = state(id);
    if (!animal || !animal->tamed || animal->owner != owner) return false;
    animal->sitting = !animal->sitting;
    return true;
}

bool AnimalSystem::set_variant(const EntityId id, std::string variant) {
    auto* animal = state(id);
    if (animal == nullptr || variant.empty()) return false;
    animal->variant = std::move(variant);
    return true;
}

bool AnimalSystem::accepts_food(const Entity& entity, const std::string_view item) const noexcept {
    const auto name = entity.type().name().path();
    if (name == "cow" || name == "sheep" || name == "goat") return item == "wheat";
    if (name == "pig") return item == "carrot" || item == "potato" || item == "beetroot";
    if (name == "chicken") return item == "wheat_seeds";
    if (name == "rabbit") return item == "carrot" || item == "dandelion";
    if (name == "horse") return item == "golden_carrot" || item == "golden_apple";
    return false;
}

bool AnimalSystem::accepts_taming_item(
    const Entity& entity, const std::string_view item) const noexcept {
    return entity.type().name().path() == "wolf" && item == "bone";
}

std::optional<EntityId> AnimalSystem::breed(const EntityId first,
                                            const EntityId second,
                                            EntityManager& entities) {
    auto* first_state = state(first);
    auto* second_state = state(second);
    auto* first_entity = entities.find(first);
    auto* second_entity = entities.find(second);
    if (first == second || first_state == nullptr || second_state == nullptr ||
        first_entity == nullptr || second_entity == nullptr ||
        !first_state->in_love() || !second_state->in_love() ||
        first_entity->type().id() != second_entity->type().id()) {
        return std::nullopt;
    }
    const auto midpoint = (first_entity->position() + second_entity->position()) * 0.5;
    auto& child = entities.spawn(first_entity->type().name().to_string(), midpoint);
    if (!attach(child)) return std::nullopt;
    auto* child_state = state(child.id());
    child_state->age_ticks = -24'000;
    child_state->variant = first_state->variant;
    first_state->love_ticks = 0;
    second_state->love_ticks = 0;
    first_state->age_ticks = 6'000;
    second_state->age_ticks = 6'000;
    return child.id();
}

void AnimalSystem::tick(EntityManager& entities, const double delta_seconds) {
    const auto ticks = static_cast<std::int32_t>(
        std::max(0.0, std::floor(delta_seconds * 20.0)));
    std::erase_if(animals_, [&](auto& entry) {
        if (entities.find(entry.first) == nullptr) return true;
        auto& animal = entry.second;
        if (animal.age_ticks < 0) {
            animal.age_ticks = std::min(0, animal.age_ticks + ticks);
        } else if (animal.age_ticks > 0) {
            animal.age_ticks = std::max(0, animal.age_ticks - ticks);
        }
        const auto elapsed = static_cast<std::uint32_t>(ticks);
        animal.love_ticks = elapsed >= animal.love_ticks ? 0 : animal.love_ticks - elapsed;
        return false;
    });
}

std::size_t AnimalSystem::size() const noexcept { return animals_.size(); }

} // namespace mc::entity