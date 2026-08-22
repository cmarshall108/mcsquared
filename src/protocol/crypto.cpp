#include "mc/protocol/crypto.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace mc::protocol {
namespace {

[[noreturn]] void throw_crypto_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed");
}

[[nodiscard]] std::string signed_hex(std::span<const std::uint8_t> digest) {
    Bytes magnitude(digest.begin(), digest.end());
    const bool negative = (magnitude.front() & 0x80U) != 0;
    if (negative) {
        for (auto& byte : magnitude) {
            byte = static_cast<std::uint8_t>(~byte);
        }
        for (auto iterator = magnitude.rbegin(); iterator != magnitude.rend(); ++iterator) {
            ++(*iterator);
            if (*iterator != 0) {
                break;
            }
        }
    }

    const auto first = std::find_if(magnitude.begin(), magnitude.end(), [](const auto byte) {
        return byte != 0;
    });
    if (first == magnitude.end()) {
        return "0";
    }

    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    if (negative) {
        result.push_back('-');
    }
    const auto first_byte = *first;
    if ((first_byte >> 4U) != 0) {
        result.push_back(digits[first_byte >> 4U]);
    }
    result.push_back(digits[first_byte & 0x0FU]);
    for (auto iterator = std::next(first); iterator != magnitude.end(); ++iterator) {
        result.push_back(digits[*iterator >> 4U]);
        result.push_back(digits[*iterator & 0x0FU]);
    }
    return result;
}

[[nodiscard]] std::array<std::uint8_t, 20> sha1(
    const std::string_view server_id,
    const std::span<const std::uint8_t> shared_secret,
    const std::span<const std::uint8_t> public_key_der) {
    auto* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        throw_crypto_error("SHA-1 context allocation");
    }
    const auto cleanup = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
        context, EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(context, EVP_sha1(), nullptr) != 1 ||
        EVP_DigestUpdate(context, server_id.data(), server_id.size()) != 1 ||
        EVP_DigestUpdate(context, shared_secret.data(), shared_secret.size()) != 1 ||
        EVP_DigestUpdate(context, public_key_der.data(), public_key_der.size()) != 1) {
        throw_crypto_error("SHA-1 update");
    }

    std::array<std::uint8_t, 20> digest{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context, digest.data(), &size) != 1 || size != digest.size()) {
        throw_crypto_error("SHA-1 finalization");
    }
    return digest;
}

} // namespace

class AesCfb8Cipher::Impl final {
public:
    explicit Impl(const std::span<const std::uint8_t> shared_secret)
        : encrypt_context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free),
          decrypt_context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free) {
        if (shared_secret.size() != 16 || !encrypt_context || !decrypt_context) {
            throw std::invalid_argument("AES-CFB8 requires a 16-byte shared secret");
        }
        if (EVP_EncryptInit_ex(encrypt_context.get(), EVP_aes_128_cfb8(), nullptr,
                              shared_secret.data(), shared_secret.data()) != 1 ||
            EVP_DecryptInit_ex(decrypt_context.get(), EVP_aes_128_cfb8(), nullptr,
                              shared_secret.data(), shared_secret.data()) != 1) {
            throw_crypto_error("AES-CFB8 initialization");
        }
    }

    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> encrypt_context;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> decrypt_context;
};

AesCfb8Cipher::AesCfb8Cipher(const std::span<const std::uint8_t> shared_secret)
    : impl_(std::make_unique<Impl>(shared_secret)) {}
AesCfb8Cipher::~AesCfb8Cipher() = default;
AesCfb8Cipher::AesCfb8Cipher(AesCfb8Cipher&&) noexcept = default;
AesCfb8Cipher& AesCfb8Cipher::operator=(AesCfb8Cipher&&) noexcept = default;

void AesCfb8Cipher::encrypt(const std::span<std::uint8_t> data) {
    Bytes output(data.size() + EVP_MAX_BLOCK_LENGTH);
    int output_size = 0;
    if (EVP_EncryptUpdate(impl_->encrypt_context.get(), output.data(), &output_size,
                          data.data(), static_cast<int>(data.size())) != 1 ||
        output_size != static_cast<int>(data.size())) {
        throw_crypto_error("AES-CFB8 encryption");
    }
    std::copy_n(output.begin(), output_size, data.begin());
}

void AesCfb8Cipher::decrypt(const std::span<std::uint8_t> data) {
    Bytes output(data.size() + EVP_MAX_BLOCK_LENGTH);
    int output_size = 0;
    if (EVP_DecryptUpdate(impl_->decrypt_context.get(), output.data(), &output_size,
                          data.data(), static_cast<int>(data.size())) != 1 ||
        output_size != static_cast<int>(data.size())) {
        throw_crypto_error("AES-CFB8 decryption");
    }
    std::copy_n(output.begin(), output_size, data.begin());
}

class RsaKeyPair::Impl final {
public:
    Impl() : key(nullptr, EVP_PKEY_free) {
        auto* context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (context == nullptr) {
            throw_crypto_error("RSA context allocation");
        }
        const auto cleanup = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
            context, EVP_PKEY_CTX_free);
        EVP_PKEY* generated = nullptr;
        if (EVP_PKEY_keygen_init(context) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(context, 1024) <= 0 ||
            EVP_PKEY_keygen(context, &generated) <= 0) {
            throw_crypto_error("RSA key generation");
        }
        key.reset(generated);
    }

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key;
};

RsaKeyPair::RsaKeyPair() : impl_(std::make_unique<Impl>()) {}
RsaKeyPair::~RsaKeyPair() = default;
RsaKeyPair::RsaKeyPair(RsaKeyPair&&) noexcept = default;
RsaKeyPair& RsaKeyPair::operator=(RsaKeyPair&&) noexcept = default;

Bytes RsaKeyPair::public_key_der() const {
    const auto size = i2d_PUBKEY(impl_->key.get(), nullptr);
    if (size <= 0) {
        throw_crypto_error("RSA public key encoding");
    }
    Bytes encoded(static_cast<std::size_t>(size));
    auto* output = encoded.data();
    if (i2d_PUBKEY(impl_->key.get(), &output) != size) {
        throw_crypto_error("RSA public key encoding");
    }
    return encoded;
}

Bytes RsaKeyPair::decrypt(const std::span<const std::uint8_t> ciphertext) const {
    auto* context = EVP_PKEY_CTX_new(impl_->key.get(), nullptr);
    if (context == nullptr) {
        throw_crypto_error("RSA decryption context allocation");
    }
    const auto cleanup = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
        context, EVP_PKEY_CTX_free);
    std::size_t output_size = 0;
    if (EVP_PKEY_decrypt_init(context) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) <= 0 ||
        EVP_PKEY_decrypt(context, nullptr, &output_size, ciphertext.data(), ciphertext.size()) <= 0) {
        throw_crypto_error("RSA decryption sizing");
    }
    Bytes output(output_size);
    if (EVP_PKEY_decrypt(context, output.data(), &output_size,
                         ciphertext.data(), ciphertext.size()) <= 0) {
        throw_crypto_error("RSA decryption");
    }
    output.resize(output_size);
    return output;
}

Bytes rsa_encrypt(const std::span<const std::uint8_t> public_key_der,
                  const std::span<const std::uint8_t> plaintext) {
    const auto* encoded = public_key_der.data();
    auto* decoded = d2i_PUBKEY(nullptr, &encoded, static_cast<long>(public_key_der.size()));
    if (decoded == nullptr || encoded != public_key_der.data() + public_key_der.size()) {
        EVP_PKEY_free(decoded);
        throw_crypto_error("RSA public key decoding");
    }
    const auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(decoded, EVP_PKEY_free);
    auto* context = EVP_PKEY_CTX_new(key.get(), nullptr);
    if (context == nullptr) {
        throw_crypto_error("RSA encryption context allocation");
    }
    const auto cleanup = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
        context, EVP_PKEY_CTX_free);
    std::size_t output_size = 0;
    if (EVP_PKEY_encrypt_init(context) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) <= 0 ||
        EVP_PKEY_encrypt(context, nullptr, &output_size, plaintext.data(), plaintext.size()) <= 0) {
        throw_crypto_error("RSA encryption sizing");
    }
    Bytes output(output_size);
    if (EVP_PKEY_encrypt(context, output.data(), &output_size,
                         plaintext.data(), plaintext.size()) <= 0) {
        throw_crypto_error("RSA encryption");
    }
    output.resize(output_size);
    return output;
}

std::string minecraft_server_hash(const std::string_view server_id,
                                  const std::span<const std::uint8_t> shared_secret,
                                  const std::span<const std::uint8_t> public_key_der) {
    return signed_hex(sha1(server_id, shared_secret, public_key_der));
}

Uuid create_offline_uuid(const std::string_view player_name) {
    const auto input = std::string("OfflinePlayer:") + std::string(player_name);
    Uuid uuid{};
    unsigned int size = 0;
    if (EVP_Digest(input.data(), input.size(), uuid.data(), &size, EVP_md5(), nullptr) != 1 ||
        size != uuid.size()) {
        throw_crypto_error("offline UUID generation");
    }
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0FU) | 0x30U);
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3FU) | 0x80U);
    return uuid;
}

} // namespace mc::protocol