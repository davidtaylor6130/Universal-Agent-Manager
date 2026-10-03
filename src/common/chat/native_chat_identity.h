#pragma once

#include "common/models/app_models.h"

#include <string>
#include <string_view>

namespace uam::chat_identity
{
	std::string NativeWorkspaceForLocalDeduplication(const ChatSession& chat);
	std::string NativeWorkspaceForHistoryImport(const ChatSession& chat);
	std::string NativeIdentityKeyForLocalDeduplication(const ChatSession& chat);
	std::string NativeIdentityKeyForHistoryImport(const ChatSession& chat);
	/// <summary>Codex writer ownership is per host and thread, independent of its workspace.</summary>
	std::string CodexWriterIdentityKey(const ChatSession& chat);
	std::string NativeIdentityKeyHash(std::string_view key);
} // namespace uam::chat_identity
