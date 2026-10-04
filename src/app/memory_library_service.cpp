#include "common/config/execution_host_config.h"
#include "common/utils/hash_utils.h"
#include "remote/runner_client.h"
#include "app/memory_library_service.h"

#include "app/chat_domain_service.h"
#include "app/memory_service.h"
#include "common/memory/memory_categories.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/utils/io_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/sensitive_text.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace
{
	std::string CanonicalRootKey(const fs::path& root)
	{
		return uam::paths::Utf8PathString(uam::paths::NormalizeExistingPath(root));
	}
	std::string WorkspaceLabel(const fs::path& workspace_root, const ChatSession& chat)
	{
		const std::string filename = uam::paths::Utf8PathString(workspace_root.filename());
		const std::string fallback = uam::strings::NonEmptyOrFallback(filename, "Project memory");
		return uam::strings::TrimOrFallback(chat.title, fallback);
	}
	fs::path FolderWorkspaceRoot(const ChatFolder& folder)
	{
		return PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(uam::strings::Trim(folder.directory));
	}
	void SetError(std::string* error_out, std::string_view message)
	{
		if (error_out != nullptr)
		{
			error_out->assign(message);
		}
	}
	void ClearError(std::string* error_out)
	{
		if (error_out != nullptr)
		{
			error_out->clear();
		}
	}
	void AddUniqueMemoryLibraryRoot(std::vector<MemoryLibraryService::Root>& roots,
	                                std::set<std::string>& seen,
	                                std::string scope_type,
	                                std::string folder_id,
	                                std::string label,
	                                const fs::path& root_path)
	{
		if (root_path.empty())
		{
			return;
		}

		const std::string key = CanonicalRootKey(root_path);
		if (!seen.insert(key).second)
		{
			return;
		}

		MemoryLibraryService::Root root;
		root.scope_type = std::move(scope_type);
		root.folder_id = std::move(folder_id);
		root.label = uam::strings::TrimOrFallback(label, "Project memory");
		root.root_path = root_path;
		roots.push_back(std::move(root));
	}
	std::vector<MemoryLibraryService::Root> CollectAllMemoryRoots(const uam::AppState& app)
	{
		std::vector<MemoryLibraryService::Root> roots;
		std::set<std::string> seen;

		if (!app.data_root.empty())
		{
			AddUniqueMemoryLibraryRoot(roots, seen, "global", "", "Global memory", MemoryService::GlobalMemoryRoot(app.data_root));
		}

		for (const ChatFolder& folder : app.folders)
		{
			if (!uam::paths::IsControllerLocalWorkspace(folder))
			{
				continue;
			}
			const fs::path workspace_root = FolderWorkspaceRoot(folder);
			if (workspace_root.empty())
			{
				continue;
			}
			AddUniqueMemoryLibraryRoot(roots, seen, "folder", folder.id, uam::strings::TrimOrFallback(folder.title, "Project memory"), MemoryService::LocalMemoryRoot(workspace_root));
		}

		for (const ChatSession& chat : app.chats)
		{
			const ChatFolder* folder = uam::paths::FindWorkspaceFolderById(app, chat.folder_id);
			if (uam::strings::IsBlank(chat.workspace_directory) &&
			    !uam::paths::HasGitWorktreeDirectory(chat) &&
			    (folder == nullptr || uam::strings::IsBlank(folder->directory)))
			{
				continue;
			}
			const fs::path workspace_root = uam::paths::ResolveControllerWorkspaceRootPath(app, chat);
			if (workspace_root.empty())
			{
				continue;
			}
			AddUniqueMemoryLibraryRoot(roots, seen, "folder", chat.folder_id, WorkspaceLabel(workspace_root, chat), MemoryService::LocalMemoryRoot(workspace_root));
		}

		return roots;
	}
}

bool MemoryLibraryService::ResolveScope(const uam::AppState& app, std::string_view scope_type, std::string_view folder_id, Scope& out_scope, std::string* error_out)
{
	ClearError(error_out);
	out_scope = Scope{};
	const std::string normalized_scope = uam::strings::TrimAndLowerAscii(scope_type);
	if (normalized_scope == "all")
	{
		out_scope.scope_type = "all";
		out_scope.folder_id.clear();
		out_scope.label = "All memory";
		out_scope.root_path.clear();
		out_scope.roots = CollectAllMemoryRoots(app);
		for (const ChatFolder& folder : app.folders)
		{
			if (uam::paths::IsControllerLocalWorkspace(folder)) continue;
			Scope remote;
			if (!ResolveScope(app, "folder", folder.id, remote, error_out)) return false;
			out_scope.remote_scopes.push_back(std::move(remote));
		}
		return true;
	}

	if (normalized_scope == "global")
	{
		out_scope.scope_type = "global";
		out_scope.folder_id.clear();
		out_scope.label = "Global memory";
		out_scope.root_path = MemoryService::GlobalMemoryRoot(app.data_root);
		out_scope.roots = {Root{"global", "", out_scope.label, out_scope.root_path}};
		return true;
	}

	if (normalized_scope == "folder" || normalized_scope == "local")
	{
		const std::string normalized_folder_id = uam::strings::Trim(folder_id);
		const ChatFolder* folder = ChatDomainService().FindFolderById(app, normalized_folder_id);
		if (folder == nullptr)
		{
			SetError(error_out, "Folder not found: " + normalized_folder_id);
			return false;
		}
		if (!uam::paths::IsControllerLocalWorkspace(*folder))
		{
			const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, folder->execution_host_id);
			if (host == nullptr) { SetError(error_out, "The project's SSH host no longer exists."); return false; }
			out_scope.scope_type = "folder";
			out_scope.folder_id = folder->id;
			out_scope.label = uam::strings::TrimOrFallback(folder->title, "Project memory") + " · " + host->label;
			out_scope.remote_host = *host;
			out_scope.remote_workspace = folder->directory;
			return true;
		}

		const fs::path workspace_root = FolderWorkspaceRoot(*folder);
		if (workspace_root.empty())
		{
			SetError(error_out, "Folder has no workspace directory.");
			return false;
		}

		out_scope.scope_type = "folder";
		out_scope.folder_id = folder->id;
		out_scope.label = uam::strings::TrimOrFallback(folder->title, "Project memory");
		out_scope.root_path = MemoryService::LocalMemoryRoot(workspace_root);
		out_scope.roots = {Root{"folder", folder->id, out_scope.label, out_scope.root_path}};
		return true;
	}

	SetError(error_out, "Unsupported memory scope.");
	return false;
}


namespace
{
	std::unique_ptr<uam::remote::RunnerClient> MemoryClient(const MemoryLibraryService::Scope& scope)
	{
		const ExecutionHost& host = *scope.remote_host;
		return std::make_unique<uam::remote::RunnerClient>(PlatformServicesFactory::Instance().process_service,
		    uam::remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version), host.runner_version, host.runner_protocol_version);
	}
	std::string RemoteMemoryPrefix(const MemoryLibraryService::Scope& scope)
	{
		return "remote:" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(scope.remote_host->id + "\n" + scope.folder_id)) + ":";
	}
	void SetRemoteEntryScope(const MemoryLibraryService::Scope& scope, MemoryLibraryService::Entry& entry)
	{
		entry.scope_type = "folder"; entry.folder_id = scope.folder_id; entry.scope_label = scope.label;
		entry.root_path = uam::paths::PathFromUtf8(scope.remote_workspace + "/.UAM");
		entry.file_path = entry.root_path / uam::paths::PathFromUtf8(entry.id);
	}
}

std::vector<MemoryLibraryService::Entry> MemoryLibraryService::ListEntries(const Scope& scope, std::string* error_out)
{
	if (scope.remote_host)
	{
		std::vector<Entry> entries;
		if (!MemoryClient(scope)->ListMemoryEntries(uam::paths::PathFromUtf8(scope.remote_workspace), entries, error_out)) return {};
		for (Entry& entry : entries) SetRemoteEntryScope(scope, entry);
		return entries;
	}
	std::vector<Entry> entries;
	if (!scope.roots.empty() || !scope.root_path.empty()) entries = MemoryLibraryStore::ListEntries(scope, error_out);
	if (error_out != nullptr && !error_out->empty()) return {};
	for (const Scope& remote : scope.remote_scopes)
	{
		std::vector<Entry> remote_entries = ListEntries(remote, error_out);
		if (error_out != nullptr && !error_out->empty()) return {};
		for (Entry& entry : remote_entries) { entry.id = RemoteMemoryPrefix(remote) + entry.id; entries.push_back(std::move(entry)); }
	}
	return entries;
}
bool MemoryLibraryService::CreateEntry(const Scope& scope, const Draft& draft, Entry* created, std::string* error_out)
{
	if (!scope.remote_host) return MemoryLibraryStore::CreateEntry(scope, draft, created, error_out);
	Entry entry;
	if (!MemoryClient(scope)->CreateMemoryEntry(uam::paths::PathFromUtf8(scope.remote_workspace), draft, entry, error_out)) return false;
	SetRemoteEntryScope(scope, entry);
	if (created != nullptr) *created = std::move(entry);
	return true;
}
bool MemoryLibraryService::DeleteEntry(const Scope& scope, std::string_view id, std::string* error_out)
{
	if (scope.remote_host) return MemoryClient(scope)->DeleteMemoryEntry(uam::paths::PathFromUtf8(scope.remote_workspace), id, error_out);
	if (scope.scope_type == "all" && id.starts_with("remote:"))
	{
		for (const Scope& remote : scope.remote_scopes)
		{
			const std::string prefix = RemoteMemoryPrefix(remote);
			if (id.starts_with(prefix)) return DeleteEntry(remote, id.substr(prefix.size()), error_out);
		}
		SetError(error_out, "The memory entry's SSH workspace no longer exists."); return false;
	}
	return MemoryLibraryStore::DeleteEntry(scope, id, error_out);
}
