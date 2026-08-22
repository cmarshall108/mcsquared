#include "mc/entity/projectile.hpp"

#include <cmath>
#include <stdexcept>

namespace mc::entity {

EntityId ProjectileSystem::spawn(EntityManager& entities,
                                 const std::string_view type,
                                 const Vec3 position,
                                 const Vec3 velocity,
                                 const EntityId owner,
                                 const float damage,
                                const std::uint32_t lifetime_ticks,
                                std::optional<mc::item::ItemStack> recovery) {
    if (!std::isfinite(damage) || damage < 0.0F || lifetime_ticks == 0) {
        throw std::invalid_argument("projectile fields are invalid");
    }
    auto& projectile = entities.spawn(type, position);
    projectile.set_velocity(velocity);
    projectiles_.emplace(
        projectile.id(), State{owner, damage, lifetime_ticks, std::move(recovery)});
    return projectile.id();
}

std::vector<ProjectileImpact> ProjectileSystem::tick(
    EntityManager& entities, world::World& world) {
    std::vector<ProjectileImpact> impacts;
    for (auto iterator = projectiles_.begin(); iterator != projectiles_.end();) {
        const auto projectile_id = iterator->first;
        auto* projectile = entities.find(projectile_id);
        if (!projectile || iterator->second.lifetime_ticks-- == 0) {
            if (projectile) {
                impacts.push_back({projectile_id, std::nullopt, projectile->position(),
                                   iterator->second.recovery});
                static_cast<void>(entities.remove(projectile_id));
            }
            iterator = projectiles_.erase(iterator);
            continue;
        }
        const auto position = projectile->position();
        const core::BlockPosition block_position{
            static_cast<std::int32_t>(std::floor(position.x)),
            static_cast<std::int32_t>(std::floor(position.y)),
            static_cast<std::int32_t>(std::floor(position.z))};
        if (block_position.y < world::min_build_y || block_position.y >= world::max_build_y ||
            world.solid(block_position)) {
            impacts.push_back({projectile_id, std::nullopt, position,
                               iterator->second.recovery});
            static_cast<void>(entities.remove(projectile_id));
            iterator = projectiles_.erase(iterator);
            continue;
        }

        LivingEntity* target = nullptr;
        for (const auto candidate_id : entities.query({
                 {position.x - 0.4, position.y - 0.4, position.z - 0.4},
                 {position.x + 0.4, position.y + 0.4, position.z + 0.4}})) {
            if (candidate_id == projectile_id || candidate_id == iterator->second.owner) continue;
            target = dynamic_cast<LivingEntity*>(entities.find(candidate_id));
            if (target && target->alive()) break;
            target = nullptr;
        }
        if (target) {
            if (iterator->second.damage > 0.0F) {
                static_cast<void>(target->damage(
                    iterator->second.damage,
                    {DamageType::projectile, iterator->second.owner, false, false}));
            }
            static_cast<void>(target->knockback(projectile->position() - projectile->velocity(), 0.25));
            impacts.push_back({projectile_id, target->id(), position, std::nullopt});
            static_cast<void>(entities.remove(projectile_id));
            iterator = projectiles_.erase(iterator);
            continue;
        }
        ++iterator;
    }
    return impacts;
}

std::size_t ProjectileSystem::size() const noexcept { return projectiles_.size(); }

} // namespace mc::entity