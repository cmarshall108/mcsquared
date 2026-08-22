#include "session_auth.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <curl/curl.h>

namespace mc::server {
namespace {

constexpr std::size_t max_response_size = 1U * 1024U * 1024U;

class JsonValue final {
public:
	using Object = std::map<std::string, JsonValue>;
	using Array = std::vector<JsonValue>;
	using Storage = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;

	explicit JsonValue(Storage storage) : storage_(std::move(storage)) {}

	[[nodiscard]] const Object& object() const {
		if (const auto* value = std::get_if<Object>(&storage_)) return *value;
		throw SessionAuthenticationError("session response value is not an object");
	}
	[[nodiscard]] const Array& array() const {
		if (const auto* value = std::get_if<Array>(&storage_)) return *value;
		throw SessionAuthenticationError("session response value is not an array");
	}
	[[nodiscard]] const std::string& string() const {
		if (const auto* value = std::get_if<std::string>(&storage_)) return *value;
		throw SessionAuthenticationError("session response value is not a string");
	}

private:
	Storage storage_;
};

class JsonParser final {
public:
	explicit JsonParser(const std::string_view input) : input_(input) {}

	[[nodiscard]] JsonValue parse() {
		auto result = parse_value();
		skip_space();
		if (offset_ != input_.size()) fail("trailing JSON data");
		return result;
	}

private:
	[[noreturn]] static void fail(const char* message) {
		throw SessionAuthenticationError(message);
	}
	void skip_space() noexcept {
		while (offset_ < input_.size() &&
			(input_[offset_] == ' ' || input_[offset_] == '\n' ||
			 input_[offset_] == '\r' || input_[offset_] == '\t')) ++offset_;
	}
	[[nodiscard]] char take() {
		if (offset_ >= input_.size()) fail("truncated JSON response");
		return input_[offset_++];
	}
	void expect(const char expected) {
		if (take() != expected) fail("unexpected JSON token");
	}
	void expect_word(const std::string_view expected) {
		if (input_.substr(offset_, expected.size()) != expected) fail("invalid JSON literal");
		offset_ += expected.size();
	}
	static void append_utf8(std::string& output, const std::uint32_t codepoint) {
		if (codepoint <= 0x7FU) output.push_back(static_cast<char>(codepoint));
		else if (codepoint <= 0x7FFU) {
			output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
			output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
		} else {
			output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
			output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
			output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
		}
	}
	[[nodiscard]] static std::uint32_t hex_digit(const char value) {
		if (value >= '0' && value <= '9') return static_cast<std::uint32_t>(value - '0');
		if (value >= 'a' && value <= 'f') return static_cast<std::uint32_t>(value - 'a' + 10);
		if (value >= 'A' && value <= 'F') return static_cast<std::uint32_t>(value - 'A' + 10);
		fail("invalid JSON unicode escape");
	}
	[[nodiscard]] std::string parse_string() {
		expect('"');
		std::string result;
		while (true) {
			const auto value = take();
			if (value == '"') return result;
			if (static_cast<unsigned char>(value) < 0x20U) fail("control character in JSON string");
			if (value != '\\') {
				result.push_back(value);
				continue;
			}
			switch (take()) {
			case '"': result.push_back('"'); break;
			case '\\': result.push_back('\\'); break;
			case '/': result.push_back('/'); break;
			case 'b': result.push_back('\b'); break;
			case 'f': result.push_back('\f'); break;
			case 'n': result.push_back('\n'); break;
			case 'r': result.push_back('\r'); break;
			case 't': result.push_back('\t'); break;
			case 'u': {
				std::uint32_t codepoint = 0;
				for (int index = 0; index < 4; ++index) codepoint = codepoint * 16U + hex_digit(take());
				if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) fail("unsupported JSON surrogate");
				append_utf8(result, codepoint);
				break;
			}
			default: fail("invalid JSON escape");
			}
		}
	}
	[[nodiscard]] JsonValue parse_number() {
		const auto start = offset_;
		if (input_[offset_] == '-') ++offset_;
		while (offset_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[offset_]))) ++offset_;
		if (offset_ < input_.size() && input_[offset_] == '.') {
			++offset_;
			while (offset_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[offset_]))) ++offset_;
		}
		try {
			return JsonValue(std::stod(std::string(input_.substr(start, offset_ - start))));
		} catch (const std::exception&) {
			fail("invalid JSON number");
		}
	}
	[[nodiscard]] JsonValue parse_array() {
		expect('[');
		JsonValue::Array result;
		skip_space();
		if (offset_ < input_.size() && input_[offset_] == ']') { ++offset_; return JsonValue(std::move(result)); }
		while (true) {
			result.push_back(parse_value());
			skip_space();
			const auto separator = take();
			if (separator == ']') return JsonValue(std::move(result));
			if (separator != ',') fail("invalid JSON array separator");
		}
	}
	[[nodiscard]] JsonValue parse_object() {
		expect('{');
		JsonValue::Object result;
		skip_space();
		if (offset_ < input_.size() && input_[offset_] == '}') { ++offset_; return JsonValue(std::move(result)); }
		while (true) {
			skip_space();
			auto key = parse_string();
			skip_space();
			expect(':');
			auto [iterator, inserted] = result.emplace(std::move(key), parse_value());
			static_cast<void>(iterator);
			if (!inserted) fail("duplicate JSON object key");
			skip_space();
			const auto separator = take();
			if (separator == '}') return JsonValue(std::move(result));
			if (separator != ',') fail("invalid JSON object separator");
		}
	}
	[[nodiscard]] JsonValue parse_value() {
		skip_space();
		if (offset_ >= input_.size()) fail("truncated JSON value");
		switch (input_[offset_]) {
		case '{': return parse_object();
		case '[': return parse_array();
		case '"': return JsonValue(parse_string());
		case 't': expect_word("true"); return JsonValue(true);
		case 'f': expect_word("false"); return JsonValue(false);
		case 'n': expect_word("null"); return JsonValue(nullptr);
		default: return parse_number();
		}
	}

	std::string_view input_;
	std::size_t offset_{0};
};

[[nodiscard]] const JsonValue& required(const JsonValue::Object& object,
											const std::string& key) {
	const auto found = object.find(key);
	if (found == object.end()) throw SessionAuthenticationError("session response is missing a field");
	return found->second;
}

[[nodiscard]] protocol::Uuid parse_uuid(const std::string_view input) {
	protocol::Uuid result{};
	std::size_t nibble = 0;
	for (const auto character : input) {
		if (character == '-') continue;
		if (nibble >= 32) throw SessionAuthenticationError("session UUID is too long");
		std::uint8_t value = 0;
		if (character >= '0' && character <= '9') value = static_cast<std::uint8_t>(character - '0');
		else if (character >= 'a' && character <= 'f') value = static_cast<std::uint8_t>(character - 'a' + 10);
		else if (character >= 'A' && character <= 'F') value = static_cast<std::uint8_t>(character - 'A' + 10);
		else throw SessionAuthenticationError("session UUID contains an invalid character");
		result[nibble / 2] = static_cast<std::uint8_t>(result[nibble / 2] | (value << (nibble % 2 == 0 ? 4U : 0U)));
		++nibble;
	}
	if (nibble != 32) throw SessionAuthenticationError("session UUID has an invalid length");
	return result;
}

[[nodiscard]] bool equal_ignore_case(const std::string_view left,
										 const std::string_view right) noexcept {
	return left.size() == right.size() &&
		std::equal(left.begin(), left.end(), right.begin(), [](const char a, const char b) {
			return std::tolower(static_cast<unsigned char>(a)) ==
				std::tolower(static_cast<unsigned char>(b));
		});
}

[[nodiscard]] std::size_t append_response(char* data,
										  const std::size_t size,
										  const std::size_t count,
										  void* user_data) {
	const auto bytes = size * count;
	auto& output = *static_cast<std::string*>(user_data);
	if (bytes > max_response_size - output.size()) return 0;
	output.append(data, bytes);
	return bytes;
}

class CurlRuntime final {
public:
	CurlRuntime() {
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
			throw SessionAuthenticationError("failed to initialize libcurl");
		}
	}
	~CurlRuntime() { curl_global_cleanup(); }
};

} // namespace

SessionAuthenticator::SessionAuthenticator(std::string endpoint)
	: endpoint_(std::move(endpoint)) {
	static const CurlRuntime runtime;
	static_cast<void>(runtime);
	if (endpoint_.empty()) throw std::invalid_argument("session server URL must not be empty");
}

std::optional<protocol::login::GameProfile> SessionAuthenticator::authenticate(
	const std::string_view username,
	const std::string_view server_hash) const {
	using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
	CurlHandle curl(curl_easy_init(), curl_easy_cleanup);
	if (!curl) throw SessionAuthenticationError("failed to create session request");
	const auto escape = [&](const std::string_view value) {
		using Escaped = std::unique_ptr<char, decltype(&curl_free)>;
		Escaped encoded(curl_easy_escape(curl.get(), value.data(), static_cast<int>(value.size())), curl_free);
		if (!encoded) throw SessionAuthenticationError("failed to encode session request");
		return std::string(encoded.get());
	};
	const auto url = endpoint_ + "?username=" + escape(username) + "&serverId=" + escape(server_hash);
	std::string response;
	curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5'000L);
	curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 10'000L);
	curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "mcsquared/0.1");
	curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, append_response);
	curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
	const auto result = curl_easy_perform(curl.get());
	if (result != CURLE_OK) {
		throw SessionAuthenticationError(
			std::string("session request failed: ") + curl_easy_strerror(result));
	}
	long status = 0;
	curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
	if (status == 204) return std::nullopt;
	if (status != 200) throw SessionAuthenticationError("session server returned an unexpected status");

	const auto root = JsonParser(response).parse().object();
	protocol::login::GameProfile profile{
		parse_uuid(required(root, "id").string()),
		required(root, "name").string(),
		{},
	};
	if (profile.name.empty() || profile.name.size() > 16 ||
		!equal_ignore_case(profile.name, username)) {
		throw SessionAuthenticationError("session response profile name does not match Login Hello");
	}
	const auto properties = root.find("properties");
	if (properties != root.end()) {
		for (const auto& property_value : properties->second.array()) {
			const auto& property = property_value.object();
			protocol::login::ProfileProperty decoded{
				required(property, "name").string(),
				required(property, "value").string(),
				std::nullopt,
			};
			if (const auto signature = property.find("signature"); signature != property.end()) {
				decoded.signature = signature->second.string();
			}
			profile.properties.push_back(std::move(decoded));
		}
	}
	if (profile.properties.size() > 16) {
		throw SessionAuthenticationError("session response contains too many profile properties");
	}
	return profile;
}

} // namespace mc::server