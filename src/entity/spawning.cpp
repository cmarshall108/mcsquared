#include "mc/entity/spawning.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

namespace mc::entity {
namespace {

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

struct SpawnRule final {
    std::string_view entity;
    std::uint32_t weight;
    SpawnHabitat habitat;
};

[[nodiscard]] std::vector<SpawnRule> spawn_rules(const world::BiomeId biome,
                                                 const EntityCategory category) {
    using enum SpawnHabitat;
    if (category == EntityCategory::monster) {
        if (biome == world::BiomeId::desert) {
            return {{"minecraft:husk", 80, surface_land},
                    {"minecraft:zombie", 20, surface_land},
                    {"minecraft:skeleton", 30, surface_land},
                    {"minecraft:creeper", 30, surface_land}};
        }
        if (biome == world::BiomeId::ocean) {
            return {{"minecraft:drowned", 100, water}};
        }
        return {{"minecraft:zombie", 100, surface_land},
                {"minecraft:skeleton", 80, surface_land},
                {"minecraft:creeper", 80, surface_land},
                {"minecraft:spider", 80, surface_land}};
    }
    if (category == EntityCategory::creature) {
        switch (biome) {
        case world::BiomeId::plains:
            return {{"minecraft:cow", 80, surface_land},
                    {"minecraft:sheep", 70, surface_land},
                    {"minecraft:pig", 60, surface_land},
                    {"minecraft:chicken", 60, surface_land},
                    {"minecraft:horse", 20, surface_land}};
        case world::BiomeId::forest:
            return {{"minecraft:cow", 50, surface_land},
                    {"minecraft:pig", 50, surface_land},
                    {"minecraft:chicken", 40, surface_land},
                    {"minecraft:wolf", 12, surface_land},
                    {"minecraft:fox", 10, surface_land}};
        case world::BiomeId::desert:
            return {{"minecraft:camel", 80, surface_land},
                    {"minecraft:rabbit", 30, surface_land}};
        case world::BiomeId::mountains:
            return {{"minecraft:goat", 80, surface_land},
                    {"minecraft:sheep", 30, surface_land},
                    {"minecraft:rabbit", 15, surface_land}};
        case world::BiomeId::ocean: return {};
        }
    }
    if (category == EntityCategory::water_ambient && biome == world::BiomeId::ocean) {
        return {{"minecraft:cod", 70, water},
                {"minecraft:salmon", 50, water},
                {"minecraft:squid", 35, water},
                {"minecraft:tropical_fish", 25, water}};
    }
    if (category == EntityCategory::water_creature && biome == world::BiomeId::ocean) {
        return {{"minecraft:dolphin", 20, water},
                {"minecraft:turtle", 10, water}};
    }
    if (category == EntityCategory::ambient) {
        return {{"minecraft:bat", 100, underground}};
    }
    if (category == EntityCategory::axolotls) {
        return {{"minecraft:axolotl", 100, underground}};
    }
    return {};
}

} // namespace

NaturalSpawner::NaturalSpawner(const EntityTypeRegistry& registry, const std::uint64_t seed)
    : registry_(&registry), seed_(seed) {}

std::vector<EntityId> NaturalSpawner::spawn_cycle(
    EntityManager& entities,
    const EntityCategory category,
    const std::vector<Vec3>& candidates,
    const std::uint8_t light_level,
    const bool peaceful,
    const std::size_t attempt_limit) {
    std::vector<SpawnCandidate> contextual;
    contextual.reserve(candidates.size());
    for (const auto position : candidates) {
        contextual.push_back({position, world::BiomeId::plains, SpawnHabitat::surface_land});
    }
    return spawn_cycle(
        entities, category, contextual, light_level, peaceful, attempt_limit);
}

std::vector<EntityId> NaturalSpawner::spawn_cycle(
    EntityManager& entities,
    const EntityCategory category,
    const std::vector<SpawnCandidate>& candidates,
    const std::uint8_t light_level,
    const bool peaceful,
    const std::size_t attempt_limit) {
    std::vector<EntityId> spawned;
    const auto properties = category_properties(category);
    if (properties.maximum < 0 || candidates.empty() || attempt_limit == 0 ||
        (category == EntityCategory::monster && (peaceful || light_level > 7)) ||
        (category == EntityCategory::creature && light_level < 9)) {
        return spawned;
    }
    const auto available = static_cast<std::size_t>(properties.maximum) -
        std::min(static_cast<std::size_t>(properties.maximum), entities.count(category));
    const auto attempts = std::min({attempt_limit, candidates.size(), available});
    for (std::size_t index = 0; index < attempts; ++index) {
        const auto& candidate = candidates[index];
        const auto rules = spawn_rules(candidate.biome, category);
        std::vector<std::pair<const EntityType*, std::uint32_t>> eligible;
        std::uint64_t total_weight = 0;
        for (const auto& rule : rules) {
            if (rule.habitat != candidate.habitat) continue;
            try {
                const auto& type = registry_->by_name(rule.entity);
                if (type.properties().category != category ||
                    type.properties().max_health <= 0.0F) continue;
                eligible.emplace_back(&type, rule.weight);
                total_weight += rule.weight;
            } catch (const std::out_of_range&) {
            }
        }
        if (eligible.empty() || total_weight == 0) continue;
        const auto random = mix(seed_ ^ mix(cycle_) ^ index);
        auto selected = random % total_weight;
        const EntityType* type = eligible.back().first;
        for (const auto& [candidate_type, weight] : eligible) {
            if (selected < weight) {
                type = candidate_type;
                break;
            }
            selected -= weight;
        }
        auto& entity = entities.spawn(type->name().to_string(), candidate.position);
        spawned.push_back(entity.id());
    }
    ++cycle_;
    return spawned;
}

std::size_t NaturalSpawner::despawn_distant(
    EntityManager& entities,
    const std::vector<Vec3>& player_positions) const {
    std::vector<EntityId> removed;
    for (const auto id : entities.ids()) {
        const auto* entity = entities.find(id);
        if (entity == nullptr) continue;
        const auto properties = category_properties(entity->type().properties().category);
        if (properties.persistent || player_positions.empty()) continue;
        auto nearest = std::numeric_limits<double>::max();
        for (const auto player : player_positions) {
            nearest = std::min(nearest, (entity->position() - player).length_squared());
        }
        const auto distance = static_cast<double>(properties.despawn_distance);
        if (nearest > distance * distance) removed.push_back(id);
    }
    for (const auto id : removed) static_cast<void>(entities.remove(id));
    return removed.size();
}

} // namespace mc::entity