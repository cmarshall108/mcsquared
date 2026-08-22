#include "mc/server/server.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] std::uint16_t parse_port(const std::string_view text) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > 65'535) {
        throw std::invalid_argument("port must be an integer from 1 to 65535");
    }
    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::size_t parse_size(const std::string_view text, const char* name) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    }
    return value;
}

} // namespace

int main(const int argc, const char* const argv[]) {
    try {
        mc::server::Config config;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--port" && index + 1 < argc) {
                config.port = parse_port(argv[++index]);
            } else if (argument == "--motd" && index + 1 < argc) {
                config.motd = argv[++index];
            } else if (argument == "--encrypted-offline") {
                config.encrypted_offline = true;
            } else if (argument == "--online-mode") {
                config.online_mode = true;
            } else if (argument == "--hardcore") {
                config.hardcore = true;
            } else if (argument == "--session-server-url" && index + 1 < argc) {
                config.session_server_url = argv[++index];
            } else if (argument == "--login-query-channel" && index + 1 < argc) {
                config.login_query_channel = argv[++index];
            } else if (argument == "--login-query-payload" && index + 1 < argc) {
                config.login_query_payload = argv[++index];
            } else if (argument == "--require-login-query-response") {
                config.require_login_query_response = true;
            } else if (argument == "--configuration-tags" && index + 1 < argc) {
                config.configuration_tags_path = argv[++index];
            } else if (argument == "--configuration-registries" && index + 1 < argc) {
                config.configuration_registries_path = argv[++index];
            } else if (argument == "--configuration-registry-fallback" && index + 1 < argc) {
                config.configuration_registry_fallback_path = argv[++index];
            } else if (argument == "--resource-pack-url" && index + 1 < argc) {
                config.resource_pack_url = argv[++index];
            } else if (argument == "--resource-pack-hash" && index + 1 < argc) {
                config.resource_pack_hash = argv[++index];
            } else if (argument == "--require-resource-pack") {
                config.require_resource_pack = true;
            } else if (argument == "--recipe-sync" && index + 1 < argc) {
                config.recipe_sync_path = argv[++index];
            } else if (argument == "--keepalive-interval-ms" && index + 1 < argc) {
                config.keep_alive_interval = std::chrono::milliseconds(
                    parse_size(argv[++index], "keepalive interval"));
            } else if (argument == "--keepalive-timeout-ms" && index + 1 < argc) {
                config.keep_alive_timeout = std::chrono::milliseconds(
                    parse_size(argv[++index], "keepalive timeout"));
            } else if (argument == "--idle-timeout-ms" && index + 1 < argc) {
                config.idle_timeout = std::chrono::milliseconds(
                    parse_size(argv[++index], "idle timeout"));
            } else if (argument == "--world" && index + 1 < argc) {
                config.world_path = argv[++index];
            } else if (argument == "--autosave-interval-ms" && index + 1 < argc) {
                config.autosave_interval = std::chrono::milliseconds(
                    parse_size(argv[++index], "autosave interval"));
            } else if (argument == "--max-connections" && index + 1 < argc) {
                config.max_active_connections = parse_size(argv[++index], "max connections");
            } else if (argument == "--max-connections-per-ip" && index + 1 < argc) {
                config.max_connections_per_ip = parse_size(argv[++index], "per-IP connections");
            } else if (argument == "--connection-rate-limit" && index + 1 < argc) {
                config.max_connection_attempts_per_second =
                    parse_size(argv[++index], "connection rate limit");
            } else {
                std::cerr << "Usage: mcsquared [--port PORT] [--motd TEXT] "
                             "[--encrypted-offline|--online-mode] [--hardcore] "
                             "[--session-server-url URL] "
                             "[--login-query-channel ID] [--login-query-payload TEXT] "
                             "[--require-login-query-response] [--configuration-tags PATH] "
                             "[--configuration-registries PATH] "
                             "[--configuration-registry-fallback PATH] "
                             "[--resource-pack-url URL] [--resource-pack-hash SHA1] "
                             "[--require-resource-pack] "
                             "[--recipe-sync PATH] "
                             "[--keepalive-interval-ms N] [--keepalive-timeout-ms N] "
                             "[--idle-timeout-ms N] "
                             "[--world PATH] [--autosave-interval-ms N] "
                             "[--max-connections N] "
                             "[--max-connections-per-ip N] "
                             "[--connection-rate-limit N]\n";
                return 2;
            }
        }

        mc::server::Server server(std::move(config));
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}