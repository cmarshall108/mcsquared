#include "mc/item/item.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace mc::item {

Item::Item(const std::uint32_t id,
           mc::core::ResourceLocation name,
           ItemProperties properties)
        : id_(id),
            name_(std::move(name)),
            properties_(properties),
            protocol_id_(std::nullopt) {
    if (properties_.max_stack_size == 0 || properties_.max_stack_size > 99) {
        throw std::invalid_argument("item stack limit must be from 1 to 99");
    }
    if (properties_.max_damage != 0 && properties_.max_stack_size != 1) {
        throw std::invalid_argument("damageable items must have stack size one");
    }
    if (properties_.nutrition > 20 || !std::isfinite(properties_.saturation_modifier) ||
        properties_.saturation_modifier < 0.0F) {
        throw std::invalid_argument("food properties are invalid");
    }
}

std::uint32_t Item::id() const noexcept { return id_; }
const mc::core::ResourceLocation& Item::name() const noexcept { return name_; }
const ItemProperties& Item::properties() const noexcept { return properties_; }
std::optional<std::uint32_t> Item::protocol_id() const noexcept { return protocol_id_; }

ItemRegistry::ItemRegistry() {
    register_item(mc::core::ResourceLocation::parse("minecraft:air"));
    register_item(mc::core::ResourceLocation::parse("minecraft:stone"), {64, 0, 2});
    register_item(mc::core::ResourceLocation::parse("minecraft:cobblestone"));
    register_item(mc::core::ResourceLocation::parse("minecraft:dirt"), {64, 0, 3});
    register_item(mc::core::ResourceLocation::parse("minecraft:grass_block"), {64, 0, 4});
    register_item(mc::core::ResourceLocation::parse("minecraft:sand"), {64, 0, 6});
    register_item(mc::core::ResourceLocation::parse("minecraft:gravel"), {64, 0, 7});
    register_item(mc::core::ResourceLocation::parse("minecraft:coal"));
    register_item(mc::core::ResourceLocation::parse("minecraft:raw_iron"));
    register_item(mc::core::ResourceLocation::parse("minecraft:iron_ingot"));
    register_item(mc::core::ResourceLocation::parse("minecraft:oak_log"), {64, 0, 10});
    register_item(mc::core::ResourceLocation::parse("minecraft:oak_planks"));
    register_item(mc::core::ResourceLocation::parse("minecraft:stick"));
    register_item(mc::core::ResourceLocation::parse("minecraft:crafting_table"));
    register_item(mc::core::ResourceLocation::parse("minecraft:torch"));
    register_item(mc::core::ResourceLocation::parse("minecraft:wooden_pickaxe"), {1, 59, std::nullopt});
    register_item(mc::core::ResourceLocation::parse("minecraft:bread"),
                  {64, 0, std::nullopt, 5, 0.6F});
    register_item(mc::core::ResourceLocation::parse("minecraft:bow"),
                  {1, 384, std::nullopt});
    register_item(mc::core::ResourceLocation::parse("minecraft:arrow"));
    register_item(mc::core::ResourceLocation::parse("minecraft:shield"),
                  {1, 336, std::nullopt});
}

const Item& ItemRegistry::by_id(const std::uint32_t id) const {
    if (id >= items_.size()) {
        throw std::out_of_range("item ID is not registered");
    }
    return items_[id];
}

const Item& ItemRegistry::by_name(const mc::core::ResourceLocation& name) const {
    const auto item = std::find_if(items_.begin(), items_.end(), [&](const auto& candidate) {
        return candidate.name() == name;
    });
    if (item == items_.end()) {
        throw std::out_of_range("item name is not registered");
    }
    return *item;
}

const Item& ItemRegistry::by_name(const std::string_view name) const {
    return by_name(mc::core::ResourceLocation::parse(name));
}

const Item& ItemRegistry::by_protocol_id(const std::uint32_t id) const {
    const auto item = std::find_if(items_.begin(), items_.end(), [&](const auto& candidate) {
        return candidate.protocol_id() == id;
    });
    if (item == items_.end()) {
        throw std::out_of_range("item protocol ID is not registered");
    }
    return *item;
}

std::size_t ItemRegistry::size() const noexcept { return items_.size(); }

const Item& ItemRegistry::register_item(mc::core::ResourceLocation name,
                                        const ItemProperties properties) {
    if (std::any_of(items_.begin(), items_.end(), [&](const auto& item) {
            return item.name() == name;
        })) {
        throw std::invalid_argument("item name is already registered");
    }
    const auto id = static_cast<std::uint32_t>(items_.size());
    items_.emplace_back(id, std::move(name), properties);
    return items_.back();
}

ItemRegistryLoadReport ItemRegistry::load_normalized(std::istream& input) {
    std::string line;
    if (!std::getline(input, line) || !line.starts_with("MCREGISTRIES1\t")) {
        throw std::runtime_error("normalized registry stream has an invalid header");
    }
    ItemRegistryLoadReport report;
    while (std::getline(input, line)) {
        if (!line.starts_with("I\t")) {
            continue;
        }
        const auto first_tab = line.find('\t', 2);
        if (first_tab == std::string::npos) {
            throw std::runtime_error("normalized item registry record is invalid");
        }
        std::uint32_t protocol_id = 0;
        const auto id_text = std::string_view(line).substr(2, first_tab - 2);
        const auto [end, error] = std::from_chars(
            id_text.data(), id_text.data() + id_text.size(), protocol_id);
        if (error != std::errc{} || end != id_text.data() + id_text.size() ||
            protocol_id != report.encountered) {
            throw std::runtime_error("normalized item protocol ID is invalid");
        }
        ++report.encountered;
        const auto name = mc::core::ResourceLocation::parse(
            std::string_view(line).substr(first_tab + 1));
        const auto existing = std::find_if(items_.begin(), items_.end(), [&](const auto& item) {
            return item.name() == name;
        });
        if (existing != items_.end()) {
            existing->protocol_id_ = protocol_id;
            ++report.existing;
        } else {
            static_cast<void>(register_item(name));
            auto& item = items_.back();
            item.protocol_id_ = protocol_id;
            ++report.loaded;
        }
    }
    return report;
}

ItemStack::ItemStack(const std::uint32_t item_id,
                     const std::uint16_t count,
                     const std::uint16_t damage,
                     DataComponents components)
    : item_id_(item_id),
      count_(count),
      damage_(damage),
      components_(std::move(components)) {
    normalize();
}

bool ItemStack::empty() const noexcept { return count_ == 0; }
std::uint32_t ItemStack::item_id() const noexcept { return item_id_; }
std::uint16_t ItemStack::count() const noexcept { return count_; }
std::uint16_t ItemStack::damage() const noexcept { return damage_; }
const DataComponents& ItemStack::components() const noexcept { return components_; }

bool ItemStack::can_merge(const ItemStack& other) const noexcept {
    return !empty() && !other.empty() && item_id_ == other.item_id_ &&
        damage_ == other.damage_ && components_ == other.components_;
}

std::uint16_t ItemStack::insert_from(ItemStack& source, const ItemRegistry& registry) {
    source.validate(registry);
    if (source.empty()) {
        return 0;
    }
    const auto& item = registry.by_id(source.item_id_);
    if (empty()) {
        item_id_ = source.item_id_;
        damage_ = source.damage_;
        components_ = source.components_;
    } else if (!can_merge(source)) {
        return source.count_;
    }
    const auto capacity = static_cast<std::uint16_t>(item.properties().max_stack_size - count_);
    const auto moved = std::min(capacity, source.count_);
    count_ = static_cast<std::uint16_t>(count_ + moved);
    source.count_ = static_cast<std::uint16_t>(source.count_ - moved);
    source.normalize();
    return source.count_;
}

ItemStack ItemStack::take(const std::uint16_t requested) {
    const auto removed = std::min(requested, count_);
    ItemStack result(item_id_, removed, damage_, components_);
    count_ = static_cast<std::uint16_t>(count_ - removed);
    normalize();
    return result;
}

bool ItemStack::apply_damage(const std::uint16_t amount, const ItemRegistry& registry) {
    validate(registry);
    if (empty() || amount == 0) return false;
    const auto maximum = registry.by_id(item_id_).properties().max_damage;
    if (maximum == 0) return false;
    const auto next = static_cast<std::uint32_t>(damage_) + amount;
    if (next >= maximum) {
        count_ = 0;
        normalize();
        return true;
    }
    damage_ = static_cast<std::uint16_t>(next);
    return false;
}

void ItemStack::validate(const ItemRegistry& registry) const {
    if (empty()) {
        if (item_id_ != 0 || damage_ != 0 || !components_.empty()) {
            throw std::invalid_argument("empty stack is not normalized");
        }
        return;
    }
    if (item_id_ == 0) {
        throw std::invalid_argument("air cannot form a non-empty item stack");
    }
    const auto& item = registry.by_id(item_id_);
    if (count_ > item.properties().max_stack_size) {
        throw std::invalid_argument("stack exceeds item limit");
    }
    if (damage_ > item.properties().max_damage) {
        throw std::invalid_argument("stack damage exceeds item durability");
    }
}

void ItemStack::normalize() noexcept {
    if (count_ == 0) {
        item_id_ = 0;
        damage_ = 0;
        components_.clear();
    }
}

} // namespace mc::item