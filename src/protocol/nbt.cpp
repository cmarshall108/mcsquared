#include "mc/protocol/nbt.hpp"

#include <bit>
#include <limits>
#include <stdexcept>

namespace mc::protocol::nbt {
namespace {

[[nodiscard]] Type read_type(Reader& input) {
	const auto id = input.read_u8();
	if (id > static_cast<std::uint8_t>(Type::long_array)) {
		throw DecodeError("NBT tag type is out of bounds");
	}
	return static_cast<Type>(id);
}

[[nodiscard]] std::string read_modified_utf(Reader& input, const Limits& limits) {
	const auto size = input.read_u16_be();
	if (size > limits.max_string_bytes) {
		throw DecodeError("NBT string exceeds limit");
	}
	const auto bytes = input.read_bytes(size);
	std::string output;
	output.reserve(bytes.size());
	for (std::size_t index = 0; index < bytes.size();) {
		std::uint32_t unit = 0;
		const auto first = bytes[index++];
		if ((first & 0x80U) == 0) {
			if (first == 0) throw DecodeError("NBT modified UTF contains raw NUL");
			unit = first;
		} else if ((first & 0xE0U) == 0xC0U) {
			if (index >= bytes.size() || (bytes[index] & 0xC0U) != 0x80U) {
				throw DecodeError("NBT modified UTF is malformed");
			}
			unit = static_cast<std::uint32_t>(first & 0x1FU) << 6U |
				static_cast<std::uint32_t>(bytes[index++] & 0x3FU);
			if (unit != 0 && unit < 0x80U) throw DecodeError("NBT modified UTF is overlong");
		} else if ((first & 0xF0U) == 0xE0U) {
			if (index + 1 >= bytes.size() || (bytes[index] & 0xC0U) != 0x80U ||
				(bytes[index + 1] & 0xC0U) != 0x80U) {
				throw DecodeError("NBT modified UTF is malformed");
			}
			unit = static_cast<std::uint32_t>(first & 0x0FU) << 12U |
				static_cast<std::uint32_t>(bytes[index] & 0x3FU) << 6U |
				static_cast<std::uint32_t>(bytes[index + 1] & 0x3FU);
			index += 2;
			if (unit < 0x800U) throw DecodeError("NBT modified UTF is overlong");
		} else {
			throw DecodeError("NBT modified UTF lead byte is invalid");
		}

		std::uint32_t code_point = unit;
		if (unit >= 0xD800U && unit <= 0xDBFFU) {
			if (index + 2 >= bytes.size() || (bytes[index] & 0xF0U) != 0xE0U) {
				throw DecodeError("NBT modified UTF surrogate is incomplete");
			}
			const auto low = static_cast<std::uint32_t>(bytes[index] & 0x0FU) << 12U |
				static_cast<std::uint32_t>(bytes[index + 1] & 0x3FU) << 6U |
				static_cast<std::uint32_t>(bytes[index + 2] & 0x3FU);
			if (low < 0xDC00U || low > 0xDFFFU) {
				throw DecodeError("NBT modified UTF surrogate pair is invalid");
			}
			index += 3;
			code_point = 0x10000U + ((unit - 0xD800U) << 10U) + (low - 0xDC00U);
		} else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
			throw DecodeError("NBT modified UTF contains an unpaired surrogate");
		}

		if (code_point <= 0x7FU) {
			output.push_back(static_cast<char>(code_point));
		} else if (code_point <= 0x7FFU) {
			output.push_back(static_cast<char>(0xC0U | code_point >> 6U));
			output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
		} else if (code_point <= 0xFFFFU) {
			output.push_back(static_cast<char>(0xE0U | code_point >> 12U));
			output.push_back(static_cast<char>(0x80U | (code_point >> 6U & 0x3FU)));
			output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
		} else {
			output.push_back(static_cast<char>(0xF0U | code_point >> 18U));
			output.push_back(static_cast<char>(0x80U | (code_point >> 12U & 0x3FU)));
			output.push_back(static_cast<char>(0x80U | (code_point >> 6U & 0x3FU)));
			output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
		}
	}
	return output;
}

void append_modified_unit(Bytes& output, const std::uint16_t unit) {
	if (unit >= 1 && unit <= 0x7FU) {
		output.push_back(static_cast<std::uint8_t>(unit));
	} else if (unit <= 0x7FFU) {
		output.push_back(static_cast<std::uint8_t>(0xC0U | unit >> 6U));
		output.push_back(static_cast<std::uint8_t>(0x80U | (unit & 0x3FU)));
	} else {
		output.push_back(static_cast<std::uint8_t>(0xE0U | unit >> 12U));
		output.push_back(static_cast<std::uint8_t>(0x80U | (unit >> 6U & 0x3FU)));
		output.push_back(static_cast<std::uint8_t>(0x80U | (unit & 0x3FU)));
	}
}

[[nodiscard]] Bytes encode_modified_utf(const std::string_view value) {
	Bytes output;
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<std::uint8_t>(value[index++]);
		std::uint32_t code_point = first;
		std::size_t continuation_count = 0;
		if ((first & 0x80U) == 0) continuation_count = 0;
		else if ((first & 0xE0U) == 0xC0U) { code_point &= 0x1FU; continuation_count = 1; }
		else if ((first & 0xF0U) == 0xE0U) { code_point &= 0x0FU; continuation_count = 2; }
		else if ((first & 0xF8U) == 0xF0U) { code_point &= 0x07U; continuation_count = 3; }
		else throw std::invalid_argument("NBT string contains malformed UTF-8");
		if (index + continuation_count > value.size()) {
			throw std::invalid_argument("NBT string contains truncated UTF-8");
		}
		for (std::size_t count = 0; count < continuation_count; ++count) {
			const auto continuation = static_cast<std::uint8_t>(value[index++]);
			if ((continuation & 0xC0U) != 0x80U) {
				throw std::invalid_argument("NBT string contains malformed UTF-8");
			}
			code_point = code_point << 6U | (continuation & 0x3FU);
		}
		if (code_point > 0x10FFFFU || (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
			throw std::invalid_argument("NBT string code point is invalid");
		}
		if (code_point <= 0xFFFFU) {
			append_modified_unit(output, static_cast<std::uint16_t>(code_point));
		} else {
			code_point -= 0x10000U;
			append_modified_unit(output, static_cast<std::uint16_t>(0xD800U + (code_point >> 10U)));
			append_modified_unit(output, static_cast<std::uint16_t>(0xDC00U + (code_point & 0x3FFU)));
		}
	}
	return output;
}

void write_modified_utf(Bytes& output, const std::string_view value, const Limits& limits) {
	auto encoded = encode_modified_utf(value);
	if (encoded.size() > limits.max_string_bytes ||
		encoded.size() > std::numeric_limits<std::uint16_t>::max()) {
		throw std::length_error("NBT string exceeds limit");
	}
	write_u16_be(output, static_cast<std::uint16_t>(encoded.size()));
	output.insert(output.end(), encoded.begin(), encoded.end());
}

[[nodiscard]] std::size_t checked_count(Reader& input, const Limits& limits) {
	const auto count = input.read_i32_be();
	if (count < 0 || static_cast<std::size_t>(count) > limits.max_collection_entries) {
		throw DecodeError("NBT collection length is out of bounds");
	}
	return static_cast<std::size_t>(count);
}

[[nodiscard]] Tag read_payload(Reader& input, Type type, const Limits& limits, std::size_t depth) {
	if (depth > limits.max_depth) throw DecodeError("NBT depth exceeds limit");
	Tag tag{type, {}};
	switch (type) {
	case Type::end: return tag;
	case Type::byte: tag.value = input.read_i8(); break;
	case Type::short_integer: tag.value = input.read_i16_be(); break;
	case Type::integer: tag.value = input.read_i32_be(); break;
	case Type::long_integer: tag.value = input.read_i64_be(); break;
	case Type::float_number: tag.value = input.read_f32_be(); break;
	case Type::double_number: tag.value = input.read_f64_be(); break;
	case Type::byte_array: {
		ByteArray values(checked_count(input, limits));
		for (auto& value : values) value = input.read_i8();
		tag.value = std::move(values);
		break;
	}
	case Type::string: tag.value = read_modified_utf(input, limits); break;
	case Type::list: {
		List list{read_type(input), {}};
		const auto count = checked_count(input, limits);
		if (list.element_type == Type::end && count != 0) {
			throw DecodeError("NBT non-empty list has END element type");
		}
		list.values.reserve(count);
		for (std::size_t index = 0; index < count; ++index) {
			list.values.push_back(read_payload(input, list.element_type, limits, depth + 1));
		}
		tag.value = std::move(list);
		break;
	}
	case Type::compound: {
		Compound compound;
		while (true) {
			const auto child_type = read_type(input);
			if (child_type == Type::end) break;
			if (compound.entries.size() >= limits.max_collection_entries) {
				throw DecodeError("NBT compound entry count exceeds limit");
			}
			auto name = read_modified_utf(input, limits);
			compound.entries.emplace_back(
				std::move(name), read_payload(input, child_type, limits, depth + 1));
		}
		tag.value = std::move(compound);
		break;
	}
	case Type::integer_array: {
		IntegerArray values(checked_count(input, limits));
		for (auto& value : values) value = input.read_i32_be();
		tag.value = std::move(values);
		break;
	}
	case Type::long_array: {
		LongArray values(checked_count(input, limits));
		for (auto& value : values) value = input.read_i64_be();
		tag.value = std::move(values);
		break;
	}
	}
	return tag;
}

template <typename T>
[[nodiscard]] const T& value_as(const Tag& tag) {
	const auto* value = std::get_if<T>(&tag.value);
	if (value == nullptr) throw std::invalid_argument("NBT tag value does not match type");
	return *value;
}

void write_payload(Bytes& output, const Tag& tag, const Limits& limits, std::size_t depth) {
	if (depth > limits.max_depth) throw std::length_error("NBT depth exceeds limit");
	switch (tag.type) {
	case Type::end: break;
	case Type::byte: write_i8(output, value_as<std::int8_t>(tag)); break;
	case Type::short_integer: write_i16_be(output, value_as<std::int16_t>(tag)); break;
	case Type::integer: write_i32_be(output, value_as<std::int32_t>(tag)); break;
	case Type::long_integer: write_i64_be(output, value_as<std::int64_t>(tag)); break;
	case Type::float_number: write_f32_be(output, value_as<float>(tag)); break;
	case Type::double_number: write_f64_be(output, value_as<double>(tag)); break;
	case Type::byte_array: {
		const auto& values = value_as<ByteArray>(tag);
		if (values.size() > limits.max_collection_entries) throw std::length_error("NBT array exceeds limit");
		write_i32_be(output, static_cast<std::int32_t>(values.size()));
		for (const auto value : values) write_i8(output, value);
		break;
	}
	case Type::string: write_modified_utf(output, value_as<std::string>(tag), limits); break;
	case Type::list: {
		const auto& list = value_as<List>(tag);
		if (list.values.size() > limits.max_collection_entries ||
			(list.element_type == Type::end && !list.values.empty())) {
			throw std::length_error("NBT list exceeds limit or has invalid type");
		}
		output.push_back(static_cast<std::uint8_t>(list.element_type));
		write_i32_be(output, static_cast<std::int32_t>(list.values.size()));
		for (const auto& value : list.values) {
			if (value.type != list.element_type) throw std::invalid_argument("NBT list type mismatch");
			write_payload(output, value, limits, depth + 1);
		}
		break;
	}
	case Type::compound: {
		const auto& compound = value_as<Compound>(tag);
		if (compound.entries.size() > limits.max_collection_entries) throw std::length_error("NBT compound exceeds limit");
		for (const auto& [name, value] : compound.entries) {
			if (value.type == Type::end) throw std::invalid_argument("NBT compound contains named END tag");
			output.push_back(static_cast<std::uint8_t>(value.type));
			write_modified_utf(output, name, limits);
			write_payload(output, value, limits, depth + 1);
		}
		output.push_back(0);
		break;
	}
	case Type::integer_array: {
		const auto& values = value_as<IntegerArray>(tag);
		if (values.size() > limits.max_collection_entries) throw std::length_error("NBT array exceeds limit");
		write_i32_be(output, static_cast<std::int32_t>(values.size()));
		for (const auto value : values) write_i32_be(output, value);
		break;
	}
	case Type::long_array: {
		const auto& values = value_as<LongArray>(tag);
		if (values.size() > limits.max_collection_entries) throw std::length_error("NBT array exceeds limit");
		write_i32_be(output, static_cast<std::int32_t>(values.size()));
		for (const auto value : values) write_i64_be(output, value);
		break;
	}
	}
	if (output.size() > limits.max_bytes) throw std::length_error("NBT payload exceeds byte limit");
}

} // namespace

Tag read_any_tag(Reader& input, const Limits limits) {
	const auto before = input.remaining();
	auto tag = read_payload(input, read_type(input), limits, 0);
	if (before - input.remaining() > limits.max_bytes) {
		throw DecodeError("NBT payload exceeds byte limit");
	}
	return tag;
}

void write_any_tag(Bytes& output, const Tag& tag, const Limits limits) {
	const auto start = output.size();
	output.push_back(static_cast<std::uint8_t>(tag.type));
	write_payload(output, tag, limits, 0);
	if (output.size() - start > limits.max_bytes) throw std::length_error("NBT payload exceeds byte limit");
}

Tag string_tag(std::string value) {
	return Tag{Type::string, std::move(value)};
}

} // namespace mc::protocol::nbt