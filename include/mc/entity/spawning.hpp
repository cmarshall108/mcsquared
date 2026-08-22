#pragma once

#include "mc/entity/entity.hpp"
#include "mc/world/chunk.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::entity {

enum class SpawnHabitat {
    surface_land,
    water,
    underground,
    air,
};

struct SpawnCandidate final {
    Vec3 position;
    world::BiomeId biome;
    SpawnHabitat habitat;
};

class NaturalSpawner final {
public:
    NaturalSpawner(const EntityTypeRegistry& registry, std::uint64_t seed);

    [[nodiscard]] std::vector<EntityId> spawn_cycle(
        EntityManager& entities,
        EntityCategory category,
        const std::vector<Vec3>& candidates,
        std::uint8_t light_level,
        bool peaceful,
        std::size_t attempt_limit = 8);
    [[nodiscard]] std::vector<EntityId> spawn_cycle(
        EntityManager& entities,
        EntityCategory category,
        const std::vector<SpawnCandidate>& candidates,
        std::uint8_t light_level,
        bool peaceful,
        std::size_t attempt_limit = 8);
    [[nodiscard]] std::size_t despawn_distant(
        EntityManager& entities,
        const std::vector<Vec3>& player_positions) const;

private:
    const EntityTypeRegistry* registry_;
    std::uint64_t seed_;
    std::uint64_t cycle_{0};
};

} // namespace mc::entity