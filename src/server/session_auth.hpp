#pragma once

#include "mc/protocol/packets.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace mc::server {

class SessionAuthenticationError final : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class SessionAuthenticator final {
public:
	explicit SessionAuthenticator(std::string endpoint);

	[[nodiscard]] std::optional<protocol::login::GameProfile> authenticate(
		std::string_view username,
		std::string_view server_hash) const;

private:
	std::string endpoint_;
};

} // namespace mc::server