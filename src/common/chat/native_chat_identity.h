#pragma once

#include "common/models/app_models.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace uam::chat_identity
{
	std::string NativeWorkspaceForLocalDeduplication(const ChatSession& chat);
	std::string NativeWorkspaceForHistoryImport(const ChatSession& chat);
	std::string NativeIdentityKeyForLocalDeduplication(const ChatSession& chat);
	std::string NativeIdentityKeyForHistoryImport(const ChatSession& chat);
	/// <summary>Raw local workspace text to its resolved form; valid only within one pass over unchanged directories.</summary>
	using NativeWorkspaceCache = std::unordered_map<std::string, std::string>;
	/// <summary>Same result as the uncached form, resolving each distinct local workspace once per cache.</summary>
	std::string NativeIdentityKeyForHistoryImport(const ChatSession& chat, NativeWorkspaceCache& workspaces);
	/// <summary>Codex writer ownership is per host and thread, independent of its workspace.</summary>
	std::string CodexWriterIdentityKey(const ChatSession& chat);
	std::string NativeIdentityKeyHash(std::string_view key);
} // namespace uam::chat_identity
