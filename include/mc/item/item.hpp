#pragma once

#include "mc/core/types.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc::item {

using DataComponents = std::map<mc::core::ResourceLocation, std::string>;

struct ItemProperties final {
	std::uint16_t max_stack_size{64};
	std::uint16_t max_damage{0};
	std::optional<std::uint32_t> block_id;
	std::uint8_t nutrition{0};
	float saturation_modifier{0.0F};
};

class Item final {
public:
	Item(std::uint32_t id,
		 mc::core::ResourceLocation name,
		 ItemProperties properties);

	[[nodiscard]] std::uint32_t id() const noexcept;
	[[nodiscard]] const mc::core::ResourceLocation& name() const noexcept;
	[[nodiscard]] const ItemProperties& properties() const noexcept;
	[[nodiscard]] std::optional<std::uint32_t> protocol_id() const noexcept;

private:
	friend class ItemRegistry;

	std::uint32_t id_;
	mc::core::ResourceLocation name_;
	ItemProperties properties_;
	std::optional<std::uint32_t> protocol_id_;
};

struct ItemRegistryLoadReport final {
	std::size_t encountered{0};
	std::size_t loaded{0};
	std::size_t existing{0};
};

class ItemRegistry final {
public:
	ItemRegistry();

	[[nodiscard]] const Item& by_id(std::uint32_t id) const;
	[[nodiscard]] const Item& by_name(const mc::core::ResourceLocation& name) const;
	[[nodiscard]] const Item& by_name(std::string_view name) const;
	[[nodiscard]] const Item& by_protocol_id(std::uint32_t id) const;
	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] ItemRegistryLoadReport load_normalized(std::istream& input);

	const Item& register_item(mc::core::ResourceLocation name, ItemProperties properties = {});

private:
	std::vector<Item> items_;
};

class ItemStack final {
public:
	ItemStack() = default;
	ItemStack(std::uint32_t item_id,
			  std::uint16_t count,
			  std::uint16_t damage = 0,
			  DataComponents components = {});

	[[nodiscard]] bool empty() const noexcept;
	[[nodiscard]] std::uint32_t item_id() const noexcept;
	[[nodiscard]] std::uint16_t count() const noexcept;
	[[nodiscard]] std::uint16_t damage() const noexcept;
	[[nodiscard]] const DataComponents& components() const noexcept;
	[[nodiscard]] bool can_merge(const ItemStack& other) const noexcept;
	[[nodiscard]] std::uint16_t insert_from(ItemStack& source, const ItemRegistry& registry);
	[[nodiscard]] ItemStack take(std::uint16_t count);
	[[nodiscard]] bool apply_damage(std::uint16_t amount, const ItemRegistry& registry);
	void validate(const ItemRegistry& registry) const;

	bool operator==(const ItemStack&) const = default;

private:
	void normalize() noexcept;

	std::uint32_t item_id_{0};
	std::uint16_t count_{0};
	std::uint16_t damage_{0};
	DataComponents components_;
};

} // namespace mc::item