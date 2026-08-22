#include "mc/inventory/inventory.hpp"

#include <algorithm>
#include <charconv>
#include <functional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace mc::inventory {
namespace {

[[nodiscard]] std::vector<std::string_view> split(const std::string_view input,
                                                  const char delimiter) {
    std::vector<std::string_view> output;
    std::size_t start = 0;
    while (true) {
        const auto separator = input.find(delimiter, start);
        output.push_back(input.substr(start, separator - start));
        if (separator == std::string_view::npos) {
            return output;
        }
        start = separator + 1;
    }
}

[[nodiscard]] std::size_t parse_size(const std::string_view value) {
    std::size_t result = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("normalized recipe contains an invalid integer");
    }
    return result;
}

[[nodiscard]] bool armor_slot_accepts(
    const std::size_t slot,
    const mc::item::ItemStack& stack,
    const mc::item::ItemRegistry& registry) {
    if (stack.empty()) {
        return true;
    }
    const auto& path = registry.by_id(stack.item_id()).name().path();
    switch (slot) {
    case 0:
        return path.ends_with("_boots");
    case 1:
        return path.ends_with("_leggings");
    case 2:
        return path.ends_with("_chestplate") || path == "elytra";
    case 3:
        return path.ends_with("_helmet") || path == "carved_pumpkin";
    default:
        return false;
    }
}

} // namespace

Container::Container(const mc::item::ItemRegistry& registry,
                     const std::size_t slot_count,
                     SlotValidator slot_validator)
    : registry_(&registry),
      slots_(slot_count),
      slot_validator_(std::move(slot_validator)) {
    if (slot_count == 0) {
        throw std::invalid_argument("container must contain at least one slot");
    }
}

std::size_t Container::slot_count() const noexcept { return slots_.size(); }

const mc::item::ItemStack& Container::slot(const std::size_t index) const {
    if (index >= slots_.size()) {
        throw std::out_of_range("container slot is out of range");
    }
    return slots_[index];
}

void Container::set_slot(const std::size_t index, mc::item::ItemStack stack) {
    if (index >= slots_.size()) {
        throw std::out_of_range("container slot is out of range");
    }
    stack.validate(*registry_);
    if (slot_validator_ && !slot_validator_(index, stack, *registry_)) {
        throw std::invalid_argument("item is not valid for container slot");
    }
    slots_[index] = std::move(stack);
    ++state_id_;
}

mc::item::ItemStack Container::insert(mc::item::ItemStack stack) {
    stack.validate(*registry_);
    if (stack.empty()) {
        return stack;
    }
    const auto initial_count = stack.count();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto& slot_value = slots_[index];
        if ((!slot_validator_ || slot_validator_(index, stack, *registry_)) &&
            slot_value.can_merge(stack)) {
            static_cast<void>(slot_value.insert_from(stack, *registry_));
            if (stack.empty()) {
                if (stack.count() != initial_count) {
                    ++state_id_;
                }
                return stack;
            }
        }
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto& slot_value = slots_[index];
        if (slot_value.empty() &&
            (!slot_validator_ || slot_validator_(index, stack, *registry_))) {
            static_cast<void>(slot_value.insert_from(stack, *registry_));
            if (stack.empty()) {
                if (stack.count() != initial_count) {
                    ++state_id_;
                }
                return stack;
            }
        }
    }
    if (stack.count() != initial_count) {
        ++state_id_;
    }
    return stack;
}

mc::item::ItemStack Container::take(const std::size_t index, const std::uint16_t count) {
    if (index >= slots_.size()) {
        throw std::out_of_range("container slot is out of range");
    }
    auto result = slots_[index].take(count);
    if (!result.empty()) {
        ++state_id_;
    }
    return result;
}

std::uint32_t Container::state_id() const noexcept { return state_id_; }

bool Container::apply_transaction(const std::uint32_t expected_state_id,
                                  std::vector<SlotChange> changes) {
    if (expected_state_id != state_id_) {
        return false;
    }
    auto updated = slots_;
    for (auto& change : changes) {
        if (change.index >= updated.size()) {
            return false;
        }
        try {
            change.stack.validate(*registry_);
        } catch (const std::exception&) {
            return false;
        }
        if (slot_validator_ &&
            !slot_validator_(change.index, change.stack, *registry_)) {
            return false;
        }
        updated[change.index] = std::move(change.stack);
    }
    if (updated == slots_) {
        return true;
    }
    slots_ = std::move(updated);
    ++state_id_;
    return true;
}

Inventory::Inventory(const mc::item::ItemRegistry& registry)
    : main_(registry, 36), armor_(registry, 4, armor_slot_accepts), offhand_(registry, 1),
      ender_chest_(registry, 27) {}

Container& Inventory::main() noexcept { return main_; }
Container& Inventory::armor() noexcept { return armor_; }
Container& Inventory::offhand() noexcept { return offhand_; }
Container& Inventory::ender_chest() noexcept { return ender_chest_; }
const mc::item::ItemStack& Inventory::cursor() const noexcept { return cursor_; }
void Inventory::set_cursor(mc::item::ItemStack stack) {
    stack.validate(*main_.registry_);
    cursor_ = std::move(stack);
}

bool Ingredient::matches(const mc::item::ItemStack& stack) const noexcept {
    if (item_ids.empty()) {
        return stack.empty();
    }
    return !stack.empty() &&
        std::find(item_ids.begin(), item_ids.end(), stack.item_id()) != item_ids.end();
}

Recipe Recipe::shaped(mc::core::ResourceLocation id,
                      const std::size_t width,
                      const std::size_t height,
                      std::vector<Ingredient> ingredients,
                      mc::item::ItemStack result) {
    if (width == 0 || height == 0 || width > 3 || height > 3 ||
        ingredients.size() != width * height) {
        throw std::invalid_argument("shaped recipe dimensions are invalid");
    }
    return Recipe(std::move(id), RecipeType::shaped, width, height,
                  std::move(ingredients), std::move(result));
}

Recipe Recipe::shapeless(mc::core::ResourceLocation id,
                         std::vector<Ingredient> ingredients,
                         mc::item::ItemStack result) {
    if (ingredients.empty() || ingredients.size() > 9 ||
        std::any_of(ingredients.begin(), ingredients.end(), [](const auto& ingredient) {
            return ingredient.item_ids.empty();
        })) {
        throw std::invalid_argument("shapeless recipe ingredients are invalid");
    }
    return Recipe(std::move(id), RecipeType::shapeless, 0, 0,
                  std::move(ingredients), std::move(result));
}

Recipe::Recipe(mc::core::ResourceLocation id,
               const RecipeType type,
               const std::size_t width,
               const std::size_t height,
               std::vector<Ingredient> ingredients,
               mc::item::ItemStack result)
    : id_(std::move(id)),
      type_(type),
      width_(width),
      height_(height),
      ingredients_(std::move(ingredients)),
      result_(std::move(result)) {
    if (result_.empty()) {
        throw std::invalid_argument("recipe result must not be empty");
    }
}

const mc::core::ResourceLocation& Recipe::id() const noexcept { return id_; }
RecipeType Recipe::type() const noexcept { return type_; }
const mc::item::ItemStack& Recipe::result() const noexcept { return result_; }

std::optional<std::vector<std::uint16_t>> Recipe::match(
    const Container& grid,
    const std::size_t grid_width,
    const std::size_t grid_height) const {
    if (grid_width == 0 || grid_height == 0 ||
        grid_width * grid_height != grid.slot_count()) {
        throw std::invalid_argument("crafting grid dimensions do not match container");
    }
    std::vector<std::uint16_t> consumption(grid.slot_count(), 0);
    if (type_ == RecipeType::shaped) {
        if (width_ > grid_width || height_ > grid_height) {
            return std::nullopt;
        }
        for (std::size_t offset_y = 0; offset_y <= grid_height - height_; ++offset_y) {
            for (std::size_t offset_x = 0; offset_x <= grid_width - width_; ++offset_x) {
                for (const bool mirrored : {false, true}) {
                    auto matched = true;
                    std::fill(consumption.begin(), consumption.end(), 0);
                    for (std::size_t y = 0; y < grid_height && matched; ++y) {
                        for (std::size_t x = 0; x < grid_width; ++x) {
                            const auto inside = x >= offset_x && x < offset_x + width_ &&
                                y >= offset_y && y < offset_y + height_;
                            Ingredient empty_ingredient;
                            const Ingredient* ingredient = &empty_ingredient;
                            if (inside) {
                                const auto pattern_x = mirrored
                                    ? width_ - 1 - (x - offset_x)
                                    : x - offset_x;
                                ingredient = &ingredients_[(y - offset_y) * width_ + pattern_x];
                            }
                            const auto slot_index = y * grid_width + x;
                            if (!ingredient->matches(grid.slot(slot_index))) {
                                matched = false;
                                break;
                            }
                            if (!ingredient->item_ids.empty()) {
                                consumption[slot_index] = 1;
                            }
                        }
                    }
                    if (matched) {
                        return consumption;
                    }
                }
            }
        }
        return std::nullopt;
    }

    std::vector<std::size_t> occupied_slots;
    for (std::size_t index = 0; index < grid.slot_count(); ++index) {
        if (!grid.slot(index).empty()) {
            occupied_slots.push_back(index);
        }
    }
    if (occupied_slots.size() != ingredients_.size()) {
        return std::nullopt;
    }
    std::vector<bool> used(ingredients_.size(), false);
    const std::function<bool(std::size_t)> assign = [&](const std::size_t slot_number) {
        if (slot_number == occupied_slots.size()) {
            return true;
        }
        const auto slot_index = occupied_slots[slot_number];
        for (std::size_t ingredient_index = 0;
             ingredient_index < ingredients_.size();
             ++ingredient_index) {
            if (!used[ingredient_index] &&
                ingredients_[ingredient_index].matches(grid.slot(slot_index))) {
                used[ingredient_index] = true;
                consumption[slot_index] = 1;
                if (assign(slot_number + 1)) {
                    return true;
                }
                consumption[slot_index] = 0;
                used[ingredient_index] = false;
            }
        }
        return false;
    };
    return assign(0) ? std::optional(consumption) : std::nullopt;
}

CraftingManager::CraftingManager(const mc::item::ItemRegistry& registry)
    : registry_(&registry) {
    const auto log = registry.by_name("oak_log").id();
    const auto planks = registry.by_name("oak_planks").id();
    const auto stick = registry.by_name("stick").id();
    const auto table = registry.by_name("crafting_table").id();
    const auto pickaxe = registry.by_name("wooden_pickaxe").id();
    register_recipe(Recipe::shapeless(
        mc::core::ResourceLocation::parse("minecraft:oak_planks"),
        {Ingredient{{log}}},
        mc::item::ItemStack(planks, 4)));
    register_recipe(Recipe::shaped(
        mc::core::ResourceLocation::parse("minecraft:stick"), 1, 2,
        {Ingredient{{planks}}, Ingredient{{planks}}},
        mc::item::ItemStack(stick, 4)));
    register_recipe(Recipe::shaped(
        mc::core::ResourceLocation::parse("minecraft:crafting_table"), 2, 2,
        {Ingredient{{planks}}, Ingredient{{planks}},
         Ingredient{{planks}}, Ingredient{{planks}}},
        mc::item::ItemStack(table, 1)));
    register_recipe(Recipe::shaped(
        mc::core::ResourceLocation::parse("minecraft:wooden_pickaxe"), 3, 3,
        {Ingredient{{planks}}, Ingredient{{planks}}, Ingredient{{planks}},
         Ingredient{}, Ingredient{{stick}}, Ingredient{},
         Ingredient{}, Ingredient{{stick}}, Ingredient{}},
        mc::item::ItemStack(pickaxe, 1)));
}

void CraftingManager::register_recipe(Recipe recipe) {
    recipe.result().validate(*registry_);
    if (std::any_of(recipes_.begin(), recipes_.end(), [&](const auto& existing) {
            return existing.id() == recipe.id();
        })) {
        throw std::invalid_argument("recipe ID is already registered");
    }
    recipes_.push_back(std::move(recipe));
}

RecipeLoadReport CraftingManager::load_normalized(std::istream& input) {
    std::string line;
    if (!std::getline(input, line) || !line.starts_with("MCRECIPES1\t")) {
        throw std::runtime_error("normalized recipe stream has an invalid header");
    }

    RecipeLoadReport report;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        ++report.encountered;
        const auto fields = split(line, '\t');
        if ((fields[0] == "S" && fields.size() != 7) ||
            (fields[0] == "L" && fields.size() != 5) ||
            (fields[0] != "S" && fields[0] != "L")) {
            throw std::runtime_error("normalized recipe record has an invalid shape");
        }

        const auto recipe_id = mc::core::ResourceLocation::parse(fields[1]);
        if (std::any_of(recipes_.begin(), recipes_.end(), [&](const auto& recipe) {
                return recipe.id() == recipe_id;
            })) {
            ++report.skipped;
            continue;
        }

        const mc::item::Item* result_item = nullptr;
        try {
            result_item = &registry_->by_name(fields[2]);
        } catch (const std::out_of_range&) {
            ++report.skipped;
            continue;
        }
        const auto result_count = parse_size(fields[3]);
        if (result_count == 0 || result_count > result_item->properties().max_stack_size) {
            ++report.skipped;
            continue;
        }

        const auto parse_ingredients = [&](const std::string_view encoded,
                                           const bool allow_empty)
            -> std::optional<std::vector<Ingredient>> {
            std::vector<Ingredient> ingredients;
            for (const auto cell : split(encoded, '|')) {
                Ingredient ingredient;
                if (!cell.empty()) {
                    for (const auto name : split(cell, ',')) {
                        try {
                            ingredient.item_ids.push_back(registry_->by_name(name).id());
                        } catch (const std::out_of_range&) {
                        }
                    }
                    std::sort(ingredient.item_ids.begin(), ingredient.item_ids.end());
                    ingredient.item_ids.erase(
                        std::unique(ingredient.item_ids.begin(), ingredient.item_ids.end()),
                        ingredient.item_ids.end());
                    if (ingredient.item_ids.empty()) {
                        return std::nullopt;
                    }
                } else if (!allow_empty) {
                    return std::nullopt;
                }
                ingredients.push_back(std::move(ingredient));
            }
            return ingredients;
        };

        std::optional<Recipe> recipe;
        if (fields[0] == "S") {
            const auto width = parse_size(fields[4]);
            const auto height = parse_size(fields[5]);
            auto ingredients = parse_ingredients(fields[6], true);
            if (ingredients && ingredients->size() == width * height) {
                recipe = Recipe::shaped(
                    recipe_id,
                    width,
                    height,
                    std::move(*ingredients),
                    mc::item::ItemStack(
                        result_item->id(), static_cast<std::uint16_t>(result_count)));
            }
        } else {
            auto ingredients = parse_ingredients(fields[4], false);
            if (ingredients) {
                recipe = Recipe::shapeless(
                    recipe_id,
                    std::move(*ingredients),
                    mc::item::ItemStack(
                        result_item->id(), static_cast<std::uint16_t>(result_count)));
            }
        }
        if (!recipe) {
            ++report.skipped;
            continue;
        }
        register_recipe(std::move(*recipe));
        ++report.loaded;
    }
    return report;
}

std::optional<CraftingMatch> CraftingManager::match(
    const Container& grid,
    const std::size_t grid_width,
    const std::size_t grid_height) const {
    for (const auto& recipe : recipes_) {
        if (auto consumption = recipe.match(grid, grid_width, grid_height)) {
            return CraftingMatch{recipe.id(), recipe.result(), std::move(*consumption)};
        }
    }
    return std::nullopt;
}

bool CraftingManager::craft_once(Container& grid,
                                 const std::size_t grid_width,
                                 const std::size_t grid_height,
                                 Container& output) const {
    const auto matched = match(grid, grid_width, grid_height);
    if (!matched) {
        return false;
    }
    auto grid_copy = grid;
    auto output_copy = output;
    for (std::size_t index = 0; index < matched->consumption.size(); ++index) {
        if (matched->consumption[index] != 0) {
            const auto removed = grid_copy.take(index, matched->consumption[index]);
            if (removed.count() != matched->consumption[index]) {
                return false;
            }
        }
    }
    if (!output_copy.insert(matched->result).empty()) {
        return false;
    }
    grid = std::move(grid_copy);
    output = std::move(output_copy);
    return true;
}

std::size_t CraftingManager::size() const noexcept { return recipes_.size(); }

} // namespace mc::inventory