#include "mc/player/player.hpp"

#include <cassert>

namespace {

void test_statistics() {
    mc::player::Statistics statistics;
    statistics.increment(8, 1, 20);
    statistics.increment(8, 1, 5);
    statistics.increment(0, 2);
    assert(statistics.value(8, 1) == 25);
    assert(statistics.snapshot().size() == 2);
    assert(statistics.drain_updates().size() == 2);
    assert(statistics.drain_updates().empty());
}

void test_experience_progression() {
    assert(mc::player::experience_from_total(0).level == 0);
    assert(mc::player::experience_from_total(7).level == 1);
    assert(mc::player::experience_from_total(16).level == 2);
    const auto fifty = mc::player::experience_from_total(50);
    assert(fifty.level == 4);
    assert(std::abs(fifty.progress - 2.0F / 3.0F) < 0.0001F);
    const auto maximum = mc::player::experience_from_total(
        std::numeric_limits<std::int32_t>::max());
    assert(maximum.total == std::numeric_limits<std::int32_t>::max());
    assert(maximum.level <= 21'863);
    assert(mc::player::experience_drop_on_death(0) == 0);
    assert(mc::player::experience_drop_on_death(50) == 28);
    assert(mc::player::experience_drop_on_death(1'000'000) == 100);
}

void test_hunger_and_food() {
    mc::player::SurvivalState state;
    static_cast<void>(state.take_dirty());
    state.add_exhaustion(20.0F);
    assert(state.saturation() == 0.0F);
    assert(state.food_level() == 20);
    state.add_exhaustion(4.0F);
    assert(state.food_level() == 19);
    assert(state.consume_food(1, 0.5F));
    assert(state.food_level() == 20);
    assert(state.saturation() == 1.0F);
    assert(state.take_dirty());

    state.add_exhaustion(40.0F);
    state.add_exhaustion(20.0F);
    assert(state.food_level() == 6);
    state.set_input(true, false, false);
    assert(!state.sprinting());
}

void test_movement_and_fall_damage() {
    mc::player::SurvivalState state;
    state.record_movement({0.0, 10.0, 0.0}, {1.0, 7.0, 0.0}, false, true);
    state.record_movement({1.0, 7.0, 0.0}, {2.0, 2.0, 0.0}, false, true);
    assert(state.fall_distance() == 8.0F);
    state.record_movement({2.0, 2.0, 0.0}, {2.0, 0.0, 0.0}, true, false);
    assert(state.health() == 13.0F);
    assert(state.fall_distance() == 0.0F);
    assert(state.exhaustion() > 0.0F);
}

void test_air_regeneration_and_starvation() {
    mc::player::SurvivalState drowning;
    for (int tick = 0; tick < 320; ++tick) drowning.tick(true, mc::player::Difficulty::normal);
    assert(drowning.health() == 18.0F);

    mc::player::SurvivalState regeneration;
    assert(regeneration.damage(4.0F));
    for (int tick = 0; tick < 80; ++tick) {
        regeneration.tick(false, mc::player::Difficulty::normal);
    }
    assert(regeneration.health() == 17.0F);

    mc::player::SurvivalState starving;
    starving.add_exhaustion(100.0F);
    assert(starving.food_level() == 15);
    for (int cycle = 0; cycle < 15; ++cycle) starving.add_exhaustion(4.0F);
    assert(starving.food_level() == 0);
    for (int tick = 0; tick < 80; ++tick) starving.tick(false, mc::player::Difficulty::normal);
    assert(starving.health() == 19.0F);
    assert(starving.damage(100.0F));
    starving.reset();
    assert(starving.health() == 20.0F);
    assert(starving.food_level() == 20);
    assert(starving.air_ticks() == 300);
}

void test_difficulty_survival_rules() {
    assert(mc::player::scale_hostile_damage(
        4.0F, mc::player::Difficulty::peaceful) == 0.0F);
    assert(mc::player::scale_hostile_damage(
        4.0F, mc::player::Difficulty::easy) == 3.0F);
    assert(mc::player::scale_hostile_damage(
        4.0F, mc::player::Difficulty::normal) == 4.0F);
    assert(mc::player::scale_hostile_damage(
        4.0F, mc::player::Difficulty::hard) == 6.0F);

    mc::player::SurvivalState peaceful;
    peaceful.add_exhaustion(20.0F);
    assert(peaceful.damage(5.0F));
    for (int tick = 0; tick < 20; ++tick) {
        peaceful.tick(false, mc::player::Difficulty::peaceful);
    }
    assert(peaceful.food_level() == 20);
    assert(peaceful.health() == 16.0F);

    mc::player::SurvivalState easy;
    easy.add_exhaustion(100.0F);
    for (int cycle = 0; cycle < 15; ++cycle) easy.add_exhaustion(4.0F);
    for (int tick = 0; tick < 1'600; ++tick) {
        easy.tick(false, mc::player::Difficulty::easy);
    }
    assert(easy.health() == 10.0F);

    mc::player::SurvivalState hard;
    hard.add_exhaustion(100.0F);
    for (int cycle = 0; cycle < 15; ++cycle) hard.add_exhaustion(4.0F);
    for (int tick = 0; tick < 1'600; ++tick) {
        hard.tick(false, mc::player::Difficulty::hard);
    }
    assert(hard.health() == 0.0F);
}

void test_input_and_combat_state() {
    mc::player::SurvivalState state;
    state.set_input(true, true, true);
    assert(state.sprinting() && state.sneaking() && state.jumping());
    const auto full_attack = state.attack(42);
    assert(full_attack.damage_multiplier == 1.0F);
    assert(!full_attack.critical && !full_attack.sweeping);
    assert(state.combat_target() == 42);
    assert(state.combat_ticks() == 100);
    const auto weak_attack = state.attack(43);
    assert(weak_attack.damage_multiplier == 0.2F);
    for (int tick = 0; tick < 5; ++tick) state.tick(false, mc::player::Difficulty::normal);
    state.set_input(false, false, false);
    const auto sweeping = state.attack(44);
    assert(sweeping.sweeping);

    mc::player::SurvivalState falling;
    falling.record_movement({0.0, 5.0, 0.0}, {0.0, 4.0, 0.0}, false, false);
    const auto critical = falling.attack(45);
    assert(critical.critical);
    assert(critical.damage_multiplier == 1.5F);
    for (int tick = 0; tick < 100; ++tick) falling.tick(false, mc::player::Difficulty::normal);
    assert(!falling.combat_target());
}

void test_shield_blocking() {
    mc::player::SurvivalState state;
    state.set_input(true, false, false);
    state.set_blocking(true);
    assert(state.blocking());
    assert(!state.sprinting());
    assert(state.blocks_attack({0.0, 0.0, 2.0}, {0.0, 0.0, 0.0}, 0.0F));
    assert(!state.blocks_attack({0.0, 0.0, -2.0}, {0.0, 0.0, 0.0}, 0.0F));
    assert(state.blocks_attack({-2.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, 90.0F));
    const auto blocked = state.damage(6.0F, true);
    assert(blocked.applied && blocked.blocked == 3.0F);
    assert(state.health() == 17.0F);
    for (int tick = 0; tick < 10; ++tick) {
        state.tick(false, mc::player::Difficulty::normal);
    }
    const auto environmental = state.damage(4.0F, false);
    assert(environmental.blocked == 0.0F);
    assert(state.health() == 13.0F);
    state.set_blocking(false);
    assert(!state.blocking());
}

void test_player_status_effects() {
    mc::player::SurvivalState state;
    assert(state.apply_effect(0, {1, 2, false, true}));
    assert(state.movement_speed_multiplier() > 1.0F);
    state.tick(false, mc::player::Difficulty::normal);
    state.tick(false, mc::player::Difficulty::normal);
    assert(state.effects().empty());
    assert(state.drain_expired_effects() == std::vector<std::int32_t>{0});

    assert(state.damage(5.0F));
    assert(state.apply_effect(9, {0, 50, false, true}));
    state.tick(false, mc::player::Difficulty::normal);
    assert(state.health() == 16.0F);
    assert(state.apply_effect(18, {0, 25, false, true}));
    state.tick(false, mc::player::Difficulty::normal);
    assert(state.health() == 15.0F);
}

void test_typed_damage_armor_and_absorption() {
    mc::player::SurvivalState armored;
    armored.set_armor(10.0F, 0.0F);
    assert(armored.damage(
        10.0F, {mc::entity::DamageType::melee, 42, false, false}).applied);
    assert(armored.health() == 12.0F);
    assert(armored.last_damage_source()->attacker == 42);
    assert(armored.hurt_invulnerability_ticks() == 10);
    assert(!armored.damage(
        10.0F, {mc::entity::DamageType::melee, 42, false, false}).applied);
    assert(armored.damage(
        2.0F, {mc::entity::DamageType::generic, std::nullopt, true, true}).applied);
    assert(armored.health() == 10.0F);

    mc::player::SurvivalState protected_player;
    assert(protected_player.apply_effect(11, {0, 20, false, true}));
    assert(!protected_player.damage(
        4.0F, {mc::entity::DamageType::fire, std::nullopt, false, true}).applied);
    assert(protected_player.apply_effect(21, {1, 20, false, true}));
    assert(protected_player.absorption() == 8.0F);
    assert(protected_player.damage(
        6.0F, {mc::entity::DamageType::generic, std::nullopt, true, true}).applied);
    assert(protected_player.health() == 20.0F);
    assert(protected_player.absorption() == 2.0F);
    assert(protected_player.remove_effect(21));
    assert(protected_player.absorption() == 0.0F);

    assert(protected_player.apply_effect(21, {0, 1, false, true}));
    protected_player.tick(false, mc::player::Difficulty::normal);
    assert(protected_player.absorption() == 0.0F);
}

void test_movement_modes() {
    mc::player::SurvivalState state;
    state.set_input(false, true, false);
    assert(state.movement_mode() == mc::player::MovementMode::sneaking);
    assert(state.eye_height() == 1.27F);
    assert(state.body_height() == 1.5F);
    assert(state.movement_speed_multiplier() == 0.3F);

    state.set_input(true, false, true);
    state.tick(true, mc::player::Difficulty::normal);
    assert(state.movement_mode() == mc::player::MovementMode::swimming);
    assert(state.eye_height() == 0.4F);
    assert(state.body_height() == 0.6F);

    mc::player::SurvivalState gliding;
    gliding.record_movement({0.0, 10.0, 0.0}, {0.0, 9.0, 0.0}, false, false);
    assert(gliding.start_gliding());
    const auto before = gliding.exhaustion();
    gliding.record_movement({0.0, 9.0, 0.0}, {5.0, 7.0, 0.0}, false, false);
    assert(gliding.movement_mode() == mc::player::MovementMode::gliding);
    assert(gliding.exhaustion() == before);
    assert(gliding.fall_distance() < 1.0F);
    gliding.record_movement({5.0, 7.0, 0.0}, {5.0, 6.0, 0.0}, true, false);
    assert(gliding.movement_mode() == mc::player::MovementMode::standing);

    mc::player::SurvivalState flying;
    flying.set_flying(true);
    assert(flying.flying());
    assert(flying.movement_mode() == mc::player::MovementMode::flying);
    flying.record_movement({0.0, 20.0, 0.0}, {8.0, 5.0, 0.0}, false, true);
    flying.record_movement({8.0, 5.0, 0.0}, {8.0, 0.0, 0.0}, true, false);
    assert(flying.health() == 20.0F);
    assert(flying.exhaustion() == 0.0F);
    assert(flying.fall_distance() == 0.0F);
    flying.set_flying(false);
    assert(!flying.flying());
}

} // namespace

int main() {
    test_statistics();
    test_experience_progression();
    test_hunger_and_food();
    test_movement_and_fall_damage();
    test_air_regeneration_and_starvation();
    test_difficulty_survival_rules();
    test_input_and_combat_state();
    test_shield_blocking();
    test_player_status_effects();
    test_typed_damage_armor_and_absorption();
    test_movement_modes();
}