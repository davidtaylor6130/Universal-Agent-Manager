#pragma once

#include <optional>
#include <string>
#include <string_view>

// Saved secrets in the macOS Keychain, written by UAM itself. Not offered on Windows. Values never enter argv or environment.
namespace uam::secrets::keychain
{
	inline constexpr std::string_view kService = "Universal Agent Manager secrets";

	/// True on macOS only: Windows Credential Manager would let any program of the user read the value; desktop Linux is out of scope.
	bool Available();
	bool Save(std::string_view account, std::string_view value, std::string* error);
	/// May show the system prompt. nullopt with an empty error means nothing is saved for account.
	std::optional<std::string> Load(std::string_view account, std::string* error);
	/// Succeeds when nothing is saved for account.
	bool Forget(std::string_view account, std::string* error);

}
