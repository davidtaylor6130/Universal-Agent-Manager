#pragma once

#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <string>
#include <string_view>

namespace uam::voice_input
{
	inline constexpr std::string_view kSystemMode = "system";
	inline constexpr std::string_view kLocalMode = "local";
	inline constexpr std::string_view kServerMode = "server";
	inline constexpr std::string_view kDefaultServerBaseUrl = "https://api.openai.com";
	inline constexpr std::string_view kDefaultServerEndpoint = "/v1/audio/transcriptions";
	inline constexpr std::string_view kDefaultServerModel = "whisper-1";
	inline constexpr std::string_view kDefaultApiKeyEnv = "OPENAI_API_KEY";

	inline std::string NormalizeMode(std::string_view value)
	{
		const std::string normalized = uam::strings::TrimAndLowerAscii(value);
		return normalized == kLocalMode || normalized == kServerMode ? normalized : std::string(kSystemMode);
	}

	inline bool HasControlCharacters(std::string_view value)
	{
		for (const unsigned char character : value)
			if (character < 32 || character == 127) return true;
		return false;
	}

	inline bool IsEnvironmentVariableName(std::string_view value)
	{
		if (value.empty() || value.size() > 128 || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_'))
		{
			return false;
		}
		for (const char c : value)
		{
			if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
			{
				return false;
			}
		}
		return true;
	}

	inline bool IsSafeServerBaseUrl(std::string_view value)
	{
		if (value.size() > 4096 || HasControlCharacters(value) || value.find('\\') != std::string_view::npos) return false;
		const std::string url = uam::strings::TrimAndLowerAscii(value);
		if (url.find('@') != std::string::npos || url.find('?') != std::string::npos || url.find('#') != std::string::npos || url.find_first_of(" \t\r\n") != std::string::npos)
		{
			return false;
		}
		const bool https = url.starts_with("https://");
		const bool http = url.starts_with("http://");
		if (!https && !http) return false;
		const std::size_t authority_at = https ? 8 : 7;
		const std::string authority = url.substr(authority_at, url.find('/', authority_at) - authority_at);
		if (authority.empty()) return false;
		if (https)
		{
			return true;
		}
		return authority == "localhost" || authority.starts_with("localhost:") ||
		       authority == "127.0.0.1" || authority.starts_with("127.0.0.1:") ||
		       authority == "[::1]" || authority.starts_with("[::1]:");
	}

	inline bool ValidateServerSettings(std::string_view base_url, std::string_view endpoint,
	                                   std::string_view model, std::string_view api_key_env,
	                                   std::string* error_out = nullptr)
	{
		if (!IsSafeServerBaseUrl(base_url))
		{
			if (error_out) *error_out = "Voice server must use HTTPS (HTTP is allowed only for localhost).";
			return false;
		}
		const std::string path = uam::strings::Trim(endpoint);
		if (path.size() > 2048 || HasControlCharacters(endpoint) || path.find_first_of(" \\") != std::string::npos || path.empty() || path.front() != '/' || path.find("//") == 0 || path.find('?') != std::string::npos || path.find('#') != std::string::npos)
		{
			if (error_out) *error_out = "Voice server endpoint must be an absolute path without a query or fragment.";
			return false;
		}
		if (model.size() > 256 || HasControlCharacters(model) || uam::strings::Trim(model).empty())
		{
			if (error_out) *error_out = "Voice transcription model is required.";
			return false;
		}
		if (!api_key_env.empty() && !IsEnvironmentVariableName(api_key_env))
		{
			if (error_out) *error_out = "Voice API credential environment variable name is invalid.";
			return false;
		}
		return true;
	}

	inline std::string BuildServerUrl(std::string_view base_url, std::string_view endpoint)
	{
		std::string base = uam::strings::Trim(base_url);
		while (base.size() > 8 && base.back() == '/') base.pop_back();
		return base + uam::strings::Trim(endpoint);
	}

	inline bool ParseTranscriptResponse(std::string_view response, std::string& transcript, std::string* error_out = nullptr)
	{
		transcript.clear();
		if (response.size() > 1024 * 1024)
		{
			if (error_out) *error_out = "Voice server response exceeds the 1 MiB limit.";
			return false;
		}
		const nlohmann::json parsed = nlohmann::json::parse(response, nullptr, false);
		if (!parsed.is_object() || !parsed.contains("text") || !parsed["text"].is_string())
		{
			if (error_out) *error_out = "Voice server returned an invalid transcription response.";
			return false;
		}
		transcript = uam::strings::Trim(parsed["text"].get<std::string>());
		if (transcript.empty())
		{
			if (error_out) *error_out = "Voice server returned an empty transcription.";
			return false;
		}
		return true;
	}
} // namespace uam::voice_input
