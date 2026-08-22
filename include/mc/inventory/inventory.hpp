#pragma once

#include "mc/core/types.hpp"
#include "mc/item/item.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <optional>
#include <vector>

namespace mc::inventory {

class Container final {
public:
	using SlotValidator = std::function<bool(
		std::size_t, const mc::item::ItemStack&, const mc::item::ItemRegistry&)>;

	struct SlotChange final {
		std::size_t index;
		mc::item::ItemStack stack;
	};

	Container(const mc::item::ItemRegistry& registry,
			  std::size_t slot_count,
			  SlotValidator slot_validator = {});

	[[nodiscard]] std::size_t slot_count() const noexcept;
	[[nodiscard]] const mc::item::ItemStack& slot(std::size_t index) const;
	void set_slot(std::size_t index, mc::item::ItemStack stack);
	[[nodiscard]] mc::item::ItemStack insert(mc::item::ItemStack stack);
	[[nodiscard]] mc::item::ItemStack take(std::size_t index, std::uint16_t count);
	[[nodiscard]] std::uint32_t state_id() const noexcept;
	[[nodiscard]] bool apply_transaction(std::uint32_t expected_state_id,
										 std::vector<SlotChange> changes);

private:
	friend class Inventory;
	const mc::item::ItemRegistry* registry_;
	std::vector<mc::item::ItemStack> slots_;
	SlotValidator slot_validator_;
	std::uint32_t state_id_{0};
};

class Inventory final {
public:
	explicit Inventory(const mc::item::ItemRegistry& registry);

	[[nodiscard]] Container& main() noexcept;
	[[nodiscard]] Container& armor() noexcept;
	[[nodiscard]] Container& offhand() noexcept;
	[[nodiscard]] Container& ender_chest() noexcept;
	[[nodiscard]] const mc::item::ItemStack& cursor() const noexcept;
	void set_cursor(mc::item::ItemStack stack);

private:
	Container main_;
	Container armor_;
	Container offhand_;
	Container ender_chest_;
	mc::item::ItemStack cursor_;
};

struct Ingredient final {
	std::vector<std::uint32_t> item_ids;

	[[nodiscard]] bool matches(const mc::item::ItemStack& stack) const noexcept;
};

enum class RecipeType {
	shaped,
	shapeless,
};

class Recipe final {
public:
	static Recipe shaped(mc::core::ResourceLocation id,
						 std::size_t width,
						 std::size_t height,
						 std::vector<Ingredient> ingredients,
						 mc::item::ItemStack result);
	static Recipe shapeless(mc::core::ResourceLocation id,
							std::vector<Ingredient> ingredients,
							mc::item::ItemStack result);

	[[nodiscard]] const mc::core::ResourceLocation& id() const noexcept;
	[[nodiscard]] RecipeType type() const noexcept;
	[[nodiscard]] const mc::item::ItemStack& result() const noexcept;
	[[nodiscard]] std::optional<std::vector<std::uint16_t>> match(
		const Container& grid,
		std::size_t grid_width,
		std::size_t grid_height) const;

private:
	Recipe(mc::core::ResourceLocation id,
		   RecipeType type,
		   std::size_t width,
		   std::size_t height,
		   std::vector<Ingredient> ingredients,
		   mc::item::ItemStack result);

	mc::core::ResourceLocation id_;
	RecipeType type_;
	std::size_t width_;
	std::size_t height_;
	std::vector<Ingredient> ingredients_;
	mc::item::ItemStack result_;
};

struct CraftingMatch final {
	mc::core::ResourceLocation recipe_id;
	mc::item::ItemStack result;
	std::vector<std::uint16_t> consumption;
};

struct RecipeLoadReport final {
	std::size_t encountered{0};
	std::size_t loaded{0};
	std::size_t skipped{0};
};

class CraftingManager final {
public:
	explicit CraftingManager(const mc::item::ItemRegistry& registry);

	void register_recipe(Recipe recipe);
	[[nodiscard]] RecipeLoadReport load_normalized(std::istream& input);
	[[nodiscard]] std::optional<CraftingMatch> match(const Container& grid,
													 std::size_t grid_width,
													 std::size_t grid_height) const;
	[[nodiscard]] bool craft_once(Container& grid,
								  std::size_t grid_width,
								  std::size_t grid_height,
								  Container& output) const;
	[[nodiscard]] std::size_t size() const noexcept;

private:
	const mc::item::ItemRegistry* registry_;
	std::vector<Recipe> recipes_;
};

} // namespace mc::inventory