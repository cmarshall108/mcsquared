#pragma once

#include "mc/protocol/codec.hpp"
#include "mc/protocol/connection_state.hpp"
#include "mc/protocol/nbt.hpp"
#include "mc/world/chunk.hpp"

#include <array>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace mc::protocol {

struct Handshake final {
	std::int32_t protocol_version;
	std::string server_address;
	std::uint16_t server_port;
	ConnectionState next_state;
};

[[nodiscard]] Handshake decode_handshake(Reader& packet);

struct CookieResponse final {
	std::string key;
	std::optional<Bytes> payload;
};

[[nodiscard]] Bytes encode_cookie_request(std::int32_t packet_id, std::string_view key);
[[nodiscard]] CookieResponse decode_cookie_response(Reader& packet,
												 std::int32_t expected_packet_id);

namespace login {

enum class ServerboundPacketId : std::int32_t {
	hello = 0x00,
	key = 0x01,
	custom_query_answer = 0x02,
	acknowledged = 0x03,
	cookie_response = 0x04,
};

enum class ClientboundPacketId : std::int32_t {
	disconnect = 0x00,
	hello = 0x01,
	finished = 0x02,
	compression = 0x03,
	custom_query = 0x04,
	cookie_request = 0x05,
};

struct Hello final {
	std::string name;
	Uuid profile_id;
};

struct EncryptionResponse final {
	Bytes encrypted_secret;
	Bytes encrypted_challenge;
};

struct CustomQueryAnswer final {
	std::int32_t transaction_id;
	std::optional<Bytes> payload;
};

struct ProfileProperty final {
	std::string name;
	std::string value;
	std::optional<std::string> signature;

	bool operator==(const ProfileProperty&) const = default;
};

struct GameProfile final {
	Uuid id;
	std::string name;
	std::vector<ProfileProperty> properties;
};

[[nodiscard]] Hello decode_hello(Reader& packet);
[[nodiscard]] EncryptionResponse decode_encryption_response(Reader& packet);
[[nodiscard]] Bytes encode_encryption_request(std::string_view server_id,
											  std::span<const std::uint8_t> public_key,
											  std::span<const std::uint8_t> challenge,
											  bool should_authenticate);
[[nodiscard]] Bytes encode_disconnect(std::string_view json_component);
[[nodiscard]] Bytes encode_custom_query(std::int32_t transaction_id,
										std::string_view channel,
										std::span<const std::uint8_t> payload);
[[nodiscard]] CustomQueryAnswer decode_custom_query_answer(Reader& packet);
[[nodiscard]] Bytes encode_compression(std::size_t threshold);
[[nodiscard]] Bytes encode_finished_payload(const GameProfile& profile,
											 const Uuid& session_id);
[[nodiscard]] Bytes encode_finished(const GameProfile& profile, const Uuid& session_id);
[[nodiscard]] Bytes encode_finished(const Hello& hello, const Uuid& session_id);

} // namespace login

namespace configuration {

struct TagData final {
	std::string name;
	std::vector<std::int32_t> entries;
};

struct RegistryTags final {
	std::string registry;
	std::vector<TagData> tags;
};

struct RegistryData final {
	std::string registry;
	std::vector<std::string> entries;
};

struct KnownPack final {
	std::string name_space;
	std::string id;
	std::string version;

	bool operator==(const KnownPack&) const = default;
};

struct ClientInformation final {
	std::string language;
	std::int8_t view_distance;
	std::uint8_t chat_visibility;
	bool chat_colors;
	std::uint8_t model_customisation;
	std::uint8_t main_hand;
	bool text_filtering_enabled;
	bool allows_listing;
	std::uint8_t particle_status;
};

struct CustomPayload final {
	std::string channel;
	Bytes data;
};

enum class ResourcePackAction : std::uint8_t {
	successfully_loaded,
	declined,
	failed_download,
	accepted,
	downloaded,
	invalid_url,
	failed_reload,
	discarded,
};

struct ResourcePackResponse final {
	Uuid id;
	ResourcePackAction action;
};

struct CustomClickAction final {
	std::string id;
	std::optional<nbt::Tag> payload;
};

enum class ServerboundPacketId : std::int32_t {
	client_information = 0x00,
	cookie_response = 0x01,
	custom_payload = 0x02,
	finish = 0x03,
	keep_alive = 0x04,
	pong = 0x05,
	resource_pack = 0x06,
	select_known_packs = 0x07,
	custom_click_action = 0x08,
	accept_code_of_conduct = 0x09,
};

enum class ClientboundPacketId : std::int32_t {
	cookie_request = 0x00,
	custom_payload = 0x01,
	disconnect = 0x02,
	finish = 0x03,
	keep_alive = 0x04,
	ping = 0x05,
	reset_chat = 0x06,
	registry_data = 0x07,
	resource_pack_pop = 0x08,
	resource_pack_push = 0x09,
	store_cookie = 0x0A,
	transfer = 0x0B,
	update_enabled_features = 0x0C,
	update_tags = 0x0D,
	select_known_packs = 0x0E,
	custom_report_details = 0x0F,
	server_links = 0x10,
	clear_dialog = 0x11,
	show_dialog = 0x12,
	code_of_conduct = 0x13,
};

[[nodiscard]] Bytes encode_brand(std::string_view brand);
[[nodiscard]] Bytes encode_custom_payload(std::string_view channel,
										  std::span<const std::uint8_t> data);
[[nodiscard]] Bytes encode_disconnect_text(std::string_view text);
[[nodiscard]] Bytes encode_cookie_request(std::string_view key);
[[nodiscard]] ClientInformation decode_client_information(Reader& packet);
[[nodiscard]] CookieResponse decode_cookie_response(Reader& packet);
[[nodiscard]] CustomPayload decode_custom_payload(Reader& packet);
[[nodiscard]] Bytes encode_finish();
void decode_finish(Reader& packet);
[[nodiscard]] Bytes encode_keep_alive(std::int64_t id);
[[nodiscard]] std::int64_t decode_keep_alive(Reader& packet);
[[nodiscard]] Bytes encode_ping(std::int32_t id);
[[nodiscard]] std::int32_t decode_pong(Reader& packet);
[[nodiscard]] Bytes encode_reset_chat();
[[nodiscard]] Bytes encode_resource_pack_pop(std::optional<Uuid> id);
[[nodiscard]] Bytes encode_resource_pack_push(Uuid id,
										   std::string_view url,
										   std::string_view hash,
										   bool required);
[[nodiscard]] ResourcePackResponse decode_resource_pack_response(Reader& packet);
[[nodiscard]] CustomClickAction decode_custom_click_action(Reader& packet);
[[nodiscard]] Bytes encode_store_cookie(std::string_view key,
										std::span<const std::uint8_t> payload);
[[nodiscard]] Bytes encode_transfer(std::string_view host, std::uint16_t port);
[[nodiscard]] Bytes encode_enabled_features();
[[nodiscard]] Bytes encode_enabled_features(std::span<const std::string> features);
[[nodiscard]] Bytes encode_select_known_packs();
[[nodiscard]] std::vector<KnownPack> decode_selected_known_packs(Reader& packet);
[[nodiscard]] std::vector<RegistryTags> load_normalized_tags(std::istream& input);
[[nodiscard]] std::vector<RegistryData> load_normalized_registry_data(
	std::istream& input);
[[nodiscard]] std::vector<Bytes> load_registry_fallback_packets(std::istream& input);
[[nodiscard]] Bytes encode_registry_data(const RegistryData& registry);
[[nodiscard]] Bytes encode_tags(std::span<const RegistryTags> registries);
[[nodiscard]] Bytes encode_empty_tags();
[[nodiscard]] Bytes encode_custom_report_details(
	std::span<const std::pair<std::string, std::string>> details);
[[nodiscard]] Bytes encode_empty_server_links();
[[nodiscard]] Bytes encode_clear_dialog();
[[nodiscard]] Bytes encode_show_dialog(const nbt::Tag& direct_dialog);
[[nodiscard]] Bytes encode_code_of_conduct(std::string_view text);
void decode_accept_code_of_conduct(Reader& packet);

} // namespace configuration

namespace play {

[[nodiscard]] std::vector<Bytes> load_recipe_sync_packets(std::istream& input);

enum class ServerboundPacketId : std::int32_t {
	accept_teleportation = 0x00,
	attack = 0x01,
	chat_command = 0x07,
	chunk_batch_received = 0x0B,
	client_command = 0x0C,
	client_tick_end = 0x0D,
	configuration_acknowledged = 0x10,
	container_button_click = 0x11,
	container_click = 0x12,
	container_close = 0x13,
	container_slot_state_changed = 0x14,
	cookie_response = 0x15,
	keep_alive = 0x1C,
	interact = 0x1A,
	move_player_pos = 0x1E,
	move_player_pos_rot = 0x1F,
	move_player_rot = 0x20,
	move_player_status_only = 0x21,
	pick_item_from_block = 0x24,
	pick_item_from_entity = 0x25,
	ping_request = 0x26,
	place_recipe = 0x27,
	player_abilities = 0x28,
	player_action = 0x29,
	player_command = 0x2A,
	player_input = 0x2B,
	player_loaded = 0x2C,
	pong = 0x2D,
	recipe_book_change_settings = 0x2E,
	recipe_book_seen_recipe = 0x2F,
	rename_item = 0x30,
	resource_pack = 0x31,
	seen_advancements = 0x32,
	select_trade = 0x33,
	set_beacon = 0x34,
	set_carried_item = 0x35,
	set_creative_mode_slot = 0x38,
	swing = 0x3F,
	use_item_on = 0x42,
	use_item = 0x43,
};

enum class ClientboundPacketId : std::int32_t {
	add_entity = 0x01,
	animate = 0x02,
	award_stats = 0x03,
	block_changed_ack = 0x04,
	block_destruction = 0x05,
	block_event = 0x07,
	block_update = 0x08,
	change_difficulty = 0x0A,
	chunk_batch_finished = 0x0B,
	chunk_batch_start = 0x0C,
	clear_titles = 0x0E,
	container_close = 0x11,
	container_set_content = 0x12,
	container_set_data = 0x13,
	container_set_slot = 0x14,
	cookie_request = 0x15,
	custom_payload = 0x18,
	disconnect = 0x20,
	entity_event = 0x22,
	entity_position_sync = 0x23,
	forget_level_chunk = 0x25,
	game_event = 0x26,
	game_rule_values = 0x27,
	game_test_highlight_pos = 0x28,
	hurt_animation = 0x2A,
	initialize_border = 0x2B,
	keep_alive = 0x2C,
	level_chunk_with_light = 0x2D,
	level_event = 0x2E,
	level_particles = 0x2F,
	login = 0x31,
	move_entity_pos = 0x35,
	move_entity_pos_rot = 0x36,
	move_entity_rot = 0x38,
	open_book = 0x3A,
	open_screen = 0x3B,
	open_sign_editor = 0x3C,
	ping = 0x3D,
	pong_response = 0x3E,
	player_abilities = 0x40,
	player_info_remove = 0x45,
	player_info_update = 0x46,
	player_position = 0x48,
	recipe_book_remove = 0x4B,
	recipe_book_settings = 0x4C,
	player_rotation = 0x49,
	remove_entities = 0x4D,
	remove_mob_effect = 0x4E,
	reset_score = 0x4F,
	resource_pack_pop = 0x50,
	resource_pack_push = 0x51,
	respawn = 0x52,
	rotate_head = 0x53,
	select_advancements_tab = 0x55,
	set_action_bar_text = 0x57,
	set_border_center = 0x58,
	set_border_lerp_size = 0x59,
	set_border_size = 0x5A,
	set_border_warning_delay = 0x5B,
	set_border_warning_distance = 0x5C,
	set_chunk_cache_center = 0x5E,
	set_chunk_cache_radius = 0x5F,
	set_cursor_item = 0x60,
	set_default_spawn_position = 0x61,
	set_display_objective = 0x62,
	set_entity_data = 0x63,
	set_entity_link = 0x64,
	set_entity_motion = 0x65,
	set_experience = 0x67,
	set_health = 0x68,
	set_held_slot = 0x69,
	set_objective = 0x6A,
	set_passengers = 0x6B,
	set_player_inventory = 0x6C,
	set_player_team = 0x6D,
	set_score = 0x6E,
	set_subtitle_text = 0x70,
	set_time = 0x71,
	set_title_text = 0x72,
	set_titles_animation = 0x73,
	sound_entity = 0x74,
	sound = 0x75,
	stop_sound = 0x77,
	system_chat = 0x79,
	tab_list = 0x7A,
	take_item_entity = 0x7C,
	ticking_state = 0x7F,
	ticking_step = 0x80,
	update_advancements = 0x82,
	update_attributes = 0x83,
	update_mob_effect = 0x84,
	custom_report_details = 0x88,
	server_links = 0x89,
	projectile_power = 0x87,
};

struct EntityVector final {
	double x;
	double y;
	double z;
};

struct SimpleItemStack final {
	std::int32_t item_id{0};
	std::int32_t count{0};

	[[nodiscard]] bool empty() const noexcept { return count == 0; }
};

struct StatisticEntry final {
	std::int32_t type_id;
	std::int32_t value_id;
	std::int32_t value;
};

struct ContainerSlotStateChange final {
	std::int32_t slot;
	std::int32_t container;
	bool state;
};

struct CreativeSlotChange final {
	std::int16_t slot;
	SimpleItemStack item;
};

enum class ContainerInput : std::uint8_t {
	pickup,
	quick_move,
	swap,
	clone,
	throw_item,
	quick_craft,
	pickup_all,
};

struct ContainerClick final {
	std::int32_t container_id;
	std::int32_t state_id;
	std::int16_t slot;
	std::int8_t button;
	ContainerInput input;
	std::vector<std::pair<std::int16_t, SimpleItemStack>> changed_slots;
	SimpleItemStack carried_item;
};

enum class ObjectiveMethod : std::uint8_t {
	add,
	remove,
	change,
};

enum class ObjectiveRenderType : std::uint8_t {
	integer,
	hearts,
};

struct TeamParameters final {
	std::string display_name;
	std::string prefix;
	std::string suffix;
	std::uint8_t name_tag_visibility{0};
	std::uint8_t collision_rule{0};
	std::uint8_t options{0};
};

struct AttributeModifier final {
	std::string id;
	double amount;
	std::uint8_t operation;
};

struct AttributeSnapshot final {
	std::int32_t attribute_id;
	double base;
	std::vector<AttributeModifier> modifiers;
};

struct BeaconSelection final {
	std::optional<std::int32_t> primary;
	std::optional<std::int32_t> secondary;
};

struct PlaceRecipe final {
	std::int32_t container_id;
	std::int32_t display_id;
	bool use_max_items;
};

struct RecipeBookSettingChange final {
	std::uint8_t book_type;
	bool open;
	bool filtering;
};

struct ParticleOptions final {
	std::int32_t type_id;
	Bytes data;
};

struct SeenAdvancements final {
	bool opened_tab;
	std::optional<std::string> tab;
};

using EntityMetadataValue = std::variant<
	std::uint8_t, bool, std::int32_t, float, std::string, SimpleItemStack>;

struct EntityMetadataEntry final {
	std::uint8_t index;
	EntityMetadataValue value;
};

struct EntitySpawn final {
	std::int32_t id;
	Uuid uuid;
	std::int32_t type;
	EntityVector position;
	EntityVector movement;
	float pitch;
	float yaw;
	float head_yaw;
	std::int32_t data{0};
};

struct EntityInteraction final {
	std::int32_t entity_id;
	std::uint8_t hand;
	EntityVector location;
	bool secondary_action;
};

struct PlayerPosition final {
	double x;
	double y;
	double z;
	std::optional<float> yaw;
	std::optional<float> pitch;
	bool on_ground;
	bool horizontal_collision;
};

struct PlayerRotation final {
	float yaw;
	float pitch;
	bool on_ground;
	bool horizontal_collision;
};

enum class PlayerActionType : std::uint8_t {
	start_destroy_block,
	abort_destroy_block,
	stop_destroy_block,
	drop_all_items,
	drop_item,
	release_use_item,
	swap_item_with_offhand,
	stab,
};

struct PlayerAction final {
	PlayerActionType action;
	BlockPosition position;
	std::uint8_t direction;
	std::int32_t sequence;
};

enum class PlayerCommandAction : std::uint8_t {
	stop_sleeping,
	start_sprinting,
	stop_sprinting,
	start_riding_jump,
	stop_riding_jump,
	open_inventory,
	start_fall_flying,
};

struct PlayerCommand final {
	std::int32_t entity_id;
	PlayerCommandAction action;
	std::int32_t data;
};

struct PlayerInput final {
	bool forward;
	bool backward;
	bool left;
	bool right;
	bool jump;
	bool shift;
	bool sprint;
};

enum class ClientCommandAction : std::uint8_t {
	perform_respawn,
	request_stats,
	request_gamerule_values,
};

struct BlockHit final {
	BlockPosition position;
	std::uint8_t direction;
	float offset_x;
	float offset_y;
	float offset_z;
	bool inside;
	bool world_border_hit;
};

struct UseItemOn final {
	std::uint8_t hand;
	BlockHit hit;
	std::int32_t sequence;
};

struct UseItem final {
	std::uint8_t hand;
	std::int32_t sequence;
	float yaw;
	float pitch;
};

struct PickItemFromBlock final {
	BlockPosition position;
	bool include_data;
};

struct PickItemFromEntity final {
	std::int32_t entity_id;
	bool include_data;
};

[[nodiscard]] Bytes encode_login(bool hardcore = false);
[[nodiscard]] Bytes encode_award_stats(std::span<const StatisticEntry> statistics);
[[nodiscard]] Bytes encode_keep_alive(std::int64_t id);
[[nodiscard]] std::int64_t decode_keep_alive(Reader& packet);
[[nodiscard]] std::int32_t decode_attack(Reader& packet);
[[nodiscard]] std::string decode_chat_command(Reader& packet);
[[nodiscard]] EntityInteraction decode_interact(Reader& packet);
[[nodiscard]] Bytes encode_player_position(std::int32_t teleport_id,
										   double x,
										   double y,
										   double z,
										   float yaw,
										   float pitch);
[[nodiscard]] Bytes encode_respawn(bool keep_all_data = false,
								 std::uint8_t game_mode = 0,
								 std::int8_t previous_game_mode = -1);
[[nodiscard]] std::int32_t decode_teleport_acknowledgement(Reader& packet);
[[nodiscard]] float decode_chunk_batch_received(Reader& packet);
[[nodiscard]] PlayerPosition decode_player_position(Reader& packet);
[[nodiscard]] PlayerRotation decode_player_rotation(Reader& packet);
[[nodiscard]] std::pair<bool, bool> decode_player_status(Reader& packet);
void decode_client_tick_end(Reader& packet);
void decode_configuration_acknowledged(Reader& packet);
[[nodiscard]] CookieResponse decode_cookie_response(Reader& packet);
void decode_player_loaded(Reader& packet);
[[nodiscard]] std::int64_t decode_ping_request(Reader& packet);
[[nodiscard]] PickItemFromBlock decode_pick_item_from_block(Reader& packet);
[[nodiscard]] PickItemFromEntity decode_pick_item_from_entity(Reader& packet);
[[nodiscard]] bool decode_player_abilities(Reader& packet);
[[nodiscard]] PlayerAction decode_player_action(Reader& packet);
[[nodiscard]] PlayerCommand decode_player_command(Reader& packet);
[[nodiscard]] PlayerInput decode_player_input(Reader& packet);
[[nodiscard]] ClientCommandAction decode_client_command(Reader& packet);
[[nodiscard]] std::int32_t decode_pong(Reader& packet);
[[nodiscard]] std::string decode_rename_item(Reader& packet);
[[nodiscard]] configuration::ResourcePackResponse decode_resource_pack_response(Reader& packet);
[[nodiscard]] std::int32_t decode_select_trade(Reader& packet);
[[nodiscard]] std::pair<std::int32_t, std::int32_t> decode_container_button_click(
	Reader& packet);
[[nodiscard]] ContainerClick decode_container_click(Reader& packet);
[[nodiscard]] std::int32_t decode_container_close(Reader& packet);
[[nodiscard]] ContainerSlotStateChange decode_container_slot_state_change(Reader& packet);
[[nodiscard]] BeaconSelection decode_set_beacon(Reader& packet);
[[nodiscard]] PlaceRecipe decode_place_recipe(Reader& packet);
[[nodiscard]] RecipeBookSettingChange decode_recipe_book_setting_change(Reader& packet);
[[nodiscard]] std::int32_t decode_recipe_book_seen(Reader& packet);
[[nodiscard]] SeenAdvancements decode_seen_advancements(Reader& packet);
[[nodiscard]] std::int16_t decode_set_carried_item(Reader& packet);
[[nodiscard]] CreativeSlotChange decode_set_creative_mode_slot(Reader& packet);
[[nodiscard]] std::uint8_t decode_swing(Reader& packet);
[[nodiscard]] UseItemOn decode_use_item_on(Reader& packet);
[[nodiscard]] UseItem decode_use_item(Reader& packet);
[[nodiscard]] Bytes encode_initialize_border(double center_x,
										  double center_z,
										  double old_size,
										  double new_size,
										  std::int64_t lerp_time,
										  std::int32_t max_size,
										  std::int32_t warning_blocks,
										  std::int32_t warning_time);
[[nodiscard]] Bytes encode_player_abilities(bool invulnerable,
										 bool flying,
										 bool can_fly,
										 bool instant_build,
										 float flying_speed,
										 float walking_speed);
[[nodiscard]] Bytes encode_border_center(double x, double z);
[[nodiscard]] Bytes encode_border_lerp_size(double old_size,
										 double new_size,
										 std::int64_t lerp_time);
[[nodiscard]] Bytes encode_border_size(double size);
[[nodiscard]] Bytes encode_border_warning_delay(std::int32_t warning_time);
[[nodiscard]] Bytes encode_border_warning_distance(std::int32_t warning_blocks);
[[nodiscard]] Bytes encode_chunk_cache_radius(std::int32_t radius);
[[nodiscard]] Bytes encode_experience(float progress,
									  std::int32_t total,
									  std::int32_t level);
[[nodiscard]] Bytes encode_health(float health, std::int32_t food, float saturation);
[[nodiscard]] Bytes encode_set_time(std::int64_t game_time,
									 std::int64_t day_time,
									 float rate = 1.0F);
[[nodiscard]] Bytes encode_held_slot(std::int32_t slot);
[[nodiscard]] Bytes encode_clear_titles(bool reset_times);
[[nodiscard]] Bytes encode_block_changed_ack(std::int32_t sequence);
[[nodiscard]] Bytes encode_block_destruction(std::int32_t breaker_id,
											  BlockPosition position,
											  std::int32_t progress);
[[nodiscard]] Bytes encode_block_event(BlockPosition position,
										std::uint8_t first,
										std::uint8_t second,
										std::int32_t block_id);
[[nodiscard]] Bytes encode_block_update(BlockPosition position,
										 std::int32_t block_state_id);
[[nodiscard]] std::int32_t protocol_block_state_id(world::BlockId block);
[[nodiscard]] Bytes encode_paletted_container(std::span<const std::int32_t> values,
											 std::uint8_t minimum_bits,
											 std::uint8_t maximum_local_bits,
											 std::uint8_t global_bits);
[[nodiscard]] Bytes encode_game_rule_values(
	std::span<const std::pair<std::string, std::string>> values);
[[nodiscard]] Bytes encode_game_event(std::uint8_t event, float parameter);
[[nodiscard]] Bytes encode_level_event(std::int32_t type,
									  BlockPosition position,
									  std::int32_t data,
									  bool global_event);
[[nodiscard]] Bytes encode_level_particles(ParticleOptions particle,
										 bool override_limiter,
										 bool always_show,
										 EntityVector position,
										 float offset_x,
										 float offset_y,
										 float offset_z,
										 float max_speed,
										 std::int32_t count);
[[nodiscard]] Bytes encode_hurt_animation(std::int32_t entity_id, float yaw);
[[nodiscard]] Bytes encode_change_difficulty(std::uint8_t difficulty, bool locked);
[[nodiscard]] Bytes encode_game_test_highlight(BlockPosition absolute,
											   BlockPosition relative);
[[nodiscard]] Bytes encode_ping(std::int32_t id);
[[nodiscard]] Bytes encode_player_rotation(float yaw,
										 bool relative_yaw,
										 float pitch,
										 bool relative_pitch);
[[nodiscard]] Bytes encode_select_advancements_tab(
	std::optional<std::string_view> tab);
[[nodiscard]] Bytes encode_ticking_state(float tick_rate, bool frozen);
[[nodiscard]] Bytes encode_ticking_step(std::int32_t steps);
[[nodiscard]] Bytes encode_projectile_power(std::int32_t entity_id, double power);
[[nodiscard]] Bytes encode_sound_entity(std::int32_t sound_id,
										std::uint8_t source,
										std::int32_t entity_id,
										float volume,
										float pitch,
										std::int64_t seed);
[[nodiscard]] Bytes encode_sound(std::int32_t sound_id,
								std::uint8_t source,
								EntityVector position,
								float volume,
								float pitch,
								std::int64_t seed);
[[nodiscard]] Bytes encode_stop_sound(std::optional<std::uint8_t> source,
									 std::optional<std::string_view> sound);
[[nodiscard]] Bytes encode_add_entity(const EntitySpawn& entity);
[[nodiscard]] Bytes encode_animate(std::int32_t entity_id, std::uint8_t action);
[[nodiscard]] Bytes encode_entity_event(std::int32_t entity_id, std::int8_t event_id);
[[nodiscard]] Bytes encode_entity_position_sync(std::int32_t entity_id,
												 EntityVector position,
												 EntityVector movement,
												 float yaw,
												 float pitch,
												 bool on_ground);
[[nodiscard]] Bytes encode_move_entity_position(std::int32_t entity_id,
											 EntityVector delta,
											 bool on_ground);
[[nodiscard]] Bytes encode_move_entity_position_rotation(std::int32_t entity_id,
												  EntityVector delta,
												  float yaw,
												  float pitch,
												  bool on_ground);
[[nodiscard]] Bytes encode_move_entity_rotation(std::int32_t entity_id,
											 float yaw,
											 float pitch,
											 bool on_ground);
[[nodiscard]] Bytes encode_remove_entities(std::span<const std::int32_t> entity_ids);
[[nodiscard]] Bytes encode_rotate_head(std::int32_t entity_id, float head_yaw);
[[nodiscard]] Bytes encode_entity_link(std::int32_t source_id, std::int32_t destination_id);
[[nodiscard]] Bytes encode_entity_motion(std::int32_t entity_id, EntityVector movement);
[[nodiscard]] Bytes encode_passengers(std::int32_t vehicle_id,
										  std::span<const std::int32_t> passengers);
[[nodiscard]] Bytes encode_take_item_entity(std::int32_t item_id,
											 std::int32_t player_id,
											 std::int32_t amount);
[[nodiscard]] Bytes encode_cookie_request(std::string_view key);
[[nodiscard]] Bytes encode_custom_payload(std::string_view channel,
										  std::span<const std::uint8_t> data);
[[nodiscard]] Bytes encode_disconnect_text(std::string_view text);
[[nodiscard]] Bytes encode_pong_response(std::int64_t time);
[[nodiscard]] Bytes encode_resource_pack_pop(std::optional<Uuid> id);
[[nodiscard]] Bytes encode_resource_pack_push(Uuid id,
										   std::string_view url,
										   std::string_view hash,
										   bool required);
[[nodiscard]] Bytes encode_action_bar_text(std::string_view text);
[[nodiscard]] Bytes encode_subtitle_text(std::string_view text);
[[nodiscard]] Bytes encode_title_text(std::string_view text);
[[nodiscard]] Bytes encode_titles_animation(std::int32_t fade_in,
										 std::int32_t stay,
										 std::int32_t fade_out);
[[nodiscard]] Bytes encode_system_chat(std::string_view text, bool overlay);
[[nodiscard]] Bytes encode_tab_list(std::string_view header, std::string_view footer);
[[nodiscard]] Bytes encode_custom_report_details(
	std::span<const std::pair<std::string, std::string>> details);
[[nodiscard]] Bytes encode_empty_server_links();
[[nodiscard]] Bytes encode_container_close(std::int32_t container_id);
[[nodiscard]] Bytes encode_container_content(std::int32_t container_id,
										  std::int32_t state_id,
										  std::span<const SimpleItemStack> items,
										  SimpleItemStack carried);
[[nodiscard]] Bytes encode_container_data(std::int32_t container_id,
										std::int16_t data_id,
										std::int16_t value);
[[nodiscard]] Bytes encode_container_slot(std::int32_t container_id,
										std::int32_t state_id,
										std::int16_t slot,
										SimpleItemStack item);
[[nodiscard]] Bytes encode_open_book(std::uint8_t hand);
[[nodiscard]] Bytes encode_open_screen(std::int32_t container_id,
									  std::int32_t menu_type,
									  std::string_view title);
[[nodiscard]] Bytes encode_open_sign_editor(BlockPosition position, bool front_text);
[[nodiscard]] Bytes encode_cursor_item(SimpleItemStack item);
[[nodiscard]] Bytes encode_player_inventory(std::int32_t slot, SimpleItemStack item);
[[nodiscard]] Bytes encode_player_info_remove(std::span<const Uuid> profile_ids);
[[nodiscard]] Bytes encode_player_info_initialize(const login::GameProfile& profile,
												   std::uint8_t game_mode,
												   bool listed,
												   std::int32_t latency,
												   std::int32_t list_order,
												   bool show_hat);
[[nodiscard]] Bytes encode_reset_score(std::string_view owner,
									  std::optional<std::string_view> objective);
[[nodiscard]] Bytes encode_display_objective(std::uint8_t slot,
										std::string_view objective);
[[nodiscard]] Bytes encode_objective(std::string_view name,
									 ObjectiveMethod method,
									 std::string_view display_name = {},
									 ObjectiveRenderType render_type = ObjectiveRenderType::integer);
[[nodiscard]] Bytes encode_score(std::string_view owner,
								 std::string_view objective,
								 std::int32_t score,
								 std::optional<std::string_view> display = std::nullopt);
[[nodiscard]] Bytes encode_team(std::string_view name,
								std::uint8_t method,
								std::optional<TeamParameters> parameters,
								std::span<const std::string> players);
[[nodiscard]] Bytes encode_update_attributes(
	std::int32_t entity_id, std::span<const AttributeSnapshot> attributes);
[[nodiscard]] Bytes encode_update_mob_effect(std::int32_t entity_id,
											std::int32_t effect_id,
											std::int32_t amplifier,
											std::int32_t duration_ticks,
											bool ambient,
											bool visible,
											bool show_icon,
											bool blend);
[[nodiscard]] Bytes encode_remove_mob_effect(std::int32_t entity_id,
											std::int32_t effect_id);
[[nodiscard]] Bytes encode_entity_metadata(
	std::int32_t entity_id, std::span<const EntityMetadataEntry> entries);
[[nodiscard]] Bytes encode_recipe_book_remove(std::span<const std::int32_t> display_ids);
[[nodiscard]] Bytes encode_recipe_book_settings(
	const std::array<bool, 8>& open_filtering_pairs);
[[nodiscard]] Bytes encode_empty_advancements(bool reset, bool show_advancements);
[[nodiscard]] Bytes encode_chunk_cache_center(std::int32_t chunk_x,
										   std::int32_t chunk_z);
[[nodiscard]] Bytes encode_forget_level_chunk(std::int32_t chunk_x,
										   std::int32_t chunk_z);
[[nodiscard]] Bytes encode_default_spawn_position(BlockPosition position,
														 float yaw = 0.0F,
														 float pitch = 0.0F);
[[nodiscard]] Bytes encode_chunk_batch_start();
[[nodiscard]] Bytes encode_chunk_batch_finished(std::int32_t batch_size);
[[nodiscard]] Bytes encode_level_chunks_load_start();
[[nodiscard]] Bytes encode_level_chunk(const world::Chunk& chunk);

} // namespace play

} // namespace mc::protocol