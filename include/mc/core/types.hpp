#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace mc::core {

struct ChunkPosition;

struct BlockPosition final {
	std::int32_t x;
	std::int32_t y;
	std::int32_t z;

	auto operator<=>(const BlockPosition&) const = default;
};

using Uuid = std::array<std::uint8_t, 16>;

class ResourceLocation final {
public:
	ResourceLocation(std::string name_space, std::string path);

	[[nodiscard]] static ResourceLocation parse(std::string_view value);
	[[nodiscard]] const std::string& name_space() const noexcept;
	[[nodiscard]] const std::string& path() const noexcept;
	[[nodiscard]] std::string to_string() const;

	auto operator<=>(const ResourceLocation&) const = default;

private:
	std::string name_space_;
	std::string path_;
};

} // namespace mc::core