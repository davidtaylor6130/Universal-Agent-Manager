#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// OS credential-store access for existing provider logins. Callers own validation, wording and secret lifetime.
namespace uam::platform
{
	/// macOS Keychain helper: prints {"status":<OSStatus>[,"credential":...]} as JSON; "silent" disables Keychain interaction.
	std::vector<std::string> MacKeychainReadArguments(std::string_view service, std::string_view account, bool allow_prompt);
	/// Linux Secret Service helper: secret-tool search by go-keyring attributes; --unlock only for an explicit refresh.
	std::vector<std::string> SecretServiceSearchArguments(std::string_view service, std::string_view account, bool allow_prompt);

	/// UAM's own secrets. macOS: Keychain via Security.framework in this process, so the item's access list trusts
	/// UAM and other programs (including the `security` tool) must ask the desktop user. Windows: Credential Manager,
	/// readable by any process of the same user. Other systems report no store (nullopt/false).
	std::optional<std::string> ReadAppSecret(std::string_view service, std::string_view account);
	bool WriteAppSecret(std::string_view service, std::string_view account, std::string_view secret);

#if defined(_WIN32)
	enum class StoredCredentialStatus { Found, NotFound, Unreadable };
	struct StoredCredential
	{
		StoredCredentialStatus status = StoredCredentialStatus::Unreadable;
		std::string secret; // Private memory only.
	};
	/// Reads a Windows Credential Manager generic entry without showing UI; blobs larger than limit are Unreadable.
	StoredCredential ReadWindowsGenericCredential(std::string_view target, std::size_t limit);
#endif
}
