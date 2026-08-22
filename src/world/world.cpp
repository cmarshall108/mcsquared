#include "mc/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mc::world {
namespace {

struct PositionHash final {
    [[nodiscard]] std::size_t operator()(const ChunkPosition position) const noexcept {
        const auto x = static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.x));
        const auto z = static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.z));
        auto value = (x << 32U) | z;
        value ^= value >> 30U;
        value *= 0xBF58476D1CE4E5B9ULL;
        value ^= value >> 27U;
        value *= 0x94D049BB133111EBULL;
        value ^= value >> 31U;
        return static_cast<std::size_t>(value);
    }
};

[[nodiscard]] std::int32_t floor_div(const std::int32_t value,
                                     const std::int32_t divisor) noexcept {
    const auto quotient = value / divisor;
    const auto remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

[[nodiscard]] std::size_t local_coordinate(const std::int32_t value) noexcept {
    const auto remainder = value % chunk_width;
    return static_cast<std::size_t>(remainder < 0 ? remainder + chunk_width : remainder);
}

template <typename Value>
[[nodiscard]] Value parse_state_value(const std::map<std::string, std::string>& state,
                                      const std::string_view name,
                                      const Value fallback) {
    const auto found = state.find(std::string(name));
    if (found == state.end()) return fallback;
    std::istringstream input(found->second);
    Value value{};
    std::string trailing;
    if (!(input >> value) || (input >> trailing)) {
        throw std::runtime_error("world metadata state value is invalid");
    }
    return value;
}

template <typename Value>
[[nodiscard]] std::string state_string(const Value value) {
    std::ostringstream output;
    output.precision(std::numeric_limits<double>::max_digits10);
    output << value;
    return output.str();
}

} // namespace

class ChunkManager::Impl final {
public:
    Impl(ChunkGenerator generator,
         const std::size_t max_loaded_chunks,
         std::shared_ptr<LevelStorage> storage)
        : generator_(std::move(generator)),
          max_loaded_chunks_(max_loaded_chunks),
          storage_(std::move(storage)) {
        if (max_loaded_chunks_ == 0) {
            throw std::invalid_argument("maximum loaded chunk count must be positive");
        }
    }

    ~Impl() {
        try {
            static_cast<void>(save_dirty());
        } catch (...) {
        }
    }

    std::shared_ptr<Chunk> load(const ChunkPosition position) {
        std::lock_guard lock(mutex_);
        if (const auto existing = chunks_.find(position); existing != chunks_.end()) {
            existing->second.last_access = ++access_counter_;
            return existing->second.chunk;
        }

        auto loaded = storage_ ? storage_->load_chunk(position) : std::nullopt;
        auto chunk = std::make_shared<Chunk>(
            loaded ? std::move(*loaded) : generator_.generate(position));
        if (storage_ && !loaded) {
            storage_->save_chunk(*chunk);
        }
        chunk->mark_saved();
        chunks_.emplace(position, Entry{chunk, ++access_counter_});
        evict_to_limit();
        return chunk;
    }

    bool unload(const ChunkPosition position) {
        std::lock_guard lock(mutex_);
        const auto iterator = chunks_.find(position);
        if (iterator == chunks_.end() || iterator->second.chunk.use_count() != 1) {
            return false;
        }
        save(iterator->second.chunk);
        chunks_.erase(iterator);
        return true;
    }

    void unload_unused() {
        std::lock_guard lock(mutex_);
        evict_to_limit();
    }

    std::size_t save_dirty() {
        std::lock_guard lock(mutex_);
        if (!storage_) return 0;
        std::size_t saved = 0;
        for (auto& [position, entry] : chunks_) {
            static_cast<void>(position);
            if (entry.chunk->dirty()) {
                save(entry.chunk);
                ++saved;
            }
        }
        return saved;
    }

    std::size_t loaded_count() const {
        std::lock_guard lock(mutex_);
        return chunks_.size();
    }

private:
    struct Entry final {
        std::shared_ptr<Chunk> chunk;
        std::uint64_t last_access;
    };

    void save(const std::shared_ptr<Chunk>& chunk) {
        if (storage_ && chunk->dirty()) {
            storage_->save_chunk(*chunk);
            chunk->mark_saved();
        }
    }

    void evict_to_limit() {
        while (chunks_.size() > max_loaded_chunks_) {
            auto oldest = chunks_.end();
            auto oldest_access = std::numeric_limits<std::uint64_t>::max();
            for (auto iterator = chunks_.begin(); iterator != chunks_.end(); ++iterator) {
                if (iterator->second.chunk.use_count() == 1 &&
                    iterator->second.last_access < oldest_access) {
                    oldest = iterator;
                    oldest_access = iterator->second.last_access;
                }
            }
            if (oldest == chunks_.end()) {
                return;
            }
            save(oldest->second.chunk);
            chunks_.erase(oldest);
        }
    }

    ChunkGenerator generator_;
    std::size_t max_loaded_chunks_;
    std::shared_ptr<LevelStorage> storage_;
    mutable std::mutex mutex_;
    std::unordered_map<ChunkPosition, Entry, PositionHash> chunks_;
    std::uint64_t access_counter_{0};
};

ChunkManager::ChunkManager(ChunkGenerator generator,
                           const std::size_t max_loaded_chunks,
                           std::shared_ptr<LevelStorage> storage)
    : impl_(std::make_unique<Impl>(
          std::move(generator), max_loaded_chunks, std::move(storage))) {}
ChunkManager::~ChunkManager() = default;

std::shared_ptr<Chunk> ChunkManager::load(const ChunkPosition position) {
    return impl_->load(position);
}

bool ChunkManager::unload(const ChunkPosition position) {
    return impl_->unload(position);
}

void ChunkManager::unload_unused() {
    impl_->unload_unused();
}

std::size_t ChunkManager::save_dirty() {
    return impl_->save_dirty();
}

std::size_t ChunkManager::loaded_count() const {
    return impl_->loaded_count();
}

WorldSettings World::load_persisted_settings(WorldSettings settings) {
    if (!settings.storage_path) return settings;
    LevelStorage storage(*settings.storage_path);
    const auto metadata = storage.load_metadata();
    if (!metadata) return settings;
    settings.generation.seed = metadata->seed;
    const auto& state = metadata->world_state;
    settings.game_time = parse_state_value(state, "game_time", settings.game_time);
    settings.day_time = parse_state_value(state, "day_time", settings.day_time);
    settings.advance_time = parse_state_value(state, "advance_time", settings.advance_time);
    settings.weather_duration = parse_state_value(
        state, "weather_duration", settings.weather_duration);
    settings.raining = parse_state_value(state, "raining", settings.raining);
    settings.thundering = parse_state_value(state, "thundering", settings.thundering);
    settings.weather_cycle = parse_state_value(
        state, "weather_cycle", settings.weather_cycle);
    const auto difficulty = parse_state_value(
        state, "difficulty", static_cast<std::uint32_t>(settings.difficulty));
    if (difficulty > 3) {
        throw std::runtime_error("world metadata difficulty is invalid");
    }
    settings.difficulty = static_cast<std::uint8_t>(difficulty);
    settings.rain_level = parse_state_value(state, "rain_level", settings.rain_level);
    settings.thunder_level = parse_state_value(
        state, "thunder_level", settings.thunder_level);
    return settings;
}

World::World(WorldSettings settings)
    : World(load_persisted_settings(std::move(settings)), nullptr) {}

World::World(WorldSettings settings, std::nullptr_t)
    : storage_(settings.storage_path
          ? std::make_shared<LevelStorage>(*settings.storage_path)
          : nullptr),
      chunks_(
          ChunkGenerator(settings.generation),
          settings.max_loaded_chunks,
          storage_),
      metadata_{.seed = settings.generation.seed},
      game_time_(settings.game_time), day_time_(settings.day_time),
    advance_time_(settings.advance_time), weather_seed_(settings.generation.seed),
    weather_duration_(std::max<std::uint32_t>(1, settings.weather_duration)),
    raining_(settings.raining), thundering_(settings.thundering),
    weather_cycle_(settings.weather_cycle),
    rain_level_(settings.rain_level < 0.0F
        ? (settings.raining ? 1.0F : 0.0F)
        : std::clamp(settings.rain_level, 0.0F, 1.0F)),
    thunder_level_(settings.thunder_level < 0.0F
        ? (settings.thundering ? 1.0F : 0.0F)
        : std::clamp(settings.thunder_level, 0.0F, 1.0F)),
    difficulty_(settings.difficulty) {
    if (storage_) {
        if (const auto persisted = storage_->load_metadata()) {
            has_persisted_state_ = true;
            metadata_ = *persisted;
            game_rules_ = metadata_.game_rules;
            const auto& state = metadata_.world_state;
            border_.center_x = parse_state_value(
                state, "border_center_x", border_.center_x);
            border_.center_z = parse_state_value(
                state, "border_center_z", border_.center_z);
            border_.size = parse_state_value(state, "border_size", border_.size);
            border_.lerp_start_size = parse_state_value(
                state, "border_lerp_start_size", border_.lerp_start_size);
            border_.lerp_target_size = parse_state_value(
                state, "border_lerp_target_size", border_.lerp_target_size);
            border_.lerp_total_ticks = parse_state_value(
                state, "border_lerp_total_ticks", border_.lerp_total_ticks);
            border_.lerp_remaining_ticks = parse_state_value(
                state, "border_lerp_remaining_ticks", border_.lerp_remaining_ticks);
            border_.warning_distance = parse_state_value(
                state, "border_warning_distance", border_.warning_distance);
            border_.warning_time = parse_state_value(
                state, "border_warning_time", border_.warning_time);
        }
    }
}

World::~World() {
    try {
        static_cast<void>(save_all());
    } catch (...) {
    }
}

std::shared_ptr<Chunk> World::chunk(const ChunkPosition position) {
    return chunks_.load(position);
}

BlockId World::block(const mc::core::BlockPosition position) {
    if (position.y < min_build_y || position.y >= max_build_y) {
        throw std::out_of_range("global block Y is outside build height");
    }
    const auto value = chunk({floor_div(position.x, chunk_width),
                              floor_div(position.z, chunk_width)});
    return value->block(local_coordinate(position.x), position.y,
                        local_coordinate(position.z));
}

void World::set_block(const mc::core::BlockPosition position, const BlockId block_id) {
    if (position.y < min_build_y || position.y >= max_build_y) {
        throw std::out_of_range("global block Y is outside build height");
    }
    const auto value = chunk({floor_div(position.x, chunk_width),
                              floor_div(position.z, chunk_width)});
    value->set_block(local_coordinate(position.x), position.y,
                     local_coordinate(position.z), block_id);
}

std::int32_t World::surface_height(const std::int32_t x, const std::int32_t z) {
    const auto value = chunk({floor_div(x, chunk_width), floor_div(z, chunk_width)});
    return value->height(local_coordinate(x), local_coordinate(z));
}

BiomeId World::biome(const std::int32_t x, const std::int32_t y, const std::int32_t z) {
    const auto value = chunk({floor_div(x, chunk_width), floor_div(z, chunk_width)});
    const auto clamped_y = std::clamp(y, min_build_y, max_build_y - 1);
    return value->biome(
        local_coordinate(x) / 4,
        static_cast<std::size_t>(clamped_y - min_build_y) / 4,
        local_coordinate(z) / 4);
}

bool World::solid(const mc::core::BlockPosition position) {
    const auto value = block(position);
    return value != BlockId::air && value != BlockId::water &&
        value != BlockId::short_grass && value != BlockId::dandelion &&
        value != BlockId::poppy;
}

bool World::line_of_sight(const WorldPoint from,
                          const WorldPoint to,
                          const std::optional<mc::core::BlockPosition> allowed) {
    const auto delta_x = to.x - from.x;
    const auto delta_y = to.y - from.y;
    const auto delta_z = to.z - from.z;
    const auto distance = std::sqrt(
        delta_x * delta_x + delta_y * delta_y + delta_z * delta_z);
    if (!std::isfinite(distance) || distance > 128.0) return false;
    const auto steps = std::max(1, static_cast<int>(std::ceil(distance * 8.0)));
    for (int step = 0; step <= steps; ++step) {
        const auto scale = static_cast<double>(step) / static_cast<double>(steps);
        const mc::core::BlockPosition position{
            static_cast<std::int32_t>(std::floor(from.x + delta_x * scale)),
            static_cast<std::int32_t>(std::floor(from.y + delta_y * scale)),
            static_cast<std::int32_t>(std::floor(from.z + delta_z * scale))};
        if (allowed && position == *allowed) continue;
        if (position.y < min_build_y || position.y >= max_build_y || solid(position)) {
            return false;
        }
    }
    return true;
}

bool World::collides(const WorldPoint minimum, const WorldPoint maximum) {
    if (minimum.x >= maximum.x || minimum.y >= maximum.y || minimum.z >= maximum.z) {
        return false;
    }
    const auto min_x = static_cast<std::int32_t>(std::floor(minimum.x));
    const auto min_y = static_cast<std::int32_t>(std::floor(minimum.y));
    const auto min_z = static_cast<std::int32_t>(std::floor(minimum.z));
    const auto max_x = static_cast<std::int32_t>(std::floor(maximum.x - 1.0e-7));
    const auto max_y = static_cast<std::int32_t>(std::floor(maximum.y - 1.0e-7));
    const auto max_z = static_cast<std::int32_t>(std::floor(maximum.z - 1.0e-7));
    for (auto y = min_y; y <= max_y; ++y) {
        for (auto z = min_z; z <= max_z; ++z) {
            for (auto x = min_x; x <= max_x; ++x) {
                if (y < min_build_y || y >= max_build_y || solid({x, y, z})) return true;
            }
        }
    }
    return false;
}

std::vector<mc::core::BlockPosition> World::settle_falling_column(
    const mc::core::BlockPosition position) {
    std::vector<mc::core::BlockPosition> changed;
    for (auto y = std::max(position.y, min_build_y + 1); y < max_build_y; ++y) {
        const mc::core::BlockPosition source{position.x, y, position.z};
        const auto source_block = block(source);
        if (source_block != BlockId::sand && source_block != BlockId::gravel) continue;

        auto destination_y = y;
        while (destination_y > min_build_y &&
               block({position.x, destination_y - 1, position.z}) == BlockId::air) {
            --destination_y;
        }
        if (destination_y == y) continue;
        set_block(source, BlockId::air);
        const mc::core::BlockPosition destination{position.x, destination_y, position.z};
        set_block(destination, source_block);
        changed.push_back(source);
        changed.push_back(destination);
    }
    return changed;
}

std::size_t World::save_dirty() {
    return chunks_.save_dirty();
}

std::size_t World::save_all() {
    const auto saved = chunks_.save_dirty();
    if (!storage_) return saved;
    metadata_.game_rules = game_rules_;
    metadata_.world_state = {
        {"game_time", state_string(game_time_)},
        {"day_time", state_string(day_time_)},
        {"advance_time", state_string(advance_time_)},
        {"weather_duration", state_string(weather_duration_)},
        {"raining", state_string(raining_)},
        {"thundering", state_string(thundering_)},
        {"weather_cycle", state_string(weather_cycle_)},
        {"rain_level", state_string(rain_level_)},
        {"thunder_level", state_string(thunder_level_)},
        {"difficulty", state_string(static_cast<std::uint32_t>(difficulty_))},
        {"border_center_x", state_string(border_.center_x)},
        {"border_center_z", state_string(border_.center_z)},
        {"border_size", state_string(border_.size)},
        {"border_lerp_start_size", state_string(border_.lerp_start_size)},
        {"border_lerp_target_size", state_string(border_.lerp_target_size)},
        {"border_lerp_total_ticks", state_string(border_.lerp_total_ticks)},
        {"border_lerp_remaining_ticks", state_string(border_.lerp_remaining_ticks)},
        {"border_warning_distance", state_string(border_.warning_distance)},
        {"border_warning_time", state_string(border_.warning_time)},
    };
    storage_->save_metadata(metadata_);
    return saved;
}

void World::tick_time(const std::uint64_t ticks) noexcept {
    game_time_ += ticks;
    if (advance_time_) day_time_ += ticks;
}

std::uint64_t World::game_time() const noexcept { return game_time_; }
std::uint64_t World::day_time() const noexcept { return day_time_; }
void World::set_day_time(const std::uint64_t day_time) noexcept { day_time_ = day_time; }
void World::set_advance_time(const bool advance) noexcept { advance_time_ = advance; }

WeatherUpdate World::tick_weather(const std::uint32_t ticks) noexcept {
    WeatherUpdate update;
    if (weather_cycle_ && ticks >= weather_duration_) {
        raining_ = !raining_;
        const auto random = weather_seed_ ^ game_time_ * 0x9E3779B97F4A7C15ULL;
        thundering_ = raining_ && (random >> 32U) % 4U == 0;
        weather_duration_ = static_cast<std::uint32_t>(
            6'000 + random % 12'001U);
        update.raining = raining_;
    } else if (weather_cycle_) {
        weather_duration_ -= ticks;
    }
    const auto fade = 0.01F * static_cast<float>(ticks);
    const auto next_rain = std::clamp(
        rain_level_ + (raining_ ? fade : -fade), 0.0F, 1.0F);
    const auto next_thunder = std::clamp(
        thunder_level_ + (thundering_ ? fade : -fade), 0.0F, 1.0F);
    if (next_rain != rain_level_) {
        rain_level_ = next_rain;
        update.rain_level = rain_level_;
    }
    if (next_thunder != thunder_level_) {
        thunder_level_ = next_thunder;
        update.thunder_level = thunder_level_;
    }
    return update;
}

void World::set_weather(const bool raining,
                        const bool thundering,
                        const std::uint32_t duration) noexcept {
    raining_ = raining;
    thundering_ = raining && thundering;
    weather_duration_ = std::max<std::uint32_t>(1, duration);
}
void World::set_weather_cycle(const bool advance) noexcept { weather_cycle_ = advance; }
bool World::raining() const noexcept { return raining_; }
float World::rain_level() const noexcept { return rain_level_; }
float World::thunder_level() const noexcept { return thunder_level_; }

const WorldBorder& World::border() const noexcept { return border_; }

void World::set_border_center(const double x, const double z) {
    if (!std::isfinite(x) || !std::isfinite(z) ||
        std::abs(x) > 29'999'984.0 || std::abs(z) > 29'999'984.0) {
        throw std::invalid_argument("world border center is invalid");
    }
    border_.center_x = x;
    border_.center_z = z;
}

void World::set_border_size(const double size) {
    if (!std::isfinite(size) || size <= 0.0 || size > 59'999'968.0) {
        throw std::invalid_argument("world border size is invalid");
    }
    border_.size = size;
    border_.lerp_start_size = size;
    border_.lerp_target_size = size;
    border_.lerp_total_ticks = 0;
    border_.lerp_remaining_ticks = 0;
}

void World::set_border_lerp_size(const double size, const std::uint64_t duration_ticks) {
    if (!std::isfinite(size) || size <= 0.0 || size > 59'999'968.0 ||
        duration_ticks == 0) {
        throw std::invalid_argument("world border lerp is invalid");
    }
    border_.lerp_start_size = border_.size;
    border_.lerp_target_size = size;
    border_.lerp_total_ticks = duration_ticks;
    border_.lerp_remaining_ticks = duration_ticks;
}

void World::tick_border(const std::uint64_t ticks) noexcept {
    if (border_.lerp_remaining_ticks == 0 || ticks == 0) return;
    border_.lerp_remaining_ticks -= std::min(ticks, border_.lerp_remaining_ticks);
    const auto progress = 1.0 - static_cast<double>(border_.lerp_remaining_ticks) /
        static_cast<double>(border_.lerp_total_ticks);
    border_.size = border_.lerp_start_size +
        (border_.lerp_target_size - border_.lerp_start_size) * progress;
    if (border_.lerp_remaining_ticks == 0) {
        border_.size = border_.lerp_target_size;
        border_.lerp_start_size = border_.size;
        border_.lerp_total_ticks = 0;
    }
}

void World::set_border_warning_distance(const std::int32_t blocks) {
    if (blocks < 0) throw std::invalid_argument("border warning distance is invalid");
    border_.warning_distance = blocks;
}

void World::set_border_warning_time(const std::int32_t seconds) {
    if (seconds < 0) throw std::invalid_argument("border warning time is invalid");
    border_.warning_time = seconds;
}

bool World::inside_border(const WorldPoint point, const double margin) const noexcept {
    if (!std::isfinite(point.x) || !std::isfinite(point.z) ||
        !std::isfinite(margin) || margin < 0.0) {
        return false;
    }
    const auto radius = border_.size * 0.5 - margin;
    return radius >= 0.0 && point.x >= border_.center_x - radius &&
        point.x <= border_.center_x + radius &&
        point.z >= border_.center_z - radius && point.z <= border_.center_z + radius;
}

std::uint8_t World::difficulty() const noexcept { return difficulty_; }

void World::set_difficulty(const std::uint8_t difficulty) {
    if (difficulty > 3) throw std::invalid_argument("world difficulty is invalid");
    difficulty_ = difficulty;
}

bool World::has_persisted_state() const noexcept { return has_persisted_state_; }

mc::core::BlockPosition World::spawn() const noexcept { return metadata_.spawn; }

void World::set_spawn(const mc::core::BlockPosition spawn) {
    if (spawn.y < min_build_y || spawn.y >= max_build_y) {
        throw std::invalid_argument("world spawn is invalid");
    }
    metadata_.spawn = spawn;
}

std::optional<std::string> World::game_rule(const std::string& name) const {
    const auto found = game_rules_.find(name);
    return found == game_rules_.end()
        ? std::nullopt
        : std::optional<std::string>(found->second);
}

void World::set_game_rule(std::string name, std::string value) {
    if (name.empty() || name.size() > 128 || value.size() > 128) {
        throw std::invalid_argument("world game rule is invalid");
    }
    game_rules_[std::move(name)] = std::move(value);
}

ChunkManager& World::chunks() noexcept {
    return chunks_;
}

const ChunkManager& World::chunks() const noexcept {
    return chunks_;
}

} // namespace mc::world