#include "mc/entity/entity.hpp"
#include "mc/entity/dropped_item.hpp"
#include "mc/entity/projectile.hpp"
#include "mc/entity/ai.hpp"
#include "mc/entity/animal.hpp"
#include "mc/entity/spawning.hpp"
#include "mc/entity/tracking.hpp"

#include <cassert>
#include <fstream>
#include <limits>
#include <memory>

namespace {

mc::entity::EntityTypeRegistry loaded_registry() {
    mc::entity::EntityTypeRegistry registry;
    std::ifstream input(MC_RUNTIME_REGISTRIES_PATH);
    assert(input);
    const auto report = registry.load_normalized(input);
    assert(report.encountered == 158);
    assert(registry.size() == 158);
    return registry;
}

void test_entity_registry() {
    const auto registry = loaded_registry();
    assert(registry.by_protocol_id(0).name().to_string() == "minecraft:acacia_boat");
    assert(registry.by_name("zombie").properties().category == mc::entity::EntityCategory::monster);
    assert(registry.by_name("cow").properties().category == mc::entity::EntityCategory::creature);
    assert(registry.by_name("cod").properties().aquatic);
    assert(!registry.by_name("cod").properties().affected_by_gravity);
    assert(registry.by_name("drowned").properties().aquatic);
}

void test_entity_lifecycle() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 1234);
    auto& cow = entities.spawn("cow", {1.0, 10.0, 2.0});
    auto& zombie = entities.spawn("zombie", {4.0, 10.0, 2.0});
    assert(cow.id() != zombie.id());
    assert(cow.uuid() != zombie.uuid());
    assert(entities.count(mc::entity::EntityCategory::creature) == 1);
    assert(entities.count(mc::entity::EntityCategory::monster) == 1);

    cow.set_velocity({0.2, 0.0, 0.0});
    entities.tick(0.05);
    assert(cow.position().x > 1.0);
    assert(cow.position().y < 10.0);
    assert(cow.tick_count() == 1);
    cow.set_velocity({1.0, -0.5, 1.0});
    cow.apply_water_physics(0.8, 0.02);
    assert(cow.velocity().x == 0.8);
    assert(cow.velocity().y == -0.38);
    assert(entities.query({{0.0, 0.0, 0.0}, {3.0, 20.0, 4.0}}).size() == 1);

    auto* living = dynamic_cast<mc::entity::LivingEntity*>(entities.find(zombie.id()));
    assert(living != nullptr);
    assert(living->damage(living->max_health()));
    entities.tick(0.05);
    assert(entities.find(zombie.id()) == nullptr);
    assert(entities.size() == 1);
}

void test_death_experience_events() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 1235);
    auto& zombie = dynamic_cast<mc::entity::LivingEntity&>(
        entities.spawn("zombie", {4.0, 10.0, 2.0}));
    const auto id = zombie.id();
    assert(zombie.damage(zombie.max_health()));
    entities.tick(0.05);
    assert(entities.find(id) == nullptr);
    const auto deaths = entities.drain_deaths();
    assert(deaths.size() == 1);
    assert(deaths.front().entity_id == id);
    assert(deaths.front().position.x == 4.0);
    assert(deaths.front().type == "zombie");
    assert(deaths.front().experience == 5);
    assert(entities.drain_deaths().empty());
}

class ToggleGoal final : public mc::entity::Goal {
public:
    ToggleGoal(std::uint32_t priority, bool& enabled, std::uint32_t& ticks)
        : Goal(priority, mc::entity::GoalControl::move), enabled_(&enabled), ticks_(&ticks) {}
    bool can_start(mc::entity::Entity&, mc::entity::Brain&) override { return *enabled_; }
    void tick(mc::entity::Entity&, mc::entity::Brain&, double) override { ++(*ticks_); }

private:
    bool* enabled_;
    std::uint32_t* ticks_;
};

void test_goal_arbitration_and_navigation() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 55);
    auto& cow = entities.spawn("cow", {0.5, 0.0, 0.5});
    mc::entity::Brain brain;
    mc::entity::GoalSelector selector;
    bool high_enabled = true;
    bool low_enabled = true;
    std::uint32_t high_ticks = 0;
    std::uint32_t low_ticks = 0;
    selector.add(std::make_unique<ToggleGoal>(0, high_enabled, high_ticks));
    selector.add(std::make_unique<ToggleGoal>(1, low_enabled, low_ticks));
    selector.tick(cow, brain, 0.05);
    assert(high_ticks == 1 && low_ticks == 0);
    high_enabled = false;
    selector.tick(cow, brain, 0.05);
    assert(low_ticks == 1);

    const mc::entity::Navigation navigation;
    const auto path = navigation.find_path(
        {0, 0, 0}, {2, 0, 0},
        [](const mc::core::BlockPosition position) {
            return position != mc::core::BlockPosition{1, 0, 0};
        });
    constexpr mc::core::BlockPosition expected_start{0, 0, 0};
    constexpr mc::core::BlockPosition expected_end{2, 0, 0};
    assert(path.front() == expected_start);
    assert(path.back() == expected_end);
    assert(path.size() == 5);
    assert(navigation.steer(cow, path, 0.2));
}

void test_melee_goal() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 77);
    auto& zombie = entities.spawn("zombie", {0.0, 0.0, 0.0});
    auto& cow = entities.spawn("cow", {1.0, 0.0, 0.0});
    auto* living_cow = dynamic_cast<mc::entity::LivingEntity*>(&cow);
    assert(living_cow != nullptr);
    const auto initial_health = living_cow->health();
    mc::entity::Brain brain;
    brain.set(mc::entity::MemoryKey::target_entity, cow.id());
    mc::entity::GoalSelector selector;
    selector.add(std::make_unique<mc::entity::MeleeAttackGoal>(0, entities, 2.0F, 0.2));
    selector.tick(zombie, brain, 0.05);
    assert(living_cow->health() == initial_health - 2.0F);
    assert(living_cow->velocity().x > 0.0);
    assert(living_cow->velocity().y == 0.4);
    assert(living_cow->velocity().z == 0.0);

    const auto movement = living_cow->velocity();
    assert(!living_cow->knockback(living_cow->position(), 0.4));
    assert(living_cow->velocity() == movement);
}

void test_panic_and_flocking_goals() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 80);
    auto& first = entities.spawn("cow", {1.0, 0.0, 0.0});
    static_cast<void>(entities.spawn("cow", {8.0, 0.0, 0.0}));
    mc::entity::Brain panic_brain;
    panic_brain.set(mc::entity::MemoryKey::panic, true);
    panic_brain.set(mc::entity::MemoryKey::last_seen_position, mc::entity::Vec3{0.0, 0.0, 0.0});
    mc::entity::GoalSelector panic_selector;
    panic_selector.add(std::make_unique<mc::entity::PanicGoal>(0, 0.4, 2));
    panic_selector.tick(first, panic_brain, 0.05);
    assert(first.velocity().x > 0.0);
    panic_selector.tick(first, panic_brain, 0.05);
    panic_selector.tick(first, panic_brain, 0.05);
    assert(!panic_brain.contains(mc::entity::MemoryKey::panic));

    first.set_velocity({});
    mc::entity::Brain flock_brain;
    mc::entity::GoalSelector flock_selector;
    flock_selector.add(std::make_unique<mc::entity::FlockGoal>(0, entities, 0.2, 10.0, 3.0));
    flock_selector.tick(first, flock_brain, 0.05);
    assert(first.velocity().x > 0.0);
    assert(flock_brain.contains(mc::entity::MemoryKey::flock_leader));

    auto& cod = entities.spawn("cod", {0.0, 10.0, 0.0});
    static_cast<void>(entities.spawn("cod", {0.0, 10.0, 8.0}));
    mc::entity::MobAiSystem ai(entities, 81);
    assert(ai.attach(cod.id()));
    ai.tick(0.05);
    assert(cod.velocity().z > 0.0);

    assert(ai.attach(first.id()));
    assert(ai.notify_damage(first.id(), {0.0, 0.0, 0.0}));
    ai.tick(0.05);
    assert(first.velocity().x > 0.0);

    first.set_velocity({});
    assert(ai.tempt(first.id(), {10.0, 0.0, 0.0}));
    ai.tick(0.05);
    assert(first.velocity().x > 0.0);

    auto& owned = entities.spawn("cow", {0.0, 0.0, 0.0});
    assert(ai.attach(owned.id()));
    assert(ai.follow_owner(owned.id(), {-10.0, 0.0, 0.0}));
    ai.tick(0.05);
    assert(owned.velocity().x < 0.0);
    assert(ai.set_suspended(owned.id(), true));
    assert(owned.velocity() == mc::entity::Vec3{});
    ai.tick(0.05);
    assert(owned.velocity() == mc::entity::Vec3{});
    assert(ai.set_suspended(owned.id(), false));
    ai.tick(0.05);
    assert(owned.velocity().x < 0.0);

    mc::entity::Brain owner_brain;
    mc::entity::GoalSelector owner_selector;
    owner_selector.add(std::make_unique<mc::entity::OwnerFollowGoal>(0, 0.2, 3.0, 2.0));
    first.set_position({0.0, 0.0, 0.0});
    first.set_velocity({});
    owner_brain.set(mc::entity::MemoryKey::owner_position,
                    mc::entity::Vec3{4.0, 0.0, 0.0});
    owner_selector.tick(first, owner_brain, 0.05);
    assert(first.velocity().x > 0.0);
    first.set_position({2.5, 0.0, 0.0});
    owner_selector.tick(first, owner_brain, 0.05);
    assert(first.velocity() == mc::entity::Vec3{});
}

void test_damage_context_and_invulnerability() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 78);
    auto& attacker = entities.spawn("zombie", {0.0, 0.0, 0.0});
    auto& target = dynamic_cast<mc::entity::LivingEntity&>(
        entities.spawn("cow", {1.0, 0.0, 0.0}));

    assert(target.damage(
        2.0F, {mc::entity::DamageType::melee, attacker.id(), false}));
    assert(target.last_damage_source()->type == mc::entity::DamageType::melee);
    assert(target.last_damage_source()->attacker == attacker.id());
    assert(target.hurt_invulnerability_ticks() == 10);
    const auto health_after_melee = target.health();

    assert(!target.damage(1.0F, {mc::entity::DamageType::fire, std::nullopt, false}));
    assert(target.health() == health_after_melee);
    assert(target.damage(1.0F, {mc::entity::DamageType::fire, std::nullopt, true}));
    assert(target.last_damage_source()->type == mc::entity::DamageType::fire);
    assert(target.hurt_invulnerability_ticks() == 10);

    entities.tick(0.45);
    assert(target.hurt_invulnerability_ticks() == 1);
    assert(!target.damage(1.0F));
    entities.tick(0.05);
    assert(target.hurt_invulnerability_ticks() == 0);
    assert(target.damage(1.0F, {mc::entity::DamageType::projectile, attacker.id(), false}));
}

void test_armor_mitigation() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 79);
    auto& target = dynamic_cast<mc::entity::LivingEntity&>(
        entities.spawn("cow", {0.0, 0.0, 0.0}));
    target.set_attribute(mc::core::ResourceLocation::parse("minecraft:armor"), 20.0);
    target.set_attribute(
        mc::core::ResourceLocation::parse("minecraft:armor_toughness"), 0.0);

    assert(!target.damage(std::numeric_limits<float>::quiet_NaN()));
    assert(target.damage(5.0F));
    assert(target.health() == 8.5F);
    assert(target.damage(
        1.0F, {mc::entity::DamageType::void_damage, std::nullopt, true, true}));
    assert(target.health() == 7.5F);
}

void test_spawning_and_default_ai() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 91);
    mc::entity::NaturalSpawner spawner(registry, 1001);
    const std::vector<mc::entity::Vec3> candidates{
        {200.0, 0.0, 200.0}, {202.0, 0.0, 200.0}, {204.0, 0.0, 200.0}};
    const auto monsters = spawner.spawn_cycle(
        entities, mc::entity::EntityCategory::monster, candidates, 0, false, 3);
    assert(monsters.size() == 3);
    assert(entities.count(mc::entity::EntityCategory::monster) == 3);
    assert(spawner.spawn_cycle(
        entities, mc::entity::EntityCategory::monster, candidates, 0, true, 3).empty());

    auto& cow = entities.spawn("cow", {201.0, 0.0, 200.0});
    mc::entity::MobAiSystem ai(entities, 44);
    assert(ai.attach(monsters.front()));
    assert(ai.attach(cow.id()));
    assert(ai.set_target(monsters.front(), cow.id()));
    const auto health = dynamic_cast<mc::entity::LivingEntity&>(cow).health();
    ai.tick(0.05);
    assert(dynamic_cast<mc::entity::LivingEntity&>(cow).health() < health);

    assert(spawner.despawn_distant(entities, {{0.0, 0.0, 0.0}}) == 3);
    assert(entities.find(cow.id()) != nullptr);
}

void test_biome_and_habitat_spawning() {
    const auto registry = loaded_registry();
    const auto spawn_names = [&](const mc::entity::EntityCategory category,
                                 const std::vector<mc::entity::SpawnCandidate>& candidates,
                                 const std::uint8_t light) {
        mc::entity::EntityManager entities(registry, 123, 2);
        mc::entity::NaturalSpawner spawner(registry, 456);
        const auto spawned = spawner.spawn_cycle(
            entities, category, candidates, light, false, candidates.size());
        std::vector<std::string> names;
        for (const auto id : spawned) {
            names.push_back(entities.find(id)->type().name().to_string());
        }
        return names;
    };

    const std::vector plains = {
        mc::entity::SpawnCandidate{{0.0, 64.0, 0.0}, mc::world::BiomeId::plains,
                                   mc::entity::SpawnHabitat::surface_land},
        mc::entity::SpawnCandidate{{2.0, 64.0, 0.0}, mc::world::BiomeId::plains,
                                   mc::entity::SpawnHabitat::surface_land}};
    const auto plains_names = spawn_names(mc::entity::EntityCategory::creature, plains, 15);
    assert(plains_names.size() == 2);
    for (const auto& name : plains_names) {
        assert(name == "minecraft:cow" || name == "minecraft:sheep" ||
               name == "minecraft:pig" || name == "minecraft:chicken" ||
               name == "minecraft:horse");
    }

    const std::vector desert = {
        mc::entity::SpawnCandidate{{0.0, 64.0, 0.0}, mc::world::BiomeId::desert,
                                   mc::entity::SpawnHabitat::surface_land}};
    const auto desert_names = spawn_names(mc::entity::EntityCategory::creature, desert, 15);
    assert(desert_names.size() == 1);
    assert(desert_names.front() == "minecraft:camel" ||
           desert_names.front() == "minecraft:rabbit");

    const std::vector mountains = {
        mc::entity::SpawnCandidate{{0.0, 100.0, 0.0}, mc::world::BiomeId::mountains,
                                   mc::entity::SpawnHabitat::surface_land}};
    const auto mountain_names = spawn_names(mc::entity::EntityCategory::creature, mountains, 15);
    assert(mountain_names.size() == 1);
    assert(mountain_names.front() == "minecraft:goat" ||
           mountain_names.front() == "minecraft:sheep" ||
           mountain_names.front() == "minecraft:rabbit");

    const std::vector ocean = {
        mc::entity::SpawnCandidate{{0.0, 61.0, 0.0}, mc::world::BiomeId::ocean,
                                   mc::entity::SpawnHabitat::water},
        mc::entity::SpawnCandidate{{2.0, 61.0, 0.0}, mc::world::BiomeId::ocean,
                                   mc::entity::SpawnHabitat::water}};
    const auto fish_names = spawn_names(mc::entity::EntityCategory::water_ambient, ocean, 15);
    assert(fish_names.size() == 2);
    for (const auto& name : fish_names) {
        assert(name == "minecraft:cod" || name == "minecraft:salmon" ||
               name == "minecraft:squid" || name == "minecraft:tropical_fish");
    }
    assert(spawn_names(mc::entity::EntityCategory::creature, ocean, 15).empty());
    const auto drowned = spawn_names(mc::entity::EntityCategory::monster, ocean, 0);
    assert(drowned.size() == 2);
    assert(drowned.front() == "minecraft:drowned");
}

void test_animal_breeding_and_taming() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 500);
    auto& first = entities.spawn("cow", {0.0, 0.0, 0.0});
    auto& second = entities.spawn("cow", {2.0, 0.0, 0.0});
    mc::entity::AnimalSystem animals;
    assert(animals.attach(first));
    assert(animals.attach(second));
    assert(animals.set_variant(first.id(), "warm"));
    assert(animals.accepts_food(first, "wheat"));
    assert(!animals.accepts_food(first, "carrot"));
    assert(animals.set_in_love(first.id()));
    assert(animals.set_in_love(second.id()));
    const auto child_id = animals.breed(first.id(), second.id(), entities);
    assert(child_id.has_value());
    assert(animals.state(*child_id)->baby());
    assert(animals.state(*child_id)->variant == "warm");
    assert(!animals.state(first.id())->in_love());

    const auto owner = first.uuid();
    assert(animals.tame(*child_id, owner));
    assert(animals.state(*child_id)->owner == owner);
    assert(animals.toggle_sitting(*child_id, owner));
    assert(animals.state(*child_id)->sitting);
    assert(animals.toggle_sitting(*child_id, owner));
    assert(!animals.state(*child_id)->sitting);
    animals.tick(entities, 1'200.0);
    assert(animals.state(*child_id)->adult());
    assert(animals.state(first.id())->adult());
}

void test_entity_relationships() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 700);
    auto& first = entities.spawn("cow", {0.0, 0.0, 0.0});
    auto& second = entities.spawn("cow", {0.0, 0.0, 0.0});
    auto& third = entities.spawn("cow", {0.0, 0.0, 0.0});
    assert(entities.mount(first.id(), second.id()));
    assert(entities.mount(second.id(), third.id()));
    assert(!entities.mount(third.id(), first.id()));
    assert(first.vehicle() == second.id());
    assert(second.passengers().front() == first.id());
    assert(entities.set_leash(first.id(), third.id()));
    first.set_team("red");
    assert(first.team() == "red");

    assert(entities.add_external_passenger(third.id(), 10'000));
    assert(third.passengers().back() == 10'000);
    assert(!entities.add_external_passenger(third.id(), first.id()));
    assert(entities.remove_external_passenger(third.id(), 10'000));
    assert(!entities.remove_external_passenger(third.id(), 10'000));

    assert(entities.remove(second.id()));
    assert(!first.vehicle().has_value());
    assert(!third.passengers().size());
    assert(entities.remove(third.id()));
    assert(!first.leash_holder().has_value());
}

void test_equipment_and_effects() {
    const auto registry = loaded_registry();
    mc::item::ItemRegistry items;
    mc::entity::EntityManager entities(registry, 808);
    auto& cow = entities.spawn("cow", {0.0, 0.0, 0.0});
    auto& living = dynamic_cast<mc::entity::LivingEntity&>(cow);
    living.equip(
        mc::entity::LivingEntity::EquipmentSlot::main_hand,
        mc::item::ItemStack(items.by_name("wooden_pickaxe").id(), 1),
        items);
    assert(!living.equipment(mc::entity::LivingEntity::EquipmentSlot::main_hand).empty());
    living.apply_effect(
        mc::core::ResourceLocation::parse("minecraft:speed"),
        {1, 2, false, true});
    entities.tick(0.05);
    assert(living.effects().size() == 1);
    entities.tick(0.05);
    assert(living.effects().empty());
}

void test_active_effects_and_resistances() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 809);
    auto& living = dynamic_cast<mc::entity::LivingEntity&>(
        entities.spawn("cow", {1.0, 0.0, 0.0}));
    assert(living.damage(4.0F));
    living.apply_effect(
        mc::core::ResourceLocation::parse("minecraft:regeneration"),
        {0, 50, false, true});
    for (int tick = 0; tick < 50; ++tick) entities.tick(0.05);
    assert(living.health() == 7.0F);

    living.apply_effect(
        mc::core::ResourceLocation::parse("minecraft:poison"),
        {0, 25, false, true});
    for (int tick = 0; tick < 25; ++tick) entities.tick(0.05);
    assert(living.health() == 6.0F);

    living.apply_effect(
        mc::core::ResourceLocation::parse("minecraft:speed"),
        {1, 100, false, true});
    assert(std::abs(living.movement_speed() - 0.28) < 1.0e-6);
    living.apply_effect(
        mc::core::ResourceLocation::parse("minecraft:slowness"),
        {0, 100, false, true});
    assert(living.movement_speed() < 0.28);

    living.apply_effect(
        mc::core::ResourceLocation::parse("minecraft:fire_resistance"),
        {0, 100, false, true});
    assert(!living.damage(
        2.0F, {mc::entity::DamageType::fire, std::nullopt, true, true}));

    living.set_attribute(
        mc::core::ResourceLocation::parse("minecraft:knockback_resistance"), 1.0);
    assert(!living.knockback({0.0, 0.0, 0.0}, 0.4));
}

void test_snapshot_round_trip() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager source(registry, 999);
    auto& rider = source.spawn("cow", {1.0, 2.0, 3.0});
    auto& vehicle = source.spawn("cow", {4.0, 5.0, 6.0});
    rider.set_velocity({0.1, 0.2, 0.3});
    rider.set_rotation(45.0F, 20.0F);
    rider.set_metadata(7, std::string("value"));
    rider.set_team("blue");
    auto& living = dynamic_cast<mc::entity::LivingEntity&>(rider);
    assert(living.damage(3.0F));
    assert(source.mount(rider.id(), vehicle.id()));
    assert(source.set_leash(rider.id(), vehicle.id()));

    const auto snapshots = source.snapshots();
    mc::entity::EntityManager restored(registry, 1000);
    restored.restore(snapshots);
    const auto* restored_rider = restored.find(rider.id());
    assert(restored_rider != nullptr);
    assert(restored_rider->uuid() == rider.uuid());
    assert(restored_rider->position() == rider.position());
    assert(restored_rider->vehicle() == vehicle.id());
    assert(restored_rider->leash_holder() == vehicle.id());
    assert(restored_rider->team() == "blue");
    assert(std::get<std::string>(restored_rider->metadata().at(7)) == "value");
    assert(dynamic_cast<const mc::entity::LivingEntity*>(restored_rider)->health() == living.health());
}

void test_all_classified_living_types() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 1'234'567);
    mc::entity::MobAiSystem ai(entities, 7'654'321);
    std::size_t living_count = 0;
    for (const auto& type : registry.types()) {
        if (type.properties().max_health <= 0.0F) {
            continue;
        }
        auto& entity = entities.spawn(
            type.name().to_string(), {static_cast<double>(living_count * 3), 1.0, 0.0});
        assert(dynamic_cast<mc::entity::LivingEntity*>(&entity) != nullptr);
        assert(ai.attach(entity.id()));
        ++living_count;
    }
    assert(living_count == 92);
    ai.tick(0.05);
    entities.tick(0.05);
    assert(entities.size() == living_count);
    assert(ai.size() == living_count);
}

void test_entity_tracking() {
    const auto registry = loaded_registry();
    mc::entity::EntityManager entities(registry, 444);
    auto& cow = entities.spawn("cow", {1.0, 0.0, 0.0});
    mc::entity::EntityTracker tracker;
    auto update = tracker.update(cow, {0.0, 0.0, 0.0});
    assert(update.entered && tracker.visible(cow.id()));
    assert(!tracker.update(cow, {0.0, 0.0, 0.0}).position);

    cow.set_position({2.0, 0.0, 0.0});
    cow.set_velocity({0.1, 0.0, 0.0});
    cow.set_rotation(30.0F, 5.0F);
    update = tracker.update(cow, {0.0, 0.0, 0.0});
    assert(update.position && !update.absolute_position);
    assert(update.delta.x == 1.0);
    assert(update.rotation && update.velocity && update.head_rotation);

    cow.set_position({20.0, 0.0, 0.0});
    update = tracker.update(cow, {0.0, 0.0, 0.0});
    assert(update.position && update.absolute_position);
    update = tracker.update(cow, {100.0, 0.0, 0.0});
    assert(update.left && !tracker.visible(cow.id()));
}

void test_projectile_lifecycle() {
    const auto registry = loaded_registry();
    mc::item::ItemRegistry items;
    mc::entity::EntityManager entities(registry, 445, 2);
    mc::entity::ProjectileSystem projectiles;
    mc::world::World world({{6060, 63}, 8, std::nullopt});
    const auto surface = world.surface_height(0, 0);
    auto& target = dynamic_cast<mc::entity::LivingEntity&>(
        entities.spawn("cow", {1.0, static_cast<double>(surface + 2), 0.0}));
    const auto projectile_id = projectiles.spawn(
        entities, "arrow", {0.6, static_cast<double>(surface + 2.5), 0.0},
        {0.4, 0.0, 0.0}, 1, 2.0F, 20);
    entities.tick(0.05);
    const auto impacts = projectiles.tick(entities, world);
    assert(impacts.size() == 1);
    assert(impacts.front().projectile == projectile_id);
    assert(impacts.front().target == target.id());
    assert(target.health() == target.max_health() - 2.0F);
    assert(entities.find(projectile_id) == nullptr);

    const auto arrow_item = items.by_name("arrow").id();
    const auto obstacle_y = surface + 10;
    world.set_block({0, obstacle_y, 0}, mc::world::BlockId::stone);
    const auto blocked = projectiles.spawn(
        entities, "arrow", {0.2, static_cast<double>(obstacle_y), 0.2},
        {}, 1, 2.0F, 20, mc::item::ItemStack(arrow_item, 1));
    const auto blocked_impacts = projectiles.tick(entities, world);
    assert(blocked_impacts.size() == 1);
    assert(blocked_impacts.front().projectile == blocked);
    assert(blocked_impacts.front().recovery->item_id() == arrow_item);
}

void test_dropped_item_lifecycle() {
    const auto registry = loaded_registry();
    mc::item::ItemRegistry items;
    mc::entity::EntityManager entities(registry, 446, 2);
    mc::entity::DroppedItemSystem drops;
    const auto dirt = items.by_name("dirt").id();
    const auto first = drops.spawn(
        entities, mc::item::ItemStack(dirt, 2), {0.0, 1.0, 0.0}, 2, 20);
    static_cast<void>(drops.spawn(
        entities, mc::item::ItemStack(dirt, 3), {0.2, 1.0, 0.0}, 2, 20));
    auto update = drops.tick(entities, items, mc::entity::Vec3{0.0, 1.0, 0.0});
    assert(update.pickups.empty());
    assert(drops.size() == 1);
    assert(drops.stack(first)->count() == 5);
    update = drops.tick(entities, items, mc::entity::Vec3{0.0, 1.0, 0.0});
    assert(update.pickups.size() == 1);
    assert(update.pickups.front().stack.count() == 5);
    assert(drops.size() == 0);

    const auto expiring = drops.spawn(
        entities, mc::item::ItemStack(dirt, 1), {5.0, 1.0, 0.0}, 0, 1);
    static_cast<void>(drops.tick(entities, items, std::nullopt));
    assert(entities.find(expiring) == nullptr);
}

} // namespace

int main() {
    test_entity_registry();
    test_entity_lifecycle();
    test_death_experience_events();
    test_goal_arbitration_and_navigation();
    test_melee_goal();
    test_panic_and_flocking_goals();
    test_damage_context_and_invulnerability();
    test_armor_mitigation();
    test_spawning_and_default_ai();
    test_biome_and_habitat_spawning();
    test_animal_breeding_and_taming();
    test_entity_relationships();
    test_equipment_and_effects();
    test_active_effects_and_resistances();
    test_snapshot_round_trip();
    test_all_classified_living_types();
    test_entity_tracking();
    test_projectile_lifecycle();
    test_dropped_item_lifecycle();
}