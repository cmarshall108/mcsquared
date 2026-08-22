#include "mc/protocol/packets.hpp"

#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <string_view>
#include <type_traits>

#include <openssl/evp.h>

namespace mc::protocol {
namespace {

void expect_packet_end(const Reader& packet) {
    if (!packet.empty()) {
        throw DecodeError("packet contains trailing data");
    }
}

} // namespace

Handshake decode_handshake(Reader& packet) {
    if (packet.read_varint() != 0) {
        throw DecodeError("expected handshake packet");
    }

    Handshake handshake{
        packet.read_varint(),
        packet.read_string(255),
        packet.read_u16_be(),
        ConnectionState::closed,
    };
    switch (packet.read_varint()) {
    case 1: handshake.next_state = ConnectionState::status; break;
    case 2: handshake.next_state = ConnectionState::login; break;
    case 3: handshake.next_state = ConnectionState::transfer; break;
    default: throw DecodeError("unsupported handshake intention");
    }
    expect_packet_end(packet);
    return handshake;
}

Bytes encode_cookie_request(const std::int32_t packet_id, const std::string_view key) {
    Bytes payload;
    write_string(payload, key);
    return frame_packet(packet_id, payload);
}

CookieResponse decode_cookie_response(Reader& packet,
                                      const std::int32_t expected_packet_id) {
    if (packet.read_varint() != expected_packet_id) {
        throw DecodeError("expected cookie response packet");
    }
    CookieResponse response{packet.read_string(32'767), std::nullopt};
    if (packet.read_bool()) {
        response.payload = packet.read_byte_array(5'120);
    }
    expect_packet_end(packet);
    return response;
}

namespace login {

Hello decode_hello(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::hello)) {
        throw DecodeError("expected login hello packet");
    }
    Hello hello{packet.read_string(16), packet.read_uuid()};
    expect_packet_end(packet);
    return hello;
}

EncryptionResponse decode_encryption_response(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::key)) {
        throw DecodeError("expected login key packet");
    }
    EncryptionResponse response{
        packet.read_byte_array(512),
        packet.read_byte_array(512),
    };
    expect_packet_end(packet);
    return response;
}

Bytes encode_encryption_request(const std::string_view server_id,
                                const std::span<const std::uint8_t> public_key,
                                const std::span<const std::uint8_t> challenge,
                                const bool should_authenticate) {
    Bytes payload;
    write_string(payload, server_id);
    write_byte_array(payload, public_key);
    write_byte_array(payload, challenge);
    write_bool(payload, should_authenticate);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::hello), payload);
}

Bytes encode_disconnect(const std::string_view json_component) {
    if (json_component.size() > 262'144) {
        throw std::length_error("Login disconnect component exceeds protocol limit");
    }
    Bytes payload;
    write_string(payload, json_component);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::disconnect), payload);
}

Bytes encode_custom_query(const std::int32_t transaction_id,
                          const std::string_view channel,
                          const std::span<const std::uint8_t> query_payload) {
    if (query_payload.size() > 1U * 1024U * 1024U) {
        throw std::length_error("Login custom query exceeds protocol limit");
    }
    Bytes payload;
    write_varint(payload, transaction_id);
    write_string(payload, channel);
    payload.insert(payload.end(), query_payload.begin(), query_payload.end());
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::custom_query), payload);
}

CustomQueryAnswer decode_custom_query_answer(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::custom_query_answer)) {
        throw DecodeError("expected Login custom query answer packet");
    }
    CustomQueryAnswer answer{packet.read_varint(), std::nullopt};
    if (packet.read_bool()) {
        if (packet.remaining() > 1U * 1024U * 1024U) {
            throw DecodeError("Login custom query answer exceeds protocol limit");
        }
        const auto data = packet.read_bytes(packet.remaining());
        answer.payload = Bytes(data.begin(), data.end());
    }
    expect_packet_end(packet);
    return answer;
}

Bytes encode_compression(const std::size_t threshold) {
    if (threshold > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::invalid_argument("compression threshold exceeds VarInt range");
    }
    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(threshold));
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::compression), payload);
}

Bytes encode_finished_payload(const GameProfile& profile, const Uuid& session_id) {
    if (profile.name.empty() || profile.name.size() > 16 || profile.properties.size() > 16) {
        throw std::invalid_argument("Login profile is outside protocol bounds");
    }
    Bytes payload;
    write_uuid(payload, profile.id);
    write_string(payload, profile.name);
    write_varint(payload, static_cast<std::int32_t>(profile.properties.size()));
    for (const auto& property : profile.properties) {
        if (property.name.size() > 64 || property.value.size() > 32'767 ||
            (property.signature && property.signature->size() > 1'024)) {
            throw std::invalid_argument("Login profile property is outside protocol bounds");
        }
        write_string(payload, property.name);
        write_string(payload, property.value);
        write_bool(payload, property.signature.has_value());
        if (property.signature) {
            write_string(payload, *property.signature);
        }
    }
    write_uuid(payload, session_id);
    return payload;
}

Bytes encode_finished(const GameProfile& profile, const Uuid& session_id) {
    const auto payload = encode_finished_payload(profile, session_id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::finished), payload);
}

Bytes encode_finished(const Hello& hello, const Uuid& session_id) {
    return encode_finished(GameProfile{hello.profile_id, hello.name, {}}, session_id);
}

} // namespace login

namespace configuration {
namespace {

constexpr std::size_t max_known_packs = 64;

void write_known_pack(Bytes& payload, const KnownPack& pack) {
    write_string(payload, pack.name_space);
    write_string(payload, pack.id);
    write_string(payload, pack.version);
}

} // namespace

Bytes encode_brand(const std::string_view brand) {
    Bytes data;
    write_string(data, brand);
    return encode_custom_payload("minecraft:brand", data);
}

Bytes encode_custom_payload(const std::string_view channel,
                           const std::span<const std::uint8_t> data) {
    constexpr std::size_t max_custom_payload = 1U * 1024U * 1024U;
    if (data.size() > max_custom_payload) {
        throw std::length_error("Configuration custom payload exceeds limit");
    }
    Bytes payload;
    write_identifier(payload, channel);
    payload.insert(payload.end(), data.begin(), data.end());
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::custom_payload), payload);
}

Bytes encode_disconnect_text(const std::string_view text) {
    Bytes payload;
    nbt::write_any_tag(
        payload, nbt::string_tag(std::string(text)),
        {.max_bytes = 262'144, .max_depth = 64,
         .max_collection_entries = 4'096, .max_string_bytes = 65'535});
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::disconnect), payload);
}

Bytes encode_cookie_request(const std::string_view key) {
    return mc::protocol::encode_cookie_request(
        static_cast<std::int32_t>(ClientboundPacketId::cookie_request), key);
}

ClientInformation decode_client_information(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::client_information)) {
        throw DecodeError("expected Configuration client information packet");
    }
    ClientInformation information{
        packet.read_string(16),
        packet.read_i8(),
        static_cast<std::uint8_t>(packet.read_varint()),
        packet.read_bool(),
        packet.read_u8(),
        static_cast<std::uint8_t>(packet.read_varint()),
        packet.read_bool(),
        packet.read_bool(),
        static_cast<std::uint8_t>(packet.read_varint()),
    };
    if (information.chat_visibility > 2 || information.main_hand > 1 ||
        information.particle_status > 2) {
        throw DecodeError("Configuration client information enum is out of bounds");
    }
    expect_packet_end(packet);
    return information;
}

CookieResponse decode_cookie_response(Reader& packet) {
    return mc::protocol::decode_cookie_response(
        packet, static_cast<std::int32_t>(ServerboundPacketId::cookie_response));
}

CustomPayload decode_custom_payload(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::custom_payload)) {
        throw DecodeError("expected Configuration custom payload packet");
    }
    CustomPayload payload{packet.read_identifier(), {}};
    constexpr std::size_t max_custom_payload = 1U * 1024U * 1024U;
    if (packet.remaining() > max_custom_payload) {
        throw DecodeError("Configuration custom payload exceeds limit");
    }
    const auto data = packet.read_bytes(packet.remaining());
    payload.data.assign(data.begin(), data.end());
    return payload;
}

Bytes encode_finish() {
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::finish), {});
}

void decode_finish(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::finish)) {
        throw DecodeError("expected Configuration finish packet");
    }
    expect_packet_end(packet);
}

Bytes encode_keep_alive(const std::int64_t id) {
    Bytes payload;
    write_i64_be(payload, id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::keep_alive), payload);
}

std::int64_t decode_keep_alive(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::keep_alive)) {
        throw DecodeError("expected Configuration keepalive packet");
    }
    const auto id = packet.read_i64_be();
    expect_packet_end(packet);
    return id;
}

Bytes encode_ping(const std::int32_t id) {
    Bytes payload;
    write_i32_be(payload, id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::ping), payload);
}

std::int32_t decode_pong(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::pong)) {
        throw DecodeError("expected Configuration pong packet");
    }
    const auto id = packet.read_i32_be();
    expect_packet_end(packet);
    return id;
}

Bytes encode_reset_chat() {
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::reset_chat), {});
}

Bytes encode_resource_pack_pop(const std::optional<Uuid> id) {
    Bytes payload;
    write_optional<Uuid>(payload, id, [](Bytes& output, const Uuid& value) {
        write_uuid(output, value);
    });
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::resource_pack_pop), payload);
}

Bytes encode_resource_pack_push(const Uuid id,
                                const std::string_view url,
                                const std::string_view hash,
                                const bool required) {
    if (url.size() > 32'767 || hash.size() > 40) {
        throw std::length_error("Configuration resource pack field exceeds limit");
    }
    Bytes payload;
    write_uuid(payload, id);
    write_string(payload, url);
    write_string(payload, hash);
    write_bool(payload, required);
    write_bool(payload, false);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::resource_pack_push), payload);
}

ResourcePackResponse decode_resource_pack_response(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::resource_pack)) {
        throw DecodeError("expected Configuration resource pack response");
    }
    ResourcePackResponse response{packet.read_uuid(), ResourcePackAction::discarded};
    const auto action = packet.read_varint();
    if (action < 0 || action > static_cast<std::int32_t>(ResourcePackAction::discarded)) {
        throw DecodeError("Configuration resource pack action is out of bounds");
    }
    response.action = static_cast<ResourcePackAction>(action);
    expect_packet_end(packet);
    return response;
}

CustomClickAction decode_custom_click_action(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::custom_click_action)) {
        throw DecodeError("expected Configuration custom click action");
    }
    CustomClickAction action{packet.read_identifier(), std::nullopt};
    const auto encoded = packet.read_byte_array(65'536);
    Reader tag_reader(encoded);
    auto tag = nbt::read_any_tag(
        tag_reader,
        {.max_bytes = 32'768, .max_depth = 16,
         .max_collection_entries = 8'192, .max_string_bytes = 32'767});
    if (!tag_reader.empty()) {
        throw DecodeError("Configuration custom click NBT contains trailing data");
    }
    if (tag.type != nbt::Type::end) {
        action.payload = std::move(tag);
    }
    expect_packet_end(packet);
    return action;
}

Bytes encode_store_cookie(const std::string_view key,
                          const std::span<const std::uint8_t> cookie_payload) {
    if (cookie_payload.size() > 5'120) {
        throw std::length_error("Configuration cookie payload exceeds 5120 bytes");
    }
    Bytes payload;
    write_identifier(payload, key);
    write_byte_array(payload, cookie_payload);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::store_cookie), payload);
}

Bytes encode_transfer(const std::string_view host, const std::uint16_t port) {
    if (host.empty()) {
        throw std::invalid_argument("transfer host must not be empty");
    }
    Bytes payload;
    write_string(payload, host);
    write_varint(payload, port);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::transfer), payload);
}

Bytes encode_enabled_features() {
    const std::array<std::string, 1> vanilla{"minecraft:vanilla"};
    return encode_enabled_features(vanilla);
}

Bytes encode_enabled_features(const std::span<const std::string> features) {
    if (features.size() > 1'024) {
        throw std::length_error("enabled feature set exceeds limit");
    }
    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(features.size()));
    for (const auto& feature : features) {
        write_identifier(payload, feature);
    }
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::update_enabled_features), payload);
}

Bytes encode_select_known_packs() {
    Bytes payload;
    write_varint(payload, 1);
    write_known_pack(payload, KnownPack{"minecraft", "core", "26.2"});
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::select_known_packs), payload);
}

std::vector<KnownPack> decode_selected_known_packs(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::select_known_packs)) {
        throw DecodeError("expected selected known packs packet");
    }
    const auto count = packet.read_varint();
    if (count < 0 || static_cast<std::size_t>(count) > max_known_packs) {
        throw DecodeError("known pack count is out of bounds");
    }

    std::vector<KnownPack> packs;
    packs.reserve(static_cast<std::size_t>(count));
    for (std::int32_t index = 0; index < count; ++index) {
        packs.push_back(KnownPack{
            packet.read_string(32'767),
            packet.read_string(32'767),
            packet.read_string(32'767),
        });
    }
    expect_packet_end(packet);
    return packs;
}

Bytes encode_empty_tags() {
    Bytes payload;
    write_varint(payload, 0);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::update_tags), payload);
}

Bytes encode_custom_report_details(
    const std::span<const std::pair<std::string, std::string>> details) {
    if (details.size() > 32) {
        throw std::length_error("custom report details exceed limit");
    }
    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(details.size()));
    for (const auto& [title, description] : details) {
        if (title.size() > 128 || description.size() > 4'096) {
            throw std::length_error("custom report detail field exceeds limit");
        }
        write_string(payload, title);
        write_string(payload, description);
    }
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::custom_report_details), payload);
}

Bytes encode_empty_server_links() {
    Bytes payload;
    write_varint(payload, 0);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::server_links), payload);
}

Bytes encode_clear_dialog() {
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::clear_dialog), {});
}

Bytes encode_show_dialog(const nbt::Tag& direct_dialog) {
    if (direct_dialog.type == nbt::Type::end) {
        throw std::invalid_argument("Configuration dialog must not be an END tag");
    }
    Bytes payload;
    nbt::write_any_tag(
        payload, direct_dialog,
        {.max_bytes = 2U * 1024U * 1024U, .max_depth = 64,
         .max_collection_entries = 65'536, .max_string_bytes = 65'535});
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::show_dialog), payload);
}

Bytes encode_code_of_conduct(const std::string_view text) {
    Bytes payload;
    write_string(payload, text);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::code_of_conduct), payload);
}

void decode_accept_code_of_conduct(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::accept_code_of_conduct)) {
        throw DecodeError("expected accept code of conduct packet");
    }
    expect_packet_end(packet);
}

std::vector<RegistryTags> load_normalized_tags(std::istream& input) {
    constexpr std::size_t max_registries = 1'024;
    constexpr std::size_t max_tags = 16'384;
    constexpr std::size_t max_entries = 65'536;
    auto parse_size = [](const std::string_view value) {
        std::size_t result = 0;
        const auto [end, error] = std::from_chars(
            value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size()) {
            throw std::runtime_error("normalized tag stream contains an invalid integer");
        }
        return result;
    };
    auto split = [](const std::string_view value, const char delimiter) {
        std::vector<std::string_view> fields;
        std::size_t start = 0;
        while (true) {
            const auto separator = value.find(delimiter, start);
            fields.push_back(value.substr(start, separator - start));
            if (separator == std::string_view::npos) return fields;
            start = separator + 1;
        }
    };

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("normalized tag stream is empty");
    }
    const auto header = split(line, '\t');
    if (header.size() != 3 || header[0] != "MCTAGS1" ||
        !header[1].starts_with("registries=") || !header[2].starts_with("tags=")) {
        throw std::runtime_error("normalized tag stream has an invalid header");
    }
    const auto expected_registries = parse_size(header[1].substr(11));
    const auto expected_tags = parse_size(header[2].substr(5));
    if (expected_registries > max_registries || expected_tags > max_tags) {
        throw std::runtime_error("normalized tag stream exceeds resource limits");
    }

    std::vector<RegistryTags> registries;
    std::size_t loaded_tags = 0;
    while (std::getline(input, line)) {
        const auto registry_fields = split(line, '\t');
        if (registry_fields.size() != 3 || registry_fields[0] != "R") {
            throw std::runtime_error("normalized tag registry record is invalid");
        }
        RegistryTags registry{std::string(registry_fields[1]), {}};
        const auto tag_count = parse_size(registry_fields[2]);
        if (tag_count > max_tags - loaded_tags) {
            throw std::runtime_error("normalized tag count exceeds resource limits");
        }
        registry.tags.reserve(tag_count);
        for (std::size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
            if (!std::getline(input, line)) {
                throw std::runtime_error("normalized tag stream is truncated");
            }
            const auto tag_fields = split(line, '\t');
            if (tag_fields.size() != 3 || tag_fields[0] != "T") {
                throw std::runtime_error("normalized tag record is invalid");
            }
            TagData tag{std::string(tag_fields[1]), {}};
            if (!tag_fields[2].empty()) {
                for (const auto entry : split(tag_fields[2], ',')) {
                    const auto raw_id = parse_size(entry);
                    if (raw_id > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
                        tag.entries.size() >= max_entries) {
                        throw std::runtime_error("normalized tag entry exceeds resource limits");
                    }
                    tag.entries.push_back(static_cast<std::int32_t>(raw_id));
                }
            }
            registry.tags.push_back(std::move(tag));
        }
        loaded_tags += tag_count;
        registries.push_back(std::move(registry));
    }
    if (registries.size() != expected_registries || loaded_tags != expected_tags) {
        throw std::runtime_error("normalized tag stream count does not match header");
    }
    return registries;
}

Bytes encode_tags(const std::span<const RegistryTags> registries) {
    if (registries.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("registry tag map exceeds protocol limits");
    }
    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(registries.size()));
    for (const auto& registry : registries) {
        write_identifier(payload, registry.registry);
        write_varint(payload, static_cast<std::int32_t>(registry.tags.size()));
        for (const auto& tag : registry.tags) {
            write_identifier(payload, tag.name);
            write_varint(payload, static_cast<std::int32_t>(tag.entries.size()));
            for (const auto raw_id : tag.entries) write_varint(payload, raw_id);
        }
    }
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::update_tags), payload);
}

std::vector<RegistryData> load_normalized_registry_data(std::istream& input) {
    constexpr std::size_t max_registries = 1'024;
    constexpr std::size_t max_entries = 65'536;
    auto parse_size = [](const std::string_view value) {
        std::size_t result = 0;
        const auto [end, error] = std::from_chars(
            value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size()) {
            throw std::runtime_error("normalized registry stream contains an invalid integer");
        }
        return result;
    };
    auto split = [](const std::string_view value) {
        std::vector<std::string_view> fields;
        std::size_t start = 0;
        while (true) {
            const auto separator = value.find('\t', start);
            fields.push_back(value.substr(start, separator - start));
            if (separator == std::string_view::npos) return fields;
            start = separator + 1;
        }
    };

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("normalized registry stream is empty");
    }
    const auto header = split(line);
    if (header.size() != 3 || header[0] != "MCREGISTRYDATA1" ||
        !header[1].starts_with("registries=") || !header[2].starts_with("entries=")) {
        throw std::runtime_error("normalized registry stream has an invalid header");
    }
    const auto expected_registries = parse_size(header[1].substr(11));
    const auto expected_entries = parse_size(header[2].substr(8));
    if (expected_registries > max_registries || expected_entries > max_entries) {
        throw std::runtime_error("normalized registry stream exceeds resource limits");
    }

    std::vector<RegistryData> registries;
    std::size_t loaded_entries = 0;
    while (std::getline(input, line)) {
        const auto fields = split(line);
        if (fields.size() != 3 || fields[0] != "R") {
            throw std::runtime_error("normalized registry record is invalid");
        }
        RegistryData registry{std::string(fields[1]), {}};
        const auto entry_count = parse_size(fields[2]);
        if (entry_count > max_entries - loaded_entries) {
            throw std::runtime_error("normalized registry entry count exceeds limits");
        }
        registry.entries.reserve(entry_count);
        for (std::size_t entry_index = 0; entry_index < entry_count; ++entry_index) {
            if (!std::getline(input, line)) {
                throw std::runtime_error("normalized registry stream is truncated");
            }
            const auto entry_fields = split(line);
            if (entry_fields.size() != 2 || entry_fields[0] != "E") {
                throw std::runtime_error("normalized registry entry is invalid");
            }
            registry.entries.emplace_back(entry_fields[1]);
        }
        loaded_entries += entry_count;
        registries.push_back(std::move(registry));
    }
    if (registries.size() != expected_registries || loaded_entries != expected_entries) {
        throw std::runtime_error("normalized registry stream count does not match header");
    }
    return registries;
}

std::vector<Bytes> load_registry_fallback_packets(std::istream& input) {
    constexpr std::size_t expected_registry_count = 29;
    constexpr std::size_t max_fallback_packet_size = 2U * 1024U * 1024U;
    std::string line;
    if (!std::getline(input, line) ||
        line != "MCREGISTRYFALLBACK1\tprotocol=776\tregistries=29") {
        throw std::runtime_error("registry fallback stream has an invalid header");
    }
    std::vector<Bytes> packets;
    while (std::getline(input, line)) {
        const auto first = line.find('\t');
        const auto second = line.find('\t', first + 1);
        const auto third = line.find('\t', second + 1);
        if (first != 1 || line[0] != 'R' || second == std::string::npos ||
            third == std::string::npos ||
            line.size() - third - 1 > max_fallback_packet_size * 2U) {
            throw std::runtime_error("registry fallback record is invalid");
        }
        const auto registry = line.substr(first + 1, second - first - 1);
        const auto digest = line.substr(second + 1, third - second - 1);
        const auto encoded = std::string_view(line).substr(third + 1);
        if (digest.size() != 64 || encoded.empty() || encoded.size() % 2 != 0) {
            throw std::runtime_error("registry fallback digest or hex payload is invalid");
        }
        Bytes packet;
        packet.reserve(encoded.size() / 2);
        for (std::size_t index = 0; index < encoded.size(); index += 2) {
            std::uint32_t byte = 0;
            const auto [end, error] = std::from_chars(
                encoded.data() + index, encoded.data() + index + 2, byte, 16);
            if (error != std::errc{} || end != encoded.data() + index + 2) {
                throw std::runtime_error("registry fallback contains invalid hex");
            }
            packet.push_back(static_cast<std::uint8_t>(byte));
        }
        std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
        unsigned int hash_size = 0;
        auto* hash_context = EVP_MD_CTX_new();
        if (hash_context == nullptr ||
            EVP_DigestInit_ex(hash_context, EVP_sha256(), nullptr) != 1 ||
            EVP_DigestUpdate(hash_context, packet.data(), packet.size()) != 1 ||
            EVP_DigestFinal_ex(hash_context, hash.data(), &hash_size) != 1) {
            EVP_MD_CTX_free(hash_context);
            throw std::runtime_error("failed to hash registry fallback packet");
        }
        EVP_MD_CTX_free(hash_context);
        constexpr char hex[] = "0123456789abcdef";
        std::string actual_digest;
        actual_digest.reserve(hash_size * 2U);
        for (unsigned int index = 0; index < hash_size; ++index) {
            actual_digest.push_back(hex[hash[index] >> 4U]);
            actual_digest.push_back(hex[hash[index] & 0x0FU]);
        }
        if (actual_digest != digest) {
            throw std::runtime_error("registry fallback packet digest does not match");
        }
        Reader reader(packet);
        if (reader.read_varint() !=
            static_cast<std::int32_t>(ClientboundPacketId::registry_data)) {
            throw std::runtime_error("registry fallback packet has wrong packet ID");
        }
        const auto payload_offset = packet.size() - reader.remaining();
        if (reader.read_identifier() != registry) {
            throw std::runtime_error("registry fallback packet identity does not match record");
        }
        const auto payload = std::span<const std::uint8_t>(packet).subspan(payload_offset);
        packets.push_back(frame_packet(
            static_cast<std::int32_t>(ClientboundPacketId::registry_data), payload));
    }
    if (packets.size() != expected_registry_count) {
        throw std::runtime_error("registry fallback stream must contain 29 packets");
    }
    return packets;
}

Bytes encode_registry_data(const RegistryData& registry) {
    if (registry.entries.size() >
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("registry entry list exceeds protocol limits");
    }
    Bytes payload;
    write_identifier(payload, registry.registry);
    write_varint(payload, static_cast<std::int32_t>(registry.entries.size()));
    for (const auto& entry : registry.entries) {
        write_identifier(payload, entry);
        write_bool(payload, false);
    }
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::registry_data), payload);
}

} // namespace configuration

namespace play {

std::vector<Bytes> load_recipe_sync_packets(std::istream& input) {
    std::string line;
    if (!std::getline(input, line) || line != "MCRECIPESYNC1\tprotocol=776\tpackets=2") {
        throw std::runtime_error("recipe sync stream has an invalid header");
    }
    const std::map<std::string, std::int32_t> expected_ids{
        {"recipe_book_add", 0x4A},
        {"update_recipes", 0x85},
    };
    std::vector<Bytes> packets;
    while (std::getline(input, line)) {
        const auto first = line.find('\t');
        const auto second = line.find('\t', first + 1);
        const auto third = line.find('\t', second + 1);
        if (first != 1 || line[0] != 'P' || second == std::string::npos ||
            third == std::string::npos) {
            throw std::runtime_error("recipe sync record is invalid");
        }
        const auto name = line.substr(first + 1, second - first - 1);
        const auto expected = expected_ids.find(name);
        const auto digest = line.substr(second + 1, third - second - 1);
        const auto encoded = std::string_view(line).substr(third + 1);
        if (expected == expected_ids.end() || digest.size() != 64 || encoded.empty() ||
            encoded.size() % 2 != 0 || encoded.size() > 2U * 1024U * 1024U) {
            throw std::runtime_error("recipe sync metadata is invalid");
        }
        Bytes packet;
        packet.reserve(encoded.size() / 2);
        for (std::size_t index = 0; index < encoded.size(); index += 2) {
            std::uint32_t byte = 0;
            const auto [end, error] = std::from_chars(
                encoded.data() + index, encoded.data() + index + 2, byte, 16);
            if (error != std::errc{} || end != encoded.data() + index + 2) {
                throw std::runtime_error("recipe sync contains invalid hex");
            }
            packet.push_back(static_cast<std::uint8_t>(byte));
        }
        std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
        unsigned int hash_size = 0;
        auto* context = EVP_MD_CTX_new();
        if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
            EVP_DigestUpdate(context, packet.data(), packet.size()) != 1 ||
            EVP_DigestFinal_ex(context, hash.data(), &hash_size) != 1) {
            EVP_MD_CTX_free(context);
            throw std::runtime_error("failed to hash recipe sync packet");
        }
        EVP_MD_CTX_free(context);
        constexpr char hex[] = "0123456789abcdef";
        std::string actual_digest;
        for (unsigned int index = 0; index < hash_size; ++index) {
            actual_digest.push_back(hex[hash[index] >> 4U]);
            actual_digest.push_back(hex[hash[index] & 0x0FU]);
        }
        Reader reader(packet);
        if (actual_digest != digest || reader.read_varint() != expected->second) {
            throw std::runtime_error("recipe sync digest or packet ID does not match");
        }
        const auto payload_offset = packet.size() - reader.remaining();
        packets.push_back(frame_packet(
            expected->second, std::span<const std::uint8_t>(packet).subspan(payload_offset)));
    }
    if (packets.size() != expected_ids.size()) {
        throw std::runtime_error("recipe sync stream must contain two packets");
    }
    return packets;
}

Bytes encode_login(const bool hardcore) {
    Bytes payload;
    write_i32_be(payload, 1);
    write_bool(payload, hardcore);
    write_varint(payload, 1);
    write_string(payload, "minecraft:overworld");
    write_varint(payload, 20);
    write_varint(payload, 10);
    write_varint(payload, 10);
    write_bool(payload, false);
    write_bool(payload, true);
    write_bool(payload, false);

    write_varint(payload, 0);
    write_string(payload, "minecraft:overworld");
    write_i64_be(payload, 0);
    payload.push_back(0);
    payload.push_back(0xFFU);
    write_bool(payload, false);
    write_bool(payload, false);
    write_bool(payload, false);
    write_varint(payload, 0);
    write_varint(payload, 63);

    write_bool(payload, false);
    write_bool(payload, false);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::login), payload);
}

Bytes encode_award_stats(const std::span<const StatisticEntry> statistics) {
    if (statistics.size() > 16'384) throw std::length_error("statistics exceed limit");
    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(statistics.size()));
    for (const auto& statistic : statistics) {
        if (statistic.type_id < 0 || statistic.type_id > 8 ||
            statistic.value_id < 0 || statistic.value < 0) {
            throw std::invalid_argument("statistic fields are invalid");
        }
        write_varint(payload, statistic.type_id);
        write_varint(payload, statistic.value_id);
        write_varint(payload, statistic.value);
    }
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::award_stats), payload);
}

Bytes encode_keep_alive(const std::int64_t id) {
    Bytes payload;
    write_i64_be(payload, id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::keep_alive), payload);
}

std::int64_t decode_keep_alive(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::keep_alive)) {
        throw DecodeError("expected Play keepalive packet");
    }
    const auto id = packet.read_i64_be();
    expect_packet_end(packet);
    return id;
}

Bytes encode_player_position(const std::int32_t teleport_id,
                                 const double x,
                                 const double y,
                                 const double z,
                                 const float yaw,
                                 const float pitch) {
    Bytes payload;
    write_varint(payload, teleport_id);
    write_f64_be(payload, x);
    write_f64_be(payload, y);
    write_f64_be(payload, z);
    write_f64_be(payload, 0.0);
    write_f64_be(payload, 0.0);
    write_f64_be(payload, 0.0);
    write_f32_be(payload, yaw);
    write_f32_be(payload, pitch);
    write_i32_be(payload, 0);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::player_position), payload);
}

Bytes encode_respawn(const bool keep_all_data,
                     const std::uint8_t game_mode,
                     const std::int8_t previous_game_mode) {
    if (game_mode > 3 || previous_game_mode < -1 || previous_game_mode > 3) {
        throw std::invalid_argument("respawn game mode is invalid");
    }
    Bytes payload;
    write_varint(payload, 0);
    write_string(payload, "minecraft:overworld");
    write_i64_be(payload, 0);
    write_i8(payload, static_cast<std::int8_t>(game_mode));
    write_i8(payload, previous_game_mode);
    write_bool(payload, false);
    write_bool(payload, false);
    write_bool(payload, false);
    write_varint(payload, 0);
    write_varint(payload, 63);
    write_i8(payload, keep_all_data ? 0x03 : 0x00);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::respawn), payload);
}

std::int32_t decode_teleport_acknowledgement(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::accept_teleportation)) {
        throw DecodeError("expected teleport acknowledgement packet");
    }
    const auto teleport_id = packet.read_varint();
    expect_packet_end(packet);
    return teleport_id;
}

float decode_chunk_batch_received(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::chunk_batch_received)) {
        throw DecodeError("expected chunk batch feedback packet");
    }
    const auto desired_chunks_per_tick = packet.read_f32_be();
    if (!std::isfinite(desired_chunks_per_tick) || desired_chunks_per_tick <= 0.0F) {
        throw DecodeError("chunk batch feedback rate is invalid");
    }
    expect_packet_end(packet);
    return desired_chunks_per_tick;
}

PlayerPosition decode_player_position(Reader& packet) {
    const auto packet_id = packet.read_varint();
    if (packet_id != static_cast<std::int32_t>(ServerboundPacketId::move_player_pos) &&
        packet_id != static_cast<std::int32_t>(ServerboundPacketId::move_player_pos_rot)) {
        throw DecodeError("expected Play position packet");
    }
    PlayerPosition position{
        packet.read_f64_be(),
        packet.read_f64_be(),
        packet.read_f64_be(),
        std::nullopt,
        std::nullopt,
        false,
        false,
    };
    if (packet_id == static_cast<std::int32_t>(ServerboundPacketId::move_player_pos_rot)) {
        const auto yaw = packet.read_f32_be();
        const auto pitch = packet.read_f32_be();
        if (!std::isfinite(yaw) || !std::isfinite(pitch)) {
            throw DecodeError("Play rotation contains a non-finite value");
        }
        position.yaw = yaw;
        position.pitch = pitch;
    }
    const auto flags = packet.read_u8();
    if ((flags & 0xFCU) != 0) {
        throw DecodeError("Play movement flags contain reserved bits");
    }
    position.on_ground = (flags & 0x01U) != 0;
    position.horizontal_collision = (flags & 0x02U) != 0;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || std::abs(position.x) > 30'000'000.0 ||
        std::abs(position.z) > 30'000'000.0 || std::abs(position.y) > 20'000'000.0) {
        throw DecodeError("Play position is outside world bounds");
    }
    expect_packet_end(packet);
    return position;
}

namespace {

[[nodiscard]] std::pair<bool, bool> decode_movement_flags(Reader& packet) {
    const auto flags = packet.read_u8();
    if ((flags & 0xFCU) != 0) throw DecodeError("Play movement flags contain reserved bits");
    return {(flags & 0x01U) != 0, (flags & 0x02U) != 0};
}

void decode_empty_play_packet(Reader& packet, const ServerboundPacketId expected) {
    if (packet.read_varint() != static_cast<std::int32_t>(expected)) {
        throw DecodeError("unexpected empty Play packet");
    }
    expect_packet_end(packet);
}

} // namespace

PlayerRotation decode_player_rotation(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::move_player_rot)) {
        throw DecodeError("expected Play rotation packet");
    }
    PlayerRotation rotation{packet.read_f32_be(), packet.read_f32_be(), false, false};
    if (!std::isfinite(rotation.yaw) || !std::isfinite(rotation.pitch)) {
        throw DecodeError("Play rotation contains a non-finite value");
    }
    const auto [on_ground, collision] = decode_movement_flags(packet);
    rotation.on_ground = on_ground;
    rotation.horizontal_collision = collision;
    expect_packet_end(packet);
    return rotation;
}

std::pair<bool, bool> decode_player_status(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::move_player_status_only)) {
        throw DecodeError("expected Play movement status packet");
    }
    const auto flags = decode_movement_flags(packet);
    expect_packet_end(packet);
    return flags;
}

void decode_client_tick_end(Reader& packet) {
    decode_empty_play_packet(packet, ServerboundPacketId::client_tick_end);
}

void decode_configuration_acknowledged(Reader& packet) {
    decode_empty_play_packet(packet, ServerboundPacketId::configuration_acknowledged);
}

CookieResponse decode_cookie_response(Reader& packet) {
    return mc::protocol::decode_cookie_response(
        packet, static_cast<std::int32_t>(ServerboundPacketId::cookie_response));
}

void decode_player_loaded(Reader& packet) {
    decode_empty_play_packet(packet, ServerboundPacketId::player_loaded);
}

std::int64_t decode_ping_request(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::ping_request)) {
        throw DecodeError("expected Play ping request");
    }
    const auto time = packet.read_i64_be();
    expect_packet_end(packet);
    return time;
}

PickItemFromBlock decode_pick_item_from_block(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::pick_item_from_block)) {
        throw DecodeError("expected pick item from block packet");
    }
    const PickItemFromBlock result{packet.read_position(), packet.read_bool()};
    expect_packet_end(packet);
    return result;
}

PickItemFromEntity decode_pick_item_from_entity(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::pick_item_from_entity)) {
        throw DecodeError("expected pick item from entity packet");
    }
    const auto entity_id = packet.read_varint();
    if (entity_id < 0) throw DecodeError("picked entity ID is invalid");
    const PickItemFromEntity result{entity_id, packet.read_bool()};
    expect_packet_end(packet);
    return result;
}

bool decode_player_abilities(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::player_abilities)) {
        throw DecodeError("expected Play player abilities packet");
    }
    const auto flags = packet.read_u8();
    if ((flags & 0xFDU) != 0) throw DecodeError("player ability flags contain reserved bits");
    expect_packet_end(packet);
    return (flags & 0x02U) != 0;
}

PlayerAction decode_player_action(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::player_action)) {
        throw DecodeError("expected player action packet");
    }
    const auto action = packet.read_varint();
    if (action < 0 || action > static_cast<std::int32_t>(PlayerActionType::stab)) {
        throw DecodeError("player action type is out of bounds");
    }
    PlayerAction result{
        static_cast<PlayerActionType>(action),
        packet.read_position(),
        packet.read_u8(),
        packet.read_varint(),
    };
    if (result.direction > 5 || result.sequence < 0) {
        throw DecodeError("player action direction or sequence is out of bounds");
    }
    expect_packet_end(packet);
    return result;
}

PlayerCommand decode_player_command(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::player_command)) {
        throw DecodeError("expected player command packet");
    }
    const auto entity_id = packet.read_varint();
    const auto action = packet.read_varint();
    const auto data = packet.read_varint();
    if (entity_id < 0 || action < 0 || action > 6 || data < 0) {
        throw DecodeError("player command fields are invalid");
    }
    expect_packet_end(packet);
    return {entity_id, static_cast<PlayerCommandAction>(action), data};
}

PlayerInput decode_player_input(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::player_input)) {
        throw DecodeError("expected player input packet");
    }
    const auto flags = packet.read_u8();
    if ((flags & 0x80U) != 0) throw DecodeError("player input has reserved flags");
    expect_packet_end(packet);
    return {
        (flags & 0x01U) != 0, (flags & 0x02U) != 0,
        (flags & 0x04U) != 0, (flags & 0x08U) != 0,
        (flags & 0x10U) != 0, (flags & 0x20U) != 0,
        (flags & 0x40U) != 0,
    };
}

ClientCommandAction decode_client_command(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::client_command)) {
        throw DecodeError("expected client command packet");
    }
    const auto action = packet.read_varint();
    if (action < 0 || action > 2) throw DecodeError("client command action is invalid");
    expect_packet_end(packet);
    return static_cast<ClientCommandAction>(action);
}

std::int32_t decode_pong(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::pong)) {
        throw DecodeError("expected Play pong packet");
    }
    const auto id = packet.read_i32_be();
    expect_packet_end(packet);
    return id;
}

std::string decode_rename_item(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::rename_item)) {
        throw DecodeError("expected rename item packet");
    }
    auto name = packet.read_string(32'767);
    expect_packet_end(packet);
    return name;
}

configuration::ResourcePackResponse decode_resource_pack_response(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::resource_pack)) {
        throw DecodeError("expected Play resource pack response");
    }
    configuration::ResourcePackResponse response{
        packet.read_uuid(), configuration::ResourcePackAction::discarded};
    const auto action = packet.read_varint();
    if (action < 0 ||
        action > static_cast<std::int32_t>(configuration::ResourcePackAction::discarded)) {
        throw DecodeError("Play resource pack action is out of bounds");
    }
    response.action = static_cast<configuration::ResourcePackAction>(action);
    expect_packet_end(packet);
    return response;
}

std::int32_t decode_select_trade(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::select_trade)) {
        throw DecodeError("expected select trade packet");
    }
    const auto selection = packet.read_varint();
    if (selection < 0) throw DecodeError("trade selection must not be negative");
    expect_packet_end(packet);
    return selection;
}

std::pair<std::int32_t, std::int32_t> decode_container_button_click(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::container_button_click)) {
        throw DecodeError("expected container button click packet");
    }
    const auto container = packet.read_varint();
    const auto button = packet.read_varint();
    if (container < 0 || button < 0) throw DecodeError("container button fields are invalid");
    expect_packet_end(packet);
    return {container, button};
}

ContainerClick decode_container_click(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::container_click)) {
        throw DecodeError("expected container click packet");
    }
    ContainerClick click{
        packet.read_varint(), packet.read_varint(), packet.read_i16_be(),
        packet.read_i8(), ContainerInput::pickup, {}, {}};
    if (click.container_id < 0 || click.state_id < 0) {
        throw DecodeError("container click identity is invalid");
    }
    const auto input = packet.read_varint();
    if (input < 0 || input > static_cast<std::int32_t>(ContainerInput::pickup_all)) {
        throw DecodeError("container click input is invalid");
    }
    click.input = static_cast<ContainerInput>(input);
    const auto read_hashed_stack = [](Reader& input_reader) {
        SimpleItemStack item;
        if (!input_reader.read_bool()) {
            return item;
        }
        item.item_id = input_reader.read_varint();
        item.count = input_reader.read_varint();
        if (item.item_id < 0 || item.item_id >= 1'538 ||
            item.count <= 0 || item.count > 99) {
            throw DecodeError("hashed item stack is out of bounds");
        }
        const auto added_components = input_reader.read_varint();
        if (added_components < 0 || added_components > 256) {
            throw DecodeError("hashed added-component count is out of bounds");
        }
        if (added_components != 0) {
            throw DecodeError("hashed item components are not supported");
        }
        const auto removed_components = input_reader.read_varint();
        if (removed_components < 0 || removed_components > 256) {
            throw DecodeError("hashed removed-component count is out of bounds");
        }
        if (removed_components != 0) {
            throw DecodeError("hashed item components are not supported");
        }
        return item;
    };
    const auto changed_count = packet.read_varint();
    if (changed_count < 0 || changed_count > 128) {
        throw DecodeError("container changed-slot count is out of bounds");
    }
    click.changed_slots.reserve(static_cast<std::size_t>(changed_count));
    for (std::int32_t index = 0; index < changed_count; ++index) {
        const auto slot = packet.read_i16_be();
        if (std::any_of(click.changed_slots.begin(), click.changed_slots.end(),
                        [slot](const auto& change) { return change.first == slot; })) {
            throw DecodeError("container click contains a duplicate slot");
        }
        click.changed_slots.emplace_back(slot, read_hashed_stack(packet));
    }
    click.carried_item = read_hashed_stack(packet);
    expect_packet_end(packet);
    return click;
}

std::int32_t decode_container_close(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::container_close)) {
        throw DecodeError("expected container close packet");
    }
    const auto container = packet.read_varint();
    if (container < 0) throw DecodeError("container ID is invalid");
    expect_packet_end(packet);
    return container;
}

ContainerSlotStateChange decode_container_slot_state_change(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::container_slot_state_changed)) {
        throw DecodeError("expected container slot state packet");
    }
    ContainerSlotStateChange change{
        packet.read_varint(), packet.read_varint(), packet.read_bool()};
    if (change.slot < 0 || change.container < 0) {
        throw DecodeError("container slot state fields are invalid");
    }
    expect_packet_end(packet);
    return change;
}

BeaconSelection decode_set_beacon(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::set_beacon)) {
        throw DecodeError("expected set beacon packet");
    }
    const auto read_effect = [&packet]() -> std::optional<std::int32_t> {
        if (!packet.read_bool()) return std::nullopt;
        const auto holder_id = packet.read_varint();
        if (holder_id <= 0 || holder_id > 40) {
            throw DecodeError("beacon effect holder is out of bounds");
        }
        return holder_id - 1;
    };
    BeaconSelection selection{read_effect(), read_effect()};
    expect_packet_end(packet);
    return selection;
}

PlaceRecipe decode_place_recipe(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::place_recipe)) {
        throw DecodeError("expected place recipe packet");
    }
    PlaceRecipe request{packet.read_varint(), packet.read_varint(), packet.read_bool()};
    if (request.container_id < 0 || request.display_id < 0) {
        throw DecodeError("place recipe fields are invalid");
    }
    expect_packet_end(packet);
    return request;
}

RecipeBookSettingChange decode_recipe_book_setting_change(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::recipe_book_change_settings)) {
        throw DecodeError("expected recipe book setting packet");
    }
    const auto book_type = packet.read_varint();
    if (book_type < 0 || book_type > 3) throw DecodeError("recipe book type is invalid");
    RecipeBookSettingChange change{
        static_cast<std::uint8_t>(book_type), packet.read_bool(), packet.read_bool()};
    expect_packet_end(packet);
    return change;
}

std::int32_t decode_recipe_book_seen(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::recipe_book_seen_recipe)) {
        throw DecodeError("expected recipe book seen packet");
    }
    const auto display_id = packet.read_varint();
    if (display_id < 0) throw DecodeError("recipe display ID is invalid");
    expect_packet_end(packet);
    return display_id;
}

SeenAdvancements decode_seen_advancements(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::seen_advancements)) {
        throw DecodeError("expected seen advancements packet");
    }
    const auto action = packet.read_varint();
    SeenAdvancements seen{action == 0, std::nullopt};
    if (action == 0) {
        seen.tab = packet.read_identifier();
    } else if (action != 1) {
        throw DecodeError("seen advancements action is invalid");
    }
    expect_packet_end(packet);
    return seen;
}

std::int16_t decode_set_carried_item(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::set_carried_item)) {
        throw DecodeError("expected set carried item packet");
    }
    const auto slot = packet.read_i16_be();
    if (slot < 0 || slot > 8) throw DecodeError("carried item slot is outside hotbar");
    expect_packet_end(packet);
    return slot;
}

CreativeSlotChange decode_set_creative_mode_slot(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::set_creative_mode_slot)) {
        throw DecodeError("expected set creative mode slot packet");
    }
    CreativeSlotChange change{packet.read_i16_be(), {}};
    if (change.slot < -1 || change.slot > 45) {
        throw DecodeError("creative inventory slot is out of bounds");
    }
    change.item.count = packet.read_varint();
    if (change.item.count < 0 || change.item.count > 99) {
        throw DecodeError("creative item count is out of bounds");
    }
    if (change.item.count != 0) {
        change.item.item_id = packet.read_varint();
        if (change.item.item_id < 0 || change.item.item_id >= 1'538) {
            throw DecodeError("creative item ID is out of bounds");
        }
        const auto added_components = packet.read_varint();
        const auto removed_components = packet.read_varint();
        if (added_components != 0 || removed_components != 0) {
            throw DecodeError("creative item components are not supported");
        }
    }
    expect_packet_end(packet);
    return change;
}

std::uint8_t decode_swing(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::swing)) {
        throw DecodeError("expected Play swing packet");
    }
    const auto hand = packet.read_varint();
    if (hand < 0 || hand > 1) throw DecodeError("swing hand is out of bounds");
    expect_packet_end(packet);
    return static_cast<std::uint8_t>(hand);
}

UseItemOn decode_use_item_on(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::use_item_on)) {
        throw DecodeError("expected use item on packet");
    }
    const auto hand = packet.read_varint();
    if (hand < 0 || hand > 1) throw DecodeError("use item hand is out of bounds");
    const auto position = packet.read_position();
    const auto direction = packet.read_varint();
    if (direction < 0 || direction > 5) {
        throw DecodeError("use item direction is out of bounds");
    }
    UseItemOn result{
        static_cast<std::uint8_t>(hand),
        {position, static_cast<std::uint8_t>(direction),
         packet.read_f32_be(), packet.read_f32_be(), packet.read_f32_be(),
         packet.read_bool(), packet.read_bool()},
        packet.read_varint(),
    };
    if (result.sequence < 0 ||
        !std::isfinite(result.hit.offset_x) || !std::isfinite(result.hit.offset_y) ||
        !std::isfinite(result.hit.offset_z) || result.hit.offset_x < 0 ||
        result.hit.offset_x > 1 || result.hit.offset_y < 0 || result.hit.offset_y > 1 ||
        result.hit.offset_z < 0 || result.hit.offset_z > 1) {
        throw DecodeError("use item block hit is out of bounds");
    }
    expect_packet_end(packet);
    return result;
}

UseItem decode_use_item(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::use_item)) {
        throw DecodeError("expected use item packet");
    }
    const auto hand = packet.read_varint();
    UseItem result{
        static_cast<std::uint8_t>(hand), packet.read_varint(),
        packet.read_f32_be(), packet.read_f32_be()};
    if (hand < 0 || hand > 1 || result.sequence < 0 ||
        !std::isfinite(result.yaw) || !std::isfinite(result.pitch)) {
        throw DecodeError("use item fields are out of bounds");
    }
    expect_packet_end(packet);
    return result;
}

Bytes encode_initialize_border(const double center_x,
                               const double center_z,
                               const double old_size,
                               const double new_size,
                               const std::int64_t lerp_time,
                               const std::int32_t max_size,
                               const std::int32_t warning_blocks,
                               const std::int32_t warning_time) {
    if (!std::isfinite(center_x) || !std::isfinite(center_z) ||
        !std::isfinite(old_size) || !std::isfinite(new_size) ||
        old_size < 0 || new_size < 0 || lerp_time < 0 || max_size < 0 ||
        warning_blocks < 0 || warning_time < 0) {
        throw std::invalid_argument("world border values are invalid");
    }
    Bytes payload;
    write_f64_be(payload, center_x);
    write_f64_be(payload, center_z);
    write_f64_be(payload, old_size);
    write_f64_be(payload, new_size);
    write_varlong(payload, lerp_time);
    write_varint(payload, max_size);
    write_varint(payload, warning_blocks);
    write_varint(payload, warning_time);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::initialize_border), payload);
}

Bytes encode_player_abilities(const bool invulnerable,
                              const bool flying,
                              const bool can_fly,
                              const bool instant_build,
                              const float flying_speed,
                              const float walking_speed) {
    if (!std::isfinite(flying_speed) || !std::isfinite(walking_speed) ||
        flying_speed < 0 || walking_speed < 0) {
        throw std::invalid_argument("player ability speeds are invalid");
    }
    Bytes payload;
    payload.push_back(static_cast<std::uint8_t>(
        (invulnerable ? 0x01U : 0U) | (flying ? 0x02U : 0U) |
        (can_fly ? 0x04U : 0U) | (instant_build ? 0x08U : 0U)));
    write_f32_be(payload, flying_speed);
    write_f32_be(payload, walking_speed);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::player_abilities), payload);
}

Bytes encode_border_center(const double x, const double z) {
    if (!std::isfinite(x) || !std::isfinite(z)) {
        throw std::invalid_argument("world border center is invalid");
    }
    Bytes payload;
    write_f64_be(payload, x);
    write_f64_be(payload, z);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_border_center), payload);
}

Bytes encode_border_lerp_size(const double old_size,
                              const double new_size,
                              const std::int64_t lerp_time) {
    if (!std::isfinite(old_size) || !std::isfinite(new_size) ||
        old_size < 0 || new_size < 0 || lerp_time < 0) {
        throw std::invalid_argument("world border lerp values are invalid");
    }
    Bytes payload;
    write_f64_be(payload, old_size);
    write_f64_be(payload, new_size);
    write_varlong(payload, lerp_time);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::set_border_lerp_size), payload);
}

Bytes encode_border_size(const double size) {
    if (!std::isfinite(size) || size < 0) {
        throw std::invalid_argument("world border size is invalid");
    }
    Bytes payload;
    write_f64_be(payload, size);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_border_size), payload);
}

Bytes encode_border_warning_delay(const std::int32_t warning_time) {
    if (warning_time < 0) throw std::invalid_argument("border warning time is invalid");
    Bytes payload;
    write_varint(payload, warning_time);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::set_border_warning_delay), payload);
}

Bytes encode_border_warning_distance(const std::int32_t warning_blocks) {
    if (warning_blocks < 0) throw std::invalid_argument("border warning distance is invalid");
    Bytes payload;
    write_varint(payload, warning_blocks);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::set_border_warning_distance), payload);
}

Bytes encode_chunk_cache_radius(const std::int32_t radius) {
    if (radius < 2 || radius > 32) throw std::invalid_argument("chunk cache radius is invalid");
    Bytes payload;
    write_varint(payload, radius);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_chunk_cache_radius), payload);
}

Bytes encode_experience(const float progress, const std::int32_t total, const std::int32_t level) {
    if (!std::isfinite(progress) || progress < 0 || progress > 1 || total < 0 || level < 0) {
        throw std::invalid_argument("experience values are invalid");
    }
    Bytes payload;
    write_f32_be(payload, progress);
    write_varint(payload, total);
    write_varint(payload, level);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_experience), payload);
}

Bytes encode_health(const float health, const std::int32_t food, const float saturation) {
    if (!std::isfinite(health) || !std::isfinite(saturation) || health < 0 ||
        food < 0 || food > 20 || saturation < 0) {
        throw std::invalid_argument("health values are invalid");
    }
    Bytes payload;
    write_f32_be(payload, health);
    write_varint(payload, food);
    write_f32_be(payload, saturation);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_health), payload);
}

Bytes encode_set_time(const std::int64_t game_time,
                      const std::int64_t day_time,
                      const float rate) {
    if (game_time < 0 || day_time < 0 || !std::isfinite(rate) || rate < 0.0F) {
        throw std::invalid_argument("world time fields are invalid");
    }
    Bytes payload;
    write_i64_be(payload, game_time);
    write_varint(payload, 1);
    write_varint(payload, 1);
    write_varlong(payload, day_time);
    write_f32_be(payload, 0.0F);
    write_f32_be(payload, rate);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_time), payload);
}

Bytes encode_held_slot(const std::int32_t slot) {
    if (slot < 0 || slot > 8) throw std::invalid_argument("held slot is outside hotbar");
    Bytes payload;
    write_varint(payload, slot);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_held_slot), payload);
}

namespace {

void write_literal_component(Bytes& output, const std::string_view text) {
    nbt::write_any_tag(
        output, nbt::string_tag(std::string(text)),
        {.max_bytes = 262'144, .max_depth = 64,
         .max_collection_entries = 4'096, .max_string_bytes = 65'535});
}

[[nodiscard]] Bytes encode_literal_component_packet(
    const ClientboundPacketId packet_id, const std::string_view text) {
    Bytes payload;
    write_literal_component(payload, text);
    return frame_packet(static_cast<std::int32_t>(packet_id), payload);
}

} // namespace

Bytes encode_clear_titles(const bool reset_times) {
    Bytes payload;
    write_bool(payload, reset_times);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::clear_titles), payload);
}

Bytes encode_block_changed_ack(const std::int32_t sequence) {
    if (sequence < 0) throw std::invalid_argument("block sequence must not be negative");
    Bytes payload;
    write_varint(payload, sequence);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::block_changed_ack), payload);
}

Bytes encode_block_destruction(const std::int32_t breaker_id,
                               const BlockPosition position,
                               const std::int32_t progress) {
    if (breaker_id < 0 || progress < -1 || progress > 10) {
        throw std::invalid_argument("block destruction progress is invalid");
    }
    Bytes payload;
    write_varint(payload, breaker_id);
    write_position(payload, position);
    payload.push_back(static_cast<std::uint8_t>(progress));
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::block_destruction), payload);
}

Bytes encode_block_event(const BlockPosition position,
                         const std::uint8_t first,
                         const std::uint8_t second,
                         const std::int32_t block_id) {
    if (block_id < 0 || block_id >= 1'196) {
        throw std::invalid_argument("block event registry ID is invalid");
    }
    Bytes payload;
    write_position(payload, position);
    payload.push_back(first);
    payload.push_back(second);
    write_varint(payload, block_id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::block_event), payload);
}

Bytes encode_block_update(const BlockPosition position, const std::int32_t block_state_id) {
    if (block_state_id < 0 || block_state_id > 32'365) {
        throw std::invalid_argument("block state ID is invalid");
    }
    Bytes payload;
    write_position(payload, position);
    write_varint(payload, block_state_id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::block_update), payload);
}

Bytes encode_game_rule_values(
    const std::span<const std::pair<std::string, std::string>> values) {
    if (values.size() > 1'024) throw std::length_error("game-rule map exceeds limit");
    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(values.size()));
    for (const auto& [key, value] : values) {
        write_identifier(payload, key);
        write_string(payload, value);
    }
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::game_rule_values), payload);
}

Bytes encode_game_event(const std::uint8_t event, const float parameter) {
    if (event > 13 || !std::isfinite(parameter)) {
        throw std::invalid_argument("game event fields are invalid");
    }
    Bytes payload;
    write_u8(payload, event);
    write_f32_be(payload, parameter);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::game_event), payload);
}

Bytes encode_level_event(const std::int32_t type,
                         const BlockPosition position,
                         const std::int32_t data,
                         const bool global_event) {
    Bytes payload;
    write_i32_be(payload, type);
    write_position(payload, position);
    write_i32_be(payload, data);
    write_bool(payload, global_event);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::level_event), payload);
}

Bytes encode_level_particles(ParticleOptions particle,
                             const bool override_limiter,
                             const bool always_show,
                             const EntityVector position,
                             const float offset_x,
                             const float offset_y,
                             const float offset_z,
                             const float max_speed,
                             const std::int32_t count) {
    if (particle.type_id < 0 || particle.type_id >= 125 ||
        particle.data.size() > 1'024 || count < 0 || count > 1'000'000 ||
        !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(offset_x) ||
        !std::isfinite(offset_y) || !std::isfinite(offset_z) ||
        !std::isfinite(max_speed)) {
        throw std::invalid_argument("level particle fields are invalid");
    }
    Bytes payload;
    write_bool(payload, override_limiter);
    write_bool(payload, always_show);
    write_f64_be(payload, position.x);
    write_f64_be(payload, position.y);
    write_f64_be(payload, position.z);
    write_f32_be(payload, offset_x);
    write_f32_be(payload, offset_y);
    write_f32_be(payload, offset_z);
    write_f32_be(payload, max_speed);
    write_i32_be(payload, count);
    write_varint(payload, particle.type_id);
    payload.insert(payload.end(), particle.data.begin(), particle.data.end());
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::level_particles), payload);
}

Bytes encode_hurt_animation(const std::int32_t entity_id, const float yaw) {
    if (entity_id < 0 || !std::isfinite(yaw)) {
        throw std::invalid_argument("hurt animation fields are invalid");
    }
    Bytes payload;
    write_varint(payload, entity_id);
    write_f32_be(payload, yaw);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::hurt_animation), payload);
}

Bytes encode_change_difficulty(const std::uint8_t difficulty, const bool locked) {
    if (difficulty > 3) throw std::invalid_argument("difficulty ID is invalid");
    Bytes payload;
    write_varint(payload, difficulty);
    write_bool(payload, locked);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::change_difficulty), payload);
}

Bytes encode_game_test_highlight(const BlockPosition absolute, const BlockPosition relative) {
    Bytes payload;
    write_position(payload, absolute);
    write_position(payload, relative);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::game_test_highlight_pos), payload);
}

Bytes encode_ping(const std::int32_t id) {
    Bytes payload;
    write_i32_be(payload, id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::ping), payload);
}

Bytes encode_player_rotation(const float yaw,
                             const bool relative_yaw,
                             const float pitch,
                             const bool relative_pitch) {
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) {
        throw std::invalid_argument("player rotation is non-finite");
    }
    Bytes payload;
    write_f32_be(payload, yaw);
    write_bool(payload, relative_yaw);
    write_f32_be(payload, pitch);
    write_bool(payload, relative_pitch);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::player_rotation), payload);
}

Bytes encode_select_advancements_tab(const std::optional<std::string_view> tab) {
    Bytes payload;
    write_bool(payload, tab.has_value());
    if (tab) write_identifier(payload, *tab);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::select_advancements_tab), payload);
}

Bytes encode_ticking_state(const float tick_rate, const bool frozen) {
    if (!std::isfinite(tick_rate) || tick_rate <= 0) {
        throw std::invalid_argument("tick rate is invalid");
    }
    Bytes payload;
    write_f32_be(payload, tick_rate);
    write_bool(payload, frozen);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::ticking_state), payload);
}

Bytes encode_ticking_step(const std::int32_t steps) {
    if (steps < 0) throw std::invalid_argument("tick steps must not be negative");
    Bytes payload;
    write_varint(payload, steps);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::ticking_step), payload);
}

Bytes encode_projectile_power(const std::int32_t entity_id, const double power) {
    if (entity_id < 0 || !std::isfinite(power) || power < 0) {
        throw std::invalid_argument("projectile power fields are invalid");
    }
    Bytes payload;
    write_varint(payload, entity_id);
    write_f64_be(payload, power);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::projectile_power), payload);
}

namespace {

void write_sound_header(Bytes& payload, const std::int32_t sound_id, const std::uint8_t source) {
    if (sound_id < 0 || sound_id >= 1'968 || source > 9) {
        throw std::invalid_argument("sound registry or source ID is invalid");
    }
    write_varint(payload, sound_id + 1);
    write_varint(payload, source);
}

void validate_sound_values(const float volume, const float pitch) {
    if (!std::isfinite(volume) || !std::isfinite(pitch) || volume < 0 || pitch < 0) {
        throw std::invalid_argument("sound volume or pitch is invalid");
    }
}

} // namespace

Bytes encode_sound_entity(const std::int32_t sound_id,
                          const std::uint8_t source,
                          const std::int32_t entity_id,
                          const float volume,
                          const float pitch,
                          const std::int64_t seed) {
    if (entity_id < 0) throw std::invalid_argument("sound entity ID is invalid");
    validate_sound_values(volume, pitch);
    Bytes payload;
    write_sound_header(payload, sound_id, source);
    write_varint(payload, entity_id);
    write_f32_be(payload, volume);
    write_f32_be(payload, pitch);
    write_i64_be(payload, seed);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::sound_entity), payload);
}

Bytes encode_sound(const std::int32_t sound_id,
                   const std::uint8_t source,
                   const EntityVector position,
                   const float volume,
                   const float pitch,
                   const std::int64_t seed) {
    validate_sound_values(volume, pitch);
    const auto coordinate = [](const double value) {
        if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<std::int32_t>::min()) / 8.0 ||
            value > static_cast<double>(std::numeric_limits<std::int32_t>::max()) / 8.0) {
            throw std::invalid_argument("sound position is outside packet range");
        }
        return static_cast<std::int32_t>(std::floor(value * 8.0));
    };
    Bytes payload;
    write_sound_header(payload, sound_id, source);
    write_i32_be(payload, coordinate(position.x));
    write_i32_be(payload, coordinate(position.y));
    write_i32_be(payload, coordinate(position.z));
    write_f32_be(payload, volume);
    write_f32_be(payload, pitch);
    write_i64_be(payload, seed);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::sound), payload);
}

Bytes encode_stop_sound(const std::optional<std::uint8_t> source,
                        const std::optional<std::string_view> sound_name) {
    if (source && *source > 9) throw std::invalid_argument("sound source is invalid");
    Bytes payload;
    payload.push_back(static_cast<std::uint8_t>((source ? 1U : 0U) | (sound_name ? 2U : 0U)));
    if (source) write_varint(payload, *source);
    if (sound_name) write_identifier(payload, *sound_name);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::stop_sound), payload);
}

namespace {

[[nodiscard]] std::uint8_t pack_degrees(const float degrees) {
    if (!std::isfinite(degrees)) throw std::invalid_argument("entity rotation is non-finite");
    const auto packed = static_cast<std::int32_t>(std::floor(degrees * 256.0F / 360.0F));
    return static_cast<std::uint8_t>(packed);
}

void write_lp_vector(Bytes& output, EntityVector value) {
    const auto sanitize = [](const double component) {
        if (std::isnan(component)) return 0.0;
        return std::clamp(component, -17'179'869'183.0, 17'179'869'183.0);
    };
    value = {sanitize(value.x), sanitize(value.y), sanitize(value.z)};
    const auto absolute_max = std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
    if (absolute_max < 3.051944088384301E-5) {
        output.push_back(0);
        return;
    }
    const auto scale = static_cast<std::uint64_t>(std::ceil(absolute_max));
    const auto continuation = (scale & 3U) != scale;
    const auto pack = [scale](const double component) {
        const auto normalized = component / static_cast<double>(scale);
        return static_cast<std::uint64_t>(std::llround((normalized * 0.5 + 0.5) * 32'766.0));
    };
    const auto packed = (continuation ? ((scale & 3U) | 4U) : scale) |
        (pack(value.x) << 3U) | (pack(value.y) << 18U) | (pack(value.z) << 33U);
    output.push_back(static_cast<std::uint8_t>(packed));
    output.push_back(static_cast<std::uint8_t>(packed >> 8U));
    write_i32_be(output, static_cast<std::int32_t>(packed >> 16U));
    if (continuation) write_varint(output, static_cast<std::int32_t>(scale >> 2U));
}

[[nodiscard]] EntityVector read_lp_vector(Reader& input) {
    const auto first = input.read_u8();
    if (first == 0) return {0.0, 0.0, 0.0};
    const auto second = input.read_u8();
    const auto upper = static_cast<std::uint32_t>(input.read_i32_be());
    const auto packed = static_cast<std::uint64_t>(upper) << 16U |
        static_cast<std::uint64_t>(second) << 8U | first;
    std::uint64_t scale = first & 3U;
    if ((first & 4U) != 0) {
        const auto continuation = input.read_varint();
        if (continuation < 0) throw DecodeError("LP vector scale is invalid");
        scale |= static_cast<std::uint64_t>(continuation) << 2U;
    }
    const auto unpack = [packed, scale](const unsigned shift) {
        const auto raw = std::min<std::uint64_t>((packed >> shift) & 32'767U, 32'766U);
        return (static_cast<double>(raw) * 2.0 / 32'766.0 - 1.0) *
            static_cast<double>(scale);
    };
    return {unpack(3), unpack(18), unpack(33)};
}

[[nodiscard]] std::int16_t pack_entity_delta(const double delta) {
    if (!std::isfinite(delta) || delta < -8.0 || delta >= 8.0) {
        throw std::invalid_argument("relative entity movement exceeds packet range");
    }
    return static_cast<std::int16_t>(std::llround(delta * 4'096.0));
}

void write_entity_delta(Bytes& output, const EntityVector delta) {
    write_i16_be(output, pack_entity_delta(delta.x));
    write_i16_be(output, pack_entity_delta(delta.y));
    write_i16_be(output, pack_entity_delta(delta.z));
}

void validate_entity_id(const std::int32_t entity_id) {
    if (entity_id < 0) throw std::invalid_argument("entity ID must not be negative");
}

} // namespace

std::int32_t decode_attack(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::attack)) {
        throw DecodeError("expected entity attack packet");
    }
    const auto entity_id = packet.read_varint();
    if (entity_id < 0) throw DecodeError("attacked entity ID is invalid");
    expect_packet_end(packet);
    return entity_id;
}

std::string decode_chat_command(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::chat_command)) {
        throw DecodeError("expected chat command packet");
    }
    auto command = packet.read_string(256);
    if (command.empty() || command.front() == '/') {
        throw DecodeError("chat command is invalid");
    }
    expect_packet_end(packet);
    return command;
}

CommandSuggestionRequest decode_command_suggestion(Reader& packet) {
    if (packet.read_varint() !=
        static_cast<std::int32_t>(ServerboundPacketId::command_suggestion)) {
        throw DecodeError("expected command suggestion packet");
    }
    CommandSuggestionRequest request{packet.read_varint(), packet.read_string(256)};
    if (request.id < 0 || request.command.empty()) {
        throw DecodeError("command suggestion request is invalid");
    }
    expect_packet_end(packet);
    return request;
}

EntityInteraction decode_interact(Reader& packet) {
    if (packet.read_varint() != static_cast<std::int32_t>(ServerboundPacketId::interact)) {
        throw DecodeError("expected entity interaction packet");
    }
    const auto entity_id = packet.read_varint();
    const auto hand = packet.read_varint();
    if (entity_id < 0 || hand < 0 || hand > 1) {
        throw DecodeError("entity interaction target or hand is invalid");
    }
    EntityInteraction interaction{
        entity_id, static_cast<std::uint8_t>(hand), read_lp_vector(packet), packet.read_bool()};
    expect_packet_end(packet);
    return interaction;
}

Bytes encode_add_entity(const EntitySpawn& entity) {
    validate_entity_id(entity.id);
    if (entity.type < 0 || !std::isfinite(entity.position.x) ||
        !std::isfinite(entity.position.y) || !std::isfinite(entity.position.z)) {
        throw std::invalid_argument("entity spawn fields are invalid");
    }
    Bytes payload;
    write_varint(payload, entity.id);
    write_uuid(payload, entity.uuid);
    write_varint(payload, entity.type);
    write_f64_be(payload, entity.position.x);
    write_f64_be(payload, entity.position.y);
    write_f64_be(payload, entity.position.z);
    write_lp_vector(payload, entity.movement);
    payload.push_back(pack_degrees(entity.pitch));
    payload.push_back(pack_degrees(entity.yaw));
    payload.push_back(pack_degrees(entity.head_yaw));
    write_varint(payload, entity.data);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::add_entity), payload);
}

Bytes encode_animate(const std::int32_t entity_id, const std::uint8_t action) {
    validate_entity_id(entity_id);
    Bytes payload;
    write_varint(payload, entity_id);
    payload.push_back(action);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::animate), payload);
}

Bytes encode_entity_event(const std::int32_t entity_id, const std::int8_t event_id) {
    validate_entity_id(entity_id);
    Bytes payload;
    write_i32_be(payload, entity_id);
    write_i8(payload, event_id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::entity_event), payload);
}

Bytes encode_entity_position_sync(const std::int32_t entity_id,
                                  const EntityVector position,
                                  const EntityVector movement,
                                  const float yaw,
                                  const float pitch,
                                  const bool on_ground) {
    validate_entity_id(entity_id);
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(movement.x) ||
        !std::isfinite(movement.y) || !std::isfinite(movement.z) ||
        !std::isfinite(yaw) || !std::isfinite(pitch)) {
        throw std::invalid_argument("entity synchronization values are non-finite");
    }
    Bytes payload;
    write_varint(payload, entity_id);
    write_f64_be(payload, position.x);
    write_f64_be(payload, position.y);
    write_f64_be(payload, position.z);
    write_f64_be(payload, movement.x);
    write_f64_be(payload, movement.y);
    write_f64_be(payload, movement.z);
    write_f32_be(payload, yaw);
    write_f32_be(payload, pitch);
    write_bool(payload, on_ground);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::entity_position_sync), payload);
}

Bytes encode_move_entity_position(const std::int32_t entity_id,
                                  const EntityVector delta,
                                  const bool on_ground) {
    validate_entity_id(entity_id);
    Bytes payload;
    write_varint(payload, entity_id);
    write_entity_delta(payload, delta);
    write_bool(payload, on_ground);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::move_entity_pos), payload);
}

Bytes encode_move_entity_position_rotation(const std::int32_t entity_id,
                                           const EntityVector delta,
                                           const float yaw,
                                           const float pitch,
                                           const bool on_ground) {
    validate_entity_id(entity_id);
    Bytes payload;
    write_varint(payload, entity_id);
    write_entity_delta(payload, delta);
    payload.push_back(pack_degrees(yaw));
    payload.push_back(pack_degrees(pitch));
    write_bool(payload, on_ground);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::move_entity_pos_rot), payload);
}

Bytes encode_move_entity_rotation(const std::int32_t entity_id,
                                  const float yaw,
                                  const float pitch,
                                  const bool on_ground) {
    validate_entity_id(entity_id);
    Bytes payload;
    write_varint(payload, entity_id);
    payload.push_back(pack_degrees(yaw));
    payload.push_back(pack_degrees(pitch));
    write_bool(payload, on_ground);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::move_entity_rot), payload);
}

Bytes encode_remove_entities(const std::span<const std::int32_t> entity_ids) {
    if (entity_ids.size() > 65'536) throw std::length_error("entity removal list exceeds limit");
    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(entity_ids.size()));
    for (const auto entity_id : entity_ids) {
        validate_entity_id(entity_id);
        write_varint(payload, entity_id);
    }
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::remove_entities), payload);
}

Bytes encode_rotate_head(const std::int32_t entity_id, const float head_yaw) {
    validate_entity_id(entity_id);
    Bytes payload;
    write_varint(payload, entity_id);
    payload.push_back(pack_degrees(head_yaw));
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::rotate_head), payload);
}

Bytes encode_entity_link(const std::int32_t source_id, const std::int32_t destination_id) {
    validate_entity_id(source_id);
    Bytes payload;
    write_i32_be(payload, source_id);
    write_i32_be(payload, destination_id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_entity_link), payload);
}

Bytes encode_entity_motion(const std::int32_t entity_id, const EntityVector movement) {
    validate_entity_id(entity_id);
    Bytes payload;
    write_varint(payload, entity_id);
    write_lp_vector(payload, movement);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_entity_motion), payload);
}

Bytes encode_passengers(const std::int32_t vehicle_id,
                        const std::span<const std::int32_t> passengers) {
    validate_entity_id(vehicle_id);
    if (passengers.size() > 1'024) throw std::length_error("passenger list exceeds limit");
    Bytes payload;
    write_varint(payload, vehicle_id);
    write_varint(payload, static_cast<std::int32_t>(passengers.size()));
    for (const auto passenger : passengers) {
        validate_entity_id(passenger);
        write_varint(payload, passenger);
    }
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_passengers), payload);
}

Bytes encode_take_item_entity(const std::int32_t item_id,
                             const std::int32_t player_id,
                             const std::int32_t amount) {
    validate_entity_id(item_id);
    validate_entity_id(player_id);
    if (amount < 0) throw std::invalid_argument("taken item amount must not be negative");
    Bytes payload;
    write_varint(payload, item_id);
    write_varint(payload, player_id);
    write_varint(payload, amount);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::take_item_entity), payload);
}

Bytes encode_cookie_request(const std::string_view key) {
    return mc::protocol::encode_cookie_request(
        static_cast<std::int32_t>(ClientboundPacketId::cookie_request), key);
}

Bytes encode_custom_payload(const std::string_view channel,
                           const std::span<const std::uint8_t> data) {
    if (data.size() > 1U * 1024U * 1024U) {
        throw std::length_error("Play custom payload exceeds limit");
    }
    Bytes payload;
    write_identifier(payload, channel);
    payload.insert(payload.end(), data.begin(), data.end());
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::custom_payload), payload);
}

Bytes encode_disconnect_text(const std::string_view text) {
    return encode_literal_component_packet(ClientboundPacketId::disconnect, text);
}

Bytes encode_pong_response(const std::int64_t time) {
    Bytes payload;
    write_i64_be(payload, time);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::pong_response), payload);
}

Bytes encode_resource_pack_pop(const std::optional<Uuid> id) {
    Bytes payload;
    write_optional<Uuid>(payload, id, [](Bytes& output, const Uuid& value) {
        write_uuid(output, value);
    });
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::resource_pack_pop), payload);
}

Bytes encode_resource_pack_push(const Uuid id,
                                const std::string_view url,
                                const std::string_view hash,
                                const bool required) {
    if (url.size() > 32'767 || hash.size() > 40) {
        throw std::length_error("Play resource pack field exceeds limit");
    }
    Bytes payload;
    write_uuid(payload, id);
    write_string(payload, url);
    write_string(payload, hash);
    write_bool(payload, required);
    write_bool(payload, false);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::resource_pack_push), payload);
}

Bytes encode_action_bar_text(const std::string_view text) {
    return encode_literal_component_packet(ClientboundPacketId::set_action_bar_text, text);
}

Bytes encode_subtitle_text(const std::string_view text) {
    return encode_literal_component_packet(ClientboundPacketId::set_subtitle_text, text);
}

Bytes encode_title_text(const std::string_view text) {
    return encode_literal_component_packet(ClientboundPacketId::set_title_text, text);
}

Bytes encode_titles_animation(const std::int32_t fade_in,
                              const std::int32_t stay,
                              const std::int32_t fade_out) {
    if (fade_in < 0 || stay < 0 || fade_out < 0) {
        throw std::invalid_argument("title animation times must not be negative");
    }
    Bytes payload;
    write_i32_be(payload, fade_in);
    write_i32_be(payload, stay);
    write_i32_be(payload, fade_out);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::set_titles_animation), payload);
}

Bytes encode_system_chat(const std::string_view text, const bool overlay) {
    Bytes payload;
    write_literal_component(payload, text);
    write_bool(payload, overlay);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::system_chat), payload);
}

Bytes encode_tab_list(const std::string_view header, const std::string_view footer) {
    Bytes payload;
    write_literal_component(payload, header);
    write_literal_component(payload, footer);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::tab_list), payload);
}

Bytes encode_custom_report_details(
    const std::span<const std::pair<std::string, std::string>> details) {
    if (details.size() > 32) throw std::length_error("Play report details exceed limit");
    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(details.size()));
    for (const auto& [title, description] : details) {
        if (title.size() > 128 || description.size() > 4'096) {
            throw std::length_error("Play report detail field exceeds limit");
        }
        write_string(payload, title);
        write_string(payload, description);
    }
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::custom_report_details), payload);
}

Bytes encode_empty_server_links() {
    Bytes payload;
    write_varint(payload, 0);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::server_links), payload);
}

namespace {

void write_simple_item_stack(Bytes& output, const SimpleItemStack item) {
    if (item.count < 0 || item.count > 99 || item.item_id < 0 || item.item_id >= 1'538) {
        throw std::invalid_argument("simple item stack is out of bounds");
    }
    write_varint(output, item.count);
    if (item.empty()) return;
    write_varint(output, item.item_id);
    write_varint(output, 0);
    write_varint(output, 0);
}

void validate_container_id(const std::int32_t container_id) {
    if (container_id < 0) throw std::invalid_argument("container ID must not be negative");
}

} // namespace

Bytes encode_container_close(const std::int32_t container_id) {
    validate_container_id(container_id);
    Bytes payload;
    write_varint(payload, container_id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::container_close), payload);
}

Bytes encode_container_content(const std::int32_t container_id,
                               const std::int32_t state_id,
                               const std::span<const SimpleItemStack> items,
                               const SimpleItemStack carried) {
    validate_container_id(container_id);
    if (state_id < 0 || items.size() > 1'024) {
        throw std::invalid_argument("container content state or size is invalid");
    }
    Bytes payload;
    write_varint(payload, container_id);
    write_varint(payload, state_id);
    write_varint(payload, static_cast<std::int32_t>(items.size()));
    for (const auto item : items) write_simple_item_stack(payload, item);
    write_simple_item_stack(payload, carried);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::container_set_content), payload);
}

Bytes encode_container_data(const std::int32_t container_id,
                            const std::int16_t data_id,
                            const std::int16_t value) {
    validate_container_id(container_id);
    Bytes payload;
    write_varint(payload, container_id);
    write_i16_be(payload, data_id);
    write_i16_be(payload, value);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::container_set_data), payload);
}

Bytes encode_container_slot(const std::int32_t container_id,
                            const std::int32_t state_id,
                            const std::int16_t slot,
                            const SimpleItemStack item) {
    validate_container_id(container_id);
    if (state_id < 0) throw std::invalid_argument("container state ID is invalid");
    Bytes payload;
    write_varint(payload, container_id);
    write_varint(payload, state_id);
    write_i16_be(payload, slot);
    write_simple_item_stack(payload, item);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::container_set_slot), payload);
}

Bytes encode_open_book(const std::uint8_t hand) {
    if (hand > 1) throw std::invalid_argument("book hand is invalid");
    Bytes payload;
    write_varint(payload, hand);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::open_book), payload);
}

Bytes encode_open_screen(const std::int32_t container_id,
                         const std::int32_t menu_type,
                         const std::string_view title) {
    validate_container_id(container_id);
    if (menu_type < 0) throw std::invalid_argument("menu type ID is invalid");
    Bytes payload;
    write_varint(payload, container_id);
    write_varint(payload, menu_type);
    write_literal_component(payload, title);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::open_screen), payload);
}

Bytes encode_open_sign_editor(const BlockPosition position, const bool front_text) {
    Bytes payload;
    write_position(payload, position);
    write_bool(payload, front_text);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::open_sign_editor), payload);
}

Bytes encode_cursor_item(const SimpleItemStack item) {
    Bytes payload;
    write_simple_item_stack(payload, item);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_cursor_item), payload);
}

Bytes encode_player_inventory(const std::int32_t slot, const SimpleItemStack item) {
    if (slot < 0) throw std::invalid_argument("player inventory slot is invalid");
    Bytes payload;
    write_varint(payload, slot);
    write_simple_item_stack(payload, item);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_player_inventory), payload);
}

Bytes encode_player_info_remove(const std::span<const Uuid> profile_ids) {
    if (profile_ids.size() > 1'024) throw std::length_error("player removal list exceeds limit");
    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(profile_ids.size()));
    for (const auto& profile_id : profile_ids) write_uuid(payload, profile_id);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::player_info_remove), payload);
}

Bytes encode_player_info_initialize(const login::GameProfile& profile,
                                    const std::uint8_t game_mode,
                                    const bool listed,
                                    const std::int32_t latency,
                                    const std::int32_t list_order,
                                    const bool show_hat) {
    if (profile.name.size() > 16 || profile.properties.size() > 16 ||
        game_mode > 3 || latency < 0 || list_order < 0) {
        throw std::invalid_argument("player info initialization fields are invalid");
    }
    Bytes payload;
    payload.push_back(0xFFU);
    write_varint(payload, 1);
    write_uuid(payload, profile.id);
    write_string(payload, profile.name);
    write_varint(payload, static_cast<std::int32_t>(profile.properties.size()));
    for (const auto& property : profile.properties) {
        if (property.name.size() > 64 || property.value.size() > 32'767 ||
            (property.signature && property.signature->size() > 1'024)) {
            throw std::length_error("player info profile property exceeds limit");
        }
        write_string(payload, property.name);
        write_string(payload, property.value);
        write_bool(payload, property.signature.has_value());
        if (property.signature) write_string(payload, *property.signature);
    }
    write_bool(payload, false);
    write_varint(payload, game_mode);
    write_bool(payload, listed);
    write_varint(payload, latency);
    write_bool(payload, false);
    write_varint(payload, list_order);
    write_bool(payload, show_hat);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::player_info_update), payload);
}

Bytes encode_reset_score(const std::string_view owner,
                         const std::optional<std::string_view> objective) {
    Bytes payload;
    write_string(payload, owner);
    write_bool(payload, objective.has_value());
    if (objective) write_string(payload, *objective);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::reset_score), payload);
}

Bytes encode_display_objective(const std::uint8_t slot, const std::string_view objective) {
    if (slot > 18) throw std::invalid_argument("scoreboard display slot is invalid");
    Bytes payload;
    write_varint(payload, slot);
    write_string(payload, objective);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::set_display_objective), payload);
}

Bytes encode_objective(const std::string_view name,
                       const ObjectiveMethod method,
                       const std::string_view display_name,
                       const ObjectiveRenderType render_type) {
    Bytes payload;
    write_string(payload, name);
    payload.push_back(static_cast<std::uint8_t>(method));
    if (method == ObjectiveMethod::add || method == ObjectiveMethod::change) {
        write_literal_component(payload, display_name);
        write_varint(payload, static_cast<std::int32_t>(render_type));
        write_bool(payload, false);
    }
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_objective), payload);
}

Bytes encode_score(const std::string_view owner,
                   const std::string_view objective,
                   const std::int32_t score,
                   const std::optional<std::string_view> display) {
    Bytes payload;
    write_string(payload, owner);
    write_string(payload, objective);
    write_varint(payload, score);
    write_bool(payload, display.has_value());
    if (display) write_literal_component(payload, *display);
    write_bool(payload, false);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_score), payload);
}

Bytes encode_team(const std::string_view name,
                  const std::uint8_t method,
                  const std::optional<TeamParameters> parameters,
                  const std::span<const std::string> players) {
    if (method > 4) throw std::invalid_argument("team method is invalid");
    const auto has_parameters = method == 0 || method == 2;
    const auto has_players = method == 0 || method == 3 || method == 4;
    if (has_parameters != parameters.has_value() || (!has_players && !players.empty()) ||
        players.size() > 1'024) {
        throw std::invalid_argument("team packet fields do not match method");
    }
    Bytes payload;
    write_string(payload, name);
    payload.push_back(method);
    if (parameters) {
        if (parameters->name_tag_visibility > 3 || parameters->collision_rule > 3) {
            throw std::invalid_argument("team parameter enum is invalid");
        }
        write_literal_component(payload, parameters->display_name);
        write_literal_component(payload, parameters->prefix);
        write_literal_component(payload, parameters->suffix);
        write_varint(payload, parameters->name_tag_visibility);
        write_varint(payload, parameters->collision_rule);
        write_bool(payload, false);
        payload.push_back(parameters->options);
    }
    if (has_players) {
        write_varint(payload, static_cast<std::int32_t>(players.size()));
        for (const auto& player : players) write_string(payload, player);
    }
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_player_team), payload);
}

Bytes encode_update_attributes(const std::int32_t entity_id,
                               const std::span<const AttributeSnapshot> attributes) {
    if (entity_id < 0 || attributes.size() > 128) {
        throw std::invalid_argument("attribute update entity or count is invalid");
    }
    Bytes payload;
    write_varint(payload, entity_id);
    write_varint(payload, static_cast<std::int32_t>(attributes.size()));
    for (const auto& attribute : attributes) {
        if (attribute.attribute_id < 0 || attribute.attribute_id >= 40 ||
            !std::isfinite(attribute.base) || attribute.modifiers.size() > 1'024) {
            throw std::invalid_argument("attribute snapshot is invalid");
        }
        write_varint(payload, attribute.attribute_id + 1);
        write_f64_be(payload, attribute.base);
        write_varint(payload, static_cast<std::int32_t>(attribute.modifiers.size()));
        for (const auto& modifier : attribute.modifiers) {
            if (!std::isfinite(modifier.amount) || modifier.operation > 2) {
                throw std::invalid_argument("attribute modifier is invalid");
            }
            write_identifier(payload, modifier.id);
            write_f64_be(payload, modifier.amount);
            write_varint(payload, modifier.operation);
        }
    }
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::update_attributes), payload);
}

Bytes encode_update_mob_effect(const std::int32_t entity_id,
                               const std::int32_t effect_id,
                               const std::int32_t amplifier,
                               const std::int32_t duration_ticks,
                               const bool ambient,
                               const bool visible,
                               const bool show_icon,
                               const bool blend) {
    if (entity_id < 0 || effect_id < 0 || effect_id >= 40 || amplifier < 0 ||
        duration_ticks < 0) {
        throw std::invalid_argument("mob effect update fields are invalid");
    }
    Bytes payload;
    write_varint(payload, entity_id);
    write_varint(payload, effect_id + 1);
    write_varint(payload, amplifier);
    write_varint(payload, duration_ticks);
    payload.push_back(static_cast<std::uint8_t>(
        (ambient ? 1U : 0U) | (visible ? 2U : 0U) |
        (show_icon ? 4U : 0U) | (blend ? 8U : 0U)));
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::update_mob_effect), payload);
}

Bytes encode_remove_mob_effect(const std::int32_t entity_id, const std::int32_t effect_id) {
    if (entity_id < 0 || effect_id < 0 || effect_id >= 40) {
        throw std::invalid_argument("mob effect removal fields are invalid");
    }
    Bytes payload;
    write_varint(payload, entity_id);
    write_varint(payload, effect_id + 1);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::remove_mob_effect), payload);
}

Bytes encode_entity_metadata(const std::int32_t entity_id,
                             const std::span<const EntityMetadataEntry> entries) {
    if (entity_id < 0 || entries.size() > 255) {
        throw std::invalid_argument("entity metadata target or count is invalid");
    }
    std::array<bool, 255> seen{};
    Bytes payload;
    write_varint(payload, entity_id);
    for (const auto& entry : entries) {
        if (entry.index == 0xFFU || seen[entry.index]) {
            throw std::invalid_argument("entity metadata index is invalid or duplicated");
        }
        seen[entry.index] = true;
        payload.push_back(entry.index);
        std::visit([&payload](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::uint8_t>) {
                write_varint(payload, 0);
                write_u8(payload, value);
            } else if constexpr (std::is_same_v<Value, bool>) {
                write_varint(payload, 8);
                write_bool(payload, value);
            } else if constexpr (std::is_same_v<Value, std::int32_t>) {
                write_varint(payload, 1);
                write_varint(payload, value);
            } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                write_varint(payload, 2);
                write_varlong(payload, value);
            } else if constexpr (std::is_same_v<Value, float>) {
                if (!std::isfinite(value)) {
                    throw std::invalid_argument("entity metadata float is non-finite");
                }
                write_varint(payload, 3);
                write_f32_be(payload, value);
            } else if constexpr (std::is_same_v<Value, std::string>) {
                if (value.size() > 32'767) {
                    throw std::length_error("entity metadata string exceeds limit");
                }
                write_varint(payload, 4);
                write_string(payload, value);
            } else if constexpr (std::is_same_v<Value, SimpleItemStack>) {
                write_varint(payload, 7);
                write_simple_item_stack(payload, value);
            } else if constexpr (std::is_same_v<Value, MetadataRotations>) {
                if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
                    !std::isfinite(value.z)) {
                    throw std::invalid_argument("entity metadata rotations are non-finite");
                }
                write_varint(payload, 9);
                write_f32_be(payload, value.x);
                write_f32_be(payload, value.y);
                write_f32_be(payload, value.z);
            } else if constexpr (std::is_same_v<Value, BlockPosition>) {
                write_varint(payload, 10);
                write_position(payload, value);
            } else if constexpr (std::is_same_v<Value, MetadataBlockState>) {
                if (value.id < 0 || value.id > 32'365) {
                    throw std::invalid_argument("entity metadata block state is invalid");
                }
                write_varint(payload, 14);
                write_varint(payload, value.id);
            } else if constexpr (
                std::is_same_v<Value, MetadataOptionalUnsignedInt>) {
                if (value.value && (*value.value < 0 ||
                    *value.value == std::numeric_limits<std::int32_t>::max())) {
                    throw std::invalid_argument(
                        "entity metadata optional integer is invalid");
                }
                write_varint(payload, 19);
                write_varint(payload, value.value ? *value.value + 1 : 0);
            } else {
                if (value.id > 17) {
                    throw std::invalid_argument("entity metadata pose is invalid");
                }
                write_varint(payload, 20);
                write_varint(payload, value.id);
            }
        }, entry.value);
    }
    payload.push_back(0xFFU);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::set_entity_data), payload);
}

Bytes encode_recipe_book_remove(const std::span<const std::int32_t> display_ids) {
    if (display_ids.size() > 65'536) throw std::length_error("recipe removal list exceeds limit");
    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(display_ids.size()));
    for (const auto display_id : display_ids) {
        if (display_id < 0) throw std::invalid_argument("recipe display ID is invalid");
        write_varint(payload, display_id);
    }
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::recipe_book_remove), payload);
}

Bytes encode_recipe_book_settings(const std::array<bool, 8>& open_filtering_pairs) {
    Bytes payload;
    for (const auto value : open_filtering_pairs) write_bool(payload, value);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::recipe_book_settings), payload);
}

    Bytes encode_empty_advancements(const bool reset, const bool show_advancements) {
        Bytes payload;
        write_bool(payload, reset);
        write_varint(payload, 0);
        write_varint(payload, 0);
        write_varint(payload, 0);
        write_bool(payload, show_advancements);
        return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::update_advancements), payload);
    }

Bytes encode_chunk_cache_center(const std::int32_t chunk_x, const std::int32_t chunk_z) {
    Bytes payload;
    write_varint(payload, chunk_x);
    write_varint(payload, chunk_z);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::set_chunk_cache_center), payload);
}

Bytes encode_forget_level_chunk(const std::int32_t chunk_x, const std::int32_t chunk_z) {
    Bytes payload;
    const auto packed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk_x))) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk_z)) << 32U);
    write_i64_be(payload, static_cast<std::int64_t>(packed));
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::forget_level_chunk), payload);
}

Bytes encode_default_spawn_position(const BlockPosition position,
                                        const float yaw,
                                        const float pitch) {
    Bytes payload;
    write_identifier(payload, "minecraft:overworld");
    write_position(payload, position);
    write_f32_be(payload, yaw);
    write_f32_be(payload, pitch);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::set_default_spawn_position), payload);
}

Bytes encode_chunk_batch_start() {
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::chunk_batch_start), {});
}

Bytes encode_chunk_batch_finished(const std::int32_t batch_size) {
    if (batch_size < 0) {
        throw std::invalid_argument("chunk batch size must not be negative");
    }
    Bytes payload;
    write_varint(payload, batch_size);
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::chunk_batch_finished), payload);
}

Bytes encode_command_suggestions(const std::int32_t id,
                                 const std::int32_t start,
                                 const std::int32_t length,
                                 const std::span<const std::string> suggestions) {
    if (id < 0 || start < 0 || length < 0 || suggestions.size() > 128) {
        throw std::invalid_argument("command suggestion fields are invalid");
    }
    Bytes payload;
    write_varint(payload, id);
    write_varint(payload, start);
    write_varint(payload, length);
    write_varint(payload, static_cast<std::int32_t>(suggestions.size()));
    for (const auto& suggestion : suggestions) {
        if (suggestion.empty() || suggestion.size() > 256) {
            throw std::invalid_argument("command suggestion text is invalid");
        }
        write_string(payload, suggestion);
        write_bool(payload, false);
    }
    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::command_suggestions), payload);
}

Bytes encode_command_tree(const std::span<const std::string_view> roots) {
    if (roots.empty() || roots.size() > 128) {
        throw std::invalid_argument("command tree root count is invalid");
    }
    std::vector<std::string_view> sorted(roots.begin(), roots.end());
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        throw std::invalid_argument("command tree contains duplicate roots");
    }
    for (const auto root : sorted) {
        if (root.empty() || root.size() > 64 ||
            !std::all_of(root.begin(), root.end(), [](const unsigned char value) {
                return std::islower(value) != 0 || value == '_';
            })) {
            throw std::invalid_argument("command tree literal is invalid");
        }
    }

    Bytes payload;
    write_varint(payload, static_cast<std::int32_t>(sorted.size() + 1));
    write_u8(payload, 0);
    write_varint(payload, static_cast<std::int32_t>(sorted.size()));
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        write_varint(payload, static_cast<std::int32_t>(index + 1));
    }
    for (const auto root : sorted) {
        write_u8(payload, 0x05);
        write_varint(payload, 0);
        write_string(payload, root);
    }
    write_varint(payload, 0);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::commands), payload);
}

Bytes encode_level_chunks_load_start() {
    return encode_game_event(13, 0.0F);
}

namespace {

} // namespace

std::int32_t protocol_block_state_id(const world::BlockId block) {
    switch (block) {
    case world::BlockId::air: return 0;
    case world::BlockId::stone: return 1;
    case world::BlockId::grass_block: return 9;
    case world::BlockId::dirt: return 10;
    case world::BlockId::bedrock: return 85;
    case world::BlockId::water: return 86;
    case world::BlockId::sand: return 118;
    case world::BlockId::gravel: return 124;
    case world::BlockId::iron_ore: return 131;
    case world::BlockId::coal_ore: return 133;
    case world::BlockId::oak_log: return 137;
    case world::BlockId::oak_leaves: return 279;
    case world::BlockId::short_grass: return 2'248;
    case world::BlockId::dandelion: return 2'321;
    case world::BlockId::poppy: return 2'324;
    }
    throw std::logic_error("BlockId has no protocol state mapping");
}

namespace {

[[nodiscard]] std::int32_t protocol_biome(const world::BiomeId biome) {
    switch (biome) {
    case world::BiomeId::desert: return 14;
    case world::BiomeId::forest: return 21;
    case world::BiomeId::ocean: return 35;
    case world::BiomeId::plains: return 40;
    case world::BiomeId::mountains: return 63;
    }
    throw std::logic_error("BiomeId has no protocol mapping");
}

void write_packed_values(Bytes& output,
                         const std::span<const std::uint16_t> values,
                         const std::uint8_t bits) {
    const auto values_per_word = static_cast<std::size_t>(64U / bits);
    const auto word_count = (values.size() + values_per_word - 1) / values_per_word;
    for (std::size_t word_index = 0; word_index < word_count; ++word_index) {
        std::uint64_t word = 0;
        for (std::size_t value_index = 0; value_index < values_per_word; ++value_index) {
            const auto index = word_index * values_per_word + value_index;
            if (index == values.size()) {
                break;
            }
            word |= static_cast<std::uint64_t>(values[index]) << (value_index * bits);
        }
        write_i64_be(output, static_cast<std::int64_t>(word));
    }
}

} // namespace

Bytes encode_paletted_container(const std::span<const std::int32_t> values,
                                const std::uint8_t minimum_bits,
                                const std::uint8_t maximum_local_bits,
                                const std::uint8_t global_bits) {
    if (values.empty() || minimum_bits == 0 ||
        minimum_bits > maximum_local_bits || maximum_local_bits >= global_bits ||
        global_bits > 16) {
        throw std::invalid_argument("paletted container bit widths are invalid");
    }
    std::vector<std::int32_t> palette;
    std::vector<std::uint16_t> indices;
    indices.reserve(values.size());
    for (const auto value : values) {
        if (value < 0 || value >= (std::int32_t{1} << global_bits)) {
            throw std::invalid_argument("paletted container value is out of bounds");
        }
        auto found = std::find(palette.begin(), palette.end(), value);
        if (found == palette.end()) {
            palette.push_back(value);
            found = std::prev(palette.end());
        }
        indices.push_back(static_cast<std::uint16_t>(
            std::distance(palette.begin(), found)));
    }
    Bytes output;
    if (palette.size() == 1) {
        output.push_back(0);
        write_varint(output, palette.front());
        return output;
    }

    std::uint8_t bits = minimum_bits;
    while ((std::size_t{1} << bits) < palette.size()) {
        ++bits;
    }
    if (bits > maximum_local_bits) {
        output.push_back(global_bits);
        std::vector<std::uint16_t> global_values;
        global_values.reserve(values.size());
        for (const auto value : values) {
            global_values.push_back(static_cast<std::uint16_t>(value));
        }
        write_packed_values(output, global_values, global_bits);
        return output;
    }
    output.push_back(bits);
    write_varint(output, static_cast<std::int32_t>(palette.size()));
    for (const auto entry : palette) {
        write_varint(output, entry);
    }

    write_packed_values(output, indices, bits);
    return output;
}

namespace {

using LightSection = std::array<std::uint8_t, 2'048>;

[[nodiscard]] std::array<LightSection, world::section_count + 2> sky_light_sections(
    const world::Chunk& chunk) {
    std::array<LightSection, world::section_count + 2> sections{};
    for (std::size_t light_section = 0; light_section < sections.size(); ++light_section) {
        const auto section_y = world::min_build_y - world::section_height +
            static_cast<std::int32_t>(light_section * world::section_height);
        for (std::size_t local_y = 0; local_y < world::section_height; ++local_y) {
            const auto block_y = section_y + static_cast<std::int32_t>(local_y);
            for (std::size_t z = 0; z < world::chunk_width; ++z) {
                for (std::size_t x = 0; x < world::chunk_width; ++x) {
                    if (block_y <= chunk.height(x, z)) continue;
                    const auto index = (local_y * world::chunk_width + z) *
                        world::chunk_width + x;
                    auto& packed = sections[light_section][index / 2];
                    packed |= static_cast<std::uint8_t>(
                        (index % 2 == 0 ? 0x0FU : 0xF0U));
                }
            }
        }
    }
    return sections;
}

void write_light_data(Bytes& output, const world::Chunk& chunk) {
    constexpr auto light_section_count = world::section_count + 2;
    constexpr auto all_light_sections =
        (std::uint64_t{1} << light_section_count) - 1U;
    const std::array full_light_mask{all_light_sections};
    write_bitset(output, full_light_mask);
    write_bitset(output, {});
    write_bitset(output, {});
    write_bitset(output, full_light_mask);
    const auto sky_light = sky_light_sections(chunk);
    write_varint(output, static_cast<std::int32_t>(sky_light.size()));
    for (const auto& section : sky_light) {
        write_byte_array(output, section);
    }
    write_varint(output, 0);
}

} // namespace

Bytes encode_level_chunk(const world::Chunk& chunk) {
    const auto chunk_position = chunk.position();

    Bytes section_data;
    for (std::size_t section_index = 0; section_index < world::section_count; ++section_index) {
        const auto& section = chunk.sections()[section_index];
        std::vector<std::int32_t> block_states;
        block_states.reserve(section.blocks().size());
        std::size_t fluid_count = 0;
        for (const auto block : section.blocks()) {
            block_states.push_back(protocol_block_state_id(block));
            fluid_count += block == world::BlockId::water ? 1U : 0U;
        }
        write_i16_be(section_data, static_cast<std::int16_t>(section.non_air_count()));
        write_i16_be(section_data, static_cast<std::int16_t>(fluid_count));
        const auto encoded_blocks = encode_paletted_container(block_states, 4, 8, 15);
        section_data.insert(
            section_data.end(), encoded_blocks.begin(), encoded_blocks.end());

        std::vector<std::int32_t> biomes;
        biomes.reserve(64);
        for (std::size_t quart_y = 0; quart_y < 4; ++quart_y) {
            for (std::size_t quart_z = 0; quart_z < 4; ++quart_z) {
                for (std::size_t quart_x = 0; quart_x < 4; ++quart_x) {
                    biomes.push_back(protocol_biome(chunk.biome(
                        quart_x, section_index * 4 + quart_y, quart_z)));
                }
            }
        }
        const auto encoded_biomes = encode_paletted_container(biomes, 1, 3, 7);
        section_data.insert(
            section_data.end(), encoded_biomes.begin(), encoded_biomes.end());
    }

    Bytes payload;
    write_i32_be(payload, chunk_position.x);
    write_i32_be(payload, chunk_position.z);
    constexpr std::size_t heightmap_bits = 9;
    constexpr std::size_t values_per_heightmap_word = 64 / heightmap_bits;
    std::array<std::uint64_t, 37> heightmap_words{};
    for (std::size_t value_index = 0; value_index < 256; ++value_index) {
        const auto x = value_index % 16;
        const auto z = value_index / 16;
        const auto surface_height = std::clamp(
            chunk.height(x, z), world::min_build_y, world::max_build_y - 1);
        const auto heightmap_value = static_cast<std::uint64_t>(surface_height + 65);
        const auto word_index = value_index / values_per_heightmap_word;
        const auto offset = (value_index % values_per_heightmap_word) * heightmap_bits;
        heightmap_words[word_index] |= heightmap_value << offset;
    }
    write_varint(payload, 3);
    for (const auto heightmap_type : {1, 4, 5}) {
        write_varint(payload, heightmap_type);
        write_varint(payload, static_cast<std::int32_t>(heightmap_words.size()));
        for (const auto word : heightmap_words) {
            write_i64_be(payload, static_cast<std::int64_t>(word));
        }
    }
    write_varint(payload, static_cast<std::int32_t>(section_data.size()));
    payload.insert(payload.end(), section_data.begin(), section_data.end());
    write_varint(payload, 0);
    write_light_data(payload, chunk);

    return frame_packet(
        static_cast<std::int32_t>(ClientboundPacketId::level_chunk_with_light), payload);
}

Bytes encode_light_update(const world::Chunk& chunk) {
    Bytes payload;
    write_varint(payload, chunk.position().x);
    write_varint(payload, chunk.position().z);
    write_light_data(payload, chunk);
    return frame_packet(static_cast<std::int32_t>(ClientboundPacketId::light_update), payload);
}

} // namespace play
} // namespace mc::protocol