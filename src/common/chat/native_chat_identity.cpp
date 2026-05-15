#include "common/chat/native_chat_identity.h"

#include "common/paths/path_utils.h"
#include "common/provider/provider_ids.h"
#include "common/utils/hash_utils.h"
#include "common/utils/string_utils.h"

#include <filesystem>
#include <string_view>

namespace fs = std::filesystem;

namespace
{
	constexpr char kNativeIdentityKeyDelimiter = '|';

	std::filesystem::path TrimmedWorkspacePath(const ChatSession& chat)
	{
		const std::string_view trimmed_workspace = uam::strings::TrimAsciiView(chat.workspace_directory);
		return trimmed_workspace.empty() ? fs::path{} : fs::path(std::string(trimmed_workspace));
	}

	void AppendNativeIdentityKeyPart(std::string& key, std::string_view value, bool& needs_delimiter)
	{
		if (needs_delimiter)
		{
			key.push_back(kNativeIdentityKeyDelimiter);
		}
		key.append(value);
		needs_delimiter = true;
	}

	std::string MakeNativeIdentityKey(const ChatSession& chat, std::string_view workspace_key)
	{
		std::string key;
		bool needs_delimiter = false;
		AppendNativeIdentityKeyPart(key, uam::provider_ids::CanonicalCliProviderLookupId(chat.provider_id), needs_delimiter);
		AppendNativeIdentityKeyPart(key, workspace_key, needs_delimiter);
		AppendNativeIdentityKeyPart(key, uam::strings::TrimAsciiView(chat.native_session_id), needs_delimiter);
		return key;
	}
} // namespace

namespace uam::chat_identity
{
	std::string NativeWorkspaceForLocalDeduplication(const ChatSession& chat)
	{
		const fs::path workspace_path = TrimmedWorkspacePath(chat);
		if (!workspace_path.empty())
		{
			return uam::paths::NormalizedPortablePathString(workspace_path);
		}

		return uam::strings::Trim(chat.folder_id);
	}

	std::string NativeWorkspaceForHistoryImport(const ChatSession& chat)
	{
		const fs::path workspace_path = TrimmedWorkspacePath(chat);
		if (workspace_path.empty())
		{
			return "";
		}

		return uam::paths::NormalizeExistingPortablePathString(workspace_path);
	}

	std::string NativeIdentityKeyForLocalDeduplication(const ChatSession& chat)
	{
		return MakeNativeIdentityKey(chat, NativeWorkspaceForLocalDeduplication(chat));
	}

	std::string NativeIdentityKeyForHistoryImport(const ChatSession& chat)
	{
		return MakeNativeIdentityKey(chat, NativeWorkspaceForHistoryImport(chat));
	}

	std::string NativeIdentityKeyHash(std::string_view key)
	{
		return uam::hashing::Hex64(uam::hashing::Fnv1a64(key));
	}
} // namespace uam::chat_identity
