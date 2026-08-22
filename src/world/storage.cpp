#include "mc/world/storage.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <zlib.h>

namespace mc::world {
namespace {

constexpr std::size_t sector_size = 4096;
constexpr std::size_t header_size = sector_size * 2;
constexpr std::size_t max_uncompressed_chunk_size = 16U * 1024U * 1024U;
constexpr std::array<std::uint8_t, 4> payload_magic{'M', 'C', 'C', '1'};
constexpr std::array<std::uint8_t, 4> metadata_magic_v1{'M', 'C', 'L', '1'};
constexpr std::array<std::uint8_t, 4> metadata_magic_v2{'M', 'C', 'L', '2'};

[[nodiscard]] std::int32_t floor_div(const std::int32_t value,
                                     const std::int32_t divisor) noexcept {
    const auto quotient = value / divisor;
    const auto remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

[[nodiscard]] std::uint8_t floor_mod(const std::int32_t value,
                                     const std::int32_t divisor) noexcept {
    auto result = value % divisor;
    if (result < 0) {
        result += divisor;
    }
    return static_cast<std::uint8_t>(result);
}

void append_u16(ChunkPayload& output, const std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(ChunkPayload& output, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_u64(ChunkPayload& output, const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_string(ChunkPayload& output, const std::string_view value) {
    if (value.size() > 65'535) {
        throw std::length_error("metadata string exceeds 65535 bytes");
    }
    append_u16(output, static_cast<std::uint16_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

[[nodiscard]] std::uint16_t consume_u16(std::span<const std::uint8_t>& input) {
    if (input.size() < 2) {
        throw std::runtime_error("chunk payload is truncated");
    }
    const auto value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(input[0]) << 8U | input[1]);
    input = input.subspan(2);
    return value;
}

[[nodiscard]] std::uint32_t consume_u32(std::span<const std::uint8_t>& input) {
    if (input.size() < 4) {
        throw std::runtime_error("chunk payload is truncated");
    }
    std::uint32_t value = 0;
    for (const auto byte : input.first(4)) {
        value = (value << 8U) | byte;
    }
    input = input.subspan(4);
    return value;
}

[[nodiscard]] std::uint64_t consume_u64(std::span<const std::uint8_t>& input) {
    if (input.size() < 8) {
        throw std::runtime_error("metadata payload is truncated");
    }
    std::uint64_t value = 0;
    for (const auto byte : input.first(8)) {
        value = (value << 8U) | byte;
    }
    input = input.subspan(8);
    return value;
}

[[nodiscard]] std::string consume_string(std::span<const std::uint8_t>& input) {
    const auto size = consume_u16(input);
    if (input.size() < size) {
        throw std::runtime_error("metadata string is truncated");
    }
    std::string value(reinterpret_cast<const char*>(input.data()), size);
    input = input.subspan(size);
    return value;
}

[[nodiscard]] std::uint32_t read_u32(std::fstream& file, const std::streamoff offset) {
    std::array<std::uint8_t, 4> bytes{};
    file.seekg(offset);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("failed to read region file header");
    }
    std::uint32_t value = 0;
    for (const auto byte : bytes) {
        value = (value << 8U) | byte;
    }
    return value;
}

void write_u32(std::fstream& file, const std::streamoff offset, const std::uint32_t value) {
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1) * 8);
        bytes[index] = static_cast<std::uint8_t>(value >> shift);
    }
    file.seekp(offset);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("failed to write region file header");
    }
}

[[nodiscard]] ChunkPayload compress_payload(const std::span<const std::uint8_t> payload) {
    if (payload.size() > max_uncompressed_chunk_size) {
        throw std::length_error("chunk payload exceeds storage limit");
    }
    const auto bound = compressBound(static_cast<uLong>(payload.size()));
    ChunkPayload compressed(static_cast<std::size_t>(bound));
    auto compressed_size = bound;
    if (compress2(compressed.data(), &compressed_size, payload.data(),
                  static_cast<uLong>(payload.size()), Z_DEFAULT_COMPRESSION) != Z_OK) {
        throw std::runtime_error("failed to compress region chunk");
    }
    compressed.resize(static_cast<std::size_t>(compressed_size));
    return compressed;
}

[[nodiscard]] ChunkPayload decompress_payload(const std::span<const std::uint8_t> payload) {
    for (std::size_t capacity = 256U * 1024U;
         capacity <= max_uncompressed_chunk_size;
         capacity *= 2U) {
        ChunkPayload output(capacity);
        auto output_size = static_cast<uLongf>(output.size());
        const auto result = uncompress(output.data(), &output_size, payload.data(),
                                       static_cast<uLong>(payload.size()));
        if (result == Z_OK) {
            output.resize(static_cast<std::size_t>(output_size));
            return output;
        }
        if (result != Z_BUF_ERROR) {
            throw std::runtime_error("failed to decompress region chunk");
        }
    }
    throw std::length_error("decompressed region chunk exceeds storage limit");
}

[[nodiscard]] std::size_t location_index(const std::uint8_t x, const std::uint8_t z) {
    if (x >= 32 || z >= 32) {
        throw std::out_of_range("local region coordinate is out of range");
    }
    return static_cast<std::size_t>(z) * 32 + x;
}

} // namespace

RegionPosition region_position(const ChunkPosition chunk) noexcept {
    return {floor_div(chunk.x, 32), floor_div(chunk.z, 32)};
}

std::uint8_t local_region_x(const ChunkPosition chunk) noexcept {
    return floor_mod(chunk.x, 32);
}

std::uint8_t local_region_z(const ChunkPosition chunk) noexcept {
    return floor_mod(chunk.z, 32);
}

ChunkPayload serialize_chunk(const Chunk& chunk) {
    ChunkPayload output(payload_magic.begin(), payload_magic.end());
    append_u32(output, static_cast<std::uint32_t>(chunk.position().x));
    append_u32(output, static_cast<std::uint32_t>(chunk.position().z));
    for (std::size_t z = 0; z < 16; ++z) {
        for (std::size_t x = 0; x < 16; ++x) {
            append_u32(output, static_cast<std::uint32_t>(chunk.height(x, z)));
        }
    }
    for (std::size_t quart_y = 0; quart_y < section_count * 4; ++quart_y) {
        for (std::size_t quart_z = 0; quart_z < 4; ++quart_z) {
            for (std::size_t quart_x = 0; quart_x < 4; ++quart_x) {
                output.push_back(static_cast<std::uint8_t>(
                    chunk.biome(quart_x, quart_y, quart_z)));
            }
        }
    }
    for (const auto& section : chunk.sections()) {
        for (const auto block : section.blocks()) {
            append_u16(output, static_cast<std::uint16_t>(block));
        }
    }
    return output;
}

Chunk deserialize_chunk(std::span<const std::uint8_t> payload) {
    if (payload.size() < payload_magic.size() ||
        !std::equal(payload_magic.begin(), payload_magic.end(), payload.begin())) {
        throw std::runtime_error("chunk payload has an invalid format marker");
    }
    payload = payload.subspan(payload_magic.size());
    const ChunkPosition position{
        static_cast<std::int32_t>(consume_u32(payload)),
        static_cast<std::int32_t>(consume_u32(payload)),
    };
    Chunk chunk(position);
    for (std::size_t z = 0; z < 16; ++z) {
        for (std::size_t x = 0; x < 16; ++x) {
            chunk.set_height(x, z, static_cast<std::int32_t>(consume_u32(payload)));
        }
    }
    for (std::size_t quart_y = 0; quart_y < section_count * 4; ++quart_y) {
        for (std::size_t quart_z = 0; quart_z < 4; ++quart_z) {
            for (std::size_t quart_x = 0; quart_x < 4; ++quart_x) {
                if (payload.empty()) {
                    throw std::runtime_error("chunk payload is truncated");
                }
                const auto biome = static_cast<BiomeId>(payload.front());
                payload = payload.subspan(1);
                chunk.set_biome(quart_x, quart_y, quart_z, biome);
            }
        }
    }
    for (std::size_t section = 0; section < section_count; ++section) {
        for (std::size_t y = 0; y < 16; ++y) {
            for (std::size_t z = 0; z < 16; ++z) {
                for (std::size_t x = 0; x < 16; ++x) {
                    const auto block = static_cast<BlockId>(consume_u16(payload));
                    chunk.set_block(x,
                                    min_build_y + static_cast<std::int32_t>(section * 16 + y),
                                    z,
                                    block);
                }
            }
        }
    }
    if (!payload.empty()) {
        throw std::runtime_error("chunk payload contains trailing data");
    }
    return chunk;
}

class RegionFile::Impl final {
public:
    explicit Impl(std::filesystem::path path) : path_(std::move(path)) {
        std::filesystem::create_directories(path_.parent_path());
        if (!std::filesystem::exists(path_)) {
            std::ofstream created(path_, std::ios::binary);
            const std::array<char, header_size> empty{};
            created.write(empty.data(), static_cast<std::streamsize>(empty.size()));
            if (!created) {
                throw std::runtime_error("failed to create region file");
            }
        }
        if (std::filesystem::file_size(path_) < header_size) {
            throw std::runtime_error("region file header is truncated");
        }
    }

    std::optional<ChunkPayload> read(const std::uint8_t x, const std::uint8_t z) const {
        std::lock_guard lock(mutex_);
        std::fstream file(path_, std::ios::in | std::ios::out | std::ios::binary);
        const auto index = location_index(x, z);
        const auto location = read_u32(file, static_cast<std::streamoff>(index * 4));
        const auto sector_offset = location >> 8U;
        const auto sector_count_value = location & 0xFFU;
        if (sector_offset == 0 || sector_count_value == 0) {
            return std::nullopt;
        }
        const auto record_offset = static_cast<std::streamoff>(sector_offset * sector_size);
        const auto length = read_u32(file, record_offset);
        if (length < 1 || length > sector_count_value * sector_size - 4) {
            throw std::runtime_error("region chunk length is invalid");
        }
        std::uint8_t compression = 0;
        file.seekg(record_offset + 4);
        file.read(reinterpret_cast<char*>(&compression), 1);
        if (!file || compression != 2) {
            throw std::runtime_error("unsupported region chunk compression");
        }
        ChunkPayload compressed(static_cast<std::size_t>(length - 1));
        file.read(reinterpret_cast<char*>(compressed.data()),
                  static_cast<std::streamsize>(compressed.size()));
        if (!file) {
            throw std::runtime_error("region chunk payload is truncated");
        }
        return decompress_payload(compressed);
    }

    void write(const std::uint8_t x,
               const std::uint8_t z,
               const std::span<const std::uint8_t> payload) {
        const auto compressed = compress_payload(payload);
        const auto record_size = 5U + compressed.size();
        const auto required_sectors = (record_size + sector_size - 1) / sector_size;
        if (required_sectors > 255) {
            throw std::length_error("compressed chunk exceeds region sector limit");
        }

        std::lock_guard lock(mutex_);
        std::fstream file(path_, std::ios::in | std::ios::out | std::ios::binary);
        const auto index = location_index(x, z);
        const auto old_location = read_u32(file, static_cast<std::streamoff>(index * 4));
        auto sector_offset = old_location >> 8U;
        const auto old_sectors = old_location & 0xFFU;
        if (sector_offset < 2 || old_sectors < required_sectors) {
            file.seekg(0, std::ios::end);
            const auto size = static_cast<std::uint64_t>(file.tellg());
            sector_offset = static_cast<std::uint32_t>(
                std::max<std::uint64_t>(2, (size + sector_size - 1) / sector_size));
        }
        if (sector_offset > 0xFFFFFFU) {
            throw std::length_error("region file exceeds location-table capacity");
        }

        const auto record_offset = static_cast<std::streamoff>(sector_offset * sector_size);
        write_u32(file, record_offset, static_cast<std::uint32_t>(compressed.size() + 1));
        const std::uint8_t compression = 2;
        file.seekp(record_offset + 4);
        file.write(reinterpret_cast<const char*>(&compression), 1);
        file.write(reinterpret_cast<const char*>(compressed.data()),
                   static_cast<std::streamsize>(compressed.size()));
        const auto padding_size = required_sectors * sector_size - record_size;
        const std::vector<char> padding(padding_size, 0);
        file.write(padding.data(), static_cast<std::streamsize>(padding.size()));

        const auto location = (sector_offset << 8U) | static_cast<std::uint32_t>(required_sectors);
        write_u32(file, static_cast<std::streamoff>(index * 4), location);
        const auto timestamp = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        write_u32(file,
                  static_cast<std::streamoff>(sector_size + index * 4),
                  static_cast<std::uint32_t>(timestamp));
        file.flush();
        if (!file) {
            throw std::runtime_error("failed to persist region chunk");
        }
    }

private:
    std::filesystem::path path_;
    mutable std::mutex mutex_;
};

RegionFile::RegionFile(std::filesystem::path path)
    : impl_(std::make_unique<Impl>(std::move(path))) {}
RegionFile::~RegionFile() = default;

std::optional<ChunkPayload> RegionFile::read(const std::uint8_t x,
                                             const std::uint8_t z) const {
    return impl_->read(x, z);
}

void RegionFile::write(const std::uint8_t x,
                       const std::uint8_t z,
                       const std::span<const std::uint8_t> payload) {
    impl_->write(x, z, payload);
}

class LevelStorage::Impl final {
public:
    explicit Impl(std::filesystem::path root) : root_(std::move(root)) {
        std::filesystem::create_directories(root_);
    }

    std::optional<Chunk> load_chunk(const ChunkPosition position) {
        const auto payload = region(position).read(local_region_x(position), local_region_z(position));
        if (!payload) {
            return std::nullopt;
        }
        auto chunk = deserialize_chunk(*payload);
        if (chunk.position() != position) {
            throw std::runtime_error("stored chunk position does not match region entry");
        }
        return chunk;
    }

    void save_chunk(const Chunk& chunk) {
        const auto payload = serialize_chunk(chunk);
        region(chunk.position()).write(
            local_region_x(chunk.position()), local_region_z(chunk.position()), payload);
    }

    std::optional<LevelMetadata> load_metadata() const {
        const auto path = root_ / "level.mcd";
        if (!std::filesystem::exists(path)) {
            return std::nullopt;
        }
        std::ifstream file(path, std::ios::binary);
        ChunkPayload payload{
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto is_v1 = payload.size() >= metadata_magic_v1.size() &&
            std::equal(metadata_magic_v1.begin(), metadata_magic_v1.end(), payload.begin());
        const auto is_v2 = payload.size() >= metadata_magic_v2.size() &&
            std::equal(metadata_magic_v2.begin(), metadata_magic_v2.end(), payload.begin());
        if (file.bad() || (!is_v1 && !is_v2)) {
            throw std::runtime_error("level metadata is invalid or truncated");
        }
        std::span<const std::uint8_t> input(payload);
        input = input.subspan(metadata_magic_v2.size());
        LevelMetadata metadata;
        metadata.data_version = consume_u32(input);
        if (metadata.data_version > LevelMetadata::current_data_version) {
            throw std::runtime_error("world data version is newer than this server");
        }
        metadata.seed = consume_u64(input);
        metadata.spawn = {
            static_cast<std::int32_t>(consume_u32(input)),
            static_cast<std::int32_t>(consume_u32(input)),
            static_cast<std::int32_t>(consume_u32(input)),
        };
        const auto dimension_count = consume_u16(input);
        if (dimension_count == 0 || dimension_count > 64) {
            throw std::runtime_error("level metadata dimension count is invalid");
        }
        metadata.dimensions.clear();
        for (std::uint16_t index = 0; index < dimension_count; ++index) {
            metadata.dimensions.push_back(
                mc::core::ResourceLocation::parse(consume_string(input)));
        }
        const auto rule_count = consume_u16(input);
        if (rule_count > 1024) {
            throw std::runtime_error("level metadata game rule count is invalid");
        }
        metadata.game_rules.clear();
        for (std::uint16_t index = 0; index < rule_count; ++index) {
            metadata.game_rules.emplace(consume_string(input), consume_string(input));
        }
        if (is_v2) {
            const auto state_count = consume_u16(input);
            if (state_count > 1024) {
                throw std::runtime_error("level metadata world-state count is invalid");
            }
            for (std::uint16_t index = 0; index < state_count; ++index) {
                metadata.world_state.emplace(consume_string(input), consume_string(input));
            }
        }
        if (!input.empty()) {
            throw std::runtime_error("level metadata contains trailing data");
        }
        return metadata;
    }

    void save_metadata(const LevelMetadata& metadata) {
        if (metadata.data_version > LevelMetadata::current_data_version ||
            metadata.dimensions.empty() || metadata.dimensions.size() > 64 ||
            metadata.game_rules.size() > 1024 || metadata.world_state.size() > 1024) {
            throw std::invalid_argument("level metadata values are out of bounds");
        }
        ChunkPayload payload(metadata_magic_v2.begin(), metadata_magic_v2.end());
        append_u32(payload, metadata.data_version);
        append_u64(payload, metadata.seed);
        append_u32(payload, static_cast<std::uint32_t>(metadata.spawn.x));
        append_u32(payload, static_cast<std::uint32_t>(metadata.spawn.y));
        append_u32(payload, static_cast<std::uint32_t>(metadata.spawn.z));
        append_u16(payload, static_cast<std::uint16_t>(metadata.dimensions.size()));
        for (const auto& dimension : metadata.dimensions) {
            append_string(payload, dimension.to_string());
        }
        append_u16(payload, static_cast<std::uint16_t>(metadata.game_rules.size()));
        for (const auto& [name, value] : metadata.game_rules) {
            append_string(payload, name);
            append_string(payload, value);
        }
        append_u16(payload, static_cast<std::uint16_t>(metadata.world_state.size()));
        for (const auto& [name, value] : metadata.world_state) {
            append_string(payload, name);
            append_string(payload, value);
        }

        const auto path = root_ / "level.mcd";
        const auto temporary = root_ / "level.mcd.tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(payload.data()),
                       static_cast<std::streamsize>(payload.size()));
            file.flush();
            if (!file) {
                throw std::runtime_error("failed to persist level metadata temporary file");
            }
        }
        std::filesystem::rename(temporary, path);
    }

private:
    RegionFile& region(const ChunkPosition position) {
        const auto coordinate = region_position(position);
        const auto key = std::pair{coordinate.x, coordinate.z};
        std::lock_guard lock(mutex_);
        auto& file = regions_[key];
        if (!file) {
            const auto name = "r." + std::to_string(coordinate.x) + "." +
                std::to_string(coordinate.z) + ".mca";
            file = std::make_unique<RegionFile>(root_ / "region" / name);
        }
        return *file;
    }

    std::filesystem::path root_;
    std::mutex mutex_;
    std::map<std::pair<std::int32_t, std::int32_t>, std::unique_ptr<RegionFile>> regions_;
};

LevelStorage::LevelStorage(std::filesystem::path root)
    : impl_(std::make_unique<Impl>(std::move(root))) {}
LevelStorage::~LevelStorage() = default;

std::optional<Chunk> LevelStorage::load_chunk(const ChunkPosition position) {
    return impl_->load_chunk(position);
}

void LevelStorage::save_chunk(const Chunk& chunk) {
    impl_->save_chunk(chunk);
}

std::optional<LevelMetadata> LevelStorage::load_metadata() const {
    return impl_->load_metadata();
}

void LevelStorage::save_metadata(const LevelMetadata& metadata) {
    impl_->save_metadata(metadata);
}

} // namespace mc::world