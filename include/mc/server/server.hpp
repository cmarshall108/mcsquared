#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace mc::server {

struct Config final {
    std::uint16_t port{25'565};
    std::size_t worker_count{4};
    std::size_t max_pending_connections{256};
    std::size_t max_active_connections{1024};
    std::size_t max_connections_per_ip{16};
    std::size_t max_connection_attempts_per_second{64};
    std::size_t compression_threshold{256};
    bool encrypted_offline{false};
    bool online_mode{false};
    bool hardcore{false};
    std::string session_server_url{
        "https://sessionserver.mojang.com/session/minecraft/hasJoined"};
    std::optional<std::string> login_query_channel;
    std::string login_query_payload;
    bool require_login_query_response{false};
    std::string configuration_tags_path;
    std::string configuration_registries_path;
    std::string configuration_registry_fallback_path;
    std::optional<std::string> resource_pack_url;
    std::string resource_pack_hash;
    bool require_resource_pack{false};
    std::string recipe_sync_path;
    std::chrono::milliseconds keep_alive_interval{15'000};
    std::chrono::milliseconds keep_alive_timeout{30'000};
    std::chrono::milliseconds idle_timeout{300'000};
    std::optional<std::string> world_path;
    std::chrono::milliseconds autosave_interval{300'000};
    std::string motd{"mcsquared"};
};

class Server final {
public:
    explicit Server(Config config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void run();
    void request_stop() noexcept;

private:
    class Impl;
    Impl* impl_;
};

} // namespace mc::server