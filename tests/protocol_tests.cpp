#include "mc/protocol/codec.hpp"
#include "mc/protocol/crypto.hpp"
#include "mc/protocol/nbt.hpp"
#include "mc/protocol/packets.hpp"
#include "mc/world/generation.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>

namespace {

using mc::protocol::Bytes;
using mc::protocol::DecodeError;
using mc::protocol::Reader;

void test_varints() {
    constexpr std::array values{
        std::int32_t{0}, std::int32_t{1}, std::int32_t{127},
        std::int32_t{128}, std::int32_t{255}, std::int32_t{2'097'151},
        std::numeric_limits<std::int32_t>::max(), std::int32_t{-1},
        std::numeric_limits<std::int32_t>::min()};

    for (const auto expected : values) {
        Bytes encoded;
        mc::protocol::write_varint(encoded, expected);
        Reader reader(encoded);
        assert(reader.read_varint() == expected);
        assert(reader.empty());
    }
}

void test_scalars_and_string() {
    Bytes encoded;
    mc::protocol::write_bool(encoded, true);
    mc::protocol::write_i8(encoded, -7);
    mc::protocol::write_i16_be(encoded, -12'345);
    mc::protocol::write_u16_be(encoded, 25'565);
    mc::protocol::write_varlong(encoded, std::numeric_limits<std::int64_t>::min());
    mc::protocol::write_i64_be(encoded, -9'223'372'036'854'775'000LL);
    mc::protocol::write_f32_be(encoded, 1.25F);
    mc::protocol::write_f64_be(encoded, -9.5);
    const mc::protocol::Uuid uuid{
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    mc::protocol::write_uuid(encoded, uuid);
    mc::protocol::write_position(encoded, {-33'554'432, -2'048, 33'554'431});
    mc::protocol::write_angle(encoded, 192);
    mc::protocol::write_string(encoded, "mcsquared");
    mc::protocol::write_identifier(encoded, "stone");
    mc::protocol::write_byte_array(encoded, std::array<std::uint8_t, 3>{1, 2, 3});
    mc::protocol::write_bitset(encoded, std::array<std::uint64_t, 2>{1, 1ULL << 63U});
    mc::protocol::write_optional<std::int32_t>(
        encoded, 42, [](Bytes& output, const std::int32_t value) {
            mc::protocol::write_varint(output, value);
        });
    mc::protocol::write_collection<std::int32_t>(
        encoded, std::array<std::int32_t, 3>{3, 4, 5},
        [](Bytes& output, const std::int32_t value) {
            mc::protocol::write_varint(output, value);
        });

    Reader reader(encoded);
    assert(reader.read_bool());
    assert(reader.read_i8() == -7);
    assert(reader.read_i16_be() == -12'345);
    assert(reader.read_u16_be() == 25'565);
    assert(reader.read_varlong() == std::numeric_limits<std::int64_t>::min());
    assert(reader.read_i64_be() == -9'223'372'036'854'775'000LL);
    assert(reader.read_f32_be() == 1.25F);
    assert(reader.read_f64_be() == -9.5);
    assert(reader.read_uuid() == uuid);
    assert(reader.read_position() == mc::protocol::BlockPosition({-33'554'432, -2'048, 33'554'431}));
    assert(reader.read_angle() == 192);
    assert(reader.read_string(32) == "mcsquared");
    assert(reader.read_identifier() == "minecraft:stone");
    assert(reader.read_byte_array(3) == Bytes({1, 2, 3}));
    assert(reader.read_bitset(2) == std::vector<std::uint64_t>({1, 1ULL << 63U}));
    assert(reader.read_optional([](Reader& input) { return input.read_varint(); }) == 42);
    assert(reader.read_collection(3, [](Reader& input) { return input.read_varint(); }) ==
           std::vector<std::int32_t>({3, 4, 5}));
    assert(reader.empty());
}

void test_utf8_validation() {
    Bytes encoded;
    mc::protocol::write_string(encoded, "\xF0\x9F\x98\x80");
    Reader accepted(encoded);
    assert(accepted.read_string(2) == "\xF0\x9F\x98\x80");

    try {
        Reader rejected(encoded);
        static_cast<void>(rejected.read_string(1));
        assert(false);
    } catch (const DecodeError&) {
    }

    const Bytes malformed{0x02, 0xC0, 0xAF};
    try {
        Reader reader(malformed);
        static_cast<void>(reader.read_string(2));
        assert(false);
    } catch (const DecodeError&) {
    }
}

void test_packet_framing() {
    Bytes payload;
    mc::protocol::write_string(payload, "{}");
    const auto packet = mc::protocol::frame_packet(0, payload);
    const Bytes expected{0x04, 0x00, 0x02, '{', '}'};
    assert(packet == expected);
}

void test_bounded_nbt() {
    namespace nbt = mc::protocol::nbt;
    nbt::Compound compound;
    compound.entries = {
        {"byte", {nbt::Type::byte, std::int8_t{-7}}},
        {"short", {nbt::Type::short_integer, std::int16_t{-1234}}},
        {"int", {nbt::Type::integer, std::int32_t{123'456}}},
        {"long", {nbt::Type::long_integer, std::int64_t{-9'876'543'210LL}}},
        {"float", {nbt::Type::float_number, 1.25F}},
        {"double", {nbt::Type::double_number, -3.5}},
        {"bytes", {nbt::Type::byte_array, nbt::ByteArray{-1, 0, 1}}},
        {"string", nbt::string_tag(std::string("A\0", 2) + "\xF0\x9F\x98\x80")},
        {"list", {nbt::Type::list, nbt::List{
            nbt::Type::integer,
            {{nbt::Type::integer, std::int32_t{4}},
             {nbt::Type::integer, std::int32_t{5}}}}}},
        {"compound", {nbt::Type::compound, nbt::Compound{
            {{"nested", nbt::string_tag("value")}}}}},
        {"ints", {nbt::Type::integer_array, nbt::IntegerArray{1, -2, 3}}},
        {"longs", {nbt::Type::long_array, nbt::LongArray{4, -5, 6}}},
    };
    const nbt::Tag root{nbt::Type::compound, std::move(compound)};
    Bytes encoded;
    nbt::write_any_tag(encoded, root);
    assert(encoded.front() == static_cast<std::uint8_t>(nbt::Type::compound));
    Reader reader(encoded);
    const auto decoded = nbt::read_any_tag(reader);
    assert(reader.empty());
    assert(decoded.type == nbt::Type::compound);
    Bytes reencoded;
    nbt::write_any_tag(reencoded, decoded);
    assert(reencoded == encoded);

    const Bytes invalid_array{
        static_cast<std::uint8_t>(nbt::Type::byte_array), 0x7F, 0xFF, 0xFF, 0xFF};
    try {
        Reader invalid_reader(invalid_array);
        static_cast<void>(nbt::read_any_tag(invalid_reader));
        assert(false);
    } catch (const DecodeError&) {
    }

    nbt::Tag nested = nbt::string_tag("bottom");
    for (int depth = 0; depth < 4; ++depth) {
        nested = nbt::Tag{nbt::Type::compound, nbt::Compound{{{"next", nested}}}};
    }
    try {
        Bytes limited;
        nbt::write_any_tag(limited, nested, {.max_bytes = 1'024, .max_depth = 2});
        assert(false);
    } catch (const std::length_error&) {
    }

    try {
        Bytes limited;
        nbt::write_any_tag(limited, nbt::string_tag(std::string(32, 'x')),
                           {.max_bytes = 8, .max_depth = 2,
                            .max_collection_entries = 8, .max_string_bytes = 64});
        assert(false);
    } catch (const std::length_error&) {
    }
}

void test_play_streaming_packets() {
    Bytes movement;
    mc::protocol::write_varint(movement, 0x1F);
    mc::protocol::write_f64_be(movement, -16.25);
    mc::protocol::write_f64_be(movement, 72.0);
    mc::protocol::write_f64_be(movement, 31.75);
    mc::protocol::write_f32_be(movement, 90.0F);
    mc::protocol::write_f32_be(movement, -15.0F);
    movement.push_back(0x03);
    Reader movement_reader(movement);
    const auto position = mc::protocol::play::decode_player_position(movement_reader);
    assert(position.x == -16.25);
    assert(position.y == 72.0);
    assert(position.z == 31.75);
    assert(position.yaw == 90.0F);
    assert(position.pitch == -15.0F);
    assert(position.on_ground);
    assert(position.horizontal_collision);

    Bytes rotation_bytes{0x20};
    mc::protocol::write_f32_be(rotation_bytes, 45.0F);
    mc::protocol::write_f32_be(rotation_bytes, -10.0F);
    rotation_bytes.push_back(0x01);
    Reader rotation_reader(rotation_bytes);
    const auto rotation = mc::protocol::play::decode_player_rotation(rotation_reader);
    assert(rotation.yaw == 45.0F && rotation.pitch == -10.0F);
    assert(rotation.on_ground && !rotation.horizontal_collision);

    Bytes status_bytes{0x21, 0x02};
    Reader status_reader(status_bytes);
    const auto status = mc::protocol::play::decode_player_status(status_reader);
    assert(!status.first && status.second);

    Bytes carried_bytes{0x35, 0x00, 0x08};
    Reader carried_reader(carried_bytes);
    assert(mc::protocol::play::decode_set_carried_item(carried_reader) == 8);

    Bytes swing_bytes{0x3F, 0x01};
    Reader swing_reader(swing_bytes);
    assert(mc::protocol::play::decode_swing(swing_reader) == 1);

    const auto packet_body = [](const Bytes& framed) {
        Reader frame(framed);
        const auto size = frame.read_varint();
        const auto bytes = frame.read_bytes(static_cast<std::size_t>(size));
        return Bytes(bytes.begin(), bytes.end());
    };
    const std::array state_packets{
        std::pair{mc::protocol::play::encode_initialize_border(
                      0.0, 0.0, 59'999'968.0, 59'999'968.0, 0, 29'999'984, 5, 15),
                  std::int32_t{0x2B}},
        std::pair{mc::protocol::play::encode_player_abilities(
                      false, false, false, false, 0.05F, 0.1F),
                  std::int32_t{0x40}},
        std::pair{mc::protocol::play::encode_chunk_cache_radius(2),
                  std::int32_t{0x5F}},
        std::pair{mc::protocol::play::encode_experience(0.0F, 0, 0),
                  std::int32_t{0x67}},
        std::pair{mc::protocol::play::encode_health(20.0F, 20, 5.0F),
                  std::int32_t{0x68}},
        std::pair{mc::protocol::play::encode_held_slot(0),
                  std::int32_t{0x69}},
        std::pair{mc::protocol::play::encode_set_time(100, 6'000),
              std::int32_t{0x71}},
    };
    for (const auto& [framed, expected_id] : state_packets) {
        const auto body = packet_body(framed);
        Reader state_reader(body);
        assert(state_reader.read_varint() == expected_id);
    }

    const auto statistics_body = packet_body(mc::protocol::play::encode_award_stats(
        std::array<mc::protocol::play::StatisticEntry, 2>{
            mc::protocol::play::StatisticEntry{8, 32, 1},
            mc::protocol::play::StatisticEntry{0, 2, 4}}));
    Reader statistics(statistics_body);
    assert(statistics.read_varint() == 0x03);
    assert(statistics.read_varint() == 2);
    assert(statistics.read_varint() == 8);
    assert(statistics.read_varint() == 32);
    assert(statistics.read_varint() == 1);
    assert(statistics.read_varint() == 0);
    assert(statistics.read_varint() == 2);
    assert(statistics.read_varint() == 4);
    assert(statistics.empty());

    const auto time_body = packet_body(mc::protocol::play::encode_set_time(100, 6'000));
    Reader time(time_body);
    assert(time.read_varint() == 0x71);
    assert(time.read_i64_be() == 100);
    assert(time.read_varint() == 1);
    assert(time.read_varint() == 1);
    assert(time.read_varlong() == 6'000);
    assert(time.read_f32_be() == 0.0F);
    assert(time.read_f32_be() == 1.0F);
    assert(time.empty());

    const std::array hud_packets{
        std::pair{mc::protocol::play::encode_clear_titles(true), std::int32_t{0x0E}},
        std::pair{mc::protocol::play::encode_disconnect_text("Bye"), std::int32_t{0x20}},
        std::pair{mc::protocol::play::encode_action_bar_text("Action"), std::int32_t{0x57}},
        std::pair{mc::protocol::play::encode_subtitle_text("Subtitle"), std::int32_t{0x70}},
        std::pair{mc::protocol::play::encode_title_text("Title"), std::int32_t{0x72}},
        std::pair{mc::protocol::play::encode_titles_animation(10, 70, 20), std::int32_t{0x73}},
        std::pair{mc::protocol::play::encode_system_chat("System", false), std::int32_t{0x79}},
        std::pair{mc::protocol::play::encode_tab_list("Header", "Footer"), std::int32_t{0x7A}},
    };
    for (const auto& [framed, expected_id] : hud_packets) {
        const auto body = packet_body(framed);
        Reader hud_reader(body);
        assert(hud_reader.read_varint() == expected_id);
    }

    const std::array border_packets{
        std::pair{mc::protocol::play::encode_border_center(2.5, -3.5), std::int32_t{0x58}},
        std::pair{mc::protocol::play::encode_border_lerp_size(100.0, 200.0, 40),
                  std::int32_t{0x59}},
        std::pair{mc::protocol::play::encode_border_size(300.0), std::int32_t{0x5A}},
        std::pair{mc::protocol::play::encode_border_warning_delay(15), std::int32_t{0x5B}},
        std::pair{mc::protocol::play::encode_border_warning_distance(5), std::int32_t{0x5C}},
    };
    for (const auto& [framed, expected_id] : border_packets) {
        const auto body = packet_body(framed);
        Reader border_reader(body);
        assert(border_reader.read_varint() == expected_id);
    }

    const auto system_chat_body = packet_body(
        mc::protocol::play::encode_system_chat("System", false));
    Reader system_chat_reader(system_chat_body);
    assert(system_chat_reader.read_varint() == 0x79);
    const auto system_component = mc::protocol::nbt::read_any_tag(system_chat_reader);
    assert(system_component.type == mc::protocol::nbt::Type::string);
    assert(std::get<std::string>(system_component.value) == "System");
    assert(!system_chat_reader.read_bool());
    assert(system_chat_reader.empty());

    const mc::protocol::Uuid common_id{
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const std::array common_packets{
        std::pair{mc::protocol::play::encode_cookie_request("mcsquared:test"),
                  std::int32_t{0x15}},
        std::pair{mc::protocol::play::encode_custom_payload("mcsquared:test", Bytes{1, 2}),
                  std::int32_t{0x18}},
        std::pair{mc::protocol::play::encode_pong_response(42), std::int32_t{0x3E}},
        std::pair{mc::protocol::play::encode_resource_pack_pop(common_id),
                  std::int32_t{0x50}},
        std::pair{mc::protocol::play::encode_resource_pack_push(
                      common_id, "https://example.test/pack.zip", std::string(40, 'a'), false),
                  std::int32_t{0x51}},
        std::pair{mc::protocol::play::encode_custom_report_details({}),
                  std::int32_t{0x88}},
        std::pair{mc::protocol::play::encode_empty_server_links(), std::int32_t{0x89}},
    };
    for (const auto& [framed, expected_id] : common_packets) {
        const auto body = packet_body(framed);
        Reader common_reader(body);
        assert(common_reader.read_varint() == expected_id);
    }

    Bytes cookie_response{0x15};
    mc::protocol::write_identifier(cookie_response, "mcsquared:test");
    mc::protocol::write_bool(cookie_response, false);
    Reader cookie_response_reader(cookie_response);
    assert(mc::protocol::play::decode_cookie_response(cookie_response_reader).key ==
           "mcsquared:test");

    Bytes ping_request{0x26};
    mc::protocol::write_i64_be(ping_request, -42);
    Reader ping_request_reader(ping_request);
    assert(mc::protocol::play::decode_ping_request(ping_request_reader) == -42);

    Bytes ability_request{0x28, 0x02};
    Reader ability_request_reader(ability_request);
    assert(mc::protocol::play::decode_player_abilities(ability_request_reader));

    Bytes pick_block_request{0x24};
    mc::protocol::write_position(pick_block_request, {-12, 80, 31});
    mc::protocol::write_bool(pick_block_request, true);
    Reader pick_block_reader(pick_block_request);
    const auto picked_block = mc::protocol::play::decode_pick_item_from_block(
        pick_block_reader);
    assert(picked_block.position.x == -12);
    assert(picked_block.position.y == 80);
    assert(picked_block.position.z == 31);
    assert(picked_block.include_data);

    Bytes pick_entity_request{0x25, 0x2A, 0x00};
    Reader pick_entity_reader(pick_entity_request);
    const auto picked_entity = mc::protocol::play::decode_pick_item_from_entity(
        pick_entity_reader);
    assert(picked_entity.entity_id == 42);
    assert(!picked_entity.include_data);

    Bytes player_action{0x29, 0x00};
    mc::protocol::write_position(player_action, {1, 64, -2});
    player_action.push_back(0x01);
    mc::protocol::write_varint(player_action, 17);
    Reader player_action_reader(player_action);
    const auto decoded_action = mc::protocol::play::decode_player_action(player_action_reader);
    assert(decoded_action.action ==
           mc::protocol::play::PlayerActionType::start_destroy_block);
    assert(decoded_action.position == mc::protocol::BlockPosition({1, 64, -2}));
    assert(decoded_action.direction == 1);
    assert(decoded_action.sequence == 17);

    const auto action_ack_body = packet_body(
        mc::protocol::play::encode_block_changed_ack(decoded_action.sequence));
    Reader action_ack(action_ack_body);
    assert(action_ack.read_varint() == 0x04);
    assert(action_ack.read_varint() == 17);
    assert(action_ack.empty());

    Bytes use_item_on{0x42, 0x00};
    mc::protocol::write_position(use_item_on, {2, 65, -3});
    mc::protocol::write_varint(use_item_on, 3);
    mc::protocol::write_f32_be(use_item_on, 0.5F);
    mc::protocol::write_f32_be(use_item_on, 0.25F);
    mc::protocol::write_f32_be(use_item_on, 1.0F);
    mc::protocol::write_bool(use_item_on, false);
    mc::protocol::write_bool(use_item_on, true);
    mc::protocol::write_varint(use_item_on, 18);
    Reader use_item_on_reader(use_item_on);
    const auto decoded_use_on = mc::protocol::play::decode_use_item_on(use_item_on_reader);
    assert(decoded_use_on.hand == 0);
    assert(decoded_use_on.hit.position == mc::protocol::BlockPosition({2, 65, -3}));
    assert(decoded_use_on.hit.direction == 3);
    assert(decoded_use_on.hit.offset_x == 0.5F);
    assert(decoded_use_on.hit.world_border_hit);
    assert(decoded_use_on.sequence == 18);

    Bytes use_item{0x43, 0x01};
    mc::protocol::write_varint(use_item, 19);
    mc::protocol::write_f32_be(use_item, 90.0F);
    mc::protocol::write_f32_be(use_item, -20.0F);
    Reader use_item_reader(use_item);
    const auto decoded_use = mc::protocol::play::decode_use_item(use_item_reader);
    assert(decoded_use.hand == 1);
    assert(decoded_use.sequence == 19);
    assert(decoded_use.yaw == 90.0F && decoded_use.pitch == -20.0F);

    const mc::protocol::Uuid entity_uuid{
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
    const auto add_entity_body = packet_body(mc::protocol::play::encode_add_entity({
        7, entity_uuid, 42, {1.5, 64.0, -2.5}, {0.0, 0.0, 0.0},
        45.0F, 90.0F, 180.0F, 3}));
    Reader add_entity(add_entity_body);
    assert(add_entity.read_varint() == 0x01);
    assert(add_entity.read_varint() == 7);
    assert(add_entity.read_uuid() == entity_uuid);
    assert(add_entity.read_varint() == 42);
    assert(add_entity.read_f64_be() == 1.5);
    assert(add_entity.read_f64_be() == 64.0);
    assert(add_entity.read_f64_be() == -2.5);
    assert(add_entity.read_u8() == 0);
    assert(add_entity.read_u8() == 32);
    assert(add_entity.read_u8() == 64);
    assert(add_entity.read_u8() == 128);
    assert(add_entity.read_varint() == 3);
    assert(add_entity.empty());

    const auto motion_body = packet_body(
        mc::protocol::play::encode_entity_motion(7, {1.0, 0.0, -1.0}));
    Reader motion(motion_body);
    assert(motion.read_varint() == 0x65);
    assert(motion.read_varint() == 7);
    assert(motion.remaining() >= 6);

    const auto move_body = packet_body(
        mc::protocol::play::encode_move_entity_position(7, {0.5, -0.25, 1.0}, true));
    Reader move(move_body);
    assert(move.read_varint() == 0x35);
    assert(move.read_varint() == 7);
    assert(move.read_i16_be() == 2'048);
    assert(move.read_i16_be() == -1'024);
    assert(move.read_i16_be() == 4'096);
    assert(move.read_bool());

    const std::array entity_packets{
        std::pair{mc::protocol::play::encode_animate(7, 1), std::int32_t{0x02}},
        std::pair{mc::protocol::play::encode_entity_event(7, 2), std::int32_t{0x22}},
        std::pair{mc::protocol::play::encode_entity_position_sync(
                      7, {1.0, 2.0, 3.0}, {0.1, 0.2, 0.3}, 90.0F, 45.0F, true),
                  std::int32_t{0x23}},
        std::pair{mc::protocol::play::encode_move_entity_position_rotation(
                      7, {0.0, 0.0, 0.0}, 90.0F, 45.0F, true),
                  std::int32_t{0x36}},
        std::pair{mc::protocol::play::encode_move_entity_rotation(
                      7, 90.0F, 45.0F, true),
                  std::int32_t{0x38}},
        std::pair{mc::protocol::play::encode_remove_entities(
                      std::array<std::int32_t, 3>{5, 7, 10}),
                  std::int32_t{0x4D}},
        std::pair{mc::protocol::play::encode_rotate_head(7, 180.0F),
                  std::int32_t{0x53}},
        std::pair{mc::protocol::play::encode_entity_link(7, 8),
                  std::int32_t{0x64}},
        std::pair{mc::protocol::play::encode_passengers(
                      7, std::array<std::int32_t, 2>{8, 9}),
                  std::int32_t{0x6B}},
        std::pair{mc::protocol::play::encode_take_item_entity(9, 1, 3),
                  std::int32_t{0x7C}},
    };
    for (const auto& [framed, expected_id] : entity_packets) {
        const auto body = packet_body(framed);
        Reader entity_reader(body);
        assert(entity_reader.read_varint() == expected_id);
    }

    const auto remove_body = packet_body(mc::protocol::play::encode_remove_entities(
        std::array<std::int32_t, 3>{5, 7, 10}));
    Reader remove(remove_body);
    assert(remove.read_varint() == 0x4D);
    assert(remove.read_varint() == 3);
    assert(remove.read_varint() == 5);
    assert(remove.read_varint() == 7);
    assert(remove.read_varint() == 10);

    const std::array block_packets{
        std::pair{mc::protocol::play::encode_block_destruction(7, {1, 64, -2}, -1),
                  std::int32_t{0x05}},
        std::pair{mc::protocol::play::encode_block_event({1, 64, -2}, 1, 2, 34),
                  std::int32_t{0x07}},
        std::pair{mc::protocol::play::encode_block_update({1, 64, -2}, 9),
                  std::int32_t{0x08}},
        std::pair{mc::protocol::play::encode_game_rule_values(
                      std::array{std::pair<std::string, std::string>{
                          "minecraft:do_daylight_cycle", "true"}}),
                  std::int32_t{0x27}},
        std::pair{mc::protocol::play::encode_level_event(2001, {1, 64, -2}, 9, false),
                  std::int32_t{0x2E}},
    };
    for (const auto& [framed, expected_id] : block_packets) {
        const auto body = packet_body(framed);
        Reader block_reader(body);
        assert(block_reader.read_varint() == expected_id);
    }

    const auto block_update_body = packet_body(
        mc::protocol::play::encode_block_update({1, 64, -2}, 9));
    Reader block_update(block_update_body);
    assert(block_update.read_varint() == 0x08);
    assert(block_update.read_position() == mc::protocol::BlockPosition({1, 64, -2}));
    assert(block_update.read_varint() == 9);

    const auto rules_body = packet_body(mc::protocol::play::encode_game_rule_values(
        std::array{std::pair<std::string, std::string>{
            "minecraft:do_daylight_cycle", "true"}}));
    Reader rules(rules_body);
    assert(rules.read_varint() == 0x27);
    assert(rules.read_varint() == 1);
    assert(rules.read_identifier() == "minecraft:do_daylight_cycle");
    assert(rules.read_string(32'767) == "true");

    const auto simple_particle_body = packet_body(
        mc::protocol::play::encode_level_particles(
            {0, {}}, false, true, {1.0, 64.0, -2.0},
            0.25F, 0.5F, 0.75F, 1.5F, 12));
    Reader simple_particle(simple_particle_body);
    assert(simple_particle.read_varint() == 0x2F);
    assert(!simple_particle.read_bool());
    assert(simple_particle.read_bool());
    assert(simple_particle.read_f64_be() == 1.0);
    assert(simple_particle.read_f64_be() == 64.0);
    assert(simple_particle.read_f64_be() == -2.0);
    assert(simple_particle.read_f32_be() == 0.25F);
    assert(simple_particle.read_f32_be() == 0.5F);
    assert(simple_particle.read_f32_be() == 0.75F);
    assert(simple_particle.read_f32_be() == 1.5F);
    assert(simple_particle.read_i32_be() == 12);
    assert(simple_particle.read_varint() == 0);
    assert(simple_particle.empty());

    Bytes dust_options;
    mc::protocol::write_i32_be(dust_options, 0x3366CC);
    mc::protocol::write_f32_be(dust_options, 0.8F);
    const auto dust_particle_body = packet_body(
        mc::protocol::play::encode_level_particles(
            {21, dust_options}, true, false, {0.0, 80.0, 0.0},
            0.0F, 0.0F, 0.0F, 0.0F, 1));
    Reader dust_particle(dust_particle_body);
    assert(dust_particle.read_varint() == 0x2F);
    assert(dust_particle.read_bool());
    assert(!dust_particle.read_bool());
    static_cast<void>(dust_particle.read_f64_be());
    static_cast<void>(dust_particle.read_f64_be());
    static_cast<void>(dust_particle.read_f64_be());
    static_cast<void>(dust_particle.read_f32_be());
    static_cast<void>(dust_particle.read_f32_be());
    static_cast<void>(dust_particle.read_f32_be());
    static_cast<void>(dust_particle.read_f32_be());
    assert(dust_particle.read_i32_be() == 1);
    assert(dust_particle.read_varint() == 21);
    assert(dust_particle.read_i32_be() == 0x3366CC);
    assert(dust_particle.read_f32_be() == 0.8F);
    assert(dust_particle.empty());

    const std::array inventory_packets{
        std::pair{mc::protocol::play::encode_container_close(1), std::int32_t{0x11}},
        std::pair{mc::protocol::play::encode_container_content(
                      1, 2,
                      std::array<mc::protocol::play::SimpleItemStack, 2>{
                          mc::protocol::play::SimpleItemStack{}, {1, 32}},
                      {}),
                  std::int32_t{0x12}},
        std::pair{mc::protocol::play::encode_container_data(1, 2, 3),
                  std::int32_t{0x13}},
        std::pair{mc::protocol::play::encode_container_slot(1, 2, 5, {1, 32}),
                  std::int32_t{0x14}},
        std::pair{mc::protocol::play::encode_open_book(0), std::int32_t{0x3A}},
        std::pair{mc::protocol::play::encode_open_screen(1, 0, "Chest"),
                  std::int32_t{0x3B}},
        std::pair{mc::protocol::play::encode_open_sign_editor({1, 64, -2}, true),
                  std::int32_t{0x3C}},
        std::pair{mc::protocol::play::encode_cursor_item({}), std::int32_t{0x60}},
        std::pair{mc::protocol::play::encode_player_inventory(0, {1, 32}),
                  std::int32_t{0x6C}},
    };
    for (const auto& [framed, expected_id] : inventory_packets) {
        const auto body = packet_body(framed);
        Reader inventory_reader(body);
        assert(inventory_reader.read_varint() == expected_id);
    }

    const auto content_body = packet_body(mc::protocol::play::encode_container_content(
        1, 2,
        std::array<mc::protocol::play::SimpleItemStack, 2>{
            mc::protocol::play::SimpleItemStack{}, {1, 32}},
        {}));
    Reader content(content_body);
    assert(content.read_varint() == 0x12);
    assert(content.read_varint() == 1);
    assert(content.read_varint() == 2);
    assert(content.read_varint() == 2);
    assert(content.read_varint() == 0);
    assert(content.read_varint() == 32);
    assert(content.read_varint() == 1);
    assert(content.read_varint() == 0);
    assert(content.read_varint() == 0);
    assert(content.read_varint() == 0);
    assert(content.empty());

    Bytes button_click{0x11, 0x01, 0x02};
    Reader button_click_reader(button_click);
    const std::pair<std::int32_t, std::int32_t> expected_button_click{1, 2};
    assert(mc::protocol::play::decode_container_button_click(button_click_reader) ==
           expected_button_click);
    Bytes container_click{
        0x12, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x01,
        0x00, 0x24, 0x01, 0x01, 0x05, 0x00, 0x00, 0x00};
    Reader container_click_reader(container_click);
    const auto decoded_click =
        mc::protocol::play::decode_container_click(container_click_reader);
    assert(decoded_click.container_id == 0 && decoded_click.state_id == 0);
    assert(decoded_click.slot == 36 && decoded_click.button == 0);
    assert(decoded_click.input == mc::protocol::play::ContainerInput::pickup);
    assert(decoded_click.changed_slots.size() == 1);
    assert(decoded_click.changed_slots.front().first == 36);
    assert(decoded_click.changed_slots.front().second.item_id == 1);
    assert(decoded_click.changed_slots.front().second.count == 5);
    assert(decoded_click.carried_item.empty());

    Bytes oversized_click{
        0x12, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x81, 0x01};
    Reader oversized_click_reader(oversized_click);
    try {
        static_cast<void>(
            mc::protocol::play::decode_container_click(oversized_click_reader));
        assert(false);
    } catch (const mc::protocol::DecodeError&) {
    }
    Bytes close_request{0x13, 0x01};
    Reader close_request_reader(close_request);
    assert(mc::protocol::play::decode_container_close(close_request_reader) == 1);
    Bytes slot_state{0x14, 0x02, 0x01, 0x01};
    Reader slot_state_reader(slot_state);
    const auto decoded_slot_state =
        mc::protocol::play::decode_container_slot_state_change(slot_state_reader);
    assert(decoded_slot_state.slot == 2 && decoded_slot_state.container == 1 &&
           decoded_slot_state.state);
    Bytes creative_slot{0x38, 0x00, 0x24, 0x20, 0x01, 0x00, 0x00};
    Reader creative_slot_reader(creative_slot);
    const auto decoded_creative_slot =
        mc::protocol::play::decode_set_creative_mode_slot(creative_slot_reader);
    assert(decoded_creative_slot.slot == 36);
    assert(decoded_creative_slot.item.item_id == 1);
    assert(decoded_creative_slot.item.count == 32);

    Bytes invalid_creative_slot{0x38, 0x00, 0x24, 0x01, 0x01, 0x01, 0x00};
    Reader invalid_creative_slot_reader(invalid_creative_slot);
    try {
        static_cast<void>(mc::protocol::play::decode_set_creative_mode_slot(
            invalid_creative_slot_reader));
        assert(false);
    } catch (const mc::protocol::DecodeError&) {
    }

    const std::array scoreboard_packets{
        std::pair{mc::protocol::play::encode_player_info_remove(
                      std::array<mc::protocol::Uuid, 1>{entity_uuid}),
                  std::int32_t{0x45}},
        std::pair{mc::protocol::play::encode_reset_score("Player", "objective"),
                  std::int32_t{0x4F}},
        std::pair{mc::protocol::play::encode_display_objective(1, "objective"),
                  std::int32_t{0x62}},
        std::pair{mc::protocol::play::encode_objective(
                      "objective", mc::protocol::play::ObjectiveMethod::add,
                      "Objective", mc::protocol::play::ObjectiveRenderType::integer),
                  std::int32_t{0x6A}},
        std::pair{mc::protocol::play::encode_score(
                      "Player", "objective", 42, "Forty-two"),
                  std::int32_t{0x6E}},
        std::pair{mc::protocol::play::encode_team(
                      "team", 0,
                      mc::protocol::play::TeamParameters{
                          "Team", "[", "]", 0, 0, 3},
                      std::array<std::string, 1>{"Player"}),
                  std::int32_t{0x6D}},
    };
    for (const auto& [framed, expected_id] : scoreboard_packets) {
        const auto body = packet_body(framed);
        Reader scoreboard_reader(body);
        assert(scoreboard_reader.read_varint() == expected_id);
    }

    const auto objective_body = packet_body(mc::protocol::play::encode_objective(
        "objective", mc::protocol::play::ObjectiveMethod::add,
        "Objective", mc::protocol::play::ObjectiveRenderType::hearts));
    Reader objective(objective_body);
    assert(objective.read_varint() == 0x6A);
    assert(objective.read_string(32'767) == "objective");
    assert(objective.read_u8() == 0);
    const auto objective_component = mc::protocol::nbt::read_any_tag(objective);
    assert(std::get<std::string>(objective_component.value) == "Objective");
    assert(objective.read_varint() == 1);
    assert(!objective.read_bool());
    assert(objective.empty());

    const auto team_body = packet_body(mc::protocol::play::encode_team(
        "team", 0,
        mc::protocol::play::TeamParameters{"Team", "[", "]", 0, 0, 3},
        std::array<std::string, 1>{"Player"}));
    Reader team(team_body);
    assert(team.read_varint() == 0x6D);
    assert(team.read_string(32'767) == "team");
    assert(team.read_u8() == 0);
    for (const auto expected : {"Team", "[", "]"}) {
        const auto component = mc::protocol::nbt::read_any_tag(team);
        assert(std::get<std::string>(component.value) == expected);
    }
    assert(team.read_varint() == 0);
    assert(team.read_varint() == 0);
    assert(!team.read_bool());
    assert(team.read_u8() == 3);
    assert(team.read_varint() == 1);
    assert(team.read_string(32'767) == "Player");
    assert(team.empty());

    const std::array sound_packets{
        std::pair{mc::protocol::play::encode_hurt_animation(7, 45.0F),
                  std::int32_t{0x2A}},
        std::pair{mc::protocol::play::encode_sound_entity(7, 6, 7, 1.0F, 0.5F, 42),
                  std::int32_t{0x74}},
        std::pair{mc::protocol::play::encode_sound(
                      7, 4, {1.25, 64.0, -2.25}, 1.0F, 1.0F, 43),
                  std::int32_t{0x75}},
        std::pair{mc::protocol::play::encode_stop_sound(std::nullopt, std::nullopt),
                  std::int32_t{0x77}},
        std::pair{mc::protocol::play::encode_stop_sound(4, "minecraft:block.note_block.harp"),
                  std::int32_t{0x77}},
    };
    for (const auto& [framed, expected_id] : sound_packets) {
        const auto body = packet_body(framed);
        Reader sound_reader(body);
        assert(sound_reader.read_varint() == expected_id);
    }

    const auto positioned_sound_body = packet_body(mc::protocol::play::encode_sound(
        7, 4, {1.25, 64.0, -2.25}, 1.0F, 1.0F, 43));
    Reader positioned_sound(positioned_sound_body);
    assert(positioned_sound.read_varint() == 0x75);
    assert(positioned_sound.read_varint() == 8);
    assert(positioned_sound.read_varint() == 4);
    assert(positioned_sound.read_i32_be() == 10);
    assert(positioned_sound.read_i32_be() == 512);
    assert(positioned_sound.read_i32_be() == -18);
    assert(positioned_sound.read_f32_be() == 1.0F);
    assert(positioned_sound.read_f32_be() == 1.0F);
    assert(positioned_sound.read_i64_be() == 43);

    const auto stop_sound_body = packet_body(mc::protocol::play::encode_stop_sound(
        4, "minecraft:block.note_block.harp"));
    Reader stop_sound(stop_sound_body);
    assert(stop_sound.read_varint() == 0x77);
    assert(stop_sound.read_u8() == 3);
    assert(stop_sound.read_varint() == 4);
    assert(stop_sound.read_identifier() == "minecraft:block.note_block.harp");

    const std::array misc_packets{
        std::pair{mc::protocol::play::encode_change_difficulty(2, false),
                  std::int32_t{0x0A}},
        std::pair{mc::protocol::play::encode_game_test_highlight(
                      {1, 2, 3}, {-1, -2, -3}),
                  std::int32_t{0x28}},
        std::pair{mc::protocol::play::encode_ping(42), std::int32_t{0x3D}},
        std::pair{mc::protocol::play::encode_player_rotation(
                      90.0F, true, -10.0F, false),
                  std::int32_t{0x49}},
        std::pair{mc::protocol::play::encode_select_advancements_tab(
                      "minecraft:story/root"),
                  std::int32_t{0x55}},
        std::pair{mc::protocol::play::encode_ticking_state(20.0F, false),
                  std::int32_t{0x7F}},
        std::pair{mc::protocol::play::encode_ticking_step(5),
                  std::int32_t{0x80}},
        std::pair{mc::protocol::play::encode_projectile_power(7, 1.5),
                  std::int32_t{0x87}},
    };
    for (const auto& [framed, expected_id] : misc_packets) {
        const auto body = packet_body(framed);
        Reader misc_reader(body);
        assert(misc_reader.read_varint() == expected_id);
    }

    const auto rain_event_body = packet_body(
        mc::protocol::play::encode_game_event(7, 0.5F));
    Reader rain_event(rain_event_body);
    assert(rain_event.read_varint() == 0x26);
    assert(rain_event.read_u8() == 7);
    assert(rain_event.read_f32_be() == 0.5F);
    assert(rain_event.empty());

    const auto rotation_packet_body = packet_body(
        mc::protocol::play::encode_player_rotation(90.0F, true, -10.0F, false));
    Reader player_rotation(rotation_packet_body);
    assert(player_rotation.read_varint() == 0x49);
    assert(player_rotation.read_f32_be() == 90.0F);
    assert(player_rotation.read_bool());
    assert(player_rotation.read_f32_be() == -10.0F);
    assert(!player_rotation.read_bool());

    const std::array attribute_snapshots{
        mc::protocol::play::AttributeSnapshot{
            3, 5.0,
            {mc::protocol::play::AttributeModifier{
                "minecraft:test_modifier", 2.5, 0}}}};
    const auto attributes_body = packet_body(
        mc::protocol::play::encode_update_attributes(7, attribute_snapshots));
    Reader attributes(attributes_body);
    assert(attributes.read_varint() == 0x83);
    assert(attributes.read_varint() == 7);
    assert(attributes.read_varint() == 1);
    assert(attributes.read_varint() == 4);
    assert(attributes.read_f64_be() == 5.0);
    assert(attributes.read_varint() == 1);
    assert(attributes.read_identifier() == "minecraft:test_modifier");
    assert(attributes.read_f64_be() == 2.5);
    assert(attributes.read_varint() == 0);
    assert(attributes.empty());

    const auto effect_body = packet_body(mc::protocol::play::encode_update_mob_effect(
        7, 7, 1, 200, true, true, false, true));
    Reader effect(effect_body);
    assert(effect.read_varint() == 0x84);
    assert(effect.read_varint() == 7);
    assert(effect.read_varint() == 8);
    assert(effect.read_varint() == 1);
    assert(effect.read_varint() == 200);
    assert(effect.read_u8() == 0x0B);

    const auto remove_effect_body = packet_body(
        mc::protocol::play::encode_remove_mob_effect(7, 7));
    Reader remove_effect(remove_effect_body);
    assert(remove_effect.read_varint() == 0x4E);
    assert(remove_effect.read_varint() == 7);
    assert(remove_effect.read_varint() == 8);

    Bytes beacon_request{0x34, 0x01, 0x08, 0x00};
    Reader beacon_request_reader(beacon_request);
    const auto beacon = mc::protocol::play::decode_set_beacon(beacon_request_reader);
    assert(beacon.primary == 7);
    assert(!beacon.secondary);

    const std::array metadata_entries{
        mc::protocol::play::EntityMetadataEntry{4, std::uint8_t{0x01}},
        mc::protocol::play::EntityMetadataEntry{0, true},
        mc::protocol::play::EntityMetadataEntry{1, std::int32_t{42}},
        mc::protocol::play::EntityMetadataEntry{2, 1.5F},
        mc::protocol::play::EntityMetadataEntry{3, std::string("value")},
        mc::protocol::play::EntityMetadataEntry{5, std::int64_t{300}},
        mc::protocol::play::EntityMetadataEntry{
            6, mc::protocol::play::MetadataRotations{1.0F, 2.0F, 3.0F}},
        mc::protocol::play::EntityMetadataEntry{
            7, mc::protocol::BlockPosition{1, 64, -2}},
        mc::protocol::play::EntityMetadataEntry{
            8, mc::protocol::play::MetadataBlockState{137}},
        mc::protocol::play::EntityMetadataEntry{
            9, mc::protocol::play::MetadataOptionalUnsignedInt{42}},
        mc::protocol::play::EntityMetadataEntry{
            10, mc::protocol::play::MetadataPose{5}},
    };
    const auto metadata_body = packet_body(
        mc::protocol::play::encode_entity_metadata(7, metadata_entries));
    Reader metadata(metadata_body);
    assert(metadata.read_varint() == 0x63);
    assert(metadata.read_varint() == 7);
    assert(metadata.read_u8() == 4);
    assert(metadata.read_varint() == 0);
    assert(metadata.read_u8() == 0x01);
    assert(metadata.read_u8() == 0);
    assert(metadata.read_varint() == 8);
    assert(metadata.read_bool());
    assert(metadata.read_u8() == 1);
    assert(metadata.read_varint() == 1);
    assert(metadata.read_varint() == 42);
    assert(metadata.read_u8() == 2);
    assert(metadata.read_varint() == 3);
    assert(metadata.read_f32_be() == 1.5F);
    assert(metadata.read_u8() == 3);
    assert(metadata.read_varint() == 4);
    assert(metadata.read_string(32'767) == "value");
    assert(metadata.read_u8() == 5);
    assert(metadata.read_varint() == 2);
    assert(metadata.read_varlong() == 300);
    assert(metadata.read_u8() == 6);
    assert(metadata.read_varint() == 9);
    assert(metadata.read_f32_be() == 1.0F);
    assert(metadata.read_f32_be() == 2.0F);
    assert(metadata.read_f32_be() == 3.0F);
    assert(metadata.read_u8() == 7);
    assert(metadata.read_varint() == 10);
    assert(metadata.read_position() == mc::protocol::BlockPosition(1, 64, -2));
    assert(metadata.read_u8() == 8);
    assert(metadata.read_varint() == 14);
    assert(metadata.read_varint() == 137);
    assert(metadata.read_u8() == 9);
    assert(metadata.read_varint() == 19);
    assert(metadata.read_varint() == 43);
    assert(metadata.read_u8() == 10);
    assert(metadata.read_varint() == 20);
    assert(metadata.read_varint() == 5);
    assert(metadata.read_u8() == 0xFFU);
    assert(metadata.empty());

    const auto item_metadata_body = packet_body(
        mc::protocol::play::encode_entity_metadata(
            12, std::array<mc::protocol::play::EntityMetadataEntry, 1>{
                    mc::protocol::play::EntityMetadataEntry{
                        8, mc::protocol::play::SimpleItemStack{55, 3}}}));
    Reader item_metadata(item_metadata_body);
    assert(item_metadata.read_varint() == 0x63);
    assert(item_metadata.read_varint() == 12);
    assert(item_metadata.read_u8() == 8);
    assert(item_metadata.read_varint() == 7);
    assert(item_metadata.read_varint() == 3);
    assert(item_metadata.read_varint() == 55);
    assert(item_metadata.read_varint() == 0);
    assert(item_metadata.read_varint() == 0);
    assert(item_metadata.read_u8() == 0xFF);
    assert(item_metadata.empty());

    try {
        const std::array duplicates{
            mc::protocol::play::EntityMetadataEntry{1, true},
            mc::protocol::play::EntityMetadataEntry{1, false},
        };
        static_cast<void>(mc::protocol::play::encode_entity_metadata(7, duplicates));
        assert(false);
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(mc::protocol::play::encode_entity_metadata(
            7, std::array<mc::protocol::play::EntityMetadataEntry, 1>{
                mc::protocol::play::EntityMetadataEntry{
                    1, mc::protocol::play::MetadataPose{18}}}));
        assert(false);
    } catch (const std::invalid_argument&) {
    }

    Bytes attack_request{0x01, 0x07};
    Reader attack_request_reader(attack_request);
    assert(mc::protocol::play::decode_attack(attack_request_reader) == 7);

    Bytes block_entity_query{0x02, 0x07};
    mc::protocol::write_position(block_entity_query, {1, 64, -2});
    Reader block_entity_query_reader(block_entity_query);
    const auto decoded_block_entity_query =
        mc::protocol::play::decode_block_entity_tag_query(block_entity_query_reader);
    assert(decoded_block_entity_query.transaction_id == 7);
    assert(decoded_block_entity_query.position == mc::protocol::BlockPosition(1, 64, -2));

    Bytes entity_query{0x19, 0x08, 0x0C};
    Reader entity_query_reader(entity_query);
    const auto decoded_entity_query =
        mc::protocol::play::decode_entity_tag_query(entity_query_reader);
    assert(decoded_entity_query.transaction_id == 8);
    assert(decoded_entity_query.entity_id == 12);
    try {
        Bytes invalid_entity_query{0x19, 0x08};
        mc::protocol::write_varint(invalid_entity_query, -1);
        Reader invalid_entity_query_reader(invalid_entity_query);
        static_cast<void>(
            mc::protocol::play::decode_entity_tag_query(invalid_entity_query_reader));
        assert(false);
    } catch (const mc::protocol::DecodeError&) {
    }

    Bytes bundle_selection{0x03, 0x24, 0x02};
    Reader bundle_selection_reader(bundle_selection);
    const auto decoded_bundle_selection =
        mc::protocol::play::decode_bundle_item_selection(bundle_selection_reader);
    assert(decoded_bundle_selection.slot == 36);
    assert(decoded_bundle_selection.selected_index == 2);
    try {
        Bytes invalid_bundle_selection{0x03, 0x24, 0x40};
        Reader invalid_bundle_selection_reader(invalid_bundle_selection);
        static_cast<void>(mc::protocol::play::decode_bundle_item_selection(
            invalid_bundle_selection_reader));
        assert(false);
    } catch (const mc::protocol::DecodeError&) {
    }

    const auto framed_null_tag = mc::protocol::play::encode_tag_query(7);
    const auto null_tag_body = packet_body(framed_null_tag);
    Reader null_tag(null_tag_body);
    assert(null_tag.read_varint() == 0x7B);
    assert(null_tag.read_varint() == 7);
    assert(null_tag.read_u8() == 0);
    assert(null_tag.empty());

    const mc::protocol::nbt::Tag empty_compound{
        mc::protocol::nbt::Type::compound, mc::protocol::nbt::Compound{}};
    const auto framed_block_entity =
        mc::protocol::play::encode_block_entity_data({1, 64, -2}, 3, empty_compound);
    const auto block_entity_body = packet_body(framed_block_entity);
    Reader block_entity_data(block_entity_body);
    assert(block_entity_data.read_varint() == 0x06);
    assert(block_entity_data.read_position() == mc::protocol::BlockPosition(1, 64, -2));
    assert(block_entity_data.read_varint() == 3);
    const auto decoded_compound = mc::protocol::nbt::read_any_tag(block_entity_data);
    assert(decoded_compound.type == mc::protocol::nbt::Type::compound);
    assert(block_entity_data.empty());

        Bytes command_request{0x07};
        mc::protocol::write_string(command_request, "time set day");
        Reader command_request_reader(command_request);
        assert(mc::protocol::play::decode_chat_command(command_request_reader) ==
            "time set day");

    Bytes suggestion_request{0x0F, 0x2A};
    mc::protocol::write_string(suggestion_request, "/ga");
    Reader suggestion_request_reader(suggestion_request);
    const auto suggestion =
        mc::protocol::play::decode_command_suggestion(suggestion_request_reader);
    assert(suggestion.id == 42 && suggestion.command == "/ga");

    const std::array<std::string, 2> suggestions{"gamemode", "gamerule"};
    const auto framed_suggestions =
        mc::protocol::play::encode_command_suggestions(42, 1, 2, suggestions);
    const auto suggestion_body = packet_body(framed_suggestions);
    Reader suggestion_response(suggestion_body);
    assert(suggestion_response.read_varint() == 0x0F);
    assert(suggestion_response.read_varint() == 42);
    assert(suggestion_response.read_varint() == 1);
    assert(suggestion_response.read_varint() == 2);
    assert(suggestion_response.read_varint() == 2);
    assert(suggestion_response.read_string(256) == "gamemode");
    assert(!suggestion_response.read_bool());
    assert(suggestion_response.read_string(256) == "gamerule");
    assert(!suggestion_response.read_bool());
    assert(suggestion_response.empty());

    constexpr std::array<std::string_view, 2> command_roots{"time", "say"};
    const auto framed_tree = mc::protocol::play::encode_command_tree(command_roots);
    const auto tree_body = packet_body(framed_tree);
    Reader command_tree(tree_body);
    assert(command_tree.read_varint() == 0x10);
    assert(command_tree.read_varint() == 3);
    assert(command_tree.read_u8() == 0);
    assert(command_tree.read_varint() == 2);
    assert(command_tree.read_varint() == 1);
    assert(command_tree.read_varint() == 2);
    for (const auto expected : {"say", "time"}) {
        assert(command_tree.read_u8() == 0x05);
        assert(command_tree.read_varint() == 0);
        assert(command_tree.read_string(64) == expected);
    }
    assert(command_tree.read_varint() == 0);
    assert(command_tree.empty());

    Reader encoded_motion(motion_body);
    assert(encoded_motion.read_varint() == 0x65);
    assert(encoded_motion.read_varint() == 7);
    const auto encoded_lp = encoded_motion.read_bytes(encoded_motion.remaining());
    Bytes interact_request{0x1A, 0x07, 0x01};
    interact_request.insert(interact_request.end(), encoded_lp.begin(), encoded_lp.end());
    interact_request.push_back(0x01);
    Reader interact_request_reader(interact_request);
    const auto interaction = mc::protocol::play::decode_interact(interact_request_reader);
    assert(interaction.entity_id == 7);
    assert(interaction.hand == 1);
    assert(interaction.location.x > 0.99 && interaction.location.x <= 1.0);
    assert(std::abs(interaction.location.y) < 0.001);
    assert(interaction.location.z >= -1.0 && interaction.location.z < -0.99);
    assert(interaction.secondary_action);

    Bytes rename_request{0x30};
    mc::protocol::write_string(rename_request, "Name");
    Reader rename_request_reader(rename_request);
    assert(mc::protocol::play::decode_rename_item(rename_request_reader) == "Name");

    Bytes resource_response{0x31};
    mc::protocol::write_uuid(resource_response, common_id);
    mc::protocol::write_varint(resource_response, 3);
    Reader resource_response_reader(resource_response);
    assert(mc::protocol::play::decode_resource_pack_response(resource_response_reader).action ==
           mc::protocol::configuration::ResourcePackAction::accepted);

    Bytes trade_request{0x33, 0x02};
    Reader trade_request_reader(trade_request);
    assert(mc::protocol::play::decode_select_trade(trade_request_reader) == 2);

    try {
        Bytes invalid_status{0x21, 0x04};
        Reader invalid_reader(invalid_status);
        static_cast<void>(mc::protocol::play::decode_player_status(invalid_reader));
        assert(false);
    } catch (const DecodeError&) {
    }
    try {
        static_cast<void>(mc::protocol::play::encode_border_size(-1.0));
        assert(false);
    } catch (const std::invalid_argument&) {
    }

    const auto recipe_remove_body = packet_body(
        mc::protocol::play::encode_recipe_book_remove(
            std::array<std::int32_t, 3>{1, 2, 5}));
    Reader recipe_remove(recipe_remove_body);
    assert(recipe_remove.read_varint() == 0x4B);
    assert(recipe_remove.read_varint() == 3);
    assert(recipe_remove.read_varint() == 1);
    assert(recipe_remove.read_varint() == 2);
    assert(recipe_remove.read_varint() == 5);

    const std::array<bool, 8> recipe_settings{
        true, false, false, true, true, true, false, false};
    const auto recipe_settings_body = packet_body(
        mc::protocol::play::encode_recipe_book_settings(recipe_settings));
    Reader encoded_settings(recipe_settings_body);
    assert(encoded_settings.read_varint() == 0x4C);
    for (const auto expected : recipe_settings) {
        assert(encoded_settings.read_bool() == expected);
    }

    Bytes place_recipe{0x27, 0x01, 0x05, 0x01};
    Reader place_recipe_reader(place_recipe);
    const auto place = mc::protocol::play::decode_place_recipe(place_recipe_reader);
    assert(place.container_id == 1 && place.display_id == 5 && place.use_max_items);

    Bytes change_recipe_settings{0x2E, 0x02, 0x01, 0x00};
    Reader change_recipe_settings_reader(change_recipe_settings);
    const auto setting_change =
        mc::protocol::play::decode_recipe_book_setting_change(change_recipe_settings_reader);
    assert(setting_change.book_type == 2 && setting_change.open && !setting_change.filtering);

    Bytes seen_recipe{0x2F, 0x05};
    Reader seen_recipe_reader(seen_recipe);
    assert(mc::protocol::play::decode_recipe_book_seen(seen_recipe_reader) == 5);

    Bytes player_command{0x2A, 0x01, 0x01, 0x00};
    Reader player_command_reader(player_command);
    const auto command = mc::protocol::play::decode_player_command(player_command_reader);
    assert(command.entity_id == 1);
    assert(command.action == mc::protocol::play::PlayerCommandAction::start_sprinting);
    assert(command.data == 0);

    Bytes player_input{0x2B, 0x71};
    Reader player_input_reader(player_input);
    const auto input = mc::protocol::play::decode_player_input(player_input_reader);
    assert(input.forward && !input.backward && !input.left && !input.right);
    assert(input.jump && input.shift && input.sprint);

        Bytes respawn_command{0x0C, 0x00};
        Reader respawn_command_reader(respawn_command);
        assert(mc::protocol::play::decode_client_command(respawn_command_reader) ==
            mc::protocol::play::ClientCommandAction::perform_respawn);

        const auto respawn_body = packet_body(mc::protocol::play::encode_respawn());
        Reader respawn(respawn_body);
        assert(respawn.read_varint() == 0x52);
        assert(respawn.read_varint() == 0);
        assert(respawn.read_identifier() == "minecraft:overworld");
        assert(respawn.read_i64_be() == 0);
        assert(respawn.read_i8() == 0);
        assert(respawn.read_i8() == -1);
        assert(!respawn.read_bool() && !respawn.read_bool() && !respawn.read_bool());
        assert(respawn.read_varint() == 0);
        assert(respawn.read_varint() == 63);
        assert(respawn.read_i8() == 0);
        assert(respawn.empty());

        const auto spectator_body = packet_body(
            mc::protocol::play::encode_respawn(false, 3, 0));
        Reader spectator_respawn(spectator_body);
        assert(spectator_respawn.read_varint() == 0x52);
        assert(spectator_respawn.read_varint() == 0);
        assert(spectator_respawn.read_identifier() == "minecraft:overworld");
        assert(spectator_respawn.read_i64_be() == 0);
        assert(spectator_respawn.read_i8() == 3);
        assert(spectator_respawn.read_i8() == 0);

    const auto empty_advancements_body = packet_body(
        mc::protocol::play::encode_empty_advancements(true, false));
    Reader empty_advancements(empty_advancements_body);
    assert(empty_advancements.read_varint() == 0x82);
    assert(empty_advancements.read_bool());
    assert(empty_advancements.read_varint() == 0);
    assert(empty_advancements.read_varint() == 0);
    assert(empty_advancements.read_varint() == 0);
    assert(!empty_advancements.read_bool());
    assert(empty_advancements.empty());

    Bytes opened_advancements{0x32, 0x00};
    mc::protocol::write_identifier(opened_advancements, "minecraft:story/root");
    Reader opened_advancements_reader(opened_advancements);
    const auto opened =
        mc::protocol::play::decode_seen_advancements(opened_advancements_reader);
    assert(opened.opened_tab && opened.tab == "minecraft:story/root");

    Bytes closed_advancements{0x32, 0x01};
    Reader closed_advancements_reader(closed_advancements);
    const auto closed =
        mc::protocol::play::decode_seen_advancements(closed_advancements_reader);
    assert(!closed.opened_tab && !closed.tab);

    try {
        Bytes invalid_advancements{0x32, 0x02};
        Reader invalid_advancements_reader(invalid_advancements);
        static_cast<void>(
            mc::protocol::play::decode_seen_advancements(invalid_advancements_reader));
        assert(false);
    } catch (const mc::protocol::DecodeError&) {
    }

    const auto forget = mc::protocol::play::encode_forget_level_chunk(-2, 3);
    Reader frame_reader(forget);
    const auto frame_size = frame_reader.read_varint();
    Reader packet(frame_reader.read_bytes(static_cast<std::size_t>(frame_size)));
    assert(packet.read_varint() == 0x25);
    const auto packed = static_cast<std::uint64_t>(packet.read_i64_be());
    assert(static_cast<std::int32_t>(packed) == -2);
    assert(static_cast<std::int32_t>(packed >> 32U) == 3);

    std::vector<std::int32_t> global_palette_values(4'096);
    for (std::size_t index = 0; index < global_palette_values.size(); ++index) {
        global_palette_values[index] = static_cast<std::int32_t>(index % 257);
    }
    const auto global_palette = mc::protocol::play::encode_paletted_container(
        global_palette_values, 4, 8, 15);
    Reader global_palette_reader(global_palette);
    assert(global_palette_reader.read_u8() == 15);
    const auto first_global_word =
        static_cast<std::uint64_t>(global_palette_reader.read_i64_be());
    assert((first_global_word & 0x7FFFU) == 0);
    assert(((first_global_word >> 15U) & 0x7FFFU) == 1);
    assert(((first_global_word >> 30U) & 0x7FFFU) == 2);
    assert(((first_global_word >> 45U) & 0x7FFFU) == 3);

    mc::world::ChunkGenerator generator({4'242, 63});
    auto lit_chunk = generator.generate({2, -3});
    constexpr std::size_t light_x = 4;
    constexpr std::size_t light_z = 7;
    const auto light_y = lit_chunk.height(light_x, light_z) + 1;
    const auto read_skylight = [&](const mc::world::Chunk& chunk) {
        Reader light(packet_body(mc::protocol::play::encode_light_update(chunk)));
        assert(light.read_varint() == 0x30);
        assert(light.read_varint() == 2);
        assert(light.read_varint() == -3);
        assert(light.read_bitset(1) == std::vector<std::uint64_t>{(1U << 26U) - 1U});
        assert(light.read_bitset(1).empty());
        assert(light.read_bitset(1).empty());
        assert(light.read_bitset(1) == std::vector<std::uint64_t>{(1U << 26U) - 1U});
        assert(light.read_varint() == 26);
        std::array<Bytes, 26> sections;
        for (auto& section : sections) {
            section = light.read_byte_array(2'048);
            assert(section.size() == 2'048);
        }
        assert(light.read_varint() == 0);
        assert(light.empty());
        const auto light_section = static_cast<std::size_t>(
            (light_y - (mc::world::min_build_y - mc::world::section_height)) /
            mc::world::section_height);
        const auto local_y = static_cast<std::size_t>(
            light_y - (mc::world::min_build_y - mc::world::section_height) -
            static_cast<std::int32_t>(light_section * mc::world::section_height));
        const auto index = (local_y * mc::world::chunk_width + light_z) *
            mc::world::chunk_width + light_x;
        const auto packed_light = sections[light_section][index / 2];
        return static_cast<std::uint8_t>(
            index % 2 == 0 ? packed_light & 0x0FU : packed_light >> 4U);
    };
    assert(read_skylight(lit_chunk) == 15);
    lit_chunk.set_block(light_x, light_y + 1, light_z, mc::world::BlockId::stone);
    assert(read_skylight(lit_chunk) == 0);
    assert(packet.empty());
}

void test_compressed_packet_framing() {
    const Bytes payload(128, 0x5A);
    const auto framed = mc::protocol::frame_compressed_packet(7, payload, 32);
    Reader frame_reader(framed);
    const auto frame_size = frame_reader.read_varint();
    const auto frame_payload = frame_reader.read_bytes(static_cast<std::size_t>(frame_size));
    assert(frame_reader.empty());

    const auto packet = mc::protocol::decode_frame_payload(frame_payload, 32, 1024);
    Reader packet_reader(packet);
    assert(packet_reader.read_varint() == 7);
    const auto decoded_payload = packet_reader.read_bytes(payload.size());
    assert(std::equal(decoded_payload.begin(), decoded_payload.end(), payload.begin(), payload.end()));
    assert(packet_reader.empty());

    const auto below_threshold = mc::protocol::frame_compressed_packet(1, Bytes{2, 3}, 32);
    Reader below_frame_reader(below_threshold);
    const auto below_size = below_frame_reader.read_varint();
    const auto below_payload = below_frame_reader.read_bytes(static_cast<std::size_t>(below_size));
    const auto below_packet = mc::protocol::decode_frame_payload(below_payload, 32, 1024);
    assert(below_packet == Bytes({1, 2, 3}));
}

void test_handshake_and_login_packets() {
    const mc::protocol::Uuid profile_id{
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    Bytes handshake_bytes;
    mc::protocol::write_varint(handshake_bytes, 0);
    mc::protocol::write_varint(handshake_bytes, 776);
    mc::protocol::write_string(handshake_bytes, "localhost");
    mc::protocol::write_u16_be(handshake_bytes, 25'565);
    mc::protocol::write_varint(handshake_bytes, 2);
    Reader handshake_reader(handshake_bytes);
    const auto handshake = mc::protocol::decode_handshake(handshake_reader);
    assert(handshake.protocol_version == 776);
    assert(handshake.server_address == "localhost");
    assert(handshake.server_port == 25'565);
    assert(handshake.next_state == mc::protocol::ConnectionState::login);

    Bytes hello_bytes;
    mc::protocol::write_varint(hello_bytes, 0);
    mc::protocol::write_string(hello_bytes, "CppPlayer");
    mc::protocol::write_uuid(hello_bytes, profile_id);
    Reader hello_reader(hello_bytes);
    const auto hello = mc::protocol::login::decode_hello(hello_reader);
    assert(hello.name == "CppPlayer");
    assert(hello.profile_id == profile_id);

    const mc::protocol::Uuid session_id{
        0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};
    const auto finished = mc::protocol::login::encode_finished(hello, session_id);
    Reader framed(finished);
    const auto body_size = framed.read_varint();
    assert(body_size == 44);
    const auto body = framed.read_bytes(static_cast<std::size_t>(body_size));
    Reader packet(body);
    assert(packet.read_varint() == 2);
    assert(packet.read_uuid() == profile_id);
    assert(packet.read_string(16) == "CppPlayer");
    assert(packet.read_varint() == 0);
    assert(packet.read_uuid() == session_id);
    assert(packet.empty());

    Bytes transfer_handshake;
    mc::protocol::write_varint(transfer_handshake, 0);
    mc::protocol::write_varint(transfer_handshake, 776);
    mc::protocol::write_string(transfer_handshake, "localhost");
    mc::protocol::write_u16_be(transfer_handshake, 25'565);
    mc::protocol::write_varint(transfer_handshake, 3);
    Reader transfer_reader(transfer_handshake);
    assert(mc::protocol::decode_handshake(transfer_reader).next_state ==
           mc::protocol::ConnectionState::transfer);
}

void test_cookie_packets() {
    const auto request = mc::protocol::encode_cookie_request(5, "mcsquared:transfer");
    Reader frame(request);
    const auto body_size = frame.read_varint();
    Reader packet(frame.read_bytes(static_cast<std::size_t>(body_size)));
    assert(packet.read_varint() == 5);
    assert(packet.read_string(32'767) == "mcsquared:transfer");
    assert(packet.empty());

    Bytes response;
    mc::protocol::write_varint(response, 4);
    mc::protocol::write_string(response, "mcsquared:transfer");
    mc::protocol::write_bool(response, true);
    mc::protocol::write_byte_array(response, Bytes{1, 2, 3});
    Reader response_reader(response);
    const auto decoded = mc::protocol::decode_cookie_response(response_reader, 4);
    assert(decoded.key == "mcsquared:transfer");
    assert(decoded.payload == Bytes({1, 2, 3}));

    Bytes oversized;
    mc::protocol::write_varint(oversized, 4);
    mc::protocol::write_string(oversized, "mcsquared:transfer");
    mc::protocol::write_bool(oversized, true);
    mc::protocol::write_varint(oversized, 5'121);
    oversized.resize(oversized.size() + 5'121);
    try {
        Reader oversized_reader(oversized);
        static_cast<void>(mc::protocol::decode_cookie_response(oversized_reader, 4));
        assert(false);
    } catch (const DecodeError&) {
    }
}

void test_login_cryptography() {
    const std::array<std::uint8_t, 16> secret{
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    Bytes plaintext{'m', 'i', 'n', 'e', 'c', 'r', 'a', 'f', 't'};
    auto ciphertext = plaintext;
    mc::protocol::AesCfb8Cipher cipher(secret);
    cipher.encrypt(ciphertext);
    assert(ciphertext != plaintext);
    cipher.decrypt(ciphertext);
    assert(ciphertext == plaintext);

    const auto offline_uuid = mc::protocol::create_offline_uuid("Notch");
    const mc::protocol::Uuid expected{
        0xB5, 0x0A, 0xD3, 0x85, 0x82, 0x9D, 0x31, 0x41,
        0xA2, 0x16, 0x7E, 0x7D, 0x75, 0x39, 0xBA, 0x7F};
    assert(offline_uuid == expected);

    mc::protocol::RsaKeyPair key_pair;
    const Bytes rsa_plaintext{'s', 'e', 'c', 'r', 'e', 't'};
    const auto rsa_ciphertext = mc::protocol::rsa_encrypt(key_pair.public_key_der(), rsa_plaintext);
    assert(key_pair.decrypt(rsa_ciphertext) == rsa_plaintext);

    assert(mc::protocol::minecraft_server_hash("Notch", {}, {}) ==
           "4ed1f46bbe04bc756bcb17c0c7ce3e4632f06a48");
    assert(mc::protocol::minecraft_server_hash("jeb_", {}, {}) ==
           "-7c9d5b0044c130109a5d7b5fb5c317c02b4e28c1");
    assert(mc::protocol::minecraft_server_hash("simon", {}, {}) ==
           "88e16a1019277b15d58faf0541e11910eb756f6");
}

void test_login_encryption_packets() {
    const Bytes public_key{1, 2, 3, 4};
    const Bytes challenge{5, 6, 7, 8};
    const auto framed = mc::protocol::login::encode_encryption_request(
        "", public_key, challenge, true);
    Reader frame(framed);
    const auto body_size = frame.read_varint();
    Reader request(frame.read_bytes(static_cast<std::size_t>(body_size)));
    assert(request.read_varint() == 1);
    assert(request.read_string(20).empty());
    assert(request.read_byte_array(512) == public_key);
    assert(request.read_byte_array(512) == challenge);
    assert(request.read_bool());
    assert(request.empty());

    Bytes response;
    mc::protocol::write_varint(response, 1);
    mc::protocol::write_byte_array(response, Bytes{9, 10});
    mc::protocol::write_byte_array(response, Bytes{11, 12});
    Reader response_reader(response);
    const auto decoded = mc::protocol::login::decode_encryption_response(response_reader);
    assert(decoded.encrypted_secret == Bytes({9, 10}));
    assert(decoded.encrypted_challenge == Bytes({11, 12}));
}

void test_authenticated_login_finished_packet() {
    const mc::protocol::Uuid profile_id{
        0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78,
        0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78};
    const mc::protocol::Uuid session_id{
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x40, 0x11,
        0x80, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const mc::protocol::login::GameProfile profile{
        profile_id,
        "CppPlayer",
        {{"textures", "texture-value", "texture-signature"},
         {"unsigned", "property-value", std::nullopt}},
    };
    const auto framed = mc::protocol::login::encode_finished(profile, session_id);
    Reader frame(framed);
    const auto body_size = frame.read_varint();
    Reader packet(frame.read_bytes(static_cast<std::size_t>(body_size)));
    assert(packet.read_varint() == 2);
    assert(packet.read_uuid() == profile_id);
    assert(packet.read_string(16) == "CppPlayer");
    assert(packet.read_varint() == 2);
    assert(packet.read_string(64) == "textures");
    assert(packet.read_string(32'767) == "texture-value");
    assert(packet.read_bool());
    assert(packet.read_string(1'024) == "texture-signature");
    assert(packet.read_string(64) == "unsigned");
    assert(packet.read_string(32'767) == "property-value");
    assert(!packet.read_bool());
    assert(packet.read_uuid() == session_id);
    assert(packet.empty());
}

void test_login_query_and_disconnect_packets() {
    const auto query = mc::protocol::login::encode_custom_query(
        42, "mcsquared:test", Bytes{1, 2, 3});
    Reader query_frame(query);
    const auto query_size = query_frame.read_varint();
    Reader query_packet(query_frame.read_bytes(static_cast<std::size_t>(query_size)));
    assert(query_packet.read_varint() == 4);
    assert(query_packet.read_varint() == 42);
    assert(query_packet.read_string(32'767) == "mcsquared:test");
    assert(query_packet.read_bytes(3).back() == 3);

    Bytes answer;
    mc::protocol::write_varint(answer, 2);
    mc::protocol::write_varint(answer, 42);
    mc::protocol::write_bool(answer, true);
    answer.insert(answer.end(), {4, 5, 6});
    Reader answer_reader(answer);
    const auto decoded = mc::protocol::login::decode_custom_query_answer(answer_reader);
    assert(decoded.transaction_id == 42);
    assert(decoded.payload == Bytes({4, 5, 6}));

    Bytes declined_answer;
    mc::protocol::write_varint(declined_answer, 2);
    mc::protocol::write_varint(declined_answer, 43);
    mc::protocol::write_bool(declined_answer, false);
    Reader declined_reader(declined_answer);
    const auto declined = mc::protocol::login::decode_custom_query_answer(declined_reader);
    assert(declined.transaction_id == 43);
    assert(!declined.payload.has_value());

    const auto disconnect = mc::protocol::login::encode_disconnect("{\"text\":\"No\"}");
    Reader disconnect_frame(disconnect);
    const auto disconnect_size = disconnect_frame.read_varint();
    Reader disconnect_packet(
        disconnect_frame.read_bytes(static_cast<std::size_t>(disconnect_size)));
    assert(disconnect_packet.read_varint() == 0);
    assert(disconnect_packet.read_string(262'144) == "{\"text\":\"No\"}");
}

void test_configuration_packets() {
    const auto packet_body = [](const Bytes& framed) {
        Reader frame_reader(framed);
        const auto size = frame_reader.read_varint();
        auto body = frame_reader.read_bytes(static_cast<std::size_t>(size));
        assert(frame_reader.empty());
        return Bytes(body.begin(), body.end());
    };

    const mc::protocol::Uuid resource_pack_id{
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    const auto cookie_request_body = packet_body(
        mc::protocol::configuration::encode_cookie_request("mcsquared:test"));
    Reader cookie_request(cookie_request_body);
    assert(cookie_request.read_varint() == 0x00);
    assert(cookie_request.read_identifier() == "mcsquared:test");
    assert(cookie_request.empty());

    Bytes cookie_response_bytes;
    mc::protocol::write_varint(cookie_response_bytes, 0x01);
    mc::protocol::write_identifier(cookie_response_bytes, "mcsquared:test");
    mc::protocol::write_bool(cookie_response_bytes, true);
    mc::protocol::write_byte_array(cookie_response_bytes, Bytes{1, 2, 3});
    Reader cookie_response_reader(cookie_response_bytes);
    const auto cookie_response =
        mc::protocol::configuration::decode_cookie_response(cookie_response_reader);
    assert(cookie_response.key == "mcsquared:test");
    assert(cookie_response.payload == Bytes({1, 2, 3}));

    const auto keep_alive_body = packet_body(
        mc::protocol::configuration::encode_keep_alive(-42));
    Reader keep_alive(keep_alive_body);
    assert(keep_alive.read_varint() == 0x04);
    assert(keep_alive.read_i64_be() == -42);
    assert(keep_alive.empty());
    Bytes keep_alive_response;
    mc::protocol::write_varint(keep_alive_response, 0x04);
    mc::protocol::write_i64_be(keep_alive_response, -42);
    Reader keep_alive_response_reader(keep_alive_response);
    assert(mc::protocol::configuration::decode_keep_alive(keep_alive_response_reader) == -42);

    const auto ping_body = packet_body(mc::protocol::configuration::encode_ping(123'456));
    Reader ping(ping_body);
    assert(ping.read_varint() == 0x05);
    assert(ping.read_i32_be() == 123'456);
    Bytes pong_response;
    mc::protocol::write_varint(pong_response, 0x05);
    mc::protocol::write_i32_be(pong_response, 123'456);
    Reader pong_response_reader(pong_response);
    assert(mc::protocol::configuration::decode_pong(pong_response_reader) == 123'456);

    for (const auto& empty_packet : {
             mc::protocol::configuration::encode_finish(),
             mc::protocol::configuration::encode_reset_chat(),
             mc::protocol::configuration::encode_clear_dialog()}) {
        const auto empty_body = packet_body(empty_packet);
        Reader empty_reader(empty_body);
        static_cast<void>(empty_reader.read_varint());
        assert(empty_reader.empty());
    }

    const auto resource_pack_pop_body = packet_body(
        mc::protocol::configuration::encode_resource_pack_pop(resource_pack_id));
    Reader resource_pack_pop(resource_pack_pop_body);
    assert(resource_pack_pop.read_varint() == 0x08);
    assert(resource_pack_pop.read_bool());
    assert(resource_pack_pop.read_uuid() == resource_pack_id);

    const auto stored_cookie_body = packet_body(
        mc::protocol::configuration::encode_store_cookie("mcsquared:test", Bytes{4, 5, 6}));
    Reader stored_cookie(stored_cookie_body);
    assert(stored_cookie.read_varint() == 0x0A);
    assert(stored_cookie.read_identifier() == "mcsquared:test");
    assert(stored_cookie.read_byte_array(5'120) == Bytes({4, 5, 6}));

    const auto transfer_body = packet_body(
        mc::protocol::configuration::encode_transfer("example.test", 25'565));
    Reader transfer(transfer_body);
    assert(transfer.read_varint() == 0x0B);
    assert(transfer.read_string(32'767) == "example.test");
    assert(transfer.read_varint() == 25'565);

    const auto conduct_body = packet_body(
        mc::protocol::configuration::encode_code_of_conduct("Be kind"));
    Reader conduct(conduct_body);
    assert(conduct.read_varint() == 0x13);
    assert(conduct.read_string(32'767) == "Be kind");

    Bytes client_information_bytes{0x00};
    mc::protocol::write_string(client_information_bytes, "en_us");
    mc::protocol::write_i8(client_information_bytes, 12);
    mc::protocol::write_varint(client_information_bytes, 1);
    mc::protocol::write_bool(client_information_bytes, true);
    client_information_bytes.push_back(0x7F);
    mc::protocol::write_varint(client_information_bytes, 1);
    mc::protocol::write_bool(client_information_bytes, false);
    mc::protocol::write_bool(client_information_bytes, true);
    mc::protocol::write_varint(client_information_bytes, 2);
    Reader client_information_reader(client_information_bytes);
    const auto client_information =
        mc::protocol::configuration::decode_client_information(client_information_reader);
    assert(client_information.language == "en_us");
    assert(client_information.view_distance == 12);
    assert(client_information.chat_visibility == 1);
    assert(client_information.main_hand == 1);
    assert(client_information.particle_status == 2);

    Bytes custom_payload_bytes{0x02};
    mc::protocol::write_identifier(custom_payload_bytes, "mcsquared:test");
    custom_payload_bytes.insert(custom_payload_bytes.end(), {7, 8, 9});
    Reader custom_payload_reader(custom_payload_bytes);
    const auto custom_payload =
        mc::protocol::configuration::decode_custom_payload(custom_payload_reader);
    assert(custom_payload.channel == "mcsquared:test");
    assert(custom_payload.data == Bytes({7, 8, 9}));

    Bytes resource_response_bytes{0x06};
    mc::protocol::write_uuid(resource_response_bytes, resource_pack_id);
    mc::protocol::write_varint(resource_response_bytes, 3);
    Reader resource_response_reader(resource_response_bytes);
    const auto resource_response =
        mc::protocol::configuration::decode_resource_pack_response(resource_response_reader);
    assert(resource_response.id == resource_pack_id);
    assert(resource_response.action ==
           mc::protocol::configuration::ResourcePackAction::accepted);

    const auto resource_push_body = packet_body(
        mc::protocol::configuration::encode_resource_pack_push(
            resource_pack_id, "https://example.test/pack.zip", std::string(40, 'a'), true));
    Reader resource_push(resource_push_body);
    assert(resource_push.read_varint() == 0x09);
    assert(resource_push.read_uuid() == resource_pack_id);
    assert(resource_push.read_string(32'767) == "https://example.test/pack.zip");
    assert(resource_push.read_string(40) == std::string(40, 'a'));
    assert(resource_push.read_bool());
    assert(!resource_push.read_bool());

    const std::array report_details{
        std::pair<std::string, std::string>{"Build", "mcsquared"}};
    const auto report_body = packet_body(
        mc::protocol::configuration::encode_custom_report_details(report_details));
    Reader report(report_body);
    assert(report.read_varint() == 0x0F);
    assert(report.read_varint() == 1);
    assert(report.read_string(128) == "Build");
    assert(report.read_string(4'096) == "mcsquared");

    const auto links_body = packet_body(
        mc::protocol::configuration::encode_empty_server_links());
    Reader links(links_body);
    assert(links.read_varint() == 0x10);
    assert(links.read_varint() == 0);

    const mc::protocol::nbt::Tag direct_dialog{
        mc::protocol::nbt::Type::compound,
        mc::protocol::nbt::Compound{
            {{"type", mc::protocol::nbt::string_tag("minecraft:notice")},
             {"title", mc::protocol::nbt::string_tag("Hello")}}}};
    const auto dialog_body = packet_body(
        mc::protocol::configuration::encode_show_dialog(direct_dialog));
    Reader dialog(dialog_body);
    assert(dialog.read_varint() == 0x12);
    const auto decoded_dialog = mc::protocol::nbt::read_any_tag(dialog);
    assert(decoded_dialog.type == mc::protocol::nbt::Type::compound);
    assert(dialog.empty());

    const auto configuration_disconnect_body = packet_body(
        mc::protocol::configuration::encode_disconnect_text("No"));
    Reader configuration_disconnect(configuration_disconnect_body);
    assert(configuration_disconnect.read_varint() == 0x02);
    const auto disconnect_component = mc::protocol::nbt::read_any_tag(
        configuration_disconnect);
    assert(disconnect_component.type == mc::protocol::nbt::Type::string);
    assert(std::get<std::string>(disconnect_component.value) == "No");
    assert(configuration_disconnect.empty());

    Bytes click_without_payload{0x08};
    mc::protocol::write_identifier(click_without_payload, "mcsquared:action");
    mc::protocol::write_byte_array(click_without_payload, Bytes{0x00});
    Reader click_without_payload_reader(click_without_payload);
    const auto empty_click = mc::protocol::configuration::decode_custom_click_action(
        click_without_payload_reader);
    assert(empty_click.id == "mcsquared:action");
    assert(!empty_click.payload);

    Bytes click_tag;
    mc::protocol::nbt::write_any_tag(
        click_tag,
        {mc::protocol::nbt::Type::compound,
         mc::protocol::nbt::Compound{{{"value", mc::protocol::nbt::string_tag("yes")}}}},
        {.max_bytes = 32'768, .max_depth = 16,
         .max_collection_entries = 8'192, .max_string_bytes = 32'767});
    Bytes click_with_payload{0x08};
    mc::protocol::write_identifier(click_with_payload, "mcsquared:action");
    mc::protocol::write_byte_array(click_with_payload, click_tag);
    Reader click_with_payload_reader(click_with_payload);
    const auto populated_click = mc::protocol::configuration::decode_custom_click_action(
        click_with_payload_reader);
    assert(populated_click.payload);
    assert(populated_click.payload->type == mc::protocol::nbt::Type::compound);

    Bytes malformed_click{0x08};
    mc::protocol::write_identifier(malformed_click, "mcsquared:action");
    mc::protocol::write_byte_array(malformed_click, Bytes{0x00, 0x00});
    try {
        Reader malformed_click_reader(malformed_click);
        static_cast<void>(mc::protocol::configuration::decode_custom_click_action(
            malformed_click_reader));
        assert(false);
    } catch (const DecodeError&) {
    }

    Bytes accepted_conduct{0x09};
    Reader accepted_conduct_reader(accepted_conduct);
    mc::protocol::configuration::decode_accept_code_of_conduct(accepted_conduct_reader);

    try {
        static_cast<void>(mc::protocol::configuration::encode_store_cookie(
            "mcsquared:test", Bytes(5'121)));
        assert(false);
    } catch (const std::length_error&) {
    }

    const auto known_packs_frame = mc::protocol::configuration::encode_select_known_packs();
    Reader frame(known_packs_frame);
    const auto body_size = frame.read_varint();
    Reader packet(frame.read_bytes(static_cast<std::size_t>(body_size)));
    assert(packet.read_varint() == 0x0E);
    assert(packet.read_varint() == 1);
    assert(packet.read_string(32'767) == "minecraft");
    assert(packet.read_string(32'767) == "core");
    assert(packet.read_string(32'767) == "26.2");
    assert(packet.empty());

    Bytes response;
    mc::protocol::write_varint(response, 0x07);
    mc::protocol::write_varint(response, 1);
    mc::protocol::write_string(response, "minecraft");
    mc::protocol::write_string(response, "core");
    mc::protocol::write_string(response, "26.2");
    Reader response_reader(response);
    const auto packs = mc::protocol::configuration::decode_selected_known_packs(response_reader);
    const std::vector expected_packs{
        mc::protocol::configuration::KnownPack{"minecraft", "core", "26.2"}};
    assert(packs == expected_packs);
}

void test_play_login_packet() {
    const auto framed = mc::protocol::play::encode_login();
    Reader frame(framed);
    const auto body_size = frame.read_varint();
    Reader packet(frame.read_bytes(static_cast<std::size_t>(body_size)));
    assert(packet.read_varint() == 0x31);
    assert(packet.read_i32_be() == 1);
    assert(!packet.read_bool());
    assert(packet.read_varint() == 1);
    assert(packet.read_string(32'767) == "minecraft:overworld");
    assert(packet.read_varint() == 20);
    assert(packet.read_varint() == 10);
    assert(packet.read_varint() == 10);
    assert(!packet.read_bool());
    assert(packet.read_bool());
    assert(!packet.read_bool());
    assert(packet.read_varint() == 0);
    assert(packet.read_string(32'767) == "minecraft:overworld");
    assert(packet.read_i64_be() == 0);
    assert(packet.read_bytes(1).front() == 0);
    assert(packet.read_bytes(1).front() == 0xFFU);
    assert(!packet.read_bool());
    assert(!packet.read_bool());
    assert(!packet.read_bool());
    assert(packet.read_varint() == 0);
    assert(packet.read_varint() == 63);
    assert(!packet.read_bool());
    assert(!packet.read_bool());
    assert(packet.empty());

    const auto hardcore_framed = mc::protocol::play::encode_login(true);
    Reader hardcore_frame(hardcore_framed);
    const auto hardcore_size = hardcore_frame.read_varint();
    Reader hardcore_packet(
        hardcore_frame.read_bytes(static_cast<std::size_t>(hardcore_size)));
    assert(hardcore_packet.read_varint() == 0x31);
    assert(hardcore_packet.read_i32_be() == 1);
    assert(hardcore_packet.read_bool());
}

void test_play_keep_alive_packet() {
    constexpr std::int64_t keep_alive_id = -123'456'789'012'345;
    const auto framed = mc::protocol::play::encode_keep_alive(keep_alive_id);
    Reader frame(framed);
    const auto body_size = frame.read_varint();
    Reader clientbound(frame.read_bytes(static_cast<std::size_t>(body_size)));
    assert(clientbound.read_varint() == 0x2C);
    assert(clientbound.read_i64_be() == keep_alive_id);
    assert(clientbound.empty());

    Bytes response;
    mc::protocol::write_varint(response, 0x1C);
    mc::protocol::write_i64_be(response, keep_alive_id);
    Reader serverbound(response);
    assert(mc::protocol::play::decode_keep_alive(serverbound) == keep_alive_id);
}

void test_play_spawn_bootstrap_packets() {
    const auto center_frame = mc::protocol::play::encode_chunk_cache_center(-2, 3);
    Reader center_framed(center_frame);
    const auto center_size = center_framed.read_varint();
    Reader center(center_framed.read_bytes(static_cast<std::size_t>(center_size)));
    assert(center.read_varint() == 0x5E);
    assert(center.read_varint() == -2);
    assert(center.read_varint() == 3);
    assert(center.empty());

    const auto spawn_frame = mc::protocol::play::encode_default_spawn_position(
        {-12, 70, 34}, 90.0F, -15.0F);
    Reader spawn_framed(spawn_frame);
    const auto spawn_size = spawn_framed.read_varint();
    Reader spawn(spawn_framed.read_bytes(static_cast<std::size_t>(spawn_size)));
    assert(spawn.read_varint() == 0x61);
    assert(spawn.read_identifier() == "minecraft:overworld");
    const auto spawn_position = spawn.read_position();
    assert(spawn_position.x == -12);
    assert(spawn_position.y == 70);
    assert(spawn_position.z == 34);
    assert(spawn.read_f32_be() == 90.0F);
    assert(spawn.read_f32_be() == -15.0F);
    assert(spawn.empty());

    const auto position_frame = mc::protocol::play::encode_player_position(
        7, 1.25, 65.0, -2.5, 45.0F, 10.0F);
    Reader position_framed(position_frame);
    const auto position_size = position_framed.read_varint();
    Reader position(position_framed.read_bytes(static_cast<std::size_t>(position_size)));
    assert(position.read_varint() == 0x48);
    assert(position.read_varint() == 7);
    assert(position.read_f64_be() == 1.25);
    assert(position.read_f64_be() == 65.0);
    assert(position.read_f64_be() == -2.5);
    assert(position.read_f64_be() == 0.0);
    assert(position.read_f64_be() == 0.0);
    assert(position.read_f64_be() == 0.0);
    assert(position.read_f32_be() == 45.0F);
    assert(position.read_f32_be() == 10.0F);
    assert(position.read_i32_be() == 0);
    assert(position.empty());

    Bytes acknowledgement;
    mc::protocol::write_varint(acknowledgement, 0);
    mc::protocol::write_varint(acknowledgement, 7);
    Reader acknowledgement_reader(acknowledgement);
    assert(mc::protocol::play::decode_teleport_acknowledgement(
               acknowledgement_reader) == 7);
}

void test_malformed_input() {
    const Bytes too_long{0x80, 0x80, 0x80, 0x80, 0x10};
    try {
        Reader reader(too_long);
        static_cast<void>(reader.read_varint());
        assert(false);
    } catch (const DecodeError&) {
    }

    try {
        static_cast<void>(mc::protocol::decode_frame_payload(Bytes{3, 1, 2, 3}, 8, 1024));
        assert(false);
    } catch (const DecodeError&) {
    }

    try {
        static_cast<void>(mc::protocol::decode_frame_payload(Bytes{0, 1, 2, 3}, 3, 1024));
        assert(false);
    } catch (const DecodeError&) {
    }

    const Bytes truncated_string{0x03, 'a'};
    try {
        Reader reader(truncated_string);
        static_cast<void>(reader.read_string(16));
        assert(false);
    } catch (const DecodeError&) {
    }

    const Bytes invalid_boolean{2};
    try {
        Reader reader(invalid_boolean);
        static_cast<void>(reader.read_bool());
        assert(false);
    } catch (const DecodeError&) {
    }

    const Bytes oversized_array{4, 1, 2, 3, 4};
    try {
        Reader reader(oversized_array);
        static_cast<void>(reader.read_byte_array(3));
        assert(false);
    } catch (const DecodeError&) {
    }

    const Bytes oversized_collection{4};
    try {
        Reader reader(oversized_collection);
        static_cast<void>(reader.read_collection(3, [](Reader& input) {
            return input.read_varint();
        }));
        assert(false);
    } catch (const DecodeError&) {
    }

    const Bytes invalid_identifier{3, 'A', ':', 'x'};
    try {
        Reader reader(invalid_identifier);
        static_cast<void>(reader.read_identifier());
        assert(false);
    } catch (const DecodeError&) {
    }
}

} // namespace

int main() {
    test_varints();
    test_scalars_and_string();
    test_utf8_validation();
    test_packet_framing();
    test_bounded_nbt();
    test_play_streaming_packets();
    test_compressed_packet_framing();
    test_handshake_and_login_packets();
    test_cookie_packets();
    test_login_cryptography();
    test_login_encryption_packets();
    test_authenticated_login_finished_packet();
    test_login_query_and_disconnect_packets();
    test_configuration_packets();
    test_play_login_packet();
    test_play_keep_alive_packet();
    test_play_spawn_bootstrap_packets();
    test_malformed_input();
}