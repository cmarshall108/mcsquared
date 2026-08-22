#include "mc/entity/dropped_item.hpp"

#include <set>
#include <stdexcept>

namespace mc::entity {

EntityId DroppedItemSystem::spawn(EntityManager& entities,
                                  mc::item::ItemStack stack,
                                  const Vec3 position,
                                  const std::uint32_t pickup_delay_ticks,
                                  const std::uint32_t lifetime_ticks) {
    if (stack.empty() || lifetime_ticks == 0) {
        throw std::invalid_argument("dropped item fields are invalid");
    }
    auto& item = entities.spawn("item", position);
    item.set_velocity({0.0, 0.1, 0.0});
    items_.emplace(
        item.id(), State{std::move(stack), pickup_delay_ticks, lifetime_ticks});
    return item.id();
}

DroppedItemTick DroppedItemSystem::tick(EntityManager& entities,
                                        const mc::item::ItemRegistry& registry,
                                        const std::optional<Vec3> collector) {
    DroppedItemTick result;
    std::vector<EntityId> ids;
    ids.reserve(items_.size());
    for (const auto& [id, state] : items_) {
        static_cast<void>(state);
        ids.push_back(id);
    }

    std::set<EntityId> removed;
    for (std::size_t left_index = 0; left_index < ids.size(); ++left_index) {
        const auto left_id = ids[left_index];
        auto left = items_.find(left_id);
        auto* left_entity = entities.find(left_id);
        if (left == items_.end() || !left_entity || removed.contains(left_id)) continue;
        for (std::size_t right_index = left_index + 1; right_index < ids.size(); ++right_index) {
            const auto right_id = ids[right_index];
            auto right = items_.find(right_id);
            auto* right_entity = entities.find(right_id);
            if (right == items_.end() || !right_entity || removed.contains(right_id) ||
                !left->second.stack.can_merge(right->second.stack) ||
                (left_entity->position() - right_entity->position()).length_squared() > 1.0) {
                continue;
            }
            const auto before = left->second.stack.count();
            static_cast<void>(left->second.stack.insert_from(right->second.stack, registry));
            if (left->second.stack.count() != before) result.changed.push_back(left_id);
            if (right->second.stack.empty()) {
                removed.insert(right_id);
                static_cast<void>(entities.remove(right_id));
            }
        }
    }

    for (auto iterator = items_.begin(); iterator != items_.end();) {
        const auto id = iterator->first;
        if (removed.contains(id) || entities.find(id) == nullptr) {
            iterator = items_.erase(iterator);
            continue;
        }
        auto& state = iterator->second;
        if (state.lifetime_ticks-- <= 1) {
            static_cast<void>(entities.remove(id));
            iterator = items_.erase(iterator);
            continue;
        }
        if (state.pickup_delay_ticks > 0) --state.pickup_delay_ticks;
        const auto* item_entity = entities.find(id);
        if (collector && state.pickup_delay_ticks == 0 && item_entity &&
            (item_entity->position() - *collector).length_squared() <= 2.25) {
            result.pickups.push_back({id, state.stack});
            static_cast<void>(entities.remove(id));
            iterator = items_.erase(iterator);
            continue;
        }
        ++iterator;
    }
    return result;
}

const mc::item::ItemStack* DroppedItemSystem::stack(const EntityId entity) const noexcept {
    const auto found = items_.find(entity);
    return found == items_.end() ? nullptr : &found->second.stack;
}

std::size_t DroppedItemSystem::size() const noexcept { return items_.size(); }

} // namespace mc::entity