#pragma once

namespace mc::protocol {

enum class ConnectionState {
	handshaking,
	status,
	login,
	transfer,
	configuration,
	play,
	closed,
};

class ProtocolSession;

} // namespace mc::protocol