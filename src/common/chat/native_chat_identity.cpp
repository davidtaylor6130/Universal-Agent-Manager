#include "common/chat/native_chat_identity.h"

#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/provider/provider_ids.h"
#include "common/utils/hash_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	constexpr char kNativeIdentityKeyDelimiter = '|';

	std::string_view TrimmedWorkspaceText(const ChatSession& chat)
	{
		// Native sessions run in the worktree, while UAM keeps the project folder
		// separately for navigation. Match the provider's actual working directory.
		const std::string_view worktree = uam::strings::TrimAsciiView(chat.workspace_worktree_directory);
		return worktree.empty() ? uam::strings::TrimAsciiView(chat.workspace_directory) : worktree;
	}

	std::filesystem::path TrimmedWorkspacePath(const ChatSession& chat)
	{
		const std::string_view trimmed_workspace = TrimmedWorkspaceText(chat);
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
		const std::string_view host = uam::strings::TrimAsciiView(chat.execution_host_id);
		if (!host.empty() && host != uam::execution_hosts::kLocalHostId)
		{
			return NativeWorkspaceForHistoryImport(chat);
		}

		const fs::path workspace_path = TrimmedWorkspacePath(chat);
		if (!workspace_path.empty())
		{
			return uam::paths::NormalizedPortablePathString(workspace_path);
		}

		return uam::strings::Trim(chat.folder_id);
	}

	std::string NativeWorkspaceForHistoryImport(const ChatSession& chat)
	{
		const std::string_view host = uam::strings::TrimAsciiView(chat.execution_host_id);
		if (!host.empty() && host != uam::execution_hosts::kLocalHostId)
		{
			std::string workspace = uam::strings::Trim(uam::strings::IsBlank(chat.workspace_worktree_directory) ? chat.workspace_directory : chat.workspace_worktree_directory);
			if (uam::execution_hosts::IsAbsoluteRemotePath("windows", workspace))
				std::ranges::replace(workspace, '\\', '/');
			return "remote:" + std::string(host) + ":" + workspace;
		}
		const fs::path workspace_path = TrimmedWorkspacePath(chat);
		if (workspace_path.empty())
		{
			return "";
		}

		return uam::paths::NormalizeExistingPortablePathString(workspace_path);
	}

	bool NativeProjectCheckoutsMatch(const ChatSession& lhs, const ChatSession& rhs)
	{
		const std::string workspace = NativeWorkspaceForHistoryImport(lhs);
		if (!workspace.empty() && workspace == NativeWorkspaceForHistoryImport(rhs)) return true;
		const auto local = [](const ChatSession& chat)
		{
			const std::string_view host = uam::strings::TrimAsciiView(chat.execution_host_id);
			return host.empty() || host == uam::execution_hosts::kLocalHostId;
		};
		if (!local(lhs) || !local(rhs)) return false;
		const auto checkouts = [](const ChatSession& chat)
		{
			std::vector<std::string> paths;
			for (const std::string* raw : {&chat.workspace_worktree_directory, &chat.workspace_directory, &chat.workspace_source_directory})
			{
				const std::string_view trimmed = uam::strings::TrimAsciiView(*raw);
				if (!trimmed.empty()) paths.push_back(uam::paths::NormalizeExistingPortablePathString(fs::path(std::string(trimmed))));
			}
			return paths;
		};
		const std::vector<std::string> left = checkouts(lhs);
		const std::vector<std::string> right = checkouts(rhs);
		return std::ranges::any_of(left, [&](const std::string& path) { return std::ranges::find(right, path) != right.end(); });
	}

	std::string NativeIdentityKeyForLocalDeduplication(const ChatSession& chat)
	{
		return MakeNativeIdentityKey(chat, NativeWorkspaceForLocalDeduplication(chat));
	}

	std::string NativeIdentityKeyForHistoryImport(const ChatSession& chat)
	{
		return MakeNativeIdentityKey(chat, NativeWorkspaceForHistoryImport(chat));
	}

	std::string NativeChildIdentityKeyForHistoryImport(const ChatSession& chat)
	{
		return chat.imported_read_only && uam::strings::IsBlank(chat.native_session_id) ? "passive|" + chat.id : NativeIdentityKeyForHistoryImport(chat);
	}

	std::string NativeParentIdentityKeyForHistoryImport(const ChatSession& child)
	{
		const std::string_view parent_id = uam::strings::TrimAsciiView(child.native_parent_session_id);
		if (parent_id.empty()) return {};
		ChatSession parent;
		parent.provider_id = child.provider_id;
		parent.native_session_id = std::string(parent_id);
		parent.execution_host_id = child.execution_host_id;
		parent.workspace_directory = child.workspace_directory;
		parent.workspace_worktree_directory = child.workspace_worktree_directory;
		return NativeIdentityKeyForHistoryImport(parent);
	}

	std::string NativeIdentityKeyForHistoryImport(const ChatSession& chat, NativeWorkspaceCache& workspaces)
	{
		const std::string_view host = uam::strings::TrimAsciiView(chat.execution_host_id);
		// Remote keys are lexical; only local workspaces touch the filesystem.
		if (!host.empty() && host != uam::execution_hosts::kLocalHostId) return NativeIdentityKeyForHistoryImport(chat);
		const std::string_view text = TrimmedWorkspaceText(chat);
		if (text.empty()) return MakeNativeIdentityKey(chat, "");
		const auto [workspace, inserted] = workspaces.try_emplace(std::string(text));
		if (inserted) workspace->second = uam::paths::NormalizeExistingPortablePathString(TrimmedWorkspacePath(chat));
		return MakeNativeIdentityKey(chat, workspace->second);
	}

	std::string CodexWriterIdentityKey(const ChatSession& chat)
	{
		if (!uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kCodexCli) ||
		    uam::strings::IsBlank(chat.native_session_id)) return {};
		return "codex-cli|" + uam::strings::NonEmptyOrFallback(uam::strings::Trim(chat.execution_host_id), "local") +
		    "|" + uam::strings::Trim(chat.native_session_id);
	}

	std::string NativeIdentityKeyHash(std::string_view key)
	{
		return uam::hashing::Hex64(uam::hashing::Fnv1a64(key));
	}
} // namespace uam::chat_identity
