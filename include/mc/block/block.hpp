#pragma once

#include "mc/core/types.hpp"
#include "mc/item/item.hpp"
#include "mc/world/chunk.hpp"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <string_view>
#include <vector>

namespace mc::block {

using BlockStateId = std::uint32_t;

struct BlockProperties final {
	float hardness{0.0F};
	float blast_resistance{0.0F};
	bool solid{true};
	bool replaceable{false};
	bool requires_tool{false};
	std::optional<mc::core::ResourceLocation> drops;
};

struct BlockState final {
	BlockStateId id;
	std::uint32_t block_id;
};

class Block final {
public:
	Block(std::uint32_t id,
		  mc::core::ResourceLocation name,
		  BlockProperties properties,
		  BlockStateId default_state);

	[[nodiscard]] std::uint32_t id() const noexcept;
	[[nodiscard]] const mc::core::ResourceLocation& name() const noexcept;
	[[nodiscard]] const BlockProperties& properties() const noexcept;
	[[nodiscard]] BlockStateId default_state() const noexcept;
	[[nodiscard]] std::optional<std::uint32_t> protocol_id() const noexcept;

private:
	friend class BlockRegistry;

	std::uint32_t id_;
	mc::core::ResourceLocation name_;
	BlockProperties properties_;
	BlockStateId default_state_;
	std::optional<std::uint32_t> protocol_id_;
};

class BlockEntity;

struct BlockRegistryLoadReport final {
	std::size_t encountered{0};
	std::size_t loaded{0};
	std::size_t existing{0};
};

class BlockRegistry final {
public:
	BlockRegistry();

	[[nodiscard]] const Block& by_id(std::uint32_t id) const;
	[[nodiscard]] const Block& by_name(const mc::core::ResourceLocation& name) const;
	[[nodiscard]] const Block& by_name(std::string_view name) const;
	[[nodiscard]] const Block& by_protocol_id(std::uint32_t id) const;
	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] BlockRegistryLoadReport load_normalized(std::istream& input);

	const Block& register_block(mc::core::ResourceLocation name,
								BlockProperties properties);

private:
	std::vector<Block> blocks_;
};

struct ToolContext final {
	float mining_speed{1.0F};
	bool correct_for_drops{false};
};

struct BreakResult final {
	bool broken{false};
	std::uint32_t ticks{0};
	std::vector<mc::item::ItemStack> drops;
};

class BlockInteraction final {
public:
	BlockInteraction(const BlockRegistry& blocks, const mc::item::ItemRegistry& items);

	[[nodiscard]] bool place(mc::world::Chunk& chunk,
							 std::size_t x,
							 std::int32_t y,
							 std::size_t z,
							 mc::item::ItemStack& held_stack) const;
	[[nodiscard]] BreakResult break_block(mc::world::Chunk& chunk,
										  std::size_t x,
										  std::int32_t y,
										  std::size_t z,
										  ToolContext tool) const;
	[[nodiscard]] std::uint32_t break_ticks(const mc::world::Chunk& chunk,
										  std::size_t x,
										  std::int32_t y,
										  std::size_t z,
										  ToolContext tool) const;

private:
	const BlockRegistry* blocks_;
	const mc::item::ItemRegistry* items_;
};

} // namespace mc::block