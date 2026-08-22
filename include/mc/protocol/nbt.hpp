#pragma once

#include "mc/protocol/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mc::protocol::nbt {

enum class Type : std::uint8_t {
	end,
	byte,
	short_integer,
	integer,
	long_integer,
	float_number,
	double_number,
	byte_array,
	string,
	list,
	compound,
	integer_array,
	long_array,
};

struct Tag;

struct List final {
	Type element_type{Type::end};
	std::vector<Tag> values;
};

struct Compound final {
	std::vector<std::pair<std::string, Tag>> entries;
};

using ByteArray = std::vector<std::int8_t>;
using IntegerArray = std::vector<std::int32_t>;
using LongArray = std::vector<std::int64_t>;

struct Tag final {
	using Value = std::variant<std::monostate, std::int8_t, std::int16_t,
		std::int32_t, std::int64_t, float, double, ByteArray, std::string,
		List, Compound, IntegerArray, LongArray>;

	Type type{Type::end};
	Value value{};
};

struct Limits final {
	std::size_t max_bytes{2U * 1024U * 1024U};
	std::size_t max_depth{64};
	std::size_t max_collection_entries{65'536};
	std::size_t max_string_bytes{65'535};
};

[[nodiscard]] Tag read_any_tag(Reader& input, Limits limits = {});
void write_any_tag(Bytes& output, const Tag& tag, Limits limits = {});

[[nodiscard]] Tag string_tag(std::string value);

} // namespace mc::protocol::nbt