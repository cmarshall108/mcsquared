#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mc::protocol {

using Bytes = std::vector<std::uint8_t>;
using Uuid = std::array<std::uint8_t, 16>;

struct BlockPosition final {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;

    bool operator==(const BlockPosition&) const = default;
};

class DecodeError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Reader final {
public:
    explicit Reader(std::span<const std::uint8_t> data) noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;
    [[nodiscard]] bool read_bool();
    [[nodiscard]] std::uint8_t read_u8();
    [[nodiscard]] std::int8_t read_i8();
    [[nodiscard]] std::int16_t read_i16_be();
    [[nodiscard]] std::int32_t read_varint();
    [[nodiscard]] std::int64_t read_varlong();
    [[nodiscard]] std::int32_t read_i32_be();
    [[nodiscard]] std::uint16_t read_u16_be();
    [[nodiscard]] std::int64_t read_i64_be();
    [[nodiscard]] float read_f32_be();
    [[nodiscard]] double read_f64_be();
    [[nodiscard]] Uuid read_uuid();
    [[nodiscard]] BlockPosition read_position();
    [[nodiscard]] std::uint8_t read_angle();
    [[nodiscard]] std::string read_string(std::size_t max_utf16_units);
    [[nodiscard]] std::string read_identifier();
    [[nodiscard]] Bytes read_byte_array(std::size_t max_size);
    [[nodiscard]] std::vector<std::uint64_t> read_bitset(std::size_t max_words);
    [[nodiscard]] std::span<const std::uint8_t> read_bytes(std::size_t count);

    template <typename Decoder>
    [[nodiscard]] auto read_optional(Decoder&& decoder)
        -> std::optional<std::invoke_result_t<Decoder, Reader&>> {
        using Value = std::invoke_result_t<Decoder, Reader&>;
        if (!read_bool()) {
            return std::nullopt;
        }
        return std::optional<Value>(std::invoke(std::forward<Decoder>(decoder), *this));
    }

    template <typename Decoder>
    [[nodiscard]] auto read_collection(const std::size_t max_count, Decoder&& decoder)
        -> std::vector<std::invoke_result_t<Decoder, Reader&>> {
        using Value = std::invoke_result_t<Decoder, Reader&>;
        const auto count = read_varint();
        if (count < 0 || static_cast<std::size_t>(count) > max_count) {
            throw DecodeError("collection count is out of bounds");
        }
        std::vector<Value> values;
        values.reserve(static_cast<std::size_t>(count));
        for (std::int32_t index = 0; index < count; ++index) {
            values.push_back(std::invoke(decoder, *this));
        }
        return values;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t offset_{0};
};

void write_bool(Bytes& output, bool value);
void write_u8(Bytes& output, std::uint8_t value);
void write_i8(Bytes& output, std::int8_t value);
void write_i16_be(Bytes& output, std::int16_t value);
void write_varint(Bytes& output, std::int32_t value);
void write_varlong(Bytes& output, std::int64_t value);
void write_i32_be(Bytes& output, std::int32_t value);
void write_u16_be(Bytes& output, std::uint16_t value);
void write_i64_be(Bytes& output, std::int64_t value);
void write_f32_be(Bytes& output, float value);
void write_f64_be(Bytes& output, double value);
void write_uuid(Bytes& output, const Uuid& value);
void write_position(Bytes& output, BlockPosition value);
void write_angle(Bytes& output, std::uint8_t value);
void write_string(Bytes& output, std::string_view value);
void write_identifier(Bytes& output, std::string_view value);
void write_byte_array(Bytes& output, std::span<const std::uint8_t> value);
void write_bitset(Bytes& output, std::span<const std::uint64_t> words);

template <typename Value, typename Encoder>
void write_optional(Bytes& output,
                    const std::optional<Value>& value,
                    Encoder&& encoder) {
    write_bool(output, value.has_value());
    if (value) {
        std::invoke(std::forward<Encoder>(encoder), output, *value);
    }
}

template <typename Value, typename Encoder>
void write_collection(Bytes& output,
                      const std::span<const Value> values,
                      Encoder&& encoder) {
    if (values.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("collection is too large for a protocol VarInt");
    }
    write_varint(output, static_cast<std::int32_t>(values.size()));
    for (const auto& value : values) {
        std::invoke(encoder, output, value);
    }
}

[[nodiscard]] Bytes frame_packet(std::int32_t packet_id,
                                 std::span<const std::uint8_t> payload);
[[nodiscard]] Bytes frame_compressed_packet(std::int32_t packet_id,
                                            std::span<const std::uint8_t> payload,
                                            std::size_t threshold);
[[nodiscard]] Bytes decode_frame_payload(std::span<const std::uint8_t> frame_payload,
                                         std::size_t threshold,
                                         std::size_t max_decompressed_size);

} // namespace mc::protocol