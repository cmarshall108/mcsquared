#include "mc/entity/tracking.hpp"

#include <cmath>

namespace mc::entity {
namespace {

[[nodiscard]] bool changed(const double left, const double right,
                           const double epsilon = 1.0e-5) noexcept {
    return std::abs(left - right) > epsilon;
}

[[nodiscard]] bool changed(const Vec3 left, const Vec3 right) noexcept {
    return changed(left.x, right.x) || changed(left.y, right.y) || changed(left.z, right.z);
}

} // namespace

TrackingUpdate EntityTracker::update(const Entity& entity, const Vec3 viewer) {
    const auto range = tracking_range(entity);
    const auto in_range = (entity.position() - viewer).length_squared() <= range * range;
    const auto found = states_.find(entity.id());
    if (!in_range) {
        if (found == states_.end()) return {};
        states_.erase(found);
        TrackingUpdate update;
        update.left = true;
        return update;
    }

    const State current{
        entity.position(), entity.velocity(), entity.yaw(), entity.pitch(), entity.yaw()};
    if (found == states_.end()) {
        states_.emplace(entity.id(), current);
        TrackingUpdate update;
        update.entered = true;
        return update;
    }

    auto& previous = found->second;
    TrackingUpdate update;
    update.delta = current.position - previous.position;
    update.position = changed(current.position, previous.position);
    update.absolute_position = update.position &&
        (update.delta.x < -8.0 || update.delta.x >= 8.0 ||
         update.delta.y < -8.0 || update.delta.y >= 8.0 ||
         update.delta.z < -8.0 || update.delta.z >= 8.0);
    update.rotation = changed(current.yaw, previous.yaw, 0.5) ||
        changed(current.pitch, previous.pitch, 0.5);
    update.velocity = changed(current.velocity, previous.velocity);
    update.head_rotation = changed(current.head_yaw, previous.head_yaw, 0.5);
    previous = current;
    return update;
}

bool EntityTracker::forget(const EntityId entity) noexcept {
    return states_.erase(entity) != 0;
}

bool EntityTracker::visible(const EntityId entity) const noexcept {
    return states_.contains(entity);
}

double EntityTracker::tracking_range(const Entity& entity) noexcept {
    switch (entity.type().properties().category) {
    case EntityCategory::monster: return 64.0;
    case EntityCategory::creature: return 48.0;
    case EntityCategory::ambient:
    case EntityCategory::axolotls:
    case EntityCategory::underground_water_creature:
    case EntityCategory::water_creature:
    case EntityCategory::water_ambient: return 48.0;
    case EntityCategory::misc: return 32.0;
    }
    return 32.0;
}

} // namespace mc::entity