#include "mc/protocol/codec.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <memory>

#include <zlib.h>

namespace mc::protocol {
namespace {

[[nodiscard]] std::size_t utf16_length(const std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    std::size_t units = 0;
    while (offset < bytes.size()) {
        const auto first = bytes[offset];
        std::size_t width = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7FU) {
            width = 1;
            codepoint = first;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            width = 2;
            codepoint = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            width = 3;
            codepoint = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            width = 4;
            codepoint = first & 0x07U;
        } else {
            throw DecodeError("string contains invalid UTF-8");
        }

        if (width > bytes.size() - offset) {
            throw DecodeError("string contains truncated UTF-8");
        }
        for (std::size_t index = 1; index < width; ++index) {
            const auto continuation = bytes[offset + index];
            if ((continuation & 0xC0U) != 0x80U) {
                throw DecodeError("string contains invalid UTF-8");
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3FU);
        }

        const bool overlong = (width == 2 && codepoint < 0x80U) ||
            (width == 3 && codepoint < 0x800U) ||
            (width == 4 && codepoint < 0x10000U);
        if (overlong || (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
            codepoint > 0x10FFFFU) {
            throw DecodeError("string contains invalid UTF-8");
        }
        units += codepoint > 0xFFFFU ? 2U : 1U;
        offset += width;
    }
    return units;
}

} // namespace

Reader::Reader(const std::span<const std::uint8_t> data) noexcept : data_(data) {}

bool Reader::empty() const noexcept {
    return offset_ == data_.size();
}

std::size_t Reader::remaining() const noexcept {
    return data_.size() - offset_;
}

bool Reader::read_bool() {
    const auto value = read_bytes(1).front();
    if (value > 1U) {
        throw DecodeError("boolean value is not zero or one");
    }
    return value != 0;
}

std::uint8_t Reader::read_u8() {
    return read_bytes(1).front();
}

std::int8_t Reader::read_i8() {
    return static_cast<std::int8_t>(read_u8());
}

std::int16_t Reader::read_i16_be() {
    const auto bytes = read_bytes(2);
    const auto value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) << 8U | bytes[1]);
    return static_cast<std::int16_t>(value);
}

std::int32_t Reader::read_varint() {
    std::uint32_t value = 0;
    for (std::uint32_t byte_index = 0; byte_index < 5; ++byte_index) {
        if (empty()) {
            throw DecodeError("truncated VarInt");
        }

        const auto byte = data_[offset_++];
        if (byte_index == 4 && (byte & 0xF0U) != 0) {
            throw DecodeError("VarInt exceeds 32 bits");
        }
        value |= static_cast<std::uint32_t>(byte & 0x7FU) << (7U * byte_index);
        if ((byte & 0x80U) == 0) {
            return static_cast<std::int32_t>(value);
        }
    }
    throw DecodeError("VarInt is too long");
}

std::int64_t Reader::read_varlong() {
    std::uint64_t value = 0;
    for (std::uint32_t byte_index = 0; byte_index < 10; ++byte_index) {
        if (empty()) {
            throw DecodeError("truncated VarLong");
        }
        const auto byte = data_[offset_++];
        if (byte_index == 9 && (byte & 0xFEU) != 0) {
            throw DecodeError("VarLong exceeds 64 bits");
        }
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << (7U * byte_index);
        if ((byte & 0x80U) == 0) {
            return static_cast<std::int64_t>(value);
        }
    }
    throw DecodeError("VarLong is too long");
}

std::int32_t Reader::read_i32_be() {
    const auto bytes = read_bytes(4);
    std::uint32_t value = 0;
    for (const auto byte : bytes) {
        value = (value << 8U) | byte;
    }
    return static_cast<std::int32_t>(value);
}

std::uint16_t Reader::read_u16_be() {
    const auto bytes = read_bytes(2);
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) << 8U |
        static_cast<std::uint16_t>(bytes[1]));
}

std::int64_t Reader::read_i64_be() {
    const auto bytes = read_bytes(8);
    std::uint64_t value = 0;
    for (const auto byte : bytes) {
        value = (value << 8U) | byte;
    }
    return static_cast<std::int64_t>(value);
}

float Reader::read_f32_be() {
    return std::bit_cast<float>(static_cast<std::uint32_t>(read_i32_be()));
}

double Reader::read_f64_be() {
    return std::bit_cast<double>(static_cast<std::uint64_t>(read_i64_be()));
}

Uuid Reader::read_uuid() {
    const auto bytes = read_bytes(16);
    Uuid value{};
    std::copy(bytes.begin(), bytes.end(), value.begin());
    return value;
}

BlockPosition Reader::read_position() {
    const auto packed = static_cast<std::uint64_t>(read_i64_be());
    const auto sign_extend = [](const std::uint64_t value, const unsigned bits) {
        const auto shift = 64U - bits;
        return static_cast<std::int64_t>(value << shift) >> shift;
    };
    return {
        static_cast<std::int32_t>(sign_extend(packed >> 38U, 26)),
        static_cast<std::int32_t>(sign_extend(packed & 0xFFFU, 12)),
        static_cast<std::int32_t>(sign_extend((packed >> 12U) & 0x3FFFFFFU, 26)),
    };
}

std::uint8_t Reader::read_angle() {
    return read_u8();
}

std::string Reader::read_string(const std::size_t max_utf16_units) {
    const auto encoded_size = read_varint();
    const auto max_encoded_bytes = max_utf16_units > std::numeric_limits<std::size_t>::max() / 3U
        ? std::numeric_limits<std::size_t>::max()
        : max_utf16_units * 3U;
    if (encoded_size < 0 || static_cast<std::size_t>(encoded_size) > max_encoded_bytes) {
        throw DecodeError("string length is out of bounds");
    }
    const auto bytes = read_bytes(static_cast<std::size_t>(encoded_size));
    if (utf16_length(bytes) > max_utf16_units) {
        throw DecodeError("decoded string is too long");
    }
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string Reader::read_identifier() {
    const auto value = read_string(32'767);
    const auto separator = value.find(':');
    const auto name_space = separator == std::string::npos
        ? std::string_view("minecraft")
        : std::string_view(value).substr(0, separator);
    const auto path = separator == std::string::npos
        ? std::string_view(value)
        : std::string_view(value).substr(separator + 1);
    const auto valid_namespace = [](const char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '_' ||
            character == '-' || character == '.';
    };
    const auto valid_path = [&](const char character) {
        return valid_namespace(character) || character == '/';
    };
    if (name_space.empty() || path.empty() ||
        !std::all_of(name_space.begin(), name_space.end(), valid_namespace) ||
        !std::all_of(path.begin(), path.end(), valid_path) ||
        (separator != std::string::npos && value.find(':', separator + 1) != std::string::npos)) {
        throw DecodeError("invalid resource identifier");
    }
    return separator == std::string::npos ? "minecraft:" + value : value;
}

Bytes Reader::read_byte_array(const std::size_t max_size) {
    const auto encoded_size = read_varint();
    if (encoded_size < 0 || static_cast<std::size_t>(encoded_size) > max_size) {
        throw DecodeError("byte array length is out of bounds");
    }
    const auto bytes = read_bytes(static_cast<std::size_t>(encoded_size));
    return {bytes.begin(), bytes.end()};
}

std::vector<std::uint64_t> Reader::read_bitset(const std::size_t max_words) {
    const auto count = read_varint();
    if (count < 0 || static_cast<std::size_t>(count) > max_words) {
        throw DecodeError("bitset word count is out of bounds");
    }
    std::vector<std::uint64_t> words;
    words.reserve(static_cast<std::size_t>(count));
    for (std::int32_t index = 0; index < count; ++index) {
        words.push_back(static_cast<std::uint64_t>(read_i64_be()));
    }
    return words;
}

std::span<const std::uint8_t> Reader::read_bytes(const std::size_t count) {
    if (count > remaining()) {
        throw DecodeError("packet payload is truncated");
    }
    const auto result = data_.subspan(offset_, count);
    offset_ += count;
    return result;
}

void write_bool(Bytes& output, const bool value) {
    output.push_back(value ? 1U : 0U);
}

void write_u8(Bytes& output, const std::uint8_t value) { output.push_back(value); }
void write_i8(Bytes& output, const std::int8_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
}
void write_i16_be(Bytes& output, const std::int16_t value) {
    const auto bits = static_cast<std::uint16_t>(value);
    output.push_back(static_cast<std::uint8_t>(bits >> 8U));
    output.push_back(static_cast<std::uint8_t>(bits));
}

void write_varint(Bytes& output, const std::int32_t value) {
    auto remaining = static_cast<std::uint32_t>(value);
    do {
        auto byte = static_cast<std::uint8_t>(remaining & 0x7FU);
        remaining >>= 7U;
        if (remaining != 0) {
            byte |= 0x80U;
        }
        output.push_back(byte);
    } while (remaining != 0);
}

void write_varlong(Bytes& output, const std::int64_t value) {
    auto remaining = static_cast<std::uint64_t>(value);
    do {
        auto byte = static_cast<std::uint8_t>(remaining & 0x7FU);
        remaining >>= 7U;
        if (remaining != 0) byte |= 0x80U;
        output.push_back(byte);
    } while (remaining != 0);
}

void write_i32_be(Bytes& output, const std::int32_t value) {
    const auto bits = static_cast<std::uint32_t>(value);
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
}

void write_u16_be(Bytes& output, const std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void write_i64_be(Bytes& output, const std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
}

void write_f32_be(Bytes& output, const float value) {
    write_i32_be(output, static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(value)));
}

void write_f64_be(Bytes& output, const double value) {
    write_i64_be(output, static_cast<std::int64_t>(std::bit_cast<std::uint64_t>(value)));
}

void write_uuid(Bytes& output, const Uuid& value) {
    output.insert(output.end(), value.begin(), value.end());
}

void write_position(Bytes& output, const BlockPosition value) {
    if (value.x < -33'554'432 || value.x > 33'554'431 ||
        value.z < -33'554'432 || value.z > 33'554'431 ||
        value.y < -2'048 || value.y > 2'047) {
        throw std::out_of_range("block position exceeds packed protocol range");
    }
    const auto packed = (static_cast<std::uint64_t>(value.x) & 0x3FFFFFFU) << 38U |
        (static_cast<std::uint64_t>(value.z) & 0x3FFFFFFU) << 12U |
        (static_cast<std::uint64_t>(value.y) & 0xFFFU);
    write_i64_be(output, static_cast<std::int64_t>(packed));
}

void write_angle(Bytes& output, const std::uint8_t value) { write_u8(output, value); }

void write_string(Bytes& output, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("string is too large for a protocol VarInt");
    }
    write_varint(output, static_cast<std::int32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void write_identifier(Bytes& output, const std::string_view value) {
    Bytes encoded;
    write_string(encoded, value);
    Reader reader(encoded);
    static_cast<void>(reader.read_identifier());
    write_string(output, value.find(':') == std::string_view::npos
        ? std::string("minecraft:") + std::string(value)
        : value);
}

void write_byte_array(Bytes& output, const std::span<const std::uint8_t> value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("byte array is too large for a protocol VarInt");
    }
    write_varint(output, static_cast<std::int32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void write_bitset(Bytes& output, const std::span<const std::uint64_t> words) {
    if (words.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("bitset is too large for a protocol VarInt");
    }
    write_varint(output, static_cast<std::int32_t>(words.size()));
    for (const auto word : words) {
        write_i64_be(output, static_cast<std::int64_t>(word));
    }
}

Bytes frame_packet(const std::int32_t packet_id,
                   const std::span<const std::uint8_t> payload) {
    Bytes body;
    write_varint(body, packet_id);
    body.insert(body.end(), payload.begin(), payload.end());
    if (body.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("packet is too large for a protocol VarInt");
    }

    Bytes framed;
    write_varint(framed, static_cast<std::int32_t>(body.size()));
    framed.insert(framed.end(), body.begin(), body.end());
    return framed;
}

Bytes frame_compressed_packet(const std::int32_t packet_id,
                              const std::span<const std::uint8_t> payload,
                              const std::size_t threshold) {
    Bytes body;
    write_varint(body, packet_id);
    body.insert(body.end(), payload.begin(), payload.end());
    if (body.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("packet is too large for a protocol VarInt");
    }

    Bytes compressed_body;
    if (body.size() >= threshold) {
        const auto bound = compressBound(static_cast<uLong>(body.size()));
        compressed_body.resize(static_cast<std::size_t>(bound));
        auto compressed_size = bound;
        const auto result = compress2(
            compressed_body.data(),
            &compressed_size,
            body.data(),
            static_cast<uLong>(body.size()),
            Z_DEFAULT_COMPRESSION);
        if (result != Z_OK) {
            throw std::runtime_error("zlib failed to compress packet");
        }
        compressed_body.resize(static_cast<std::size_t>(compressed_size));
    }

    Bytes framed_payload;
    write_varint(framed_payload, compressed_body.empty() ? 0 : static_cast<std::int32_t>(body.size()));
    const auto& data = compressed_body.empty() ? body : compressed_body;
    framed_payload.insert(framed_payload.end(), data.begin(), data.end());
    if (framed_payload.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("compressed packet is too large for a protocol VarInt");
    }

    Bytes framed;
    write_varint(framed, static_cast<std::int32_t>(framed_payload.size()));
    framed.insert(framed.end(), framed_payload.begin(), framed_payload.end());
    return framed;
}

Bytes decode_frame_payload(const std::span<const std::uint8_t> frame_payload,
                           const std::size_t threshold,
                           const std::size_t max_decompressed_size) {
    Reader reader(frame_payload);
    const auto encoded_size = reader.read_varint();
    if (encoded_size < 0 || static_cast<std::size_t>(encoded_size) > max_decompressed_size) {
        throw DecodeError("decompressed packet length is out of bounds");
    }

    const auto compressed_data = reader.read_bytes(reader.remaining());
    if (encoded_size == 0) {
        if (compressed_data.size() >= threshold) {
            throw DecodeError("uncompressed packet exceeds compression threshold");
        }
        return {compressed_data.begin(), compressed_data.end()};
    }
    if (static_cast<std::size_t>(encoded_size) < threshold) {
        throw DecodeError("compressed packet is below compression threshold");
    }

    Bytes output(static_cast<std::size_t>(encoded_size));
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(compressed_data.data());
    stream.avail_in = static_cast<uInt>(compressed_data.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    if (inflateInit(&stream) != Z_OK) {
        throw std::runtime_error("zlib failed to initialize decompression");
    }
    const auto cleanup = std::unique_ptr<z_stream, decltype(&inflateEnd)>(&stream, inflateEnd);
    const auto result = inflate(&stream, Z_FINISH);
    if (result != Z_STREAM_END || stream.total_out != output.size() || stream.avail_in != 0) {
        throw DecodeError("invalid compressed packet payload");
    }
    return output;
}

} // namespace mc::protocol