#pragma once

#include "mc/protocol/codec.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace mc::protocol {

class AesCfb8Cipher final {
public:
    explicit AesCfb8Cipher(std::span<const std::uint8_t> shared_secret);
    ~AesCfb8Cipher();

    AesCfb8Cipher(const AesCfb8Cipher&) = delete;
    AesCfb8Cipher& operator=(const AesCfb8Cipher&) = delete;
    AesCfb8Cipher(AesCfb8Cipher&&) noexcept;
    AesCfb8Cipher& operator=(AesCfb8Cipher&&) noexcept;

    void encrypt(std::span<std::uint8_t> data);
    void decrypt(std::span<std::uint8_t> data);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class RsaKeyPair final {
public:
    RsaKeyPair();
    ~RsaKeyPair();

    RsaKeyPair(const RsaKeyPair&) = delete;
    RsaKeyPair& operator=(const RsaKeyPair&) = delete;
    RsaKeyPair(RsaKeyPair&&) noexcept;
    RsaKeyPair& operator=(RsaKeyPair&&) noexcept;

    [[nodiscard]] Bytes public_key_der() const;
    [[nodiscard]] Bytes decrypt(std::span<const std::uint8_t> ciphertext) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] Bytes rsa_encrypt(std::span<const std::uint8_t> public_key_der,
                                std::span<const std::uint8_t> plaintext);
[[nodiscard]] std::string minecraft_server_hash(
    std::string_view server_id,
    std::span<const std::uint8_t> shared_secret,
    std::span<const std::uint8_t> public_key_der);
[[nodiscard]] Uuid create_offline_uuid(std::string_view player_name);

} // namespace mc::protocol