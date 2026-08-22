#include "mc/block/block.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <stdexcept>
#include <utility>

namespace mc::block {

Block::Block(const std::uint32_t id,
             mc::core::ResourceLocation name,
             BlockProperties properties,
             const BlockStateId default_state)
    : id_(id),
      name_(std::move(name)),
      properties_(std::move(properties)),
    default_state_(default_state),
    protocol_id_(std::nullopt) {}

std::uint32_t Block::id() const noexcept { return id_; }
const mc::core::ResourceLocation& Block::name() const noexcept { return name_; }
const BlockProperties& Block::properties() const noexcept { return properties_; }
BlockStateId Block::default_state() const noexcept { return default_state_; }
std::optional<std::uint32_t> Block::protocol_id() const noexcept { return protocol_id_; }

BlockRegistry::BlockRegistry() {
    register_block(mc::core::ResourceLocation::parse("minecraft:air"),
                   {0.0F, 0.0F, false, true, false, std::nullopt});
    register_block(mc::core::ResourceLocation::parse("minecraft:bedrock"),
                   {-1.0F, 3'600'000.0F, true, false, true, std::nullopt});
    register_block(mc::core::ResourceLocation::parse("minecraft:stone"),
                   {1.5F, 6.0F, true, false, true,
                    mc::core::ResourceLocation::parse("minecraft:cobblestone")});
    register_block(mc::core::ResourceLocation::parse("minecraft:dirt"),
                   {0.5F, 0.5F, true, false, false,
                    mc::core::ResourceLocation::parse("minecraft:dirt")});
    register_block(mc::core::ResourceLocation::parse("minecraft:grass_block"),
                   {0.6F, 0.6F, true, false, false,
                    mc::core::ResourceLocation::parse("minecraft:dirt")});
    register_block(mc::core::ResourceLocation::parse("minecraft:water"),
                   {100.0F, 100.0F, false, true, false, std::nullopt});
    register_block(mc::core::ResourceLocation::parse("minecraft:sand"),
                   {0.5F, 0.5F, true, false, false,
                    mc::core::ResourceLocation::parse("minecraft:sand")});
    register_block(mc::core::ResourceLocation::parse("minecraft:gravel"),
                   {0.6F, 0.6F, true, false, false,
                    mc::core::ResourceLocation::parse("minecraft:gravel")});
    register_block(mc::core::ResourceLocation::parse("minecraft:coal_ore"),
                   {3.0F, 3.0F, true, false, true,
                    mc::core::ResourceLocation::parse("minecraft:coal")});
    register_block(mc::core::ResourceLocation::parse("minecraft:iron_ore"),
                   {3.0F, 3.0F, true, false, true,
                    mc::core::ResourceLocation::parse("minecraft:raw_iron")});
    register_block(mc::core::ResourceLocation::parse("minecraft:oak_log"),
                   {2.0F, 2.0F, true, false, false,
                    mc::core::ResourceLocation::parse("minecraft:oak_log")});
    register_block(mc::core::ResourceLocation::parse("minecraft:oak_leaves"),
                   {0.2F, 0.2F, true, false, false, std::nullopt});
    register_block(mc::core::ResourceLocation::parse("minecraft:short_grass"),
                   {0.0F, 0.0F, false, true, false, std::nullopt});
    register_block(mc::core::ResourceLocation::parse("minecraft:dandelion"),
                   {0.0F, 0.0F, false, true, false,
                    mc::core::ResourceLocation::parse("minecraft:dandelion")});
    register_block(mc::core::ResourceLocation::parse("minecraft:poppy"),
                   {0.0F, 0.0F, false, true, false,
                    mc::core::ResourceLocation::parse("minecraft:poppy")});
}

const Block& BlockRegistry::by_id(const std::uint32_t id) const {
    if (id >= blocks_.size()) {
        throw std::out_of_range("block ID is not registered");
    }
    return blocks_[id];
}

const Block& BlockRegistry::by_name(const mc::core::ResourceLocation& name) const {
    const auto block = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto& candidate) {
        return candidate.name() == name;
    });
    if (block == blocks_.end()) {
        throw std::out_of_range("block name is not registered");
    }
    return *block;
}

const Block& BlockRegistry::by_name(const std::string_view name) const {
    return by_name(mc::core::ResourceLocation::parse(name));
}

const Block& BlockRegistry::by_protocol_id(const std::uint32_t id) const {
    const auto block = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto& candidate) {
        return candidate.protocol_id() == id;
    });
    if (block == blocks_.end()) {
        throw std::out_of_range("block protocol ID is not registered");
    }
    return *block;
}

std::size_t BlockRegistry::size() const noexcept { return blocks_.size(); }

const Block& BlockRegistry::register_block(mc::core::ResourceLocation name,
                                           BlockProperties properties) {
    if (std::any_of(blocks_.begin(), blocks_.end(), [&](const auto& block) {
            return block.name() == name;
        })) {
        throw std::invalid_argument("block name is already registered");
    }
    const auto id = static_cast<std::uint32_t>(blocks_.size());
    blocks_.emplace_back(id, std::move(name), std::move(properties), id);
    return blocks_.back();
}

BlockRegistryLoadReport BlockRegistry::load_normalized(std::istream& input) {
    std::string line;
    if (!std::getline(input, line) || !line.starts_with("MCREGISTRIES1\t")) {
        throw std::runtime_error("normalized registry stream has an invalid header");
    }
    BlockRegistryLoadReport report;
    while (std::getline(input, line)) {
        if (!line.starts_with("B\t")) {
            continue;
        }
        const auto first_tab = line.find('\t', 2);
        if (first_tab == std::string::npos) {
            throw std::runtime_error("normalized block registry record is invalid");
        }
        std::uint32_t protocol_id = 0;
        const auto id_text = std::string_view(line).substr(2, first_tab - 2);
        const auto [end, error] = std::from_chars(
            id_text.data(), id_text.data() + id_text.size(), protocol_id);
        if (error != std::errc{} || end != id_text.data() + id_text.size() ||
            protocol_id != report.encountered) {
            throw std::runtime_error("normalized block protocol ID is invalid");
        }
        ++report.encountered;
        const auto name = mc::core::ResourceLocation::parse(
            std::string_view(line).substr(first_tab + 1));
        const auto existing = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto& block) {
            return block.name() == name;
        });
        if (existing != blocks_.end()) {
            existing->protocol_id_ = protocol_id;
            ++report.existing;
        } else {
            auto properties = BlockProperties{};
            properties.drops = name;
            static_cast<void>(register_block(name, std::move(properties)));
            auto& block = blocks_.back();
            block.protocol_id_ = protocol_id;
            ++report.loaded;
        }
    }
    return report;
}

BlockInteraction::BlockInteraction(const BlockRegistry& blocks,
                                   const mc::item::ItemRegistry& items)
    : blocks_(&blocks), items_(&items) {}

bool BlockInteraction::place(mc::world::Chunk& chunk,
                             const std::size_t x,
                             const std::int32_t y,
                             const std::size_t z,
                             mc::item::ItemStack& held_stack) const {
    held_stack.validate(*items_);
    if (held_stack.empty()) {
        return false;
    }
    const auto& item = items_->by_id(held_stack.item_id());
    if (!item.properties().block_id) {
        return false;
    }
    const auto target_id = static_cast<std::uint32_t>(chunk.block(x, y, z));
    if (!blocks_->by_id(target_id).properties().replaceable) {
        return false;
    }
    const auto block_id = *item.properties().block_id;
    static_cast<void>(blocks_->by_id(block_id));
    chunk.set_block(x, y, z, static_cast<mc::world::BlockId>(block_id));
    static_cast<void>(held_stack.take(1));
    return true;
}

BreakResult BlockInteraction::break_block(mc::world::Chunk& chunk,
                                          const std::size_t x,
                                          const std::int32_t y,
                                          const std::size_t z,
                                          const ToolContext tool) const {
    const auto block_id = static_cast<std::uint32_t>(chunk.block(x, y, z));
    const auto& block = blocks_->by_id(block_id);
    if (block_id == 0 || block.properties().hardness < 0.0F) {
        return {};
    }
    const auto ticks = break_ticks(chunk, x, y, z, tool);

    BreakResult result{true, ticks, {}};
    const auto can_drop = !block.properties().requires_tool || tool.correct_for_drops;
    if (can_drop && block.properties().drops) {
        try {
            result.drops.emplace_back(items_->by_name(*block.properties().drops).id(), 1);
        } catch (const std::out_of_range&) {
        }
    }
    chunk.set_block(x, y, z, mc::world::BlockId::air);
    return result;
}

std::uint32_t BlockInteraction::break_ticks(const mc::world::Chunk& chunk,
                                            const std::size_t x,
                                            const std::int32_t y,
                                            const std::size_t z,
                                            const ToolContext tool) const {
    const auto block_id = static_cast<std::uint32_t>(chunk.block(x, y, z));
    const auto& block = blocks_->by_id(block_id);
    if (block_id == 0 || block.properties().hardness < 0.0F) return 0;
    const auto speed = std::max(tool.mining_speed, 0.001F);
    const auto divisor = block.properties().requires_tool && !tool.correct_for_drops
        ? speed
        : speed * 30.0F;
    return static_cast<std::uint32_t>(
        std::max(1.0F, std::ceil(block.properties().hardness * 30.0F / divisor)));
}

} // namespace mc::block