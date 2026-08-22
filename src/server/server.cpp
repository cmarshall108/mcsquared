#include "mc/server/server.hpp"

#include "mc/block/block.hpp"
#include "mc/entity/ai.hpp"
#include "mc/entity/animal.hpp"
#include "mc/entity/dropped_item.hpp"
#include "mc/entity/projectile.hpp"
#include "mc/entity/spawning.hpp"
#include "mc/entity/tracking.hpp"
#include "mc/item/item.hpp"
#include "mc/player/player.hpp"
#include "mc/protocol/codec.hpp"
#include "mc/protocol/crypto.hpp"
#include "mc/protocol/packets.hpp"
#include "mc/world/world.hpp"
#include "mc/world/ticks.hpp"
#include "session_auth.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace mc::server {
namespace {

constexpr std::int32_t protocol_version = 776;
constexpr std::size_t max_packet_size = 2U * 1024U * 1024U;
constexpr std::array<std::string_view, 16> implemented_command_roots{
    "clear", "difficulty", "effect", "experience", "gamemode", "gamerule",
    "give", "kill", "say", "summon", "teleport", "time", "tp", "weather",
    "worldborder", "xp"};

#ifdef _WIN32
using NativeSocket = SOCKET;
using PollDescriptor = WSAPOLLFD;
using NativeAddressLength = int;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
constexpr short socket_read_event = POLLRDNORM;
constexpr short socket_write_event = POLLWRNORM;
#else
using NativeSocket = int;
using PollDescriptor = pollfd;
using NativeAddressLength = socklen_t;
constexpr NativeSocket invalid_socket = -1;
constexpr short socket_read_event = POLLIN;
constexpr short socket_write_event = POLLOUT;
#endif

constexpr int socket_io_timeout_ms = 10'000;
constexpr std::size_t max_pending_write_bytes = 16U * 1024U * 1024U;
constexpr std::size_t max_scatter_buffers = 16;

class NetworkRuntime final {
public:
    NetworkRuntime() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("failed to initialize Winsock");
        }
#endif
    }
    ~NetworkRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

class ScopeExit final {
public:
    explicit ScopeExit(std::function<void()> action) : action_(std::move(action)) {}
    ~ScopeExit() { action_(); }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    std::function<void()> action_;
};

[[nodiscard]] int socket_error() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

[[nodiscard]] bool interrupted_socket_error(const int error) noexcept {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

[[nodiscard]] bool would_block_socket_error(const int error) noexcept {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

[[nodiscard]] std::string socket_error_text(const int error) {
    return std::system_category().message(error);
}

void close_socket(const NativeSocket descriptor) noexcept {
#ifdef _WIN32
    closesocket(descriptor);
#else
    ::close(descriptor);
#endif
}

[[nodiscard]] std::ptrdiff_t receive_socket(const NativeSocket descriptor,
                                            void* output,
                                            const std::size_t size) {
#ifdef _WIN32
    const auto bounded = static_cast<int>(std::min(
        size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    return ::recv(descriptor, static_cast<char*>(output), bounded, 0);
#else
    return ::recv(descriptor, output, size, 0);
#endif
}

[[nodiscard]] int poll_socket(PollDescriptor* descriptor, const int timeout) {
#ifdef _WIN32
    return WSAPoll(descriptor, 1, timeout);
#else
    return ::poll(descriptor, 1, timeout);
#endif
}

void wait_for_socket(const NativeSocket descriptor,
                     const short event,
                     const std::string_view operation) {
    PollDescriptor poll_descriptor{descriptor, event, 0};
    while (true) {
        const auto ready = poll_socket(&poll_descriptor, socket_io_timeout_ms);
        if (ready > 0 && (poll_descriptor.revents & event) != 0) {
            return;
        }
        if (ready == 0) {
            throw std::runtime_error(
                std::string(operation) + " timed out waiting for socket readiness");
        }
        if (ready < 0) {
            const auto error = socket_error();
            if (interrupted_socket_error(error)) {
                continue;
            }
            throw std::runtime_error(
                std::string(operation) + " poll failed: " + socket_error_text(error));
        }
        throw protocol::DecodeError(
            std::string("peer closed while waiting for socket ") + std::string(operation));
    }
}

[[nodiscard]] std::string numeric_host(const sockaddr* address,
                                       const NativeAddressLength size) {
    std::array<char, NI_MAXHOST> host{};
    if (::getnameinfo(address, size, host.data(),
                      static_cast<NativeAddressLength>(host.size()),
                      nullptr, 0, NI_NUMERICHOST) != 0) {
        return "unknown";
    }
    return host.data();
}

class Socket final {
public:
    explicit Socket(const NativeSocket descriptor = invalid_socket) noexcept
        : descriptor_(descriptor) {}
    ~Socket() {
        if (descriptor_ != invalid_socket) {
            close_socket(descriptor_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, invalid_socket)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (descriptor_ != invalid_socket) {
                close_socket(descriptor_);
            }
            descriptor_ = std::exchange(other.descriptor_, invalid_socket);
        }
        return *this;
    }

    [[nodiscard]] NativeSocket get() const noexcept { return descriptor_; }

private:
    NativeSocket descriptor_;
};

class OutputQueue final {
public:
    explicit OutputQueue(const NativeSocket descriptor) noexcept : descriptor_(descriptor) {}

    void enqueue(protocol::Bytes bytes) {
        if (bytes.size() > max_pending_write_bytes - pending_bytes_) {
            throw std::length_error("connection write queue exceeded backpressure limit");
        }
        pending_bytes_ += bytes.size();
        buffers_.push_back(std::move(bytes));
        flush(false);
    }

    void flush(const bool wait) {
        while (!buffers_.empty()) {
            const auto count = send_scattered();
            if (count > 0) {
                consume(static_cast<std::size_t>(count));
                continue;
            }
            const auto error = socket_error();
            if (interrupted_socket_error(error)) {
                continue;
            }
            if (would_block_socket_error(error)) {
                if (!wait) {
                    return;
                }
                wait_for_socket(descriptor_, socket_write_event, "write");
                continue;
            }
            throw std::runtime_error("socket send failed: " + socket_error_text(error));
        }
    }

    [[nodiscard]] bool empty() const noexcept { return buffers_.empty(); }
    [[nodiscard]] NativeSocket descriptor() const noexcept { return descriptor_; }

private:
    [[nodiscard]] std::ptrdiff_t send_scattered() {
#ifdef _WIN32
        std::array<WSABUF, max_scatter_buffers> vectors{};
        DWORD vector_count = 0;
        for (auto iterator = buffers_.begin();
             iterator != buffers_.end() && vector_count < vectors.size();
             ++iterator, ++vector_count) {
            const auto offset = iterator == buffers_.begin() ? front_offset_ : 0;
            vectors[vector_count].buf = reinterpret_cast<char*>(iterator->data() + offset);
            vectors[vector_count].len = static_cast<ULONG>(std::min(
                iterator->size() - offset,
                static_cast<std::size_t>(std::numeric_limits<ULONG>::max())));
        }
        DWORD sent = 0;
        if (::WSASend(descriptor_, vectors.data(), vector_count, &sent, 0, nullptr, nullptr) != 0) {
            return -1;
        }
        return static_cast<std::ptrdiff_t>(sent);
#else
        std::array<iovec, max_scatter_buffers> vectors{};
        std::size_t vector_count = 0;
        for (auto iterator = buffers_.begin();
             iterator != buffers_.end() && vector_count < vectors.size();
             ++iterator, ++vector_count) {
            const auto offset = iterator == buffers_.begin() ? front_offset_ : 0;
            vectors[vector_count].iov_base = iterator->data() + offset;
            vectors[vector_count].iov_len = iterator->size() - offset;
        }
        msghdr message{};
        message.msg_iov = vectors.data();
        message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(vector_count);
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        return ::sendmsg(descriptor_, &message, flags);
#endif
    }

    void consume(std::size_t count) {
        pending_bytes_ -= count;
        while (count > 0) {
            const auto available = buffers_.front().size() - front_offset_;
            if (count < available) {
                front_offset_ += count;
                return;
            }
            count -= available;
            buffers_.pop_front();
            front_offset_ = 0;
        }
    }

    NativeSocket descriptor_;
    std::deque<protocol::Bytes> buffers_;
    std::size_t front_offset_{0};
    std::size_t pending_bytes_{0};
};

thread_local OutputQueue* active_output_queue = nullptr;

void flush_pending_output(const NativeSocket descriptor, const bool wait) {
    if (active_output_queue != nullptr && active_output_queue->descriptor() == descriptor) {
        active_output_queue->flush(wait);
    }
}

[[nodiscard]] std::string json_escape(const std::string_view input) {
    std::string output;
    output.reserve(input.size());
    constexpr char hex[] = "0123456789abcdef";
    for (const auto character : input) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20U) {
                output += "\\u00";
                output.push_back(hex[byte >> 4U]);
                output.push_back(hex[byte & 0x0FU]);
            } else {
                output.push_back(character);
            }
        }
    }
    return output;
}

[[nodiscard]] std::uint8_t read_byte(const NativeSocket descriptor,
                                     protocol::AesCfb8Cipher* const cipher = nullptr) {
    flush_pending_output(descriptor, true);
    std::uint8_t byte{};
    while (true) {
        const auto count = receive_socket(descriptor, &byte, 1);
        if (count == 1) {
            if (cipher != nullptr) {
                cipher->decrypt(std::span(&byte, 1));
            }
            return byte;
        }
        if (count == 0) {
            throw protocol::DecodeError("peer closed the connection");
        }
        const auto error = socket_error();
        if (would_block_socket_error(error)) {
            wait_for_socket(descriptor, socket_read_event, "read");
            continue;
        }
        if (!interrupted_socket_error(error)) {
            throw std::runtime_error("socket receive failed: " + socket_error_text(error));
        }
    }
}

[[nodiscard]] std::int32_t read_varint(const NativeSocket descriptor,
                                       protocol::AesCfb8Cipher* const cipher = nullptr) {
    protocol::Bytes encoded;
    encoded.reserve(5);
    for (int index = 0; index < 5; ++index) {
        const auto byte = read_byte(descriptor, cipher);
        encoded.push_back(byte);
        if ((byte & 0x80U) == 0) {
            protocol::Reader reader(encoded);
            return reader.read_varint();
        }
    }
    throw protocol::DecodeError("frame length VarInt is too long");
}

void read_exact(const NativeSocket descriptor,
                const std::span<std::uint8_t> output,
                protocol::AesCfb8Cipher* const cipher = nullptr) {
    flush_pending_output(descriptor, true);
    std::size_t offset = 0;
    while (offset < output.size()) {
        const auto count = receive_socket(
            descriptor, output.data() + offset, output.size() - offset);
        if (count > 0) {
            if (cipher != nullptr) {
                cipher->decrypt(output.subspan(offset, static_cast<std::size_t>(count)));
            }
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            throw protocol::DecodeError("peer closed during packet payload");
        }
        const auto error = socket_error();
        if (would_block_socket_error(error)) {
            wait_for_socket(descriptor, socket_read_event, "read");
            continue;
        }
        if (!interrupted_socket_error(error)) {
            throw std::runtime_error("socket receive failed: " + socket_error_text(error));
        }
    }
}

[[nodiscard]] protocol::Bytes read_packet(const NativeSocket descriptor,
                                          protocol::AesCfb8Cipher* const cipher = nullptr) {
    const auto encoded_length = read_varint(descriptor, cipher);
    if (encoded_length <= 0 || static_cast<std::size_t>(encoded_length) > max_packet_size) {
        throw protocol::DecodeError("packet length is out of bounds");
    }
    protocol::Bytes packet(static_cast<std::size_t>(encoded_length));
    read_exact(descriptor, packet, cipher);
    return packet;
}

[[nodiscard]] protocol::Bytes read_compressed_packet(const NativeSocket descriptor,
                                                     const std::size_t threshold,
                                                     protocol::AesCfb8Cipher* const cipher = nullptr) {
    return protocol::decode_frame_payload(
        read_packet(descriptor, cipher), threshold, max_packet_size);
}

void write_all(NativeSocket descriptor,
               std::span<const std::uint8_t> bytes,
               protocol::AesCfb8Cipher* cipher = nullptr);

void write_compressed_packet(const NativeSocket descriptor,
                             const std::span<const std::uint8_t> uncompressed_frame,
                             const std::size_t threshold,
                             protocol::AesCfb8Cipher* const cipher = nullptr) {
    protocol::Reader frame(uncompressed_frame);
    const auto body_size = frame.read_varint();
    if (body_size < 1 || static_cast<std::size_t>(body_size) != frame.remaining()) {
        throw std::logic_error("invalid packet frame for compression");
    }
    const auto body = frame.read_bytes(static_cast<std::size_t>(body_size));
    protocol::Reader packet(body);
    const auto packet_id = packet.read_varint();
    const auto payload = packet.read_bytes(packet.remaining());
    write_all(
        descriptor, protocol::frame_compressed_packet(packet_id, payload, threshold), cipher);
}

void write_all(const NativeSocket descriptor,
               const std::span<const std::uint8_t> bytes,
               protocol::AesCfb8Cipher* const cipher) {
    protocol::Bytes encrypted;
    auto output = bytes;
    if (cipher != nullptr) {
        encrypted.assign(bytes.begin(), bytes.end());
        cipher->encrypt(encrypted);
        output = encrypted;
    }
    if (active_output_queue == nullptr || active_output_queue->descriptor() != descriptor) {
        throw std::logic_error("socket write has no active output queue");
    }
    active_output_queue->enqueue(protocol::Bytes(output.begin(), output.end()));
}

void configure_client_socket(const NativeSocket descriptor) {
#ifdef _WIN32
    constexpr DWORD timeout = 10'000;
    const auto* timeout_value = reinterpret_cast<const char*>(&timeout);
    constexpr int timeout_size = sizeof(timeout);
#else
    constexpr timeval timeout{10, 0};
    const auto* timeout_value = reinterpret_cast<const char*>(&timeout);
    constexpr socklen_t timeout_size = sizeof(timeout);
#endif
    if (::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, timeout_value, timeout_size) != 0 ||
        ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, timeout_value, timeout_size) != 0) {
        throw std::runtime_error(
            "failed to set socket timeout: " + socket_error_text(socket_error()));
    }
#ifdef SO_NOSIGPIPE
    constexpr int enabled = 1;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE,
                     reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) {
        throw std::runtime_error(
            "failed to disable SIGPIPE: " + socket_error_text(socket_error()));
    }
#endif
#ifdef _WIN32
    u_long nonblocking = 1;
    if (::ioctlsocket(descriptor, FIONBIO, &nonblocking) != 0) {
        throw std::runtime_error(
            "failed to make socket nonblocking: " + socket_error_text(socket_error()));
    }
#else
    const auto flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error(
            "failed to make socket nonblocking: " + socket_error_text(socket_error()));
    }
#endif
}

void expect_packet_end(const protocol::Reader& reader) {
    if (!reader.empty()) {
        throw protocol::DecodeError("packet contains trailing data");
    }
}

[[nodiscard]] protocol::Uuid create_session_id() {
    std::random_device random;
    protocol::Uuid uuid{};
    for (auto& byte : uuid) {
        byte = static_cast<std::uint8_t>(random());
    }
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0FU) | 0x40U);
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3FU) | 0x80U);
    return uuid;
}

[[nodiscard]] std::array<std::uint8_t, 4> create_challenge() {
    std::random_device random;
    std::array<std::uint8_t, 4> challenge{};
    for (auto& byte : challenge) {
        byte = static_cast<std::uint8_t>(random());
    }
    return challenge;
}

} // namespace

class Server::Impl final {
public:
    explicit Impl(Config config) : config_(std::move(config)) {
        if (config_.worker_count == 0 || config_.max_pending_connections == 0 ||
            config_.max_active_connections == 0 || config_.max_connections_per_ip == 0 ||
            config_.max_connection_attempts_per_second == 0) {
            throw std::invalid_argument("worker, queue, and connection limits must be positive");
        }
        if (config_.online_mode && config_.encrypted_offline) {
            throw std::invalid_argument("online mode and encrypted offline mode are mutually exclusive");
        }
        if ((config_.require_login_query_response || !config_.login_query_payload.empty()) &&
            !config_.login_query_channel) {
            throw std::invalid_argument("Login query policy requires a channel");
        }
        if (config_.require_resource_pack && !config_.resource_pack_url) {
            throw std::invalid_argument("required resource pack needs a URL");
        }
        if (!config_.resource_pack_hash.empty() &&
            (config_.resource_pack_hash.size() != 40 ||
             !std::all_of(config_.resource_pack_hash.begin(), config_.resource_pack_hash.end(),
                 [](const char character) {
                     return (character >= '0' && character <= '9') ||
                        (character >= 'a' && character <= 'f');
                 }))) {
            throw std::invalid_argument("resource pack hash must be 40 lowercase hex characters");
        }
		if (config_.keep_alive_timeout <= config_.keep_alive_interval) {
			throw std::invalid_argument("keepalive timeout must exceed its interval");
		}
        const auto tags_path = config_.configuration_tags_path.empty()
            ? std::string(MC_RUNTIME_TAGS_PATH)
            : config_.configuration_tags_path;
        std::ifstream tags_input(tags_path);
        if (!tags_input) {
            throw std::runtime_error("failed to open configuration tags: " + tags_path);
        }
        configuration_tags_ = protocol::configuration::load_normalized_tags(tags_input);
        const auto registries_path = config_.configuration_registries_path.empty()
            ? std::string(MC_RUNTIME_NETWORK_REGISTRIES_PATH)
            : config_.configuration_registries_path;
        std::ifstream registries_input(registries_path);
        if (!registries_input) {
            throw std::runtime_error(
                "failed to open configuration registries: " + registries_path);
        }
        configuration_registries_ =
            protocol::configuration::load_normalized_registry_data(registries_input);
        const auto fallback_path = config_.configuration_registry_fallback_path.empty()
            ? std::string(MC_RUNTIME_REGISTRY_FALLBACK_PATH)
            : config_.configuration_registry_fallback_path;
        std::ifstream fallback_input(fallback_path);
        if (!fallback_input) {
            throw std::runtime_error("failed to open registry fallback data: " + fallback_path);
        }
        configuration_registry_fallback_ =
            protocol::configuration::load_registry_fallback_packets(fallback_input);
        const auto recipe_sync_path = config_.recipe_sync_path.empty()
            ? std::string(MC_RUNTIME_RECIPE_SYNC_PATH)
            : config_.recipe_sync_path;
        std::ifstream recipe_sync_input(recipe_sync_path);
        if (!recipe_sync_input) {
            throw std::runtime_error("failed to open recipe sync data: " + recipe_sync_path);
        }
        recipe_sync_packets_ = protocol::play::load_recipe_sync_packets(recipe_sync_input);
    }

    ~Impl() { request_stop(); }

    void run() {
        open_listener();
        for (std::size_t index = 0; index < config_.worker_count; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }

        std::cout << "Minecraft 26.2 protocol server listening on 0.0.0.0:"
                  << config_.port << '\n';
        while (!stopping_.load()) {
            PollDescriptor event{listener_.get(), socket_read_event, 0};
            const auto ready = poll_socket(&event, 250);
            const auto poll_error = ready < 0 ? socket_error() : 0;
            if (ready < 0 && !interrupted_socket_error(poll_error)) {
                throw std::runtime_error("poll failed: " + socket_error_text(poll_error));
            }
            if (ready <= 0 || (event.revents & socket_read_event) == 0) {
                continue;
            }

            sockaddr_storage peer{};
            NativeAddressLength peer_size = sizeof(peer);
            const auto client = ::accept(
                listener_.get(), reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (client == invalid_socket) {
                const auto error = socket_error();
                if (interrupted_socket_error(error)) {
                    continue;
                }
                throw std::runtime_error("accept failed: " + socket_error_text(error));
            }
            enqueue(client, numeric_host(reinterpret_cast<const sockaddr*>(&peer), peer_size));
        }
        request_stop();
    }

    void request_stop() noexcept {
        if (stopping_.exchange(true)) {
            return;
        }
        queue_ready_.notify_all();
    }

private:
    void open_listener() {
        Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
        if (listener.get() == invalid_socket) {
            throw std::runtime_error("socket failed: " + socket_error_text(socket_error()));
        }

        constexpr int enabled = 1;
        if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) {
            throw std::runtime_error("setsockopt failed: " + socket_error_text(socket_error()));
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(config_.port);
        if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error("bind failed: " + socket_error_text(socket_error()));
        }
        if (::listen(listener.get(), SOMAXCONN) != 0) {
            throw std::runtime_error("listen failed: " + socket_error_text(socket_error()));
        }
        listener_ = std::move(listener);
    }

    struct QueuedConnection final {
        NativeSocket descriptor;
        std::string host;
        bool rate_limited;
    };

    void enqueue(const NativeSocket descriptor, std::string host) {
        std::lock_guard lock(queue_mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto& attempts = connection_attempts_by_host_[host];
        while (!attempts.empty() && now - attempts.front() >= std::chrono::seconds(1)) {
            attempts.pop_front();
        }
        const auto rate_limited =
            attempts.size() >= config_.max_connection_attempts_per_second;
        attempts.push_back(now);
        const auto host_count = active_by_host_[host];
        if (pending_.size() >= config_.max_pending_connections ||
            active_connections_ >= config_.max_active_connections ||
            host_count >= config_.max_connections_per_ip) {
            if (host_count == 0) {
                active_by_host_.erase(host);
            }
            close_socket(descriptor);
            return;
        }
        ++active_connections_;
        ++active_by_host_[host];
        pending_.push_back({descriptor, std::move(host), rate_limited});
        queue_ready_.notify_one();
    }

    void release_connection(const std::string& host) {
        std::lock_guard lock(queue_mutex_);
        if (active_connections_ > 0) {
            --active_connections_;
        }
        const auto found = active_by_host_.find(host);
        if (found != active_by_host_.end() && --found->second == 0) {
            active_by_host_.erase(found);
        }
    }

    void worker_loop() {
        while (true) {
            QueuedConnection connection{invalid_socket, {}, false};
            {
                std::unique_lock lock(queue_mutex_);
                queue_ready_.wait(lock, [this] { return stopping_.load() || !pending_.empty(); });
                if (pending_.empty()) {
                    return;
                }
                connection = std::move(pending_.front());
                pending_.pop_front();
            }

            try {
                Socket client(connection.descriptor);
                configure_client_socket(client.get());
                OutputQueue output(client.get());
                active_output_queue = &output;
                ScopeExit clear_output_queue([] { active_output_queue = nullptr; });
                handle_connection(client.get(), connection.rate_limited);
                output.flush(true);
            } catch (const std::exception& error) {
                std::cerr << "Connection closed: " << error.what() << '\n';
            }
            release_connection(connection.host);
        }
    }

    void handle_status(const NativeSocket descriptor, const bool rate_limited) const {
        const auto request_packet = read_packet(descriptor);
        protocol::Reader request(request_packet);
        if (request.read_varint() != 0) {
            throw protocol::DecodeError("expected status request packet");
        }
        expect_packet_end(request);

        const auto status = std::string{"{\"version\":{\"name\":\"26.2\",\"protocol\":"} +
            std::to_string(protocol_version) +
            "},\"players\":{\"max\":20,\"online\":0,\"sample\":[]},"
            "\"description\":{\"text\":\"" +
            json_escape(rate_limited ? "Connection rate limit exceeded" : config_.motd) +
            "\"},\"enforcesSecureChat\":true}";
        protocol::Bytes response_payload;
        protocol::write_string(response_payload, status);
        const auto response = protocol::frame_packet(0, response_payload);
        write_all(descriptor, response);

        const auto ping_packet = read_packet(descriptor);
        protocol::Reader ping(ping_packet);
        if (ping.read_varint() != 1) {
            throw protocol::DecodeError("expected ping request packet");
        }
        const auto payload = ping.read_i64_be();
        expect_packet_end(ping);

        protocol::Bytes pong_payload;
        protocol::write_i64_be(pong_payload, payload);
        const auto pong = protocol::frame_packet(1, pong_payload);
        write_all(descriptor, pong);
    }

    void handle_login(const NativeSocket descriptor, const bool transferred) {
        const auto compression_threshold = config_.compression_threshold;
        const auto hello_packet = read_packet(descriptor);
        protocol::Reader hello_reader(hello_packet);
        const auto hello = protocol::login::decode_hello(hello_reader);
        if (hello.name.empty()) {
            throw protocol::DecodeError("login name must not be empty");
        }
        {
            std::lock_guard lock(profile_mutex_);
            if (!active_profiles_.insert(hello.name).second) {
                write_all(descriptor, protocol::login::encode_disconnect(
                    "{\"text\":\"A player with that name is already connected\"}"));
                return;
            }
        }
        ScopeExit profile_lease([this, name = hello.name] {
            std::lock_guard lock(profile_mutex_);
            active_profiles_.erase(name);
        });

        if (transferred) {
            write_all(descriptor, protocol::encode_cookie_request(
                static_cast<std::int32_t>(protocol::login::ClientboundPacketId::cookie_request),
                "mcsquared:transfer"));
            const auto cookie_packet = read_packet(descriptor);
            protocol::Reader cookie_reader(cookie_packet);
            const auto cookie = protocol::decode_cookie_response(
                cookie_reader,
                static_cast<std::int32_t>(protocol::login::ServerboundPacketId::cookie_response));
            if (cookie.key != "mcsquared:transfer") {
                throw protocol::DecodeError("transfer cookie key does not match request");
            }
        }

        protocol::login::GameProfile profile{
            protocol::create_offline_uuid(hello.name), hello.name, {}};
        std::optional<protocol::AesCfb8Cipher> cipher;
        if (config_.online_mode || config_.encrypted_offline) {
            protocol::RsaKeyPair key_pair;
            const auto public_key = key_pair.public_key_der();
            const auto challenge = create_challenge();
            write_all(descriptor, protocol::login::encode_encryption_request(
                "", public_key, challenge, config_.online_mode));

            const auto key_packet = read_packet(descriptor);
            protocol::Reader key_reader(key_packet);
            const auto response = protocol::login::decode_encryption_response(key_reader);
            if (key_pair.decrypt(response.encrypted_challenge) !=
                protocol::Bytes(challenge.begin(), challenge.end())) {
                throw protocol::DecodeError("login encryption challenge does not match");
            }
            const auto shared_secret = key_pair.decrypt(response.encrypted_secret);
            if (shared_secret.size() != 16) {
                throw protocol::DecodeError("login shared secret must contain 16 bytes");
            }
            cipher.emplace(shared_secret);

            if (config_.online_mode) {
                try {
                    const auto server_hash = protocol::minecraft_server_hash(
                        "", shared_secret, public_key);
                    const auto authenticated = SessionAuthenticator(config_.session_server_url)
                        .authenticate(hello.name, server_hash);
                    if (!authenticated) {
                        write_all(descriptor, protocol::login::encode_disconnect(
                            "{\"text\":\"Failed to verify username\"}"), &*cipher);
                        return;
                    }
                    profile = *authenticated;
                } catch (const SessionAuthenticationError& error) {
                    std::cerr << "Session authentication failed: " << error.what() << '\n';
                    write_all(descriptor, protocol::login::encode_disconnect(
                        "{\"text\":\"Authentication servers are unavailable\"}"), &*cipher);
                    return;
                }
            }
        }

        if (config_.login_query_channel) {
            constexpr std::int32_t transaction_id = 0;
            const auto payload = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(config_.login_query_payload.data()),
                config_.login_query_payload.size());
            write_all(descriptor, protocol::login::encode_custom_query(
                transaction_id, *config_.login_query_channel, payload),
                cipher ? &*cipher : nullptr);
            const auto answer_packet = read_packet(descriptor, cipher ? &*cipher : nullptr);
            protocol::Reader answer_reader(answer_packet);
            const auto answer = protocol::login::decode_custom_query_answer(answer_reader);
            if (answer.transaction_id != transaction_id) {
                write_all(descriptor, protocol::login::encode_disconnect(
                    "{\"text\":\"Invalid Login query transaction\"}"),
                    cipher ? &*cipher : nullptr);
                return;
            }
            if (config_.require_login_query_response && !answer.payload) {
                write_all(descriptor, protocol::login::encode_disconnect(
                    "{\"text\":\"A required Login query was declined\"}"),
                    cipher ? &*cipher : nullptr);
                return;
            }
        }

        const auto compression = protocol::login::encode_compression(compression_threshold);
        write_all(descriptor, compression, cipher ? &*cipher : nullptr);

        const auto login_finished_payload = protocol::login::encode_finished_payload(
            profile, create_session_id());
        const auto login_finished = protocol::frame_compressed_packet(
            static_cast<std::int32_t>(protocol::login::ClientboundPacketId::finished),
            login_finished_payload,
            compression_threshold);
        write_all(descriptor, login_finished, cipher ? &*cipher : nullptr);

        const auto acknowledgement_packet = read_compressed_packet(
            descriptor, compression_threshold, cipher ? &*cipher : nullptr);
        protocol::Reader acknowledgement(acknowledgement_packet);
        if (acknowledgement.read_varint() != static_cast<std::int32_t>(
                protocol::login::ServerboundPacketId::acknowledged)) {
            throw protocol::DecodeError("expected login acknowledgement packet");
        }
        expect_packet_end(acknowledgement);

        write_compressed_packet(
            descriptor, protocol::configuration::encode_brand("mcsquared"), compression_threshold,
            cipher ? &*cipher : nullptr);
        write_compressed_packet(
            descriptor, protocol::configuration::encode_enabled_features(), compression_threshold,
            cipher ? &*cipher : nullptr);
        write_compressed_packet(
            descriptor, protocol::configuration::encode_select_known_packs(), compression_threshold,
            cipher ? &*cipher : nullptr);

        std::optional<std::vector<protocol::configuration::KnownPack>> selected_packs;
        for (std::size_t packet_count = 0; packet_count < 16 && !selected_packs; ++packet_count) {
            const auto packet = read_compressed_packet(
                descriptor, compression_threshold, cipher ? &*cipher : nullptr);
            protocol::Reader packet_id_reader(packet);
            const auto packet_id = packet_id_reader.read_varint();
            if (packet_id == static_cast<std::int32_t>(
                    protocol::configuration::ServerboundPacketId::select_known_packs)) {
                protocol::Reader selected_packs_reader(packet);
                selected_packs = protocol::configuration::decode_selected_known_packs(
                    selected_packs_reader);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::client_information)) {
                protocol::Reader information_reader(packet);
                static_cast<void>(
                    protocol::configuration::decode_client_information(information_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::custom_payload)) {
                protocol::Reader payload_reader(packet);
                static_cast<void>(protocol::configuration::decode_custom_payload(payload_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::cookie_response)) {
                protocol::Reader cookie_reader(packet);
                static_cast<void>(protocol::configuration::decode_cookie_response(cookie_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::resource_pack)) {
                protocol::Reader resource_pack_reader(packet);
                static_cast<void>(protocol::configuration::decode_resource_pack_response(
                    resource_pack_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::accept_code_of_conduct)) {
                protocol::Reader conduct_reader(packet);
                protocol::configuration::decode_accept_code_of_conduct(conduct_reader);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::custom_click_action)) {
                protocol::Reader click_reader(packet);
                static_cast<void>(
                    protocol::configuration::decode_custom_click_action(click_reader));
            } else {
                write_compressed_packet(
                    descriptor,
                    protocol::configuration::encode_disconnect_text(
                        "Unexpected packet during known-pack negotiation"),
                    compression_threshold,
                    cipher ? &*cipher : nullptr);
                return;
            }
        }
        if (!selected_packs) {
            throw protocol::DecodeError("known-pack negotiation packet limit exceeded");
        }
        const std::vector expected_packs{
            protocol::configuration::KnownPack{"minecraft", "core", "26.2"}};
        const auto core_pack_accepted = *selected_packs == expected_packs;
        if (!core_pack_accepted && !selected_packs->empty()) {
            throw protocol::DecodeError("client selected an unknown known-pack combination");
        }

        if (config_.resource_pack_url) {
            const auto resource_pack_id = create_session_id();
            write_compressed_packet(
                descriptor,
                protocol::configuration::encode_resource_pack_push(
                    resource_pack_id, *config_.resource_pack_url,
                    config_.resource_pack_hash, config_.require_resource_pack),
                compression_threshold,
                cipher ? &*cipher : nullptr);
            if (config_.require_resource_pack) {
                bool resource_pack_loaded = false;
                for (std::size_t response_count = 0;
                     response_count < 16 && !resource_pack_loaded;
                     ++response_count) {
                    const auto packet = read_compressed_packet(
                        descriptor, compression_threshold, cipher ? &*cipher : nullptr);
                    protocol::Reader packet_id_reader(packet);
                    const auto packet_id = packet_id_reader.read_varint();
                    if (packet_id != static_cast<std::int32_t>(
                            protocol::configuration::ServerboundPacketId::resource_pack)) {
                        write_compressed_packet(
                            descriptor,
                            protocol::configuration::encode_disconnect_text(
                                "Expected required resource pack response"),
                            compression_threshold,
                            cipher ? &*cipher : nullptr);
                        return;
                    }
                    protocol::Reader response_reader(packet);
                    const auto response =
                        protocol::configuration::decode_resource_pack_response(response_reader);
                    if (response.id != resource_pack_id) {
                        throw protocol::DecodeError("resource pack response UUID does not match");
                    }
                    switch (response.action) {
                    case protocol::configuration::ResourcePackAction::successfully_loaded:
                        resource_pack_loaded = true;
                        break;
                    case protocol::configuration::ResourcePackAction::accepted:
                    case protocol::configuration::ResourcePackAction::downloaded:
                        break;
                    default:
                        write_compressed_packet(
                            descriptor,
                            protocol::configuration::encode_disconnect_text(
                                "Required resource pack was not loaded"),
                            compression_threshold,
                            cipher ? &*cipher : nullptr);
                        return;
                    }
                }
                if (!resource_pack_loaded) {
                    throw protocol::DecodeError("required resource pack response limit exceeded");
                }
            }
        }

        if (core_pack_accepted) {
            for (const auto& registry : configuration_registries_) {
                write_compressed_packet(
                    descriptor,
                    protocol::configuration::encode_registry_data(registry),
                    compression_threshold,
                    cipher ? &*cipher : nullptr);
            }
        } else {
            for (const auto& registry_packet : configuration_registry_fallback_) {
                write_compressed_packet(
                    descriptor, registry_packet, compression_threshold,
                    cipher ? &*cipher : nullptr);
            }
        }
        write_compressed_packet(
            descriptor, protocol::configuration::encode_tags(configuration_tags_), compression_threshold,
            cipher ? &*cipher : nullptr);
        write_compressed_packet(
            descriptor, protocol::configuration::encode_finish(), compression_threshold,
            cipher ? &*cipher : nullptr);

        bool configuration_finished = false;
        for (std::size_t packet_count = 0;
             packet_count < 32 && !configuration_finished;
             ++packet_count) {
            const auto packet = read_compressed_packet(
                descriptor, compression_threshold, cipher ? &*cipher : nullptr);
            protocol::Reader packet_id_reader(packet);
            const auto packet_id = packet_id_reader.read_varint();
            protocol::Reader packet_reader(packet);
            if (packet_id == static_cast<std::int32_t>(
                    protocol::configuration::ServerboundPacketId::finish)) {
                protocol::configuration::decode_finish(packet_reader);
                configuration_finished = true;
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::client_information)) {
                static_cast<void>(
                    protocol::configuration::decode_client_information(packet_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::custom_payload)) {
                static_cast<void>(protocol::configuration::decode_custom_payload(packet_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::cookie_response)) {
                static_cast<void>(protocol::configuration::decode_cookie_response(packet_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::resource_pack)) {
                static_cast<void>(
                    protocol::configuration::decode_resource_pack_response(packet_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::accept_code_of_conduct)) {
                protocol::configuration::decode_accept_code_of_conduct(packet_reader);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::configuration::ServerboundPacketId::custom_click_action)) {
                static_cast<void>(
                    protocol::configuration::decode_custom_click_action(packet_reader));
            } else {
                write_compressed_packet(
                    descriptor,
                    protocol::configuration::encode_disconnect_text(
                        "Unexpected packet before Configuration finish"),
                    compression_threshold,
                    cipher ? &*cipher : nullptr);
                return;
            }
        }
        if (!configuration_finished) {
            throw protocol::DecodeError("Configuration finish packet limit exceeded");
        }

        write_compressed_packet(
            descriptor, protocol::play::encode_login(config_.hardcore), compression_threshold,
            cipher ? &*cipher : nullptr);
        write_compressed_packet(
            descriptor,
            protocol::play::encode_player_info_initialize(
                profile, 0, true, 0, 0, true),
            compression_threshold,
            cipher ? &*cipher : nullptr);
        write_compressed_packet(
            descriptor,
            protocol::play::encode_command_tree(implemented_command_roots),
            compression_threshold,
            cipher ? &*cipher : nullptr);
        for (const auto& recipe_packet : recipe_sync_packets_) {
            write_compressed_packet(
                descriptor, recipe_packet, compression_threshold,
                cipher ? &*cipher : nullptr);
        }
        block::BlockRegistry block_registry;
        item::ItemRegistry item_registry;
        entity::EntityTypeRegistry entity_registry;
        {
            std::ifstream block_input(MC_RUNTIME_REGISTRIES_PATH);
            std::ifstream item_input(MC_RUNTIME_REGISTRIES_PATH);
            std::ifstream entity_input(MC_RUNTIME_REGISTRIES_PATH);
            if (!block_input || !item_input || !entity_input) {
                throw std::runtime_error("failed to open gameplay registry manifest");
            }
            static_cast<void>(block_registry.load_normalized(block_input));
            static_cast<void>(item_registry.load_normalized(item_input));
            static_cast<void>(entity_registry.load_normalized(entity_input));
        }
        block::BlockInteraction block_interaction(block_registry, item_registry);
        world::World level({
            {.seed = 0, .sea_level = 63}, 128,
            config_.world_path
                ? std::optional<std::filesystem::path>(*config_.world_path)
                : std::nullopt});
        const auto saved_rule = [&](const std::string& name, const bool fallback) {
            const auto value = level.game_rule(name);
            return value ? *value == "true" : fallback;
        };
        std::array<item::ItemStack, 9> hotbar{};
        hotbar[0] = item::ItemStack(item_registry.by_name("dirt").id(), 64);
        hotbar[1] = item::ItemStack(item_registry.by_name("wheat").id(), 16);
        hotbar[2] = item::ItemStack(item_registry.by_name("wooden_pickaxe").id(), 1);
        hotbar[3] = item::ItemStack(item_registry.by_name("bread").id(), 16);
        hotbar[4] = item::ItemStack(item_registry.by_name("bow").id(), 1);
        hotbar[5] = item::ItemStack(item_registry.by_name("arrow").id(), 32);
        hotbar[6] = item::ItemStack(item_registry.by_name("shield").id(), 1);
        hotbar[7] = item::ItemStack(item_registry.by_name("bone").id(), 16);
        item::ItemStack offhand;
        bool keep_inventory = saved_rule("keepInventory", false);
        bool advance_time = saved_rule("doDaylightCycle", true);
        bool advance_weather = saved_rule("doWeatherCycle", true);
        bool spawn_mobs = saved_rule("doMobSpawning", true);
        level.set_advance_time(advance_time);
        level.set_weather_cycle(advance_weather);
        level.set_game_rule("keepInventory", keep_inventory ? "true" : "false");
        level.set_game_rule("doDaylightCycle", advance_time ? "true" : "false");
        level.set_game_rule("doWeatherCycle", advance_weather ? "true" : "false");
        level.set_game_rule("doMobSpawning", spawn_mobs ? "true" : "false");
        auto difficulty = config_.hardcore
            ? player::Difficulty::hard
            : static_cast<player::Difficulty>(level.difficulty());
        level.set_difficulty(static_cast<std::uint8_t>(difficulty));
        std::int16_t active_hotbar_slot = 0;
        const auto simple_stack = [&](const item::ItemStack& stack) {
            return protocol::play::SimpleItemStack{
                stack.empty() ? 0 : static_cast<std::int32_t>(
                    item_registry.by_id(stack.item_id()).protocol_id().value()),
                stack.count()};
        };
        std::array<protocol::play::SimpleItemStack, 46> player_inventory{};
        for (std::size_t slot = 0; slot < hotbar.size(); ++slot) {
            player_inventory[36 + slot] = simple_stack(hotbar[slot]);
        }
        write_compressed_packet(
            descriptor,
            protocol::play::encode_container_content(
                0, 0, player_inventory, {}),
            compression_threshold,
            cipher ? &*cipher : nullptr);
        write_compressed_packet(
            descriptor,
            protocol::play::encode_change_difficulty(
                static_cast<std::uint8_t>(difficulty), config_.hardcore),
            compression_threshold, cipher ? &*cipher : nullptr);
        write_compressed_packet(
            descriptor, protocol::play::encode_ticking_state(20.0F, false),
            compression_threshold, cipher ? &*cipher : nullptr);
        std::array initial_game_rules{
            std::pair<std::string, std::string>{
                "minecraft:advance_time", advance_time ? "true" : "false"},
            std::pair<std::string, std::string>{
                "minecraft:keep_inventory", keep_inventory ? "true" : "false"},
            std::pair<std::string, std::string>{
                "minecraft:spawn_mobs", spawn_mobs ? "true" : "false"},
            std::pair<std::string, std::string>{
                "minecraft:advance_weather", advance_weather ? "true" : "false"},
        };
        write_compressed_packet(
            descriptor, protocol::play::encode_game_rule_values(initial_game_rules),
            compression_threshold, cipher ? &*cipher : nullptr);
        const auto& initial_border = level.border();
        for (const auto& state_packet : {
                 protocol::play::encode_initialize_border(
                     initial_border.center_x, initial_border.center_z,
                     initial_border.size, initial_border.lerp_target_size,
                     static_cast<std::int64_t>(initial_border.lerp_remaining_ticks * 50U),
                     29'999'984, initial_border.warning_distance,
                     initial_border.warning_time),
                 protocol::play::encode_player_abilities(
                     false, false, false, false, 0.05F, 0.1F),
                 protocol::play::encode_chunk_cache_radius(2),
                 protocol::play::encode_experience(0.0F, 0, 0),
                 protocol::play::encode_health(20.0F, 20, 5.0F),
                 protocol::play::encode_held_slot(0),
                 protocol::play::encode_set_time(
                     static_cast<std::int64_t>(level.game_time()),
                     static_cast<std::int64_t>(level.day_time())),
                 protocol::play::encode_game_event(level.raining() ? 1 : 2, 0.0F),
                 protocol::play::encode_game_event(7, level.rain_level()),
                 protocol::play::encode_game_event(8, level.thunder_level())}) {
            write_compressed_packet(
                descriptor, state_packet, compression_threshold,
                cipher ? &*cipher : nullptr);
        }

        world::BlockTickSystem block_ticks(0xB10CULL);
        if (!level.has_persisted_state()) {
            const auto initial_chunk = level.chunk({0, 0});
            level.set_spawn({8, initial_chunk->height(8, 8) + 1, 8});
        }
        const auto spawn = level.spawn();
        const auto spawn_chunk_x = static_cast<std::int32_t>(
            std::floor(static_cast<double>(spawn.x) / 16.0));
        const auto spawn_chunk_z = static_cast<std::int32_t>(
            std::floor(static_cast<double>(spawn.z) / 16.0));
        const auto spawn_chunk = level.chunk({spawn_chunk_x, spawn_chunk_z});

        write_compressed_packet(
            descriptor,
            protocol::play::encode_chunk_cache_center(spawn_chunk_x, spawn_chunk_z),
            compression_threshold,
            cipher ? &*cipher : nullptr);
        write_compressed_packet(
            descriptor,
            protocol::play::encode_default_spawn_position(
                {spawn.x, spawn.y, spawn.z}),
            compression_threshold,
            cipher ? &*cipher : nullptr);
        std::int32_t next_teleport_id = 1;
        write_compressed_packet(
            descriptor,
            protocol::play::encode_player_position(
            next_teleport_id, static_cast<double>(spawn.x) + 0.5,
            static_cast<double>(spawn.y), static_cast<double>(spawn.z) + 0.5,
            0.0F, 0.0F),
            compression_threshold,
            cipher ? &*cipher : nullptr);
		write_compressed_packet(
			descriptor,
			protocol::play::encode_level_chunks_load_start(),
			compression_threshold,
			cipher ? &*cipher : nullptr);
        write_compressed_packet(
            descriptor,
            protocol::play::encode_chunk_batch_start(),
            compression_threshold,
            cipher ? &*cipher : nullptr);
        const auto send_generated_chunk = [&](const world::Chunk& chunk) {
            write_compressed_packet(
                descriptor,
                protocol::play::encode_level_chunk(chunk),
                compression_threshold,
                cipher ? &*cipher : nullptr);
        };
        constexpr std::int32_t chunk_radius = 2;
        std::set<std::pair<std::int32_t, std::int32_t>> loaded_chunks;
        send_generated_chunk(*spawn_chunk);
        loaded_chunks.emplace(spawn_chunk_x, spawn_chunk_z);
        for (std::int32_t offset_z = -chunk_radius; offset_z <= chunk_radius; ++offset_z) {
            for (std::int32_t offset_x = -chunk_radius; offset_x <= chunk_radius; ++offset_x) {
                if (offset_x == 0 && offset_z == 0) {
                    continue;
                }
                const auto chunk_x = spawn_chunk_x + offset_x;
                const auto chunk_z = spawn_chunk_z + offset_z;
                send_generated_chunk(*level.chunk({chunk_x, chunk_z}));
                loaded_chunks.emplace(chunk_x, chunk_z);
            }
        }
        write_compressed_packet(
            descriptor,
            protocol::play::encode_chunk_batch_finished(
                static_cast<std::int32_t>(loaded_chunks.size())),
            compression_threshold,
            cipher ? &*cipher : nullptr);

        entity::EntityManager entities(entity_registry, 0x26'02ULL, 2);
        entity::EntityTracker entity_tracker;
        entity::DroppedItemSystem dropped_items;
        entity::ProjectileSystem projectiles;
        entity::NaturalSpawner natural_spawner(entity_registry, 0x5EEDULL);
        entity::MobAiSystem mob_ai(entities, 0xA11ULL);
        entity::AnimalSystem animals;
        const auto spawn_candidates = [&](const entity::Vec3 center,
                                          const double radius) {
            std::vector<entity::SpawnCandidate> candidates;
            for (const auto [offset_x, offset_z] : std::array{
                     std::pair{-radius, -radius}, std::pair{radius, -radius},
                     std::pair{-radius, radius}, std::pair{radius, radius}}) {
                const auto x = center.x + offset_x;
                const auto z = center.z + offset_z;
                const auto block_x = static_cast<std::int32_t>(std::floor(x));
                const auto block_z = static_cast<std::int32_t>(std::floor(z));
                auto block_y = level.surface_height(block_x, block_z);
                const auto biome = level.biome(block_x, block_y, block_z);
                auto habitat = entity::SpawnHabitat::surface_land;
                if (biome == world::BiomeId::ocean) {
                    block_y = std::max(world::min_build_y + 1, block_y - 2);
                    habitat = level.block({block_x, block_y, block_z}) == world::BlockId::water
                        ? entity::SpawnHabitat::water
                        : entity::SpawnHabitat::surface_land;
                } else {
                    while (block_y > world::min_build_y &&
                           level.block({block_x, block_y, block_z}) !=
                               world::BlockId::grass_block &&
                           level.block({block_x, block_y, block_z}) != world::BlockId::sand) {
                        --block_y;
                    }
                    ++block_y;
                }
                candidates.push_back({{x, static_cast<double>(block_y), z}, biome, habitat});
            }
            return candidates;
        };
        const entity::Vec3 initial_player_position{
            static_cast<double>(spawn.x) + 0.5, static_cast<double>(spawn.y),
            static_cast<double>(spawn.z) + 0.5};
        const auto creature_ids = natural_spawner.spawn_cycle(
            entities, entity::EntityCategory::creature,
            spawn_candidates(initial_player_position, 10.0), 15, false, 4);
        const auto monster_ids = natural_spawner.spawn_cycle(
            entities, entity::EntityCategory::monster,
            spawn_candidates(initial_player_position, 18.0),
            level.daylight() ? 15 : 0,
            difficulty == player::Difficulty::peaceful, 4);
        static_cast<void>(natural_spawner.spawn_cycle(
            entities, entity::EntityCategory::water_ambient,
            spawn_candidates(initial_player_position, 18.0), 15, false, 4));
        static_cast<void>(natural_spawner.spawn_cycle(
            entities, entity::EntityCategory::water_creature,
            spawn_candidates(initial_player_position, 18.0), 15, false, 2));
        for (const auto id : entities.ids()) {
            static_cast<void>(mob_ai.attach(id));
        }
        for (const auto id : creature_ids) {
            if (auto* spawned = entities.find(id)) static_cast<void>(animals.attach(*spawned));
        }
        if (!creature_ids.empty()) {
            for (const auto id : monster_ids) {
                const auto* monster = entities.find(id);
                if (monster && !monster->type().properties().aquatic) {
                    static_cast<void>(mob_ai.set_target(id, creature_ids.front()));
                }
            }
        }
        const auto send_entity_spawn = [&](const entity::Entity& spawned,
                                           const std::int32_t data = 0) {
            write_compressed_packet(
                descriptor,
                protocol::play::encode_add_entity({
                    static_cast<std::int32_t>(spawned.id()), spawned.uuid(),
                    static_cast<std::int32_t>(spawned.type().protocol_id().value()),
                    {spawned.position().x, spawned.position().y, spawned.position().z},
                    {spawned.velocity().x, spawned.velocity().y, spawned.velocity().z},
                    spawned.pitch(), spawned.yaw(), spawned.yaw(), data}),
                compression_threshold, cipher ? &*cipher : nullptr);
        };

        std::pair<std::int32_t, std::int32_t> streamed_center{0, 0};
        auto player_center = streamed_center;
        const auto stream_chunk_window = [&](const std::pair<std::int32_t, std::int32_t> center) {
            std::set<std::pair<std::int32_t, std::int32_t>> target_chunks;
            for (std::int32_t offset_z = -chunk_radius; offset_z <= chunk_radius; ++offset_z) {
                for (std::int32_t offset_x = -chunk_radius; offset_x <= chunk_radius; ++offset_x) {
                    target_chunks.emplace(center.first + offset_x, center.second + offset_z);
                }
            }

            write_compressed_packet(
                descriptor,
                protocol::play::encode_chunk_cache_center(center.first, center.second),
                compression_threshold,
                cipher ? &*cipher : nullptr);
            for (const auto& position : loaded_chunks) {
                if (!target_chunks.contains(position)) {
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_forget_level_chunk(position.first, position.second),
                        compression_threshold,
                        cipher ? &*cipher : nullptr);
                    static_cast<void>(level.chunks().unload({position.first, position.second}));
                }
            }

            std::vector<std::pair<std::int32_t, std::int32_t>> chunks_to_load;
            std::set_difference(
                target_chunks.begin(), target_chunks.end(),
                loaded_chunks.begin(), loaded_chunks.end(),
                std::back_inserter(chunks_to_load));
            if (!chunks_to_load.empty()) {
                write_compressed_packet(
                    descriptor, protocol::play::encode_chunk_batch_start(),
                    compression_threshold, cipher ? &*cipher : nullptr);
                for (const auto& position : chunks_to_load) {
                    send_generated_chunk(*level.chunk({position.first, position.second}));
                }
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_chunk_batch_finished(
                        static_cast<std::int32_t>(chunks_to_load.size())),
                    compression_threshold,
                    cipher ? &*cipher : nullptr);
            }
            loaded_chunks = std::move(target_chunks);
            streamed_center = center;
        };

        bool teleport_confirmed = false;
        std::optional<std::int32_t> pending_teleport{next_teleport_id};
		auto teleport_sent_at = std::chrono::steady_clock::now();
        std::optional<std::int64_t> pending_keep_alive;
        auto last_keep_alive = std::chrono::steady_clock::now();
        auto last_activity = std::chrono::steady_clock::now();
        std::chrono::milliseconds latency{0};
        auto next_game_tick = std::chrono::steady_clock::now();
        auto last_autosave = std::chrono::steady_clock::now();
        std::uint64_t simulation_ticks = 0;
        auto player_position = initial_player_position;
        float player_yaw = 0.0F;
        player::SurvivalState survival;
        player::Statistics statistics;
        static_cast<void>(survival.take_dirty());
        bool player_dead = false;
        enum class GameMode : std::uint8_t { survival, creative, adventure, spectator };
        auto game_mode = GameMode::survival;
        std::optional<entity::EntityId> player_vehicle;
        bool entities_visible = false;
        std::map<entity::EntityId, std::uint16_t> experience_orbs;
        std::set<entity::EntityId> daylight_burning;
        std::int32_t total_experience = 0;
        std::map<entity::EntityId, entity::Vec3> aquatic_positions;
        std::map<entity::EntityId, std::uint32_t> player_attack_cooldowns;
        std::optional<std::uint8_t> blocking_hand;
        for (const auto id : entities.ids()) {
            const auto* spawned = entities.find(id);
            if (spawned && spawned->type().properties().aquatic) {
                aquatic_positions.emplace(id, spawned->position());
            }
        }
        struct PendingBreak final {
            core::BlockPosition position;
            std::chrono::steady_clock::time_point ready_at;
        };
        std::optional<PendingBreak> pending_break;
        const auto within_reach = [&](const core::BlockPosition position) {
            const auto delta_x = static_cast<double>(position.x) + 0.5 - player_position.x;
            const auto delta_y = static_cast<double>(position.y) + 0.5 -
                (player_position.y + survival.eye_height());
            const auto delta_z = static_cast<double>(position.z) + 0.5 - player_position.z;
            return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z <= 36.0 &&
                level.line_of_sight(
                    {player_position.x, player_position.y + survival.eye_height(),
                     player_position.z},
                    {static_cast<double>(position.x) + 0.5,
                     static_cast<double>(position.y) + 0.5,
                     static_cast<double>(position.z) + 0.5},
                    position);
        };
        const auto send_block_update = [&](const core::BlockPosition position) {
            const auto block_id = level.block(position);
            write_compressed_packet(
                descriptor,
                protocol::play::encode_block_update(
                    {position.x, position.y, position.z},
                    protocol::play::protocol_block_state_id(block_id)),
                compression_threshold, cipher ? &*cipher : nullptr);
            const auto chunk_x = static_cast<std::int32_t>(
                std::floor(static_cast<double>(position.x) / world::chunk_width));
            const auto chunk_z = static_cast<std::int32_t>(
                std::floor(static_cast<double>(position.z) / world::chunk_width));
            write_compressed_packet(
                descriptor,
                protocol::play::encode_light_update(*level.chunk({chunk_x, chunk_z})),
                compression_threshold, cipher ? &*cipher : nullptr);
        };
        const auto settle_and_send = [&](const core::BlockPosition position) {
            block_ticks.notify_neighbors(position);
        };
        const auto send_vehicle_passengers = [&](const entity::Entity& vehicle) {
            std::vector<std::int32_t> passengers;
            passengers.reserve(vehicle.passengers().size());
            for (const auto passenger : vehicle.passengers()) {
                passengers.push_back(static_cast<std::int32_t>(passenger));
            }
            write_compressed_packet(
                descriptor,
                protocol::play::encode_passengers(
                    static_cast<std::int32_t>(vehicle.id()), passengers),
                compression_threshold, cipher ? &*cipher : nullptr);
        };
        const auto sync_hotbar_slot = [&](const std::int16_t slot) {
            write_compressed_packet(
                descriptor,
                protocol::play::encode_player_inventory(
                    36 + slot, simple_stack(hotbar[static_cast<std::size_t>(slot)])),
                compression_threshold, cipher ? &*cipher : nullptr);
        };
        const auto sync_offhand = [&]() {
            write_compressed_packet(
                descriptor, protocol::play::encode_player_inventory(45, simple_stack(offhand)),
                compression_threshold, cipher ? &*cipher : nullptr);
        };
        const auto sync_player_inventory = [&]() {
            std::array<protocol::play::SimpleItemStack, 46> contents{};
            for (std::size_t slot = 0; slot < hotbar.size(); ++slot) {
                contents[36 + slot] = simple_stack(hotbar[slot]);
            }
            contents[45] = simple_stack(offhand);
            write_compressed_packet(
                descriptor,
                protocol::play::encode_container_content(0, 0, contents, {}),
                compression_threshold, cipher ? &*cipher : nullptr);
        };
        const auto select_creative_item = [&](const item::Item& selected) {
            const auto existing = std::find_if(
                hotbar.begin(), hotbar.end(), [&](const auto& stack) {
                    return !stack.empty() && stack.item_id() == selected.id();
                });
            if (existing != hotbar.end()) {
                active_hotbar_slot = static_cast<std::int16_t>(
                    std::distance(hotbar.begin(), existing));
                write_compressed_packet(
                    descriptor, protocol::play::encode_held_slot(active_hotbar_slot),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else {
                hotbar[static_cast<std::size_t>(active_hotbar_slot)] =
                    item::ItemStack(selected.id(), 1);
                sync_hotbar_slot(active_hotbar_slot);
            }
        };
        const auto tool_context = [&]() {
            const auto& held = hotbar[static_cast<std::size_t>(active_hotbar_slot)];
            if (!held.empty() &&
                item_registry.by_id(held.item_id()).name().path() == "wooden_pickaxe") {
                return block::ToolContext{2.0F, true};
            }
            return block::ToolContext{1.0F, false};
        };
        const auto send_statistics = [&](const std::vector<player::StatisticValue>& values) {
            std::vector<protocol::play::StatisticEntry> entries;
            entries.reserve(values.size());
            for (const auto& value : values) {
                entries.push_back({value.type_id, value.value_id, value.value});
            }
            if (!entries.empty()) {
                write_compressed_packet(
                    descriptor, protocol::play::encode_award_stats(entries),
                    compression_threshold, cipher ? &*cipher : nullptr);
            }
        };
            const auto send_experience = [&]() {
                const auto experience = player::experience_from_total(total_experience);
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_experience(
                        experience.progress, experience.total, experience.level),
                    compression_threshold, cipher ? &*cipher : nullptr);
            };
        const auto send_drop_metadata = [&](const entity::EntityId id) {
            const auto* stack = dropped_items.stack(id);
            if (!stack) return;
            const std::array metadata{
                protocol::play::EntityMetadataEntry{8, simple_stack(*stack)}};
            write_compressed_packet(
                descriptor,
                protocol::play::encode_entity_metadata(
                    static_cast<std::int32_t>(id), metadata),
                compression_threshold, cipher ? &*cipher : nullptr);
        };
        const auto spawn_loot = [&](const entity::DeathEvent& death) {
            std::vector<std::pair<std::string_view, std::uint16_t>> loot;
            if (death.type == "cow") loot = {{"beef", 1}, {"leather", 1}};
            else if (death.type == "pig") loot = {{"porkchop", 1}};
            else if (death.type == "chicken") loot = {{"chicken", 1}, {"feather", 1}};
            else if (death.type == "sheep") loot = {{"mutton", 1}};
            else if (death.type == "zombie") loot = {{"rotten_flesh", 1}};
            else if (death.type == "skeleton") loot = {{"bone", 1}, {"arrow", 1}};
            else if (death.type == "spider") loot = {{"string", 1}};
            for (const auto& [name, count] : loot) {
                try {
                    static_cast<void>(dropped_items.spawn(
                        entities,
                        item::ItemStack(item_registry.by_name(name).id(), count),
                        {death.position.x, death.position.y + 0.25, death.position.z}));
                } catch (const std::out_of_range&) {
                }
            }
        };
        const auto kill_player = [&](const std::string_view death_message) {
            if (player_dead) return;
            if (survival.take_dirty()) {
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_health(
                        survival.health(), survival.food_level(), survival.saturation()),
                    compression_threshold, cipher ? &*cipher : nullptr);
            }
            player_dead = true;
            statistics.increment(8, 32);
            write_compressed_packet(
                descriptor, protocol::play::encode_system_chat(death_message, false),
                compression_threshold, cipher ? &*cipher : nullptr);
            if (player_vehicle) {
                if (auto* vehicle = entities.find(*player_vehicle)) {
                    static_cast<void>(entities.remove_external_passenger(vehicle->id(), 1));
                    send_vehicle_passengers(*vehicle);
                }
                player_vehicle.reset();
            }
            if (keep_inventory) return;
            const auto dropped_experience =
                player::experience_drop_on_death(total_experience);
            if (dropped_experience > 0) {
                auto& orb = entities.spawn(
                    "experience_orb",
                    {player_position.x, player_position.y + 0.25, player_position.z});
                experience_orbs.emplace(orb.id(), dropped_experience);
            }
            total_experience = 0;
            send_experience();
            for (std::int16_t slot = 0;
                 slot < static_cast<std::int16_t>(hotbar.size()); ++slot) {
                auto& stack = hotbar[static_cast<std::size_t>(slot)];
                if (!stack.empty()) {
                    static_cast<void>(dropped_items.spawn(
                        entities, stack, player_position, 40));
                }
                stack = {};
                sync_hotbar_slot(slot);
            }
            if (!offhand.empty()) {
                static_cast<void>(dropped_items.spawn(
                    entities, offhand, player_position, 40));
            }
            offhand = {};
            sync_offhand();
        };
        while (!stopping_.load()) {
            flush_pending_output(descriptor, false);
            const auto now = std::chrono::steady_clock::now();
            if (teleport_confirmed && now >= next_game_tick) {
                next_game_tick = now + std::chrono::milliseconds(50);
                ++simulation_ticks;
                statistics.increment(8, 1);
                if (simulation_ticks % 20 == 0) {
                    send_statistics(statistics.drain_updates());
                }
                level.tick_border();
                level.tick_time();
                if (simulation_ticks % 20 == 0) {
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_set_time(
                            static_cast<std::int64_t>(level.game_time()),
                            static_cast<std::int64_t>(level.day_time())),
                        compression_threshold, cipher ? &*cipher : nullptr);
                }
                const auto weather = level.tick_weather();
                if (weather.raining) {
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_game_event(
                            *weather.raining ? 1 : 2, 0.0F),
                        compression_threshold, cipher ? &*cipher : nullptr);
                }
                if (weather.rain_level) {
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_game_event(7, *weather.rain_level),
                        compression_threshold, cipher ? &*cipher : nullptr);
                }
                if (weather.thunder_level) {
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_game_event(8, *weather.thunder_level),
                        compression_threshold, cipher ? &*cipher : nullptr);
                }
                if (now - last_autosave >= config_.autosave_interval) {
                    static_cast<void>(level.save_all());
                    last_autosave = now;
                }
                if (!player_dead && game_mode != GameMode::creative &&
                    game_mode != GameMode::spectator) {
                    const auto eye_y = static_cast<std::int32_t>(
                        std::floor(player_position.y + survival.eye_height()));
                    const auto underwater = eye_y >= world::min_build_y &&
                        eye_y < world::max_build_y &&
                        level.block({
                            static_cast<std::int32_t>(std::floor(player_position.x)),
                            eye_y,
                            static_cast<std::int32_t>(std::floor(player_position.z))}) ==
                            world::BlockId::water;
                    survival.tick(underwater, difficulty);
                    if (survival.take_dirty()) {
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_health(
                                survival.health(), survival.food_level(), survival.saturation()),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                    for (const auto effect_id : survival.drain_expired_effects()) {
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_remove_mob_effect(1, effect_id),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                    if (survival.health() <= 0.0F) {
                        kill_player("CppPlayer died");
                    }
                }
                std::vector<world::ChunkPosition> active_chunks;
                if (simulation_ticks % 20 == 0) {
                    active_chunks.reserve(loaded_chunks.size());
                    for (const auto [chunk_x, chunk_z] : loaded_chunks) {
                        active_chunks.push_back({chunk_x, chunk_z});
                    }
                }
                for (const auto changed : block_ticks.tick(level, active_chunks)) {
                    send_block_update(changed);
                }
                const auto& held = hotbar[static_cast<std::size_t>(active_hotbar_slot)];
                if (!held.empty()) {
                    const auto held_name = item_registry.by_id(held.item_id()).name().path();
                    for (const auto id : entities.ids()) {
                        auto* animal = entities.find(id);
                        if (animal && animals.accepts_food(*animal, held_name) &&
                            (animal->position() - player_position).length_squared() <= 64.0) {
                            static_cast<void>(mob_ai.tempt(id, player_position));
                        }
                    }
                }
                for (const auto id : entities.ids()) {
                    auto* owned = entities.find(id);
                    const auto* state = animals.state(id);
                    if (!owned || !state || !state->tamed || state->owner != profile.id) continue;
                    static_cast<void>(mob_ai.set_suspended(id, state->sitting));
                    if (!state->sitting) {
                        static_cast<void>(mob_ai.follow_owner(id, player_position));
                    }
                }
                mob_ai.tick(0.05);
                for (auto iterator = player_attack_cooldowns.begin();
                     iterator != player_attack_cooldowns.end();) {
                    if (iterator->second > 0) --iterator->second;
                    if (entities.find(iterator->first) == nullptr) {
                        iterator = player_attack_cooldowns.erase(iterator);
                    } else {
                        ++iterator;
                    }
                }
                if (!player_dead && game_mode != GameMode::creative &&
                    game_mode != GameMode::spectator &&
                    difficulty != player::Difficulty::peaceful) {
                    for (const auto id : entities.ids()) {
                        auto* attacker = entities.find(id);
                        if (!attacker || attacker->type().properties().category !=
                                             entity::EntityCategory::monster ||
                            (attacker->position() - player_position).length_squared() > 2.25 ||
                            player_attack_cooldowns[id] > 0) {
                            continue;
                        }
                        const auto damage = survival.damage(
                            player::scale_hostile_damage(2.0F, difficulty),
                            {entity::DamageType::melee, attacker->id(), false, false},
                            survival.blocks_attack(
                                attacker->position(), player_position, player_yaw));
                        if (damage.applied) {
                            player_attack_cooldowns[id] = 20;
                            if (damage.blocked > 0.0F && blocking_hand) {
                                auto& shield = *blocking_hand == 0
                                    ? hotbar[static_cast<std::size_t>(active_hotbar_slot)]
                                    : offhand;
                                static_cast<void>(shield.apply_damage(
                                    static_cast<std::uint16_t>(std::ceil(damage.blocked)),
                                    item_registry));
                                if (*blocking_hand == 0) {
                                    sync_hotbar_slot(active_hotbar_slot);
                                } else {
                                    sync_offhand();
                                }
                                if (shield.empty()) {
                                    survival.set_blocking(false);
                                    blocking_hand.reset();
                                }
                            }
                            write_compressed_packet(
                                descriptor,
                                protocol::play::encode_health(
                                    survival.health(), survival.food_level(),
                                    survival.saturation()),
                                compression_threshold, cipher ? &*cipher : nullptr);
                            write_compressed_packet(
                                descriptor,
                                protocol::play::encode_hurt_animation(1, 0.0F),
                                compression_threshold, cipher ? &*cipher : nullptr);
                            const auto delta_x = player_position.x - attacker->position().x;
                            const auto delta_z = player_position.z - attacker->position().z;
                            const auto length = std::hypot(delta_x, delta_z);
                            if (length > 1.0e-6) {
                                const auto strength = damage.blocked > 0.0F ? 0.2 : 0.4;
                                write_compressed_packet(
                                    descriptor,
                                    protocol::play::encode_entity_motion(
                                        1, {delta_x / length * strength, 0.4,
                                            delta_z / length * strength}),
                                    compression_threshold, cipher ? &*cipher : nullptr);
                            }
                            static_cast<void>(survival.take_dirty());
                        }
                        break;
                    }
                }
                animals.tick(entities, 0.05);
                const auto entities_before_tick = entities.ids();
                if (simulation_ticks % 20 == 0) {
                    for (const auto id : entities_before_tick) {
                        auto* living = dynamic_cast<entity::LivingEntity*>(entities.find(id));
                        if (!living) continue;
                        const auto exposed = entity::daylight_exposed(*living, level);
                        const auto was_burning = daylight_burning.contains(id);
                        if (exposed) daylight_burning.insert(id);
                        else daylight_burning.erase(id);
                        if (exposed != was_burning && entities_visible &&
                            entity_tracker.visible(id)) {
                            const std::array metadata{
                                protocol::play::EntityMetadataEntry{
                                    0, static_cast<std::uint8_t>(exposed ? 0x01 : 0x00)}};
                            write_compressed_packet(
                                descriptor,
                                protocol::play::encode_entity_metadata(
                                    static_cast<std::int32_t>(id), metadata),
                                compression_threshold, cipher ? &*cipher : nullptr);
                        }
                        if (exposed && entity::apply_daylight_burn(*living, level) &&
                            entities_visible && entity_tracker.visible(id)) {
                            write_compressed_packet(
                                descriptor,
                                protocol::play::encode_hurt_animation(
                                    static_cast<std::int32_t>(id), 0.0F),
                                compression_threshold, cipher ? &*cipher : nullptr);
                        }
                    }
                    std::erase_if(daylight_burning, [&](const auto id) {
                        return entities.find(id) == nullptr;
                    });
                }
                entities.tick(0.05);
                for (const auto& impact : projectiles.tick(entities, level)) {
                    if (impact.recovery) {
                        static_cast<void>(dropped_items.spawn(
                            entities, *impact.recovery, impact.position, 10));
                    }
                    if (!impact.target) continue;
                    if (auto* target = entities.find(*impact.target)) {
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_hurt_animation(
                                static_cast<std::int32_t>(*impact.target), 0.0F),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_entity_motion(
                                static_cast<std::int32_t>(*impact.target),
                                {target->velocity().x, target->velocity().y,
                                 target->velocity().z}),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                }
                auto drop_update = dropped_items.tick(
                    entities, item_registry,
                    player_dead ? std::nullopt : std::optional(player_position));
                for (const auto changed : drop_update.changed) {
                    if (entities_visible && entity_tracker.visible(changed)) {
                        send_drop_metadata(changed);
                    }
                }
                for (auto& pickup : drop_update.pickups) {
                    const auto original_count = pickup.stack.count();
                    const auto pickup_protocol_id = pickup.stack.empty()
                        ? std::optional<std::uint32_t>{}
                        : item_registry.by_id(pickup.stack.item_id()).protocol_id();
                    for (std::size_t slot = 0;
                         slot < hotbar.size() && !pickup.stack.empty(); ++slot) {
                        const auto before = hotbar[slot];
                        static_cast<void>(hotbar[slot].insert_from(
                            pickup.stack, item_registry));
                        if (hotbar[slot] != before) {
                            sync_hotbar_slot(static_cast<std::int16_t>(slot));
                        }
                    }
                    const auto moved = static_cast<std::int32_t>(
                        original_count - pickup.stack.count());
                    if (moved > 0 && entities_visible) {
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_take_item_entity(
                                static_cast<std::int32_t>(pickup.entity_id), 1, moved),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                            if (moved > 0 && pickup_protocol_id) {
                            statistics.increment(
                                4, static_cast<std::int32_t>(*pickup_protocol_id), moved);
                            }
                    if (!pickup.stack.empty()) {
                        static_cast<void>(dropped_items.spawn(
                            entities, std::move(pickup.stack), player_position, 10));
                    }
                }
                for (const auto& death : entities.drain_deaths()) {
                    auto& orb = entities.spawn(
                        "experience_orb",
                        {death.position.x, death.position.y + 0.25, death.position.z});
                    experience_orbs.emplace(orb.id(), death.experience);
                    spawn_loot(death);
                }
                const auto entities_after_deaths = entities.ids();
                if (player_vehicle && entities.find(*player_vehicle) == nullptr) {
                    player_vehicle.reset();
                }
                std::vector<std::int32_t> died;
                std::set_difference(
                    entities_before_tick.begin(), entities_before_tick.end(),
                    entities_after_deaths.begin(), entities_after_deaths.end(),
                    std::back_inserter(died));
                if (entities_visible && !died.empty()) {
                    std::erase_if(died, [&](const auto id) {
                        return !entity_tracker.forget(static_cast<entity::EntityId>(id));
                    });
                    if (!died.empty()) {
                        write_compressed_packet(
                            descriptor, protocol::play::encode_remove_entities(died),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                }
                for (const auto id : entities.ids()) {
                    auto* simulated = entities.find(id);
                    if (!simulated) continue;
                    auto position = simulated->position();
                    const auto category = simulated->type().properties().category;
                    const auto grounded = !simulated->type().properties().aquatic &&
                        category != entity::EntityCategory::ambient;
                    const core::BlockPosition fluid_position{
                        static_cast<std::int32_t>(std::floor(position.x)),
                        static_cast<std::int32_t>(std::floor(position.y)),
                        static_cast<std::int32_t>(std::floor(position.z))};
                    const auto immersed = fluid_position.y >= world::min_build_y &&
                        fluid_position.y < world::max_build_y &&
                        level.block(fluid_position) == world::BlockId::water;
                    if (immersed) {
                        simulated->apply_water_physics(
                            simulated->type().properties().aquatic ? 0.9 : 0.8,
                            simulated->type().properties().aquatic ? 0.04 : 0.02);
                    }
                    const auto ground = static_cast<double>(level.surface_height(
                        static_cast<std::int32_t>(std::floor(position.x)),
                        static_cast<std::int32_t>(std::floor(position.z))) + 1);
                    if (grounded && position.y < ground) {
                        position.y = ground;
                        simulated->set_position(position);
                        auto velocity = simulated->velocity();
                        velocity.y = 0.0;
                        simulated->set_velocity(velocity);
                    } else if (simulated->type().properties().aquatic) {
                        if (immersed) {
                            aquatic_positions[simulated->id()] = position;
                        } else if (const auto valid = aquatic_positions.find(simulated->id());
                                   valid != aquatic_positions.end()) {
                            position = valid->second;
                            simulated->set_position(position);
                            simulated->set_velocity({});
                        }
                    }
                    if (entities_visible) {
                        const auto update = entity_tracker.update(*simulated, player_position);
                        const auto entity_id = static_cast<std::int32_t>(simulated->id());
                        if (update.entered) {
                            const auto orb = experience_orbs.find(simulated->id());
                            const auto spawn_data = simulated->type().name().path() == "arrow"
                                ? 1
                                : orb == experience_orbs.end() ? 0 : orb->second;
                            send_entity_spawn(
                                *simulated, spawn_data);
                            if (dropped_items.stack(simulated->id())) {
                                send_drop_metadata(simulated->id());
                            }
                        } else if (update.left) {
                            write_compressed_packet(
                                descriptor,
                                protocol::play::encode_remove_entities(
                                    std::array<std::int32_t, 1>{entity_id}),
                                compression_threshold, cipher ? &*cipher : nullptr);
                        } else {
                            if (update.absolute_position) {
                                write_compressed_packet(
                                    descriptor,
                                    protocol::play::encode_entity_position_sync(
                                        entity_id, {position.x, position.y, position.z},
                                        {simulated->velocity().x, simulated->velocity().y,
                                         simulated->velocity().z},
                                        simulated->yaw(), simulated->pitch(), true),
                                    compression_threshold, cipher ? &*cipher : nullptr);
                            } else if (update.position && update.rotation) {
                                write_compressed_packet(
                                    descriptor,
                                    protocol::play::encode_move_entity_position_rotation(
                                        entity_id,
                                        {update.delta.x, update.delta.y, update.delta.z},
                                        simulated->yaw(), simulated->pitch(), true),
                                    compression_threshold, cipher ? &*cipher : nullptr);
                            } else if (update.position) {
                                write_compressed_packet(
                                    descriptor,
                                    protocol::play::encode_move_entity_position(
                                        entity_id,
                                        {update.delta.x, update.delta.y, update.delta.z}, true),
                                    compression_threshold, cipher ? &*cipher : nullptr);
                            } else if (update.rotation) {
                                write_compressed_packet(
                                    descriptor,
                                    protocol::play::encode_move_entity_rotation(
                                        entity_id, simulated->yaw(), simulated->pitch(), true),
                                    compression_threshold, cipher ? &*cipher : nullptr);
                            }
                            if (update.velocity) {
                                write_compressed_packet(
                                    descriptor,
                                    protocol::play::encode_entity_motion(
                                        entity_id,
                                        {simulated->velocity().x, simulated->velocity().y,
                                         simulated->velocity().z}),
                                    compression_threshold, cipher ? &*cipher : nullptr);
                            }
                            if (update.head_rotation) {
                                write_compressed_packet(
                                    descriptor,
                                    protocol::play::encode_rotate_head(
                                        entity_id, simulated->yaw()),
                                    compression_threshold, cipher ? &*cipher : nullptr);
                            }
                        }
                    }
                }
                if (player_vehicle) {
                    if (const auto* vehicle = entities.find(*player_vehicle)) {
                        player_position = {
                            vehicle->position().x,
                            vehicle->position().y + vehicle->type().properties().height,
                            vehicle->position().z};
                    }
                }
                for (auto iterator = experience_orbs.begin();
                     iterator != experience_orbs.end();) {
                    auto* orb = entities.find(iterator->first);
                    if (!orb) {
                        iterator = experience_orbs.erase(iterator);
                        continue;
                    }
                    if ((orb->position() - player_position).length_squared() > 2.25) {
                        ++iterator;
                        continue;
                    }
                    const auto orb_id = static_cast<std::int32_t>(iterator->first);
                    const auto value = iterator->second;
                    total_experience = std::min<std::int32_t>(
                        std::numeric_limits<std::int32_t>::max() - value,
                        total_experience) + value;
                    if (entities_visible) {
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_take_item_entity(orb_id, 1, value),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_remove_entities(
                                std::array<std::int32_t, 1>{orb_id}),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                    static_cast<void>(entity_tracker.forget(iterator->first));
                    static_cast<void>(entities.remove(iterator->first));
                    iterator = experience_orbs.erase(iterator);

                    send_experience();
                }
                if (simulation_ticks % 200 == 0) {
                    const auto before = entities.ids();
                    if (spawn_mobs) {
                        const auto candidates = spawn_candidates(player_position, 20.0);
                        auto spawned = natural_spawner.spawn_cycle(
                            entities, entity::EntityCategory::monster,
                            candidates, level.daylight() ? 15 : 0,
                            difficulty == player::Difficulty::peaceful, 2);
                        const auto passive_spawned = natural_spawner.spawn_cycle(
                            entities, entity::EntityCategory::creature,
                            candidates, 15, false, 2);
                        spawned.insert(
                            spawned.end(), passive_spawned.begin(), passive_spawned.end());
                        for (const auto category : {
                                 entity::EntityCategory::water_ambient,
                                 entity::EntityCategory::water_creature}) {
                            const auto aquatic_spawned = natural_spawner.spawn_cycle(
                                entities, category, candidates, 15, false, 2);
                            spawned.insert(
                                spawned.end(), aquatic_spawned.begin(), aquatic_spawned.end());
                        }
                        for (const auto id : spawned) {
                            static_cast<void>(mob_ai.attach(id));
                            auto* spawned_entity = entities.find(id);
                            if (spawned_entity && spawned_entity->type().properties().category ==
                                                      entity::EntityCategory::creature) {
                                static_cast<void>(animals.attach(*spawned_entity));
                            } else if (spawned_entity &&
                                       spawned_entity->type().properties().aquatic) {
                                aquatic_positions[id] = spawned_entity->position();
                            } else if (!creature_ids.empty()) {
                                static_cast<void>(mob_ai.set_target(id, creature_ids.front()));
                            }
                            static_cast<void>(spawned_entity);
                        }
                    }
                    static_cast<void>(natural_spawner.despawn_distant(
                        entities, {player_position}));
                    const auto after = entities.ids();
                    std::vector<std::int32_t> removed;
                    std::set_difference(
                        before.begin(), before.end(), after.begin(), after.end(),
                        std::back_inserter(removed));
                    if (entities_visible && !removed.empty()) {
                        std::erase_if(removed, [&](const auto id) {
                            return !entity_tracker.forget(static_cast<entity::EntityId>(id));
                        });
                        if (!removed.empty()) {
                            write_compressed_packet(
                                descriptor, protocol::play::encode_remove_entities(removed),
                                compression_threshold, cipher ? &*cipher : nullptr);
                        }
                    }
                }
            }
            if (!pending_keep_alive &&
                now - last_keep_alive >= config_.keep_alive_interval) {
                pending_keep_alive = now.time_since_epoch().count();
                last_keep_alive = now;
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_keep_alive(*pending_keep_alive),
                    compression_threshold,
                    cipher ? &*cipher : nullptr);
            }
            if (pending_keep_alive && now - last_keep_alive >= config_.keep_alive_timeout) {
                throw protocol::DecodeError("Play keepalive timed out");
            }
            if (now - last_activity >= config_.idle_timeout) {
                throw protocol::DecodeError("Play idle timeout expired");
            }
			if (!teleport_confirmed && now - teleport_sent_at >= std::chrono::seconds(10)) {
				throw protocol::DecodeError("Play teleport acknowledgement timed out");
			}

            const auto requested_events = static_cast<short>(
                socket_read_event |
                (active_output_queue != nullptr && !active_output_queue->empty()
                    ? socket_write_event
                    : 0));
            PollDescriptor event{descriptor, requested_events, 0};
            const auto ready = poll_socket(&event, 50);
            if (ready < 0) {
                const auto error = socket_error();
                if (interrupted_socket_error(error)) {
                    continue;
                }
                throw std::runtime_error("Play poll failed: " + socket_error_text(error));
            }
            if (ready == 0) {
                continue;
            }
            if ((event.revents & socket_write_event) != 0) {
                flush_pending_output(descriptor, false);
            }
            if ((event.revents & socket_read_event) == 0) {
                if (active_output_queue != nullptr && !active_output_queue->empty()) {
                    continue;
                }
                throw protocol::DecodeError("peer closed during Play");
            }

            const auto packet = read_compressed_packet(
                descriptor, compression_threshold, cipher ? &*cipher : nullptr);
            protocol::Reader packet_id_reader(packet);
            const auto packet_id = packet_id_reader.read_varint();
            if (packet_id != static_cast<std::int32_t>(
                    protocol::play::ServerboundPacketId::keep_alive) &&
                packet_id != static_cast<std::int32_t>(
                    protocol::play::ServerboundPacketId::pong)) {
                last_activity = now;
            }
            if (player_dead &&
                packet_id != static_cast<std::int32_t>(
                    protocol::play::ServerboundPacketId::client_command) &&
                packet_id != static_cast<std::int32_t>(
                    protocol::play::ServerboundPacketId::keep_alive) &&
                packet_id != static_cast<std::int32_t>(
                    protocol::play::ServerboundPacketId::pong) &&
                packet_id != static_cast<std::int32_t>(
                    protocol::play::ServerboundPacketId::client_tick_end)) {
                continue;
            }
            if (packet_id == static_cast<std::int32_t>(
                    protocol::play::ServerboundPacketId::client_command)) {
                protocol::Reader command_reader(packet);
                const auto action = protocol::play::decode_client_command(command_reader);
                if (action == protocol::play::ClientCommandAction::perform_respawn &&
                    player_dead) {
                    const auto previous_game_mode = game_mode;
                    if (config_.hardcore) game_mode = GameMode::spectator;
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_respawn(
                            false, static_cast<std::uint8_t>(game_mode),
                            config_.hardcore
                                ? static_cast<std::int8_t>(previous_game_mode)
                                : std::int8_t{-1}),
                        compression_threshold, cipher ? &*cipher : nullptr);
                    survival.reset();
                    survival.set_flying(game_mode == GameMode::spectator);
                    player_dead = false;
                    if (!keep_inventory) total_experience = 0;
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_health(
                            survival.health(), survival.food_level(), survival.saturation()),
                        compression_threshold, cipher ? &*cipher : nullptr);
                    send_experience();
                    if (config_.hardcore) {
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_player_abilities(
                                true, true, true, false, 0.05F, 0.1F),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                    static_cast<void>(survival.take_dirty());
                    player_position = initial_player_position;
                    player_yaw = 0.0F;
                    player_center = {0, 0};
                    if (streamed_center != player_center) stream_chunk_window(player_center);
                    ++next_teleport_id;
                    pending_teleport = next_teleport_id;
                    teleport_confirmed = false;
                    teleport_sent_at = now;
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_player_position(
                            next_teleport_id, player_position.x, player_position.y,
                            player_position.z, 0.0F, 0.0F),
                        compression_threshold, cipher ? &*cipher : nullptr);
                } else if (action == protocol::play::ClientCommandAction::request_stats) {
                    send_statistics(statistics.snapshot());
                } else if (action ==
                           protocol::play::ClientCommandAction::request_gamerule_values) {
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_game_rule_values(initial_game_rules),
                        compression_threshold, cipher ? &*cipher : nullptr);
                }
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::chat_ack)) {
                protocol::Reader acknowledgement_reader(packet);
                protocol::play::decode_empty_chat_ack(acknowledgement_reader);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::chat_command)) {
                protocol::Reader command_reader(packet);
                const auto command = protocol::play::decode_chat_command(command_reader);
                auto response = std::string("Unknown command");
                if (command.starts_with("say ") && command.size() > 4) {
                    response = "[CppPlayer] " + command.substr(4);
                } else if (command == "kill") {
                    static_cast<void>(survival.damage(
                        std::numeric_limits<float>::max(),
                        {entity::DamageType::void_damage, std::nullopt, true, true}));
                    kill_player("CppPlayer was killed");
                    response = "Killed CppPlayer";
                } else if (command.starts_with("teleport ") || command.starts_with("tp ")) {
                    std::istringstream input(command);
                    std::string root;
                    double x = 0.0;
                    double y = 0.0;
                    double z = 0.0;
                    std::string trailing;
                    if ((input >> root >> x >> y >> z) && !(input >> trailing) &&
                        std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
                        y >= world::min_build_y && y <= world::max_build_y - 2 &&
                        level.inside_border({x, y, z}, 0.3)) {
                        if (player_vehicle) {
                            if (auto* vehicle = entities.find(*player_vehicle)) {
                                static_cast<void>(entities.remove_external_passenger(
                                    vehicle->id(), 1));
                                send_vehicle_passengers(*vehicle);
                            }
                            player_vehicle.reset();
                        }
                        pending_break.reset();
                        survival.set_blocking(false);
                        blocking_hand.reset();
                        player_position = {x, y, z};
                        player_yaw = 0.0F;
                        player_center = {
                            static_cast<std::int32_t>(std::floor(x / 16.0)),
                            static_cast<std::int32_t>(std::floor(z / 16.0))};
                        if (streamed_center != player_center) {
                            stream_chunk_window(player_center);
                        }
                        ++next_teleport_id;
                        pending_teleport = next_teleport_id;
                        teleport_confirmed = false;
                        teleport_sent_at = now;
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_player_position(
                                next_teleport_id, x, y, z, 0.0F, 0.0F),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        response = "Teleported CppPlayer";
                    } else {
                        response = "Invalid teleport destination";
                    }
                } else if (command.starts_with("summon ")) {
                    const auto type_name = command.substr(7);
                    try {
                        const auto& type = entity_registry.by_name(type_name);
                        if (type.properties().max_health <= 0.0F ||
                            type.properties().category == entity::EntityCategory::misc) {
                            throw std::out_of_range("entity type is not living");
                        }
                        auto& summoned = entities.spawn(
                            type.name().path(),
                            {player_position.x + 1.0, player_position.y, player_position.z});
                        static_cast<void>(mob_ai.attach(summoned.id()));
                        if (summoned.type().properties().category ==
                            entity::EntityCategory::creature) {
                            static_cast<void>(animals.attach(summoned));
                        }
                        if (summoned.type().properties().aquatic) {
                            aquatic_positions[summoned.id()] = summoned.position();
                        }
                        if (entities_visible &&
                            entity_tracker.update(summoned, player_position).entered) {
                            send_entity_spawn(summoned);
                        }
                        response = "Summoned " + type.name().to_string();
                    } catch (const std::out_of_range&) {
                        response = "Invalid summon entity";
                    }
                } else if (command.starts_with("give ")) {
                    std::istringstream input(command);
                    std::string root;
                    std::string item_name;
                    std::int32_t count = 1;
                    std::string trailing;
                    input >> root >> item_name;
                    if (!(input >> count)) {
                        input.clear();
                        count = 1;
                    }
                    if (item_name.empty() || count <= 0 || count > 256 ||
                        (input >> trailing)) {
                        response = "Invalid give request";
                    } else {
                        try {
                            const auto& given_item = item_registry.by_name(item_name);
                            if (given_item.id() == 0) {
                                throw std::out_of_range("air cannot be given");
                            }
                            auto remaining = static_cast<std::uint32_t>(count);
                            while (remaining > 0) {
                                const auto batch = static_cast<std::uint16_t>(std::min(
                                    remaining,
                                    static_cast<std::uint32_t>(
                                        given_item.properties().max_stack_size)));
                                item::ItemStack stack(given_item.id(), batch);
                                for (std::size_t slot = 0;
                                     slot < hotbar.size() && !stack.empty(); ++slot) {
                                    const auto before = hotbar[slot];
                                    static_cast<void>(hotbar[slot].insert_from(
                                        stack, item_registry));
                                    if (hotbar[slot] != before) {
                                        sync_hotbar_slot(static_cast<std::int16_t>(slot));
                                    }
                                }
                                if (!stack.empty()) {
                                    static_cast<void>(dropped_items.spawn(
                                        entities, std::move(stack), player_position, 20));
                                }
                                remaining -= batch;
                            }
                            response = "Gave " + std::to_string(count) + " " + item_name;
                        } catch (const std::out_of_range&) {
                            response = "Unknown item";
                        }
                    }
                } else if (command == "clear" || command.starts_with("clear ")) {
                    std::istringstream input(command);
                    std::string root;
                    std::string item_name;
                    std::string trailing;
                    input >> root;
                    static_cast<void>(input >> item_name);
                    std::optional<std::uint32_t> item_id;
                    auto valid = !(input >> trailing);
                    if (valid && !item_name.empty()) {
                        try {
                            item_id = item_registry.by_name(item_name).id();
                        } catch (const std::out_of_range&) {
                            valid = false;
                        }
                    }
                    std::uint32_t removed = 0;
                    if (valid) {
                        for (std::size_t slot = 0; slot < hotbar.size(); ++slot) {
                            auto& stack = hotbar[slot];
                            if (stack.empty() || (item_id && stack.item_id() != *item_id)) {
                                continue;
                            }
                            removed += stack.count();
                            stack = {};
                            sync_hotbar_slot(static_cast<std::int16_t>(slot));
                        }
                        if (!offhand.empty() && (!item_id || offhand.item_id() == *item_id)) {
                            removed += offhand.count();
                            offhand = {};
                            sync_offhand();
                        }
                        response = "Cleared " + std::to_string(removed) + " items";
                    } else {
                        response = "Invalid clear request";
                    }
                } else if (command.starts_with("experience ") ||
                           command.starts_with("xp ")) {
                    std::istringstream input(command);
                    std::string root;
                    std::string action;
                    std::int64_t amount = -1;
                    std::string trailing;
                    input >> root >> action >> amount;
                    if ((action != "set" && action != "add") || amount < 0 ||
                        amount > std::numeric_limits<std::int32_t>::max() ||
                        (input >> trailing)) {
                        response = "Invalid experience request";
                    } else {
                        if (action == "set") {
                            total_experience = static_cast<std::int32_t>(amount);
                        } else {
                            total_experience = amount >
                                    std::numeric_limits<std::int32_t>::max() -
                                        total_experience
                                ? std::numeric_limits<std::int32_t>::max()
                                : total_experience + static_cast<std::int32_t>(amount);
                        }
                        send_experience();
                        response = "Set experience to " +
                            std::to_string(total_experience);
                    }
                } else if (command == "time set day") {
                    level.set_day_time(1'000);
                    response = "Set time to day";
                } else if (command == "time set night") {
                    level.set_day_time(13'000);
                    response = "Set time to night";
                } else if (command.starts_with("time set ")) {
                    std::uint64_t value = 0;
                    const auto text = std::string_view(command).substr(9);
                    const auto [end, error] = std::from_chars(
                        text.data(), text.data() + text.size(), value);
                    if (error == std::errc{} && end == text.data() + text.size()) {
                        level.set_day_time(value);
                        response = "Set time";
                    }
                } else if (command.starts_with("weather ")) {
                    std::istringstream input(command);
                    std::string root;
                    std::string weather_name;
                    std::string duration_text;
                    std::string trailing;
                    input >> root >> weather_name;
                    std::int32_t duration_seconds = 600;
                    auto valid_duration = true;
                    if (input >> duration_text) {
                        const auto [end, error] = std::from_chars(
                            duration_text.data(),
                            duration_text.data() + duration_text.size(), duration_seconds);
                        valid_duration = error == std::errc{} &&
                            end == duration_text.data() + duration_text.size() &&
                            duration_seconds > 0 && duration_seconds <= 1'000'000;
                    }
                    input.clear();
                    const auto valid_weather = weather_name == "clear" ||
                        weather_name == "rain" || weather_name == "thunder";
                    if (valid_weather && valid_duration && !(input >> trailing)) {
                        const auto raining = weather_name != "clear";
                        const auto thunder = weather_name == "thunder";
                        level.set_weather(
                            raining, thunder,
                            static_cast<std::uint32_t>(duration_seconds) * 20U);
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_game_event(raining ? 1 : 2, 0.0F),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        response = "Set weather to " + weather_name;
                    } else {
                        response = "Invalid weather";
                    }
                } else if (command.starts_with("gamemode ")) {
                    const std::map<std::string, GameMode> game_modes{
                        {"survival", GameMode::survival},
                        {"creative", GameMode::creative},
                        {"adventure", GameMode::adventure},
                        {"spectator", GameMode::spectator}};
                    const auto selected = game_modes.find(command.substr(9));
                    if (selected == game_modes.end()) {
                        response = "Invalid game mode";
                    } else {
                        game_mode = selected->second;
                        const auto creative = game_mode == GameMode::creative;
                        const auto spectator = game_mode == GameMode::spectator;
                        survival.set_flying(spectator);
                        survival.set_blocking(false);
                        blocking_hand.reset();
                        if (spectator) {
                            pending_break.reset();
                            if (player_vehicle) {
                                if (auto* vehicle = entities.find(*player_vehicle)) {
                                    static_cast<void>(entities.remove_external_passenger(
                                        vehicle->id(), 1));
                                    send_vehicle_passengers(*vehicle);
                                }
                                player_vehicle.reset();
                            }
                        }
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_game_event(
                                3, static_cast<float>(game_mode)),
                        compression_threshold, cipher ? &*cipher : nullptr);
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_player_abilities(
                            creative || spectator, survival.flying(),
                                creative || spectator, creative,
                            0.05F, 0.1F),
                        compression_threshold, cipher ? &*cipher : nullptr);
                        response = "Set game mode to " + command.substr(9);
                    }
                } else if (command.starts_with("gamerule ")) {
                    std::istringstream input(command);
                    std::string root;
                    std::string rule;
                    std::string value;
                    std::string trailing;
                    input >> root >> rule >> value;
                    const auto enabled = value == "true";
                    const auto valid_value = enabled || value == "false";
                    const auto no_trailing = !(input >> trailing);
                    std::size_t rule_index = initial_game_rules.size();
                    if (rule == "keepInventory") rule_index = 1;
                    else if (rule == "doDaylightCycle" || rule == "advance_time") {
                        rule_index = 0;
                    } else if (rule == "doMobSpawning" || rule == "spawn_mobs") {
                        rule_index = 2;
                    } else if (rule == "doWeatherCycle" || rule == "advance_weather") {
                        rule_index = 3;
                    }
                    if (!valid_value || !no_trailing ||
                        rule_index == initial_game_rules.size()) {
                        response = "Invalid gamerule";
                    } else {
                        if (rule_index == 0) {
                            advance_time = enabled;
                            level.set_advance_time(enabled);
                            level.set_game_rule("doDaylightCycle", value);
                        } else if (rule_index == 1) {
                            keep_inventory = enabled;
                            level.set_game_rule("keepInventory", value);
                        } else if (rule_index == 2) {
                            spawn_mobs = enabled;
                            level.set_game_rule("doMobSpawning", value);
                        } else {
                            advance_weather = enabled;
                            level.set_weather_cycle(enabled);
                            level.set_game_rule("doWeatherCycle", value);
                        }
                        initial_game_rules[rule_index].second = enabled ? "true" : "false";
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_game_rule_values(initial_game_rules),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        response = "Gamerule " + rule + " set to " + value;
                    }
                } else if (command.starts_with("difficulty ")) {
                    const std::map<std::string, player::Difficulty> difficulties{
                        {"peaceful", player::Difficulty::peaceful},
                        {"easy", player::Difficulty::easy},
                        {"normal", player::Difficulty::normal},
                        {"hard", player::Difficulty::hard}};
                    const auto selected = difficulties.find(command.substr(11));
                    if (config_.hardcore) {
                        response = "Difficulty is locked in Hardcore mode";
                    } else if (selected == difficulties.end()) {
                        response = "Invalid difficulty";
                    } else {
                        difficulty = selected->second;
                        level.set_difficulty(static_cast<std::uint8_t>(difficulty));
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_change_difficulty(
                                static_cast<std::uint8_t>(difficulty), false),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        if (difficulty == player::Difficulty::peaceful) {
                            std::vector<std::int32_t> removed;
                            for (const auto id : entities.ids()) {
                                const auto* candidate = entities.find(id);
                                if (!candidate || candidate->type().properties().category !=
                                                      entity::EntityCategory::monster) {
                                    continue;
                                }
                                if (entity_tracker.forget(id)) {
                                    removed.push_back(static_cast<std::int32_t>(id));
                                }
                                static_cast<void>(entities.remove(id));
                                player_attack_cooldowns.erase(id);
                            }
                            if (entities_visible && !removed.empty()) {
                                write_compressed_packet(
                                    descriptor,
                                    protocol::play::encode_remove_entities(removed),
                                    compression_threshold, cipher ? &*cipher : nullptr);
                            }
                        }
                        response = "Set difficulty to " + command.substr(11);
                    }
                } else if (command.starts_with("worldborder center ")) {
                    std::istringstream input(command);
                    std::string root;
                    std::string action;
                    double center_x = 0.0;
                    double center_z = 0.0;
                    std::string trailing;
                    if ((input >> root >> action >> center_x >> center_z) &&
                        !(input >> trailing) && std::isfinite(center_x) &&
                        std::isfinite(center_z) && std::abs(center_x) <= 29'999'984.0 &&
                        std::abs(center_z) <= 29'999'984.0) {
                        level.set_border_center(center_x, center_z);
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_border_center(center_x, center_z),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        response = "Set world border center";
                    } else {
                        response = "Invalid world border center";
                    }
                } else if (command.starts_with("worldborder set ")) {
                    std::istringstream input(command);
                    std::string root;
                    std::string action;
                    double size = 0.0;
                    std::optional<std::int32_t> duration_seconds;
                    std::string duration_text;
                    std::string trailing;
                    auto valid_duration = true;
                    if ((input >> root >> action >> size) && (input >> duration_text)) {
                        std::int32_t seconds = 0;
                        const auto [end, error] = std::from_chars(
                            duration_text.data(),
                            duration_text.data() + duration_text.size(), seconds);
                        valid_duration = error == std::errc{} &&
                            end == duration_text.data() + duration_text.size() && seconds > 0;
                        if (valid_duration) duration_seconds = seconds;
                    }
                    input.clear();
                    if (std::isfinite(size) && size > 0.0 && size <= 59'999'968.0 &&
                        valid_duration && !(input >> trailing)) {
                        if (duration_seconds) {
                            const auto old_size = level.border().size;
                            level.set_border_lerp_size(
                                size, static_cast<std::uint64_t>(*duration_seconds) * 20U);
                            write_compressed_packet(
                                descriptor,
                                protocol::play::encode_border_lerp_size(
                                    old_size, size,
                                    static_cast<std::int64_t>(*duration_seconds) * 1'000),
                                compression_threshold, cipher ? &*cipher : nullptr);
                        } else {
                            level.set_border_size(size);
                            write_compressed_packet(
                                descriptor, protocol::play::encode_border_size(size),
                                compression_threshold, cipher ? &*cipher : nullptr);
                        }
                        response = "Set world border size";
                    } else {
                        response = "Invalid world border size";
                    }
                } else if (command.starts_with("worldborder warning distance ")) {
                    std::istringstream input(command);
                    std::string root;
                    std::string warning;
                    std::string setting;
                    std::int32_t blocks = -1;
                    std::string trailing;
                    if ((input >> root >> warning >> setting >> blocks) && blocks >= 0 &&
                        !(input >> trailing)) {
                        level.set_border_warning_distance(blocks);
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_border_warning_distance(blocks),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        response = "Set world border warning distance";
                    } else {
                        response = "Invalid world border warning distance";
                    }
                } else if (command.starts_with("worldborder warning time ")) {
                    std::istringstream input(command);
                    std::string root;
                    std::string warning;
                    std::string setting;
                    std::int32_t seconds = -1;
                    std::string trailing;
                    if ((input >> root >> warning >> setting >> seconds) && seconds >= 0 &&
                        !(input >> trailing)) {
                        level.set_border_warning_time(seconds);
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_border_warning_delay(seconds),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        response = "Set world border warning time";
                    } else {
                        response = "Invalid world border warning time";
                    }
                } else if (command.starts_with("effect give ")) {
                    std::istringstream input(command);
                    std::string root;
                    std::string action;
                    std::string effect_name;
                    std::int32_t seconds = 30;
                    std::int32_t amplifier = 0;
                    input >> root >> action >> effect_name;
                    if (!(input >> seconds)) seconds = 30;
                    if (!(input >> amplifier)) amplifier = 0;
                    const std::map<std::string, std::int32_t> effect_ids{
                        {"speed", 0}, {"slowness", 1}, {"regeneration", 9},
                        {"fire_resistance", 11}, {"poison", 18}, {"wither", 19},
                        {"absorption", 21}};
                    const auto effect = effect_ids.find(effect_name);
                    if (effect != effect_ids.end() && seconds > 0 && seconds <= 86'400 &&
                        amplifier >= 0 && amplifier <= 255 &&
                        survival.apply_effect(
                            effect->second,
                            {static_cast<std::uint8_t>(amplifier),
                             static_cast<std::uint32_t>(seconds * 20), false, true})) {
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_update_mob_effect(
                                1, effect->second, amplifier, seconds * 20,
                                false, true, true, false),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        response = "Applied effect " + effect_name;
                    } else {
                        response = "Invalid effect";
                    }
                } else if (command.starts_with("effect clear")) {
                    const std::map<std::string, std::int32_t> effect_ids{
                        {"speed", 0}, {"slowness", 1}, {"regeneration", 9},
                        {"fire_resistance", 11}, {"poison", 18}, {"wither", 19},
                        {"absorption", 21}};
                    const auto name = command.size() > 13 ? command.substr(13) : std::string{};
                    std::vector<std::int32_t> removed;
                    if (name.empty()) {
                        for (const auto& [effect_id, effect] : survival.effects()) {
                            static_cast<void>(effect);
                            removed.push_back(effect_id);
                        }
                    } else if (const auto found = effect_ids.find(name);
                               found != effect_ids.end()) {
                        removed.push_back(found->second);
                    }
                    for (const auto effect_id : removed) {
                        if (!survival.remove_effect(effect_id)) continue;
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_remove_mob_effect(1, effect_id),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                    response = removed.empty() ? "No effects cleared" : "Cleared effects";
                }
                write_compressed_packet(
                    descriptor, protocol::play::encode_system_chat(response, false),
                    compression_threshold, cipher ? &*cipher : nullptr);
                if (command.starts_with("time set ")) {
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_set_time(
                            static_cast<std::int64_t>(level.game_time()),
                            static_cast<std::int64_t>(level.day_time())),
                        compression_threshold, cipher ? &*cipher : nullptr);
                }
            } else if (packet_id == static_cast<std::int32_t>(
                    protocol::play::ServerboundPacketId::accept_teleportation)) {
                protocol::Reader acknowledgement(packet);
                const auto acknowledged =
                    protocol::play::decode_teleport_acknowledgement(acknowledgement);
                if (!pending_teleport || acknowledged != *pending_teleport) {
                    throw protocol::DecodeError(
                        "Play teleport acknowledgement does not match request");
                }
                pending_teleport.reset();
                teleport_confirmed = true;
                entities_visible = true;
                for (const auto id : entities.ids()) {
                    const auto* tracked = entities.find(id);
                    if (tracked && entity_tracker.update(*tracked, player_position).entered) {
                        send_entity_spawn(*tracked);
                    }
                }
                if (player_center != streamed_center) {
                    stream_chunk_window(player_center);
                }
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::block_entity_tag_query)) {
                protocol::Reader query_reader(packet);
                const auto query =
                    protocol::play::decode_block_entity_tag_query(query_reader);
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_tag_query(query.transaction_id),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::entity_tag_query)) {
                protocol::Reader query_reader(packet);
                const auto query = protocol::play::decode_entity_tag_query(query_reader);
                static_cast<void>(entities.find(
                    static_cast<entity::EntityId>(query.entity_id)));
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_tag_query(query.transaction_id),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::attack)) {
                protocol::Reader attack_reader(packet);
                const auto target_id = protocol::play::decode_attack(attack_reader);
                if (game_mode == GameMode::spectator) continue;
                auto* target = dynamic_cast<entity::LivingEntity*>(
                    entities.find(static_cast<entity::EntityId>(target_id)));
                if (target && (target->position() - player_position).length_squared() <= 36.0 &&
                    level.line_of_sight(
                        {player_position.x, player_position.y + survival.eye_height(),
                         player_position.z},
                        {target->position().x,
                         target->position().y + target->type().properties().height * 0.5,
                         target->position().z})) {
                    const auto attack = survival.attack(target->id());
                    const auto damage = 4.0F * attack.damage_multiplier;
                    if (!target->damage(
                            damage,
                            {entity::DamageType::melee, std::nullopt, false, false})) {
                        continue;
                    }
                    static_cast<void>(mob_ai.notify_damage(target->id(), player_position));
                    static_cast<void>(target->knockback(
                        player_position, survival.sprinting() ? 0.8 : 0.4));
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_entity_motion(
                            target_id,
                            {target->velocity().x, target->velocity().y, target->velocity().z}),
                        compression_threshold, cipher ? &*cipher : nullptr);
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_hurt_animation(target_id, 0.0F),
                        compression_threshold, cipher ? &*cipher : nullptr);
                    if (attack.critical) {
                        write_compressed_packet(
                            descriptor, protocol::play::encode_animate(target_id, 4),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                    if (attack.sweeping) {
                        const auto center = target->position();
                        for (const auto nearby_id : entities.query({
                                 {center.x - 1.5, center.y - 0.5, center.z - 1.5},
                                 {center.x + 1.5, center.y + 2.5, center.z + 1.5}})) {
                            if (nearby_id == target->id()) continue;
                            auto* nearby = dynamic_cast<entity::LivingEntity*>(
                                entities.find(nearby_id));
                            if (!nearby || !level.line_of_sight(
                                    {player_position.x,
                                     player_position.y + survival.eye_height(),
                                     player_position.z},
                                    {nearby->position().x,
                                     nearby->position().y +
                                         nearby->type().properties().height * 0.5,
                                     nearby->position().z}) ||
                                !nearby->damage(
                                    1.0F,
                                    {entity::DamageType::melee, std::nullopt, false, false})) {
                                continue;
                            }
                            static_cast<void>(nearby->knockback(player_position, 0.2));
                            write_compressed_packet(
                                descriptor,
                                protocol::play::encode_hurt_animation(
                                    static_cast<std::int32_t>(nearby_id), 0.0F),
                                compression_threshold, cipher ? &*cipher : nullptr);
                        }
                    }
                    if (!target->alive()) {
                        statistics.increment(
                            6, static_cast<std::int32_t>(target->type().protocol_id().value()));
                        target->set_velocity({});
                    }
                }
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::keep_alive)) {
                protocol::Reader keep_alive_reader(packet);
                const auto response = protocol::play::decode_keep_alive(keep_alive_reader);
                if (!pending_keep_alive || response != *pending_keep_alive) {
                    throw protocol::DecodeError(
                        "Play keepalive response does not match request");
                }
                latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_keep_alive);
                static_cast<void>(latency);
                pending_keep_alive.reset();
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::interact)) {
                protocol::Reader interaction_reader(packet);
                const auto interaction = protocol::play::decode_interact(interaction_reader);
                if (game_mode == GameMode::spectator) continue;
                auto* vehicle = entities.find(
                    static_cast<entity::EntityId>(interaction.entity_id));
                if (vehicle && dynamic_cast<entity::LivingEntity*>(vehicle) != nullptr &&
                    vehicle->type().properties().category == entity::EntityCategory::creature &&
                    (vehicle->position() - player_position).length_squared() <= 16.0) {
                    auto& held = hotbar[static_cast<std::size_t>(active_hotbar_slot)];
                    const auto held_name = held.empty()
                        ? std::string_view{}
                        : std::string_view(item_registry.by_id(held.item_id()).name().path());
                    auto* animal_state = animals.state(vehicle->id());
                    if (animal_state && animal_state->tamed &&
                        animal_state->owner == profile.id && interaction.secondary_action &&
                        animals.toggle_sitting(vehicle->id(), profile.id)) {
                        static_cast<void>(mob_ai.set_suspended(
                            vehicle->id(), animals.state(vehicle->id())->sitting));
                        continue;
                    }
                    if (animal_state && !animal_state->tamed && !held.empty() &&
                        animals.accepts_taming_item(*vehicle, held_name) &&
                        animals.tame(vehicle->id(), profile.id)) {
                        static_cast<void>(held.take(1));
                        sync_hotbar_slot(active_hotbar_slot);
                        static_cast<void>(mob_ai.tempt(vehicle->id(), player_position));
                        continue;
                    }
                    if (!held.empty() && animals.accepts_food(*vehicle, held_name) &&
                        animals.set_in_love(vehicle->id())) {
                        static_cast<void>(held.take(1));
                        sync_hotbar_slot(active_hotbar_slot);
                        for (const auto nearby_id : entities.query({
                                 {vehicle->position().x - 3.0, vehicle->position().y - 1.0,
                                  vehicle->position().z - 3.0},
                                 {vehicle->position().x + 3.0, vehicle->position().y + 2.0,
                                  vehicle->position().z + 3.0}})) {
                            if (nearby_id == vehicle->id()) continue;
                            if (const auto child = animals.breed(
                                    vehicle->id(), nearby_id, entities)) {
                                static_cast<void>(mob_ai.attach(*child));
                                break;
                            }
                        }
                        continue;
                    }
                    if (player_vehicle) {
                        if (auto* previous = entities.find(*player_vehicle)) {
                            static_cast<void>(entities.remove_external_passenger(
                                previous->id(), 1));
                            send_vehicle_passengers(*previous);
                        }
                    }
                    if (entities.add_external_passenger(vehicle->id(), 1)) {
                        player_vehicle = vehicle->id();
                        send_vehicle_passengers(*vehicle);
                    }
                }
			} else if (packet_id == static_cast<std::int32_t>(
						   protocol::play::ServerboundPacketId::chunk_batch_received)) {
				protocol::Reader feedback_reader(packet);
				static_cast<void>(
					protocol::play::decode_chunk_batch_received(feedback_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::move_player_pos) ||
                       packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::move_player_pos_rot)) {
                protocol::Reader movement_reader(packet);
                const auto position = protocol::play::decode_player_position(movement_reader);
                if (position.yaw) {
                    player_yaw = *position.yaw;
                    write_compressed_packet(
                        descriptor, protocol::play::encode_rotate_head(1, player_yaw),
                        compression_threshold, cipher ? &*cipher : nullptr);
                }
                const entity::Vec3 next_position{position.x, position.y, position.z};
                const auto horizontal = std::hypot(
                    next_position.x - player_position.x,
                    next_position.z - player_position.z);
                const auto delta = next_position - player_position;
                const auto distance_squared = delta.length_squared();
                const auto invalid_movement = !player_vehicle &&
                    (distance_squared > 4'096.0 ||
                     !level.inside_border(
                         {next_position.x, next_position.y, next_position.z}, 0.3) ||
                     (game_mode != GameMode::spectator && distance_squared <= 256.0 &&
                      level.collides(
                         {next_position.x - 0.3, next_position.y,
                          next_position.z - 0.3},
                         {next_position.x + 0.3,
                          next_position.y + survival.body_height(),
                          next_position.z + 0.3})));
                if (invalid_movement) {
                    ++next_teleport_id;
                    pending_teleport = next_teleport_id;
                    teleport_confirmed = false;
                    teleport_sent_at = now;
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_player_position(
                            next_teleport_id, player_position.x, player_position.y,
                            player_position.z, 0.0F, 0.0F),
                        compression_threshold, cipher ? &*cipher : nullptr);
                    continue;
                }
                if (game_mode != GameMode::spectator) {
                    survival.record_movement(
                        player_position, next_position, position.on_ground,
                        survival.sprinting() && horizontal <= 10.0);
                }
                if (!player_vehicle && game_mode != GameMode::spectator &&
                    horizontal > 0.0 && horizontal <= 64.0) {
                    statistics.increment(
                        8, 6, static_cast<std::int32_t>(std::round(horizontal * 100.0)));
                }
                if (!player_vehicle) {
                    player_position = {position.x, position.y, position.z};
                }
                player_center = {
                    static_cast<std::int32_t>(std::floor(position.x / 16.0)),
                    static_cast<std::int32_t>(std::floor(position.z / 16.0)),
                };
                if (teleport_confirmed && player_center != streamed_center) {
                    stream_chunk_window(player_center);
                }
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::move_player_rot)) {
                protocol::Reader rotation_reader(packet);
                const auto rotation = protocol::play::decode_player_rotation(rotation_reader);
                player_yaw = rotation.yaw;
                write_compressed_packet(
                    descriptor, protocol::play::encode_rotate_head(1, player_yaw),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::move_player_status_only)) {
                protocol::Reader status_reader(packet);
                const auto [on_ground, collision] =
                    protocol::play::decode_player_status(status_reader);
                static_cast<void>(collision);
                survival.record_movement(
                    player_position, player_position, on_ground, false);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::move_vehicle)) {
                protocol::Reader vehicle_reader(packet);
                static_cast<void>(protocol::play::decode_move_vehicle(vehicle_reader));
                if (player_vehicle) {
                    if (const auto* vehicle = entities.find(*player_vehicle)) {
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_move_vehicle(
                                {vehicle->position().x, vehicle->position().y,
                                 vehicle->position().z},
                                vehicle->yaw(), vehicle->pitch()),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                }
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::paddle_boat)) {
                protocol::Reader paddle_reader(packet);
                static_cast<void>(protocol::play::decode_paddle_boat(paddle_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::client_tick_end)) {
                protocol::Reader tick_reader(packet);
                protocol::play::decode_client_tick_end(tick_reader);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::client_information)) {
                protocol::Reader information_reader(packet);
                static_cast<void>(
                    protocol::play::decode_client_information(information_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::custom_payload)) {
                protocol::Reader payload_reader(packet);
                static_cast<void>(protocol::play::decode_custom_payload(payload_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::custom_click_action)) {
                protocol::Reader click_reader(packet);
                static_cast<void>(
                    protocol::play::decode_custom_click_action(click_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::command_suggestion)) {
                protocol::Reader suggestion_reader(packet);
                const auto request =
                    protocol::play::decode_command_suggestion(suggestion_reader);
                const auto start = request.command.front() == '/' ? 1U : 0U;
                const auto prefix = std::string_view(request.command).substr(start);
                std::vector<std::string> suggestions;
                if (prefix.find(' ') == std::string_view::npos) {
                    for (const auto root : implemented_command_roots) {
                        if (root.starts_with(prefix)) suggestions.emplace_back(root);
                    }
                }
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_command_suggestions(
                        request.id, static_cast<std::int32_t>(start),
                        static_cast<std::int32_t>(prefix.size()), suggestions),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::configuration_acknowledged)) {
                protocol::Reader configuration_reader(packet);
                protocol::play::decode_configuration_acknowledged(configuration_reader);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::container_button_click)) {
                protocol::Reader button_reader(packet);
                static_cast<void>(
                    protocol::play::decode_container_button_click(button_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::container_click)) {
                protocol::Reader click_reader(packet);
                static_cast<void>(protocol::play::decode_container_click(click_reader));
                sync_player_inventory();
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::bundle_item_selected)) {
                protocol::Reader selection_reader(packet);
                static_cast<void>(
                    protocol::play::decode_bundle_item_selection(selection_reader));
                sync_player_inventory();
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::change_difficulty)) {
                protocol::Reader difficulty_reader(packet);
                static_cast<void>(
                    protocol::play::decode_change_difficulty(difficulty_reader));
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_change_difficulty(
                        static_cast<std::uint8_t>(difficulty), config_.hardcore),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::lock_difficulty)) {
                protocol::Reader lock_reader(packet);
                static_cast<void>(protocol::play::decode_lock_difficulty(lock_reader));
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_change_difficulty(
                        static_cast<std::uint8_t>(difficulty), config_.hardcore),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::change_game_mode)) {
                protocol::Reader game_mode_reader(packet);
                static_cast<void>(
                    protocol::play::decode_change_game_mode(game_mode_reader));
                const auto creative = game_mode == GameMode::creative;
                const auto spectator = game_mode == GameMode::spectator;
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_game_event(
                        3, static_cast<float>(game_mode)),
                    compression_threshold, cipher ? &*cipher : nullptr);
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_player_abilities(
                        creative || spectator, survival.flying(),
                        creative || spectator, creative, 0.05F, 0.1F),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::container_close)) {
                protocol::Reader close_reader(packet);
                static_cast<void>(protocol::play::decode_container_close(close_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::container_slot_state_changed)) {
                protocol::Reader slot_state_reader(packet);
                static_cast<void>(
                    protocol::play::decode_container_slot_state_change(slot_state_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::cookie_response)) {
                protocol::Reader cookie_reader(packet);
                static_cast<void>(protocol::play::decode_cookie_response(cookie_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::player_loaded)) {
                protocol::Reader loaded_reader(packet);
                protocol::play::decode_player_loaded(loaded_reader);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::ping_request)) {
                protocol::Reader ping_reader(packet);
                const auto ping_time = protocol::play::decode_ping_request(ping_reader);
                write_compressed_packet(
                    descriptor, protocol::play::encode_pong_response(ping_time),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::pick_item_from_block)) {
                protocol::Reader pick_reader(packet);
                const auto pick = protocol::play::decode_pick_item_from_block(pick_reader);
                static_cast<void>(pick.include_data);
                if (game_mode != GameMode::creative ||
                    pick.position.y < world::min_build_y ||
                    pick.position.y >= world::max_build_y ||
                    !within_reach({pick.position.x, pick.position.y, pick.position.z})) {
                    continue;
                }
                const auto block_id = level.block(
                    {pick.position.x, pick.position.y, pick.position.z});
                if (block_id == world::BlockId::air || block_id == world::BlockId::water) {
                    continue;
                }
                try {
                    const auto& block = block_registry.by_id(
                        static_cast<std::uint32_t>(block_id));
                    const auto& item = item_registry.by_name(block.name());
                    select_creative_item(item);
                } catch (const std::out_of_range&) {
                }
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::pick_item_from_entity)) {
                protocol::Reader pick_reader(packet);
                const auto pick = protocol::play::decode_pick_item_from_entity(pick_reader);
                static_cast<void>(pick.include_data);
                const auto* picked = entities.find(
                    static_cast<entity::EntityId>(pick.entity_id));
                if (game_mode != GameMode::creative || !picked ||
                    (picked->position() - player_position).length_squared() > 36.0 ||
                    !level.line_of_sight(
                        {player_position.x, player_position.y + survival.eye_height(),
                         player_position.z},
                        {picked->position().x,
                         picked->position().y + picked->type().properties().height * 0.5,
                         picked->position().z})) {
                    continue;
                }
                try {
                    select_creative_item(item_registry.by_name(
                        "minecraft:" + picked->type().name().path() + "_spawn_egg"));
                } catch (const std::out_of_range&) {
                }
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::place_recipe)) {
                protocol::Reader place_reader(packet);
                static_cast<void>(protocol::play::decode_place_recipe(place_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::player_abilities)) {
                protocol::Reader abilities_reader(packet);
                const auto requested_flying =
                    protocol::play::decode_player_abilities(abilities_reader);
                const auto creative = game_mode == GameMode::creative;
                const auto spectator = game_mode == GameMode::spectator;
                survival.set_flying(spectator || (creative && requested_flying));
                write_compressed_packet(
                    descriptor,
                    protocol::play::encode_player_abilities(
                        creative || spectator, survival.flying(),
                        creative || spectator, creative, 0.05F, 0.1F),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::set_creative_mode_slot)) {
                protocol::Reader creative_slot_reader(packet);
                const auto change =
                    protocol::play::decode_set_creative_mode_slot(creative_slot_reader);
                const auto sync_changed_slot = [&]() {
                    if (change.slot >= 36 && change.slot <= 44) {
                        sync_hotbar_slot(static_cast<std::int16_t>(change.slot - 36));
                    } else if (change.slot == 45) {
                        sync_offhand();
                    }
                };
                if (game_mode != GameMode::creative || change.slot < 36) {
                    sync_changed_slot();
                    continue;
                }
                item::ItemStack replacement;
                try {
                    if (!change.item.empty()) {
                        const auto& selected =
                            item_registry.by_protocol_id(
                                static_cast<std::uint32_t>(change.item.item_id));
                        replacement = item::ItemStack(
                            selected.id(), static_cast<std::uint16_t>(change.item.count));
                        replacement.validate(item_registry);
                    }
                } catch (const std::exception&) {
                    sync_changed_slot();
                    continue;
                }
                if (change.slot <= 44) {
                    hotbar[static_cast<std::size_t>(change.slot - 36)] =
                        std::move(replacement);
                } else {
                    offhand = std::move(replacement);
                }
                sync_changed_slot();
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::player_action)) {
                protocol::Reader action_reader(packet);
                const auto action = protocol::play::decode_player_action(action_reader);
                const core::BlockPosition block_position{
                    action.position.x, action.position.y, action.position.z};
                if (action.action == protocol::play::PlayerActionType::drop_item ||
                    action.action == protocol::play::PlayerActionType::drop_all_items) {
                    if (game_mode == GameMode::spectator) continue;
                    auto& held = hotbar[static_cast<std::size_t>(active_hotbar_slot)];
                    if (!held.empty()) {
                        const std::uint16_t count = action.action ==
                                protocol::play::PlayerActionType::drop_all_items
                            ? held.count()
                            : std::uint16_t{1};
                        auto dropped = held.take(count);
                        const auto dropped_protocol_id = item_registry.by_id(
                            dropped.item_id()).protocol_id();
                        if (dropped_protocol_id) {
                            statistics.increment(
                                5, static_cast<std::int32_t>(*dropped_protocol_id),
                                dropped.count());
                        }
                        const auto drop_id = dropped_items.spawn(
                            entities, std::move(dropped),
                            {player_position.x,
                             player_position.y + survival.eye_height() - 0.2,
                             player_position.z},
                            40);
                        if (auto* item_entity = entities.find(drop_id)) {
                            item_entity->set_velocity({0.0, 0.2, 0.3});
                            if (entities_visible && entity_tracker.update(
                                    *item_entity, player_position).entered) {
                                send_entity_spawn(*item_entity);
                                send_drop_metadata(drop_id);
                            }
                        }
                        sync_hotbar_slot(active_hotbar_slot);
                    }
                } else if (action.action ==
                           protocol::play::PlayerActionType::swap_item_with_offhand) {
                    if (game_mode == GameMode::spectator) continue;
                    auto& held = hotbar[static_cast<std::size_t>(active_hotbar_slot)];
                    std::swap(held, offhand);
                    sync_hotbar_slot(active_hotbar_slot);
                    sync_offhand();
                } else if (action.action ==
                           protocol::play::PlayerActionType::release_use_item) {
                    survival.set_blocking(false);
                    blocking_hand.reset();
                } else if ((game_mode == GameMode::survival ||
                            game_mode == GameMode::creative) &&
                           within_reach(block_position)) {
                    if (action.action == protocol::play::PlayerActionType::start_destroy_block) {
                        const auto chunk_x = static_cast<std::int32_t>(
                            std::floor(static_cast<double>(block_position.x) / 16.0));
                        const auto chunk_z = static_cast<std::int32_t>(
                            std::floor(static_cast<double>(block_position.z) / 16.0));
                        const auto local_x = static_cast<std::size_t>(block_position.x - chunk_x * 16);
                        const auto local_z = static_cast<std::size_t>(block_position.z - chunk_z * 16);
                        if (game_mode == GameMode::creative &&
                            level.block(block_position) != world::BlockId::bedrock) {
                            level.set_block(block_position, world::BlockId::air);
                            send_block_update(block_position);
                            settle_and_send({block_position.x, block_position.y + 1,
                                             block_position.z});
                            write_compressed_packet(
                                descriptor,
                                protocol::play::encode_block_destruction(
                                    1, action.position, -1),
                                compression_threshold, cipher ? &*cipher : nullptr);
                            write_compressed_packet(
                                descriptor,
                                protocol::play::encode_block_changed_ack(action.sequence),
                                compression_threshold, cipher ? &*cipher : nullptr);
                            continue;
                        }
                        const auto ticks = block_interaction.break_ticks(
                            *level.chunk({chunk_x, chunk_z}), local_x,
                            block_position.y, local_z, tool_context());
                        if (ticks > 0) {
                            pending_break = PendingBreak{
                                block_position,
                                now + std::chrono::milliseconds(50 * ticks)};
                        }
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_block_destruction(
                                1, action.position, 0),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    } else if (action.action ==
                               protocol::play::PlayerActionType::stop_destroy_block) {
                        const auto chunk_x = static_cast<std::int32_t>(
                            std::floor(static_cast<double>(block_position.x) / 16.0));
                        const auto chunk_z = static_cast<std::int32_t>(
                            std::floor(static_cast<double>(block_position.z) / 16.0));
                        const auto local_x = static_cast<std::size_t>(
                            block_position.x - chunk_x * 16);
                        const auto local_z = static_cast<std::size_t>(
                            block_position.z - chunk_z * 16);
                        const auto valid_break = pending_break &&
                            pending_break->position == block_position &&
                            now >= pending_break->ready_at;
                        const auto broken_block = valid_break
                            ? level.block(block_position)
                            : world::BlockId::air;
                        const auto result = valid_break
                            ? block_interaction.break_block(
                                  *level.chunk({chunk_x, chunk_z}), local_x,
                                    block_position.y, local_z, tool_context())
                            : block::BreakResult{};
                        pending_break.reset();
                        if (result.broken) {
                            const auto& block = block_registry.by_id(
                                static_cast<std::uint32_t>(broken_block));
                            if (block.protocol_id()) {
                                statistics.increment(
                                    0, static_cast<std::int32_t>(*block.protocol_id()));
                            }
                            auto& tool = hotbar[static_cast<std::size_t>(active_hotbar_slot)];
                            if (!tool.empty() && tool.apply_damage(1, item_registry)) {
                                sync_hotbar_slot(active_hotbar_slot);
                            }
                            for (auto drop : result.drops) {
                                for (std::size_t slot = 0;
                                     slot < hotbar.size() && !drop.empty(); ++slot) {
                                    const auto before = hotbar[slot];
                                    static_cast<void>(hotbar[slot].insert_from(
                                        drop, item_registry));
                                    if (hotbar[slot] != before) {
                                        sync_hotbar_slot(static_cast<std::int16_t>(slot));
                                    }
                                }
                                if (!drop.empty()) {
                                    static_cast<void>(dropped_items.spawn(
                                        entities, std::move(drop),
                                        {static_cast<double>(block_position.x) + 0.5,
                                         static_cast<double>(block_position.y) + 0.5,
                                         static_cast<double>(block_position.z) + 0.5},
                                        10));
                                }
                            }
                            send_block_update(block_position);
                            settle_and_send({block_position.x, block_position.y + 1,
                                             block_position.z});
                        }
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_block_destruction(
                                1, action.position, -1),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    } else if (action.action ==
                               protocol::play::PlayerActionType::abort_destroy_block) {
                        pending_break.reset();
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_block_destruction(
                                1, action.position, -1),
                            compression_threshold, cipher ? &*cipher : nullptr);
                    }
                }
                write_compressed_packet(
                    descriptor, protocol::play::encode_block_changed_ack(action.sequence),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::player_command)) {
                protocol::Reader command_reader(packet);
                const auto command = protocol::play::decode_player_command(command_reader);
                if (command.entity_id != 1) {
                    throw protocol::DecodeError("player command entity ID does not match player");
                }
                if (command.action == protocol::play::PlayerCommandAction::start_sprinting) {
                    survival.set_input(true, survival.sneaking(), survival.jumping());
                } else if (command.action ==
                           protocol::play::PlayerCommandAction::stop_sprinting) {
                    survival.set_input(false, survival.sneaking(), survival.jumping());
                } else if (command.action ==
                           protocol::play::PlayerCommandAction::start_fall_flying) {
                    static_cast<void>(survival.start_gliding());
                }
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::player_input)) {
                protocol::Reader input_reader(packet);
                const auto input = protocol::play::decode_player_input(input_reader);
                survival.set_input(input.sprint, input.shift, input.jump);
                if (player_vehicle) {
                    auto* vehicle = entities.find(*player_vehicle);
                    if (!vehicle) {
                        player_vehicle.reset();
                    } else if (input.shift) {
                        static_cast<void>(entities.remove_external_passenger(vehicle->id(), 1));
                        send_vehicle_passengers(*vehicle);
                        player_position = {
                            vehicle->position().x + 1.0,
                            vehicle->position().y,
                            vehicle->position().z};
                        player_vehicle.reset();
                    } else {
                        const auto speed = static_cast<double>(
                            vehicle->type().properties().movement_speed);
                        auto velocity = vehicle->velocity();
                        velocity.x = (input.right ? speed : 0.0) -
                            (input.left ? speed : 0.0);
                        velocity.z = (input.forward ? speed : 0.0) -
                            (input.backward ? speed : 0.0);
                        if (input.jump && vehicle->on_ground()) velocity.y = 0.4;
                        vehicle->set_velocity(velocity);
                    }
                }
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::pong)) {
                protocol::Reader pong_reader(packet);
                static_cast<void>(protocol::play::decode_pong(pong_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::recipe_book_change_settings)) {
                protocol::Reader settings_reader(packet);
                static_cast<void>(
                    protocol::play::decode_recipe_book_setting_change(settings_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::recipe_book_seen_recipe)) {
                protocol::Reader seen_reader(packet);
                static_cast<void>(protocol::play::decode_recipe_book_seen(seen_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::rename_item)) {
                protocol::Reader rename_reader(packet);
                static_cast<void>(protocol::play::decode_rename_item(rename_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::resource_pack)) {
                protocol::Reader resource_pack_reader(packet);
                static_cast<void>(
                    protocol::play::decode_resource_pack_response(resource_pack_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::seen_advancements)) {
                protocol::Reader advancements_reader(packet);
                static_cast<void>(
                    protocol::play::decode_seen_advancements(advancements_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::select_trade)) {
                protocol::Reader trade_reader(packet);
                static_cast<void>(protocol::play::decode_select_trade(trade_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::set_beacon)) {
                protocol::Reader beacon_reader(packet);
                static_cast<void>(protocol::play::decode_set_beacon(beacon_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::set_carried_item)) {
                protocol::Reader carried_reader(packet);
                active_hotbar_slot = protocol::play::decode_set_carried_item(carried_reader);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::spectator_action)) {
                protocol::Reader spectator_reader(packet);
                const auto target =
                    protocol::play::decode_spectator_action(spectator_reader);
                auto camera_id = std::int32_t{1};
                if (game_mode == GameMode::spectator && target &&
                    entities.find(static_cast<entity::EntityId>(*target))) {
                    camera_id = *target;
                }
                write_compressed_packet(
                    descriptor, protocol::play::encode_set_camera(camera_id),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::teleport_to_entity)) {
                protocol::Reader teleport_reader(packet);
                const auto target_uuid =
                    protocol::play::decode_teleport_to_entity(teleport_reader);
                const entity::Entity* target = nullptr;
                if (game_mode == GameMode::spectator) {
                    for (const auto id : entities.ids()) {
                        const auto* candidate = entities.find(id);
                        if (candidate && candidate->uuid() == target_uuid) {
                            target = candidate;
                            break;
                        }
                    }
                }
                if (target && level.inside_border(
                        {target->position().x, target->position().y,
                         target->position().z}, 0.3)) {
                    pending_break.reset();
                    survival.set_blocking(false);
                    blocking_hand.reset();
                    player_position = target->position();
                    player_yaw = target->yaw();
                    player_center = {
                        static_cast<std::int32_t>(std::floor(player_position.x / 16.0)),
                        static_cast<std::int32_t>(std::floor(player_position.z / 16.0))};
                    if (streamed_center != player_center) {
                        stream_chunk_window(player_center);
                    }
                    ++next_teleport_id;
                    pending_teleport = next_teleport_id;
                    teleport_confirmed = false;
                    teleport_sent_at = now;
                    write_compressed_packet(
                        descriptor,
                        protocol::play::encode_player_position(
                            next_teleport_id, player_position.x, player_position.y,
                            player_position.z, target->yaw(), target->pitch()),
                        compression_threshold, cipher ? &*cipher : nullptr);
                }
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::swing)) {
                protocol::Reader swing_reader(packet);
                static_cast<void>(protocol::play::decode_swing(swing_reader));
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::use_item_on)) {
                protocol::Reader use_reader(packet);
                const auto use = protocol::play::decode_use_item_on(use_reader);
                if (game_mode == GameMode::adventure ||
                    game_mode == GameMode::spectator) {
                    write_compressed_packet(
                        descriptor, protocol::play::encode_block_changed_ack(use.sequence),
                        compression_threshold, cipher ? &*cipher : nullptr);
                    continue;
                }
                core::BlockPosition placement{
                    use.hit.position.x, use.hit.position.y, use.hit.position.z};
                if (level.block(placement) != world::BlockId::air &&
                    level.block(placement) != world::BlockId::water &&
                    level.block(placement) != world::BlockId::short_grass &&
                    level.block(placement) != world::BlockId::dandelion &&
                    level.block(placement) != world::BlockId::poppy) {
                    constexpr std::array<core::BlockPosition, 6> face_offsets{{
                        {0, -1, 0}, {0, 1, 0}, {0, 0, -1},
                        {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};
                    const auto offset = face_offsets[use.hit.direction];
                    placement = {placement.x + offset.x, placement.y + offset.y,
                                 placement.z + offset.z};
                }
                const auto intersects_player =
                    static_cast<std::int32_t>(std::floor(player_position.x)) == placement.x &&
                    static_cast<std::int32_t>(std::floor(player_position.z)) == placement.z &&
                    placement.y >= static_cast<std::int32_t>(std::floor(player_position.y)) &&
                    placement.y <= static_cast<std::int32_t>(std::floor(player_position.y + 1.8));
                const auto occupants = entities.query({
                    {static_cast<double>(placement.x), static_cast<double>(placement.y),
                     static_cast<double>(placement.z)},
                    {static_cast<double>(placement.x + 1),
                     static_cast<double>(placement.y + 1),
                     static_cast<double>(placement.z + 1)}});
                const auto occupied_by_entity = std::any_of(
                    occupants.begin(), occupants.end(), [&](const auto id) {
                        return dynamic_cast<entity::LivingEntity*>(entities.find(id)) != nullptr;
                    });
                if (within_reach(placement) && !intersects_player && !occupied_by_entity &&
                    placement.y >= world::min_build_y && placement.y < world::max_build_y) {
                    const auto chunk_x = static_cast<std::int32_t>(
                        std::floor(static_cast<double>(placement.x) / 16.0));
                    const auto chunk_z = static_cast<std::int32_t>(
                        std::floor(static_cast<double>(placement.z) / 16.0));
                    const auto local_x = static_cast<std::size_t>(placement.x - chunk_x * 16);
                    const auto local_z = static_cast<std::size_t>(placement.z - chunk_z * 16);
                    auto& held_stack = hotbar[static_cast<std::size_t>(active_hotbar_slot)];
                    const auto creative_stack = held_stack;
                    if (block_interaction.place(
                            *level.chunk({chunk_x, chunk_z}), local_x,
                            placement.y, local_z, held_stack)) {
                        if (game_mode == GameMode::creative) held_stack = creative_stack;
                        send_block_update(placement);
                        sync_hotbar_slot(active_hotbar_slot);
                        settle_and_send(placement);
                    }
                }
                write_compressed_packet(
                    descriptor, protocol::play::encode_block_changed_ack(use.sequence),
                    compression_threshold, cipher ? &*cipher : nullptr);
            } else if (packet_id == static_cast<std::int32_t>(
                           protocol::play::ServerboundPacketId::use_item)) {
                protocol::Reader use_reader(packet);
                const auto use = protocol::play::decode_use_item(use_reader);
                if (game_mode == GameMode::spectator) {
                    write_compressed_packet(
                        descriptor, protocol::play::encode_block_changed_ack(use.sequence),
                        compression_threshold, cipher ? &*cipher : nullptr);
                    continue;
                }
                auto& held = use.hand == 0
                    ? hotbar[static_cast<std::size_t>(active_hotbar_slot)]
                    : offhand;
                if (!held.empty()) {
                    const auto& properties = item_registry.by_id(held.item_id()).properties();
                    if (item_registry.by_id(held.item_id()).name().path() == "shield") {
                        survival.set_blocking(true);
                        blocking_hand = use.hand;
                    } else if (properties.nutrition > 0 && survival.consume_food(
                            properties.nutrition, properties.saturation_modifier)) {
                        const auto used_id = item_registry.by_id(held.item_id()).protocol_id();
                        static_cast<void>(held.take(1));
                        if (use.hand == 0) sync_hotbar_slot(active_hotbar_slot);
                        else sync_offhand();
                        write_compressed_packet(
                            descriptor,
                            protocol::play::encode_health(
                                survival.health(), survival.food_level(),
                                survival.saturation()),
                            compression_threshold, cipher ? &*cipher : nullptr);
                        static_cast<void>(survival.take_dirty());
                        if (used_id) {
                            statistics.increment(2, static_cast<std::int32_t>(*used_id));
                        }
                    } else if (item_registry.by_id(held.item_id()).name().path() == "bow") {
                        auto arrow_slot = hotbar.end();
                        for (auto iterator = hotbar.begin(); iterator != hotbar.end(); ++iterator) {
                            if (!iterator->empty() &&
                                item_registry.by_id(iterator->item_id()).name().path() == "arrow") {
                                arrow_slot = iterator;
                                break;
                            }
                        }
                        if (arrow_slot != hotbar.end()) {
                            constexpr double pi = 3.141592653589793;
                            const auto yaw = static_cast<double>(use.yaw) * pi / 180.0;
                            const auto pitch = static_cast<double>(use.pitch) * pi / 180.0;
                            const auto horizontal = std::cos(pitch);
                            const entity::Vec3 velocity{
                                -std::sin(yaw) * horizontal * 3.0,
                                -std::sin(pitch) * 3.0,
                                std::cos(yaw) * horizontal * 3.0};
                            const auto projectile_id = projectiles.spawn(
                                entities, "arrow",
                                {player_position.x,
                                 player_position.y + survival.eye_height() - 0.1,
                                 player_position.z},
                                velocity, 1, 2.0F, 1'200,
                                item::ItemStack(item_registry.by_name("arrow").id(), 1));
                            if (entities_visible) {
                                if (const auto* arrow = entities.find(projectile_id);
                                    arrow && entity_tracker.update(
                                        *arrow, player_position).entered) {
                                    send_entity_spawn(*arrow, 1);
                                }
                            }
                            if (game_mode != GameMode::creative) {
                                static_cast<void>(arrow_slot->take(1));
                                const auto arrow_index = static_cast<std::int16_t>(
                                    std::distance(hotbar.begin(), arrow_slot));
                                sync_hotbar_slot(arrow_index);
                                static_cast<void>(held.apply_damage(1, item_registry));
                                if (use.hand == 0) sync_hotbar_slot(active_hotbar_slot);
                                else sync_offhand();
                            }
                            statistics.increment(
                                2, static_cast<std::int32_t>(
                                       item_registry.by_name("bow").protocol_id().value()));
                        }
                    }
                }
                write_compressed_packet(
                    descriptor, protocol::play::encode_block_changed_ack(use.sequence),
                    compression_threshold, cipher ? &*cipher : nullptr);
            }
        }
    }

    void handle_connection(const NativeSocket descriptor, const bool rate_limited) {
        const auto handshake_packet = read_packet(descriptor);
        protocol::Reader handshake_reader(handshake_packet);
        const auto handshake = protocol::decode_handshake(handshake_reader);

        switch (handshake.next_state) {
        case protocol::ConnectionState::status:
            handle_status(descriptor, rate_limited);
            return;
        case protocol::ConnectionState::login:
        case protocol::ConnectionState::transfer:
            if (handshake.protocol_version != protocol_version) {
                write_all(descriptor, protocol::login::encode_disconnect(
                    "{\"text\":\"This server requires Minecraft 26.2\"}"));
                return;
            }
            if (rate_limited) {
                write_all(descriptor, protocol::login::encode_disconnect(
                    "{\"text\":\"Connection rate limit exceeded\"}"));
                return;
            }
            handle_login(descriptor, handshake.next_state == protocol::ConnectionState::transfer);
            return;
        default:
            throw protocol::DecodeError("unsupported connection state");
        }
    }

    NetworkRuntime network_runtime_;
    Config config_;
    Socket listener_;
    std::atomic_bool stopping_{false};
    std::mutex queue_mutex_;
    std::condition_variable queue_ready_;
    std::deque<QueuedConnection> pending_;
    std::size_t active_connections_{0};
    std::map<std::string, std::size_t> active_by_host_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>>
        connection_attempts_by_host_;
    std::mutex profile_mutex_;
    std::set<std::string> active_profiles_;
	std::vector<protocol::configuration::RegistryTags> configuration_tags_;
    std::vector<protocol::configuration::RegistryData> configuration_registries_;
    std::vector<protocol::Bytes> configuration_registry_fallback_;
    std::vector<protocol::Bytes> recipe_sync_packets_;
    std::vector<std::jthread> workers_;
};

Server::Server(Config config) : impl_(new Impl(std::move(config))) {}
Server::~Server() { delete impl_; }
void Server::run() { impl_->run(); }
void Server::request_stop() noexcept { impl_->request_stop(); }

} // namespace mc::server