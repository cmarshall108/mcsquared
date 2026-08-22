#include "mc/core/types.hpp"

#include <algorithm>
#include <stdexcept>

namespace mc::core {
namespace {

[[nodiscard]] bool valid_name_space_character(const char character) noexcept {
    return (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9') || character == '_' ||
        character == '-' || character == '.';
}

[[nodiscard]] bool valid_path_character(const char character) noexcept {
    return valid_name_space_character(character) || character == '/';
}

void validate(std::string_view value,
              bool (*predicate)(char),
              const char* description) {
    if (value.empty() || !std::all_of(value.begin(), value.end(), predicate)) {
        throw std::invalid_argument(description);
    }
}

} // namespace

ResourceLocation::ResourceLocation(std::string name_space, std::string path)
    : name_space_(std::move(name_space)), path_(std::move(path)) {
    validate(name_space_, valid_name_space_character, "invalid resource namespace");
    validate(path_, valid_path_character, "invalid resource path");
}

ResourceLocation ResourceLocation::parse(const std::string_view value) {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos) {
        return {"minecraft", std::string(value)};
    }
    if (value.find(':', separator + 1) != std::string_view::npos) {
        throw std::invalid_argument("resource location contains multiple separators");
    }
    return {std::string(value.substr(0, separator)), std::string(value.substr(separator + 1))};
}

const std::string& ResourceLocation::name_space() const noexcept {
    return name_space_;
}

const std::string& ResourceLocation::path() const noexcept {
    return path_;
}

std::string ResourceLocation::to_string() const {
    return name_space_ + ':' + path_;
}

} // namespace mc::core