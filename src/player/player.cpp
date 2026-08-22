#include "mc/player/player.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mc::player {

float scale_hostile_damage(const float normal_damage,
                           const Difficulty difficulty) noexcept {
    if (!std::isfinite(normal_damage) || normal_damage <= 0.0F) return 0.0F;
    switch (difficulty) {
    case Difficulty::peaceful: return 0.0F;
    case Difficulty::easy: return std::min(normal_damage, normal_damage * 0.5F + 1.0F);
    case Difficulty::normal: return normal_damage;
    case Difficulty::hard: return normal_damage * 1.5F;
    }
    return normal_damage;
}

ExperienceState experience_from_total(const std::int32_t total) noexcept {
    auto remaining = std::max(0, total);
    std::int32_t level = 0;
    auto needed = 7;
    while (remaining >= needed && level < 21'863) {
        remaining -= needed;
        ++level;
        needed = level < 15 ? 2 * level + 7
            : level < 30 ? 5 * level - 38
                         : 9 * level - 158;
    }
    const auto progress = level == 21'863
        ? 0.0F
        : static_cast<float>(remaining) / static_cast<float>(needed);
    return {std::max(0, total), level, progress};
}

std::int32_t experience_drop_on_death(const std::int32_t total) noexcept {
    return std::min(100, experience_from_total(total).level * 7);
}

void Statistics::increment(const std::int32_t type_id,
                           const std::int32_t value_id,
                           const std::int32_t amount) {
    if (type_id < 0 || value_id < 0 || amount <= 0) return;
    const auto key = std::pair{type_id, value_id};
    auto& value = values_[key];
    value = amount > std::numeric_limits<std::int32_t>::max() - value
        ? std::numeric_limits<std::int32_t>::max()
        : value + amount;
    dirty_[key] = true;
}

std::int32_t Statistics::value(const std::int32_t type_id,
                               const std::int32_t value_id) const noexcept {
    const auto found = values_.find({type_id, value_id});
    return found == values_.end() ? 0 : found->second;
}

std::vector<StatisticValue> Statistics::snapshot() const {
    std::vector<StatisticValue> result;
    result.reserve(values_.size());
    for (const auto& [key, value] : values_) {
        result.push_back({key.first, key.second, value});
    }
    return result;
}

std::vector<StatisticValue> Statistics::drain_updates() {
    std::vector<StatisticValue> result;
    result.reserve(dirty_.size());
    for (const auto& [key, changed] : dirty_) {
        if (changed) result.push_back({key.first, key.second, values_.at(key)});
    }
    dirty_.clear();
    return result;
}

float SurvivalState::health() const noexcept { return health_; }
float SurvivalState::absorption() const noexcept { return absorption_; }
std::int32_t SurvivalState::food_level() const noexcept { return food_level_; }
float SurvivalState::saturation() const noexcept { return saturation_; }
float SurvivalState::exhaustion() const noexcept { return exhaustion_; }
std::int32_t SurvivalState::air_ticks() const noexcept { return air_ticks_; }
float SurvivalState::fall_distance() const noexcept { return fall_distance_; }
bool SurvivalState::sprinting() const noexcept { return sprinting_; }
bool SurvivalState::sneaking() const noexcept { return sneaking_; }
bool SurvivalState::jumping() const noexcept { return jumping_; }
bool SurvivalState::blocking() const noexcept { return blocking_; }
bool SurvivalState::flying() const noexcept { return flying_; }
const std::map<std::int32_t, SurvivalState::Effect>&
SurvivalState::effects() const noexcept { return effects_; }
MovementMode SurvivalState::movement_mode() const noexcept {
    if (flying_) return MovementMode::flying;
    if (gliding_) return MovementMode::gliding;
    if (underwater_ && (sprinting_ || jumping_)) return MovementMode::swimming;
    if (sneaking_) return MovementMode::sneaking;
    return MovementMode::standing;
}
float SurvivalState::eye_height() const noexcept {
    switch (movement_mode()) {
    case MovementMode::sneaking: return 1.27F;
    case MovementMode::swimming:
    case MovementMode::gliding: return 0.4F;
    case MovementMode::flying: return 1.62F;
    case MovementMode::standing: return 1.62F;
    }
    return 1.62F;
}
float SurvivalState::body_height() const noexcept {
    switch (movement_mode()) {
    case MovementMode::sneaking: return 1.5F;
    case MovementMode::swimming:
    case MovementMode::gliding: return 0.6F;
    case MovementMode::flying: return 1.8F;
    case MovementMode::standing: return 1.8F;
    }
    return 1.8F;
}
float SurvivalState::movement_speed_multiplier() const noexcept {
    auto effect_multiplier = 1.0F;
    if (const auto speed = effects_.find(0); speed != effects_.end()) {
        effect_multiplier *= 1.0F + 0.2F * static_cast<float>(speed->second.amplifier + 1U);
    }
    if (const auto slowness = effects_.find(1); slowness != effects_.end()) {
        effect_multiplier *= std::max(
            0.0F, 1.0F - 0.15F * static_cast<float>(slowness->second.amplifier + 1U));
    }
    switch (movement_mode()) {
    case MovementMode::sneaking: return 0.3F * effect_multiplier;
    case MovementMode::swimming: return 0.8F * effect_multiplier;
    case MovementMode::gliding: return effect_multiplier;
    case MovementMode::flying: return effect_multiplier;
    case MovementMode::standing: return (sprinting_ ? 1.3F : 1.0F) * effect_multiplier;
    }
    return 1.0F;
}
float SurvivalState::attack_charge() const noexcept {
    return std::min(1.0F, static_cast<float>(attack_recharge_ticks_) / 5.0F);
}
std::optional<entity::EntityId> SurvivalState::combat_target() const noexcept {
    return combat_target_;
}
std::uint32_t SurvivalState::combat_ticks() const noexcept { return combat_ticks_; }
std::uint32_t SurvivalState::hurt_invulnerability_ticks() const noexcept {
    return hurt_invulnerability_ticks_;
}
const std::optional<entity::DamageSource>&
SurvivalState::last_damage_source() const noexcept { return last_damage_source_; }

void SurvivalState::tick(const bool underwater, const Difficulty difficulty) {
    underwater_ = underwater;
    if (underwater_) gliding_ = false;
    if (difficulty == Difficulty::peaceful && food_level_ < 20) {
        food_level_ = 20;
        saturation_ = std::max(saturation_, 5.0F);
        exhaustion_ = 0.0F;
        dirty_ = true;
    }
    attack_recharge_ticks_ = std::min<std::uint32_t>(5, attack_recharge_ticks_ + 1);
    if (combat_ticks_ > 0 && --combat_ticks_ == 0) combat_target_.reset();
    if (hurt_invulnerability_ticks_ > 0) --hurt_invulnerability_ticks_;
    for (auto iterator = effects_.begin(); iterator != effects_.end();) {
        const auto effect_id = iterator->first;
        auto& effect = iterator->second;
        const auto interval = [&](const std::uint32_t base) {
            return std::max<std::uint32_t>(
                1, base >> std::min<std::uint8_t>(effect.amplifier, 6));
        };
        if (effect_id == 9 && effect.duration_ticks % interval(50) == 0) {
            heal(1.0F);
        } else if (effect_id == 18 && effect.duration_ticks % interval(25) == 0 &&
                   (health_ > 1.0F || absorption_ > 0.0F)) {
            static_cast<void>(damage(
                std::min(1.0F, std::max(0.0F, health_ - 1.0F) + absorption_),
                {entity::DamageType::poison, std::nullopt, true, true}));
        } else if (effect_id == 19 && effect.duration_ticks % interval(40) == 0) {
            static_cast<void>(damage(
                1.0F, {entity::DamageType::wither, std::nullopt, true, true}));
        }
        if (effect.duration_ticks <= 1) {
            if (effect_id == 21) {
                absorption_ = 0.0F;
                dirty_ = true;
            }
            expired_effects_.push_back(effect_id);
            iterator = effects_.erase(iterator);
        } else {
            --effect.duration_ticks;
            ++iterator;
        }
    }
    if (underwater) {
        --air_ticks_;
        if (air_ticks_ <= -20) {
            static_cast<void>(damage(
                2.0F, {entity::DamageType::drowning, std::nullopt, true, true}));
            air_ticks_ = 0;
        }
    } else {
        air_ticks_ = std::min(300, air_ticks_ + 4);
    }

    if (food_level_ >= 18 && health_ < 20.0F) {
        const auto regeneration_interval = difficulty == Difficulty::peaceful ? 20U : 80U;
        if (++regeneration_ticks_ >= regeneration_interval) {
            heal(1.0F);
            if (difficulty != Difficulty::peaceful) add_exhaustion(6.0F);
            regeneration_ticks_ = 0;
        }
    } else {
        regeneration_ticks_ = 0;
    }

    if (food_level_ == 0 && difficulty != Difficulty::peaceful) {
        if (++starvation_ticks_ >= 80) {
            const auto minimum_health = difficulty == Difficulty::easy ? 10.0F
                : difficulty == Difficulty::normal ? 1.0F
                                                   : 0.0F;
            if (health_ > minimum_health) {
                health_ = std::max(minimum_health, health_ - 1.0F);
                dirty_ = true;
            }
            starvation_ticks_ = 0;
        }
    } else {
        starvation_ticks_ = 0;
    }
}

void SurvivalState::record_movement(const entity::Vec3 from,
                                    const entity::Vec3 to,
                                    const bool on_ground,
                                    const bool sprinting) {
    const auto delta = to - from;
    const auto horizontal = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    if (std::isfinite(horizontal) && horizontal <= 10.0) {
        const auto rate = movement_mode() == MovementMode::gliding ||
            movement_mode() == MovementMode::flying ? 0.0F
            : movement_mode() == MovementMode::sneaking ? 0.005F
            : movement_mode() == MovementMode::swimming ? 0.01F
            : sprinting ? 0.1F
                        : 0.01F;
        add_exhaustion(static_cast<float>(horizontal) * rate);
    }
    if (movement_mode() == MovementMode::flying) {
        fall_distance_ = 0.0F;
    } else if (movement_mode() == MovementMode::gliding) {
        fall_distance_ = std::max(0.0F, fall_distance_ - 0.5F);
    } else if ((!on_ground || !was_on_ground_) && delta.y < 0.0 && std::isfinite(delta.y)) {
        fall_distance_ += static_cast<float>(-delta.y);
    }
    if (on_ground && !was_on_ground_) {
        const auto fall_damage = std::ceil(fall_distance_ - 3.0F);
        if (fall_damage > 0.0F) {
            static_cast<void>(damage(
                fall_damage, {entity::DamageType::fall, std::nullopt, true, true}));
        }
        fall_distance_ = 0.0F;
    }
    if (on_ground) gliding_ = false;
    was_on_ground_ = on_ground;
}

void SurvivalState::add_exhaustion(const float amount) {
    if (!std::isfinite(amount) || amount <= 0.0F) return;
    exhaustion_ = std::min(40.0F, exhaustion_ + amount);
    apply_exhaustion();
}

void SurvivalState::set_input(const bool sprinting,
                              const bool sneaking,
                              const bool jumping) noexcept {
    sprinting_ = sprinting && food_level_ > 6;
    sneaking_ = sneaking;
    jumping_ = jumping;
}

bool SurvivalState::start_gliding() noexcept {
    if (was_on_ground_ || underwater_ || flying_) return false;
    gliding_ = true;
    sneaking_ = false;
    return true;
}

void SurvivalState::set_flying(const bool flying) noexcept {
    flying_ = flying;
    if (flying_) {
        gliding_ = false;
        fall_distance_ = 0.0F;
    }
}

SurvivalState::AttackResult SurvivalState::attack(const entity::EntityId target) {
    const auto charge = attack_charge();
    const auto mode = movement_mode();
    const auto critical = charge >= 0.9F && !was_on_ground_ && fall_distance_ > 0.0F &&
        !sprinting_ && mode != MovementMode::swimming && mode != MovementMode::gliding;
    const auto sweeping = charge >= 0.9F && was_on_ground_ && !sprinting_;
    auto multiplier = 0.2F + charge * charge * 0.8F;
    if (critical) multiplier *= 1.5F;
    attack_recharge_ticks_ = 0;
    combat_target_ = target;
    combat_ticks_ = 100;
    add_exhaustion(0.1F);
    return {multiplier, critical, sweeping};
}

bool SurvivalState::apply_effect(const std::int32_t effect_id, const Effect effect) {
    if (effect_id < 0 || effect_id >= 40 || effect.duration_ticks == 0) return false;
    const auto found = effects_.find(effect_id);
    if (found != effects_.end() && found->second.amplifier > effect.amplifier) return false;
    effects_[effect_id] = effect;
    if (effect_id == 21) {
        absorption_ = static_cast<float>(effect.amplifier + 1U) * 4.0F;
        dirty_ = true;
    }
    return true;
}

bool SurvivalState::remove_effect(const std::int32_t effect_id) noexcept {
    if (effects_.erase(effect_id) == 0) return false;
    if (effect_id == 21) {
        absorption_ = 0.0F;
        dirty_ = true;
    }
    return true;
}

std::vector<std::int32_t> SurvivalState::drain_expired_effects() {
    auto result = std::move(expired_effects_);
    expired_effects_.clear();
    return result;
}

void SurvivalState::set_blocking(const bool blocking) noexcept {
    blocking_ = blocking && health_ > 0.0F;
    if (blocking_) sprinting_ = false;
}

bool SurvivalState::blocks_attack(const entity::Vec3 source,
                                  const entity::Vec3 player_position,
                                  const float yaw) const noexcept {
    if (!blocking_ || !std::isfinite(source.x) || !std::isfinite(source.z) ||
        !std::isfinite(player_position.x) || !std::isfinite(player_position.z) ||
        !std::isfinite(yaw)) {
        return false;
    }
    const auto delta_x = source.x - player_position.x;
    const auto delta_z = source.z - player_position.z;
    if (delta_x * delta_x + delta_z * delta_z <= 1.0e-12) return true;
    constexpr double pi = 3.141592653589793;
    const auto radians = static_cast<double>(yaw) * pi / 180.0;
    const auto facing_x = -std::sin(radians);
    const auto facing_z = std::cos(radians);
    return facing_x * delta_x + facing_z * delta_z >= 0.0;
}

bool SurvivalState::consume_food(const std::int32_t nutrition,
                                 const float saturation_modifier) {
    if (nutrition <= 0 || !std::isfinite(saturation_modifier) ||
        saturation_modifier < 0.0F || food_level_ >= 20) {
        return false;
    }
    food_level_ = std::min(20, food_level_ + nutrition);
    saturation_ = std::min(
        static_cast<float>(food_level_),
        saturation_ + static_cast<float>(nutrition) * saturation_modifier * 2.0F);
    dirty_ = true;
    return true;
}

bool SurvivalState::damage(const float amount) noexcept {
    return damage(amount, entity::DamageSource{}, false).applied;
}

SurvivalState::DamageResult SurvivalState::damage(const float amount,
                                                  const bool blockable) noexcept {
    return damage(amount, entity::DamageSource{}, blockable);
}

void SurvivalState::set_armor(const float armor, const float toughness) noexcept {
    armor_ = std::clamp(armor, 0.0F, 30.0F);
    armor_toughness_ = std::clamp(toughness, 0.0F, 20.0F);
}

SurvivalState::DamageResult SurvivalState::damage(
    const float amount, entity::DamageSource source, const bool blockable) noexcept {
    if (!std::isfinite(amount) || amount <= 0.0F || health_ <= 0.0F) {
        return {false, 0.0F};
    }
    if (source.type == entity::DamageType::fire && effects_.contains(11)) {
        return {false, 0.0F};
    }
    if (hurt_invulnerability_ticks_ > 0 && !source.bypasses_invulnerability) {
        return {false, 0.0F};
    }
    auto applied_amount = amount;
    if (!source.bypasses_armor) {
        const auto effective_armor = std::min(
            20.0F,
            std::max(armor_ / 5.0F,
                     armor_ - amount / (2.0F + armor_toughness_ / 4.0F)));
        applied_amount = amount * (1.0F - effective_armor / 25.0F);
    }
    const auto blocked = blockable && blocking_ ? applied_amount * 0.5F : 0.0F;
    auto remaining = applied_amount - blocked;
    const auto absorbed = std::min(absorption_, remaining);
    absorption_ -= absorbed;
    remaining -= absorbed;
    health_ = std::max(0.0F, health_ - remaining);
    if (health_ == 0.0F) blocking_ = false;
    last_damage_source_ = std::move(source);
    if (!last_damage_source_->bypasses_invulnerability) hurt_invulnerability_ticks_ = 10;
    dirty_ = true;
    return {true, blocked};
}

void SurvivalState::heal(const float amount) noexcept {
    if (!std::isfinite(amount) || amount <= 0.0F || health_ <= 0.0F) return;
    const auto healed = std::min(20.0F, health_ + amount);
    if (healed != health_) {
        health_ = healed;
        dirty_ = true;
    }
}

void SurvivalState::reset() noexcept {
    *this = SurvivalState{};
}

bool SurvivalState::take_dirty() noexcept {
    const auto result = dirty_;
    dirty_ = false;
    return result;
}

void SurvivalState::apply_exhaustion() {
    while (exhaustion_ >= 4.0F) {
        exhaustion_ -= 4.0F;
        if (saturation_ > 0.0F) {
            saturation_ = std::max(0.0F, saturation_ - 1.0F);
        } else if (food_level_ > 0) {
            --food_level_;
        }
        dirty_ = true;
    }
}

} // namespace mc::player