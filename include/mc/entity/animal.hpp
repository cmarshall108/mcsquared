#pragma once

#include "mc/entity/entity.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace mc::entity {

struct AnimalState final {
    std::int32_t age_ticks{0};
    std::uint32_t love_ticks{0};
    bool tamed{false};
    bool sitting{false};
    std::optional<mc::core::Uuid> owner;
    std::string variant{"default"};

    [[nodiscard]] bool baby() const noexcept;
    [[nodiscard]] bool adult() const noexcept;
    [[nodiscard]] bool in_love() const noexcept;
};

class AnimalSystem final {
public:
    [[nodiscard]] bool attach(const Entity& entity);
    [[nodiscard]] AnimalState* state(EntityId entity) noexcept;
    [[nodiscard]] const AnimalState* state(EntityId entity) const noexcept;
    [[nodiscard]] bool set_in_love(EntityId entity, std::uint32_t duration = 600);
    [[nodiscard]] bool tame(EntityId entity, mc::core::Uuid owner);
    [[nodiscard]] bool toggle_sitting(EntityId entity, mc::core::Uuid owner);
    [[nodiscard]] bool set_variant(EntityId entity, std::string variant);
    [[nodiscard]] bool accepts_food(const Entity& entity, std::string_view item) const noexcept;
    [[nodiscard]] bool accepts_taming_item(
        const Entity& entity, std::string_view item) const noexcept;
    [[nodiscard]] std::optional<EntityId> breed(EntityId first,
                                                EntityId second,
                                                EntityManager& entities);
    void tick(EntityManager& entities, double delta_seconds);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::map<EntityId, AnimalState> animals_;
};

} // namespace mc::entity