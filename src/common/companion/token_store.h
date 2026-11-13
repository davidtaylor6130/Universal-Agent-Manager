#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The phone connection token lives in the OS credential store, never in a file inside the data root.
// Pure decisions here; the OS access is injected so tests never touch the user's credential store.
namespace uam::companion
{
	inline constexpr std::string_view kTokenStoreField = "token_store";
	inline constexpr std::string_view kCredentialTokenStore = "credential_store";
	inline constexpr std::string_view kTokenCredentialService = "Universal Agent Manager Companion";

	inline bool IsValidToken(std::string_view token)
	{
		return token.size() == 64 && token.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
	}

	inline bool UsesCredentialStore(const nlohmann::json& config)
	{
		if (!config.is_object()) return false;
		const auto field = config.find(std::string(kTokenStoreField));
		return field != config.end() && field->is_string() && field->get_ref<const std::string&>() == kCredentialTokenStore;
	}

	inline bool HasLegacyTokenFile(const nlohmann::json& config)
	{
		if (!config.is_object()) return false;
		const auto field = config.find("token_file");
		return field != config.end() && field->is_string() && !field->get_ref<const std::string&>().empty();
	}

	/// Configured phone access has a token source: the credential store, or a legacy file awaiting migration.
	inline bool HasTokenSource(const nlohmann::json& config)
	{
		return UsesCredentialStore(config) || HasLegacyTokenFile(config);
	}

	/// One credential per data root, so isolated data roots never share or overwrite another root's token.
	/// FNV-1a keeps the name stable across builds and ASCII for every store.
	inline std::string TokenCredentialAccount(const std::filesystem::path& data_root)
	{
		const std::u8string path = data_root.lexically_normal().generic_u8string();
		std::uint64_t hash = 14695981039346656037ull;
		for (const char8_t byte : path)
		{
			hash ^= static_cast<unsigned char>(byte);
			hash *= 1099511628211ull;
		}
		static constexpr char kHex[] = "0123456789abcdef";
		std::string account = "companion-token-";
		for (int shift = 60; shift >= 0; shift -= 4) account += kHex[(hash >> shift) & 0xf];
		return account;
	}

	struct TokenStoreAccess
	{
		std::function<std::optional<std::string>()> read;
		std::function<bool(const std::string&)> write;
	};

	/// Process-wide replacement for the OS store; only tests set it.
	inline std::function<TokenStoreAccess(const std::filesystem::path&)>& TokenStoreOverrideForTests()
	{
		static std::function<TokenStoreAccess(const std::filesystem::path&)> override_factory;
		return override_factory;
	}

	/// Moves a valid legacy file token into the store and returns the configuration to save,
	/// or nullopt when nothing may change. The token value is unchanged, so paired phones keep working.
	/// The caller saves the result before deleting the legacy file.
	inline std::optional<nlohmann::json> MigrateLegacyToken(nlohmann::json config, const std::optional<std::string>& file_text, const TokenStoreAccess& store)
	{
		if (UsesCredentialStore(config) || !HasLegacyTokenFile(config) || !file_text) return std::nullopt;
		std::string token = *file_text;
		while (!token.empty() && std::string_view(" \t\r\n").find(token.back()) != std::string_view::npos) token.pop_back();
		if (!IsValidToken(token)) return std::nullopt;
		if (store.read() != token && (!store.write(token) || store.read() != token)) return std::nullopt;
		config.erase("token_file");
		config[std::string(kTokenStoreField)] = kCredentialTokenStore;
		return config;
	}
}
