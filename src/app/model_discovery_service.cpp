#include "app/model_discovery_service.h"

#include "app/provider_model_catalog_service.h"
#include "app/chat_domain_service.h"
#include "common/config/execution_host_config.h"
#include "common/config/provider_availability.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/state/app_state.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <unordered_set>

namespace uam
{
	namespace
	{
		constexpr std::size_t kMaxConcurrentDiscoveries = 2;

		std::string HostId(const std::string& id)
		{
			return strings::NonEmptyOrFallback(strings::Trim(id), "local");
		}

		std::string Workspace(const AppState& app, const std::string& path, const std::string& host)
		{
			if (host == execution_hosts::kLocalHostId)
				return paths::PortablePathString(paths::NormalizeExistingPath(paths::AbsolutePathNoThrow(paths::PathFromUtf8(path))));
			std::string normalized = strings::Trim(path);
			const ExecutionHost* execution_host = execution_hosts::Find(app.settings.execution_hosts, host);
			if (execution_host != nullptr && strings::ToLowerAscii(execution_host->platform) == "windows")
				std::ranges::replace(normalized, '\\', '/');
			while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
			return normalized;
		}

		std::string WorkspaceIdentity(const AppState& app, const std::string& path, const std::string& host_id)
		{
			const std::string normalized = Workspace(app, path, host_id);
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, host_id);
			return host_id != execution_hosts::kLocalHostId && host != nullptr && strings::ToLowerAscii(host->platform) == "windows"
			    ? strings::ToLowerAscii(normalized) : normalized;
		}

		bool RegisteredWorkspace(const AppState& app, const ModelDiscoveryScope& scope)
		{
			for (const ChatFolder& folder : app.folders)
				if (HostId(folder.execution_host_id) == scope.execution_host_id && !strings::IsBlank(folder.directory) && WorkspaceIdentity(app, folder.directory, scope.execution_host_id) == WorkspaceIdentity(app, scope.workspace_directory, scope.execution_host_id)) return true;
			for (const ChatSession& chat : app.chats)
				if (!chat.imported_read_only && chat.temporary_parent_chat_id.empty() && HostId(chat.execution_host_id) == scope.execution_host_id && WorkspaceIdentity(app, paths::PortablePathString(paths::ResolveWorkspaceRootPath(app, chat)), scope.execution_host_id) == WorkspaceIdentity(app, scope.workspace_directory, scope.execution_host_id)) return true;
			return false;
		}
	}

	StartupModelDiscoveryService::StartupModelDiscoveryService(LaunchFunction launch)
	    : m_launch(launch ? std::move(launch) : LaunchFunction(StartEphemeralAcpModelDiscovery))
	{
	}

	std::vector<ModelDiscoveryScope> StartupModelDiscoveryService::CollectScopes(const AppState& app)
	{
		std::vector<ModelDiscoveryScope> scopes;
		// Thousands of chats share a few workspaces, and resolving each one costs filesystem calls on every launch.
		std::unordered_set<std::string> seen;
		const std::function<void(const std::string&, const std::string&, const std::string&)> add = [&](const std::string& provider_id, const std::string& directory, const std::string& host_id)
		{
			if (strings::IsBlank(directory) || !seen.insert(provider_id + '\n' + host_id + '\n' + directory).second) return;
			ModelDiscoveryScope scope{provider_ids::NormalizeCliProviderAliasOrSelf(provider_id), {}, HostId(host_id)};
			// Poll drops a turned-off provider's scope anyway; skipping it here saves resolving its workspace.
			if (!provider_availability::IsEnabled(app.settings, scope.provider_id, scope.execution_host_id)) return;
			scope.workspace_directory = Workspace(app, directory, scope.execution_host_id);
			const ProviderProfile* provider = ProviderProfileStore::FindById(app.provider_profiles, scope.provider_id);
			if (provider == nullptr || !provider->supports_structured || !RegisteredWorkspace(app, scope)) return;
			if (std::ranges::find_if(scopes, [&](const ModelDiscoveryScope& existing) {
			    return existing.provider_id == scope.provider_id && existing.execution_host_id == scope.execution_host_id &&
			           WorkspaceIdentity(app, existing.workspace_directory, existing.execution_host_id) == WorkspaceIdentity(app, scope.workspace_directory, scope.execution_host_id);
			}) == scopes.end()) scopes.push_back(std::move(scope));
		};
		if (app.provider_model_catalog != nullptr)
			for (const nlohmann::json& scope : app.provider_model_catalog->GetCatalogScopes())
				add(scope.value("providerId", ""), scope.value("workspaceDirectory", ""), scope.value("executionHostId", "local"));
		for (const ChatSession& chat : app.chats)
			if (!chat.imported_read_only && chat.temporary_parent_chat_id.empty())
				add(chat.provider_id, paths::PortablePathString(paths::ResolveWorkspaceRootPath(app, chat)), chat.execution_host_id);
		for (const ChatFolder& folder : app.folders)
			add(app.settings.default_new_chat_provider_id, folder.directory, folder.execution_host_id);
		const ChatDomainService domain;
		const ChatFolder* initial_folder = domain.FindFolderById(app, app.new_chat_folder_id);
		const ChatSession* selected = domain.SelectedChat(app);
		if (initial_folder == nullptr && selected != nullptr) initial_folder = domain.FindFolderById(app, selected->folder_id);
		if (initial_folder == nullptr && !app.folders.empty()) initial_folder = &app.folders.front();
		if (initial_folder != nullptr)
			for (const ProviderProfile& provider : app.provider_profiles)
				if (ProviderRuntime::IsRuntimeEnabled(provider) && provider_availability::IsEnabled(app.settings, provider.id, initial_folder->execution_host_id))
					add(provider.id, initial_folder->directory, initial_folder->execution_host_id);
		return scopes;
	}

	void StartupModelDiscoveryService::Initialize(AppState& app)
	{
		if (m_initialized) return;
		m_initialized = true;
		for (ModelDiscoveryScope& scope : CollectScopes(app))
		{
			// One discovery answers every workspace of an account-wide provider on the same computer.
			const bool account_wide = ProviderRuntimeRegistry::ResolveById(scope.provider_id).ModelListIsAccountWide();
			const std::vector<ScopeGroup>::iterator group = !account_wide ? m_pending_groups.end() : std::ranges::find_if(m_pending_groups, [&](const ScopeGroup& existing) {
				return existing.provider_id == scope.provider_id && existing.execution_host_id == scope.execution_host_id;
			});
			if (group != m_pending_groups.end()) group->workspaces.push_back(std::move(scope.workspace_directory));
			else m_pending_groups.push_back({scope.provider_id, scope.execution_host_id, {std::move(scope.workspace_directory)}});
		}
		for (const ExecutionHost& host : app.settings.execution_hosts) m_hosts.emplace(host.id, host);
	}

	void StartupModelDiscoveryService::NoteCheckedCliVersions(AppState& app)
	{
		// Successful discoveries store the version that fetched them, so a later restart can tell whether the CLI changed.
		for (const auto& [key, state] : app.runtime_cli_versions_by_provider_id)
		{
			const std::string version = state.checked && state.check_error.empty() ? strings::Trim(state.installed_version) : std::string{};
			const auto noted = m_noted_cli_versions.find(key);
			if (noted != m_noted_cli_versions.end() && noted->second == version) continue;
			m_noted_cli_versions[key] = version;
			const std::size_t separator = key.rfind('/');
			const std::string provider = separator == std::string::npos ? key : key.substr(separator + 1);
			const std::string host = separator == std::string::npos ? std::string(execution_hosts::kLocalHostId) : key.substr(0, separator);
			app.provider_model_catalog->NoteCheckedCliVersion(provider, host, version);
		}
	}

	bool StartupModelDiscoveryService::ShareFinishedDiscoveries(AppState& app)
	{
		bool changed = false;
		for (std::vector<PendingShare>::iterator share = m_pending_shares.begin(); share != m_pending_shares.end();)
		{
			if (app.provider_model_catalog->IsDiscoveryPending(share->provider_id, share->source_workspace, share->execution_host_id)) { ++share; continue; }
			// A failed or empty discovery shares nothing; the other workspaces keep their caches and their own Refresh.
			changed = app.provider_model_catalog->ShareAccountWideCatalog(share->provider_id, share->execution_host_id, share->source_workspace, share->target_workspaces, share->source_before) > 0 || changed;
			share = m_pending_shares.erase(share);
		}
		return changed;
	}

	bool StartupModelDiscoveryService::Poll(AppState& app)
	{
		if (!m_initialized || app.provider_model_catalog == nullptr) return false;
		NoteCheckedCliVersions(app);
		bool changed = ShareFinishedDiscoveries(app);
		std::size_t active = std::ranges::count_if(app.acp_sessions, [](const std::unique_ptr<AcpSessionState>& session) {
			return session != nullptr && session->ephemeral_model_discovery && session->running;
		});
		active += app.provider_model_catalog->ActiveNativeModelDiscoveries();
		for (std::vector<ScopeGroup>::iterator group = m_pending_groups.begin(); group != m_pending_groups.end() && active < kMaxConcurrentDiscoveries;)
		{
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, group->execution_host_id);
			const std::map<std::string, ExecutionHost>::const_iterator captured_host = m_hosts.find(group->execution_host_id);
			const ProviderProfile* provider = ProviderProfileStore::FindById(app.provider_profiles, group->provider_id);
			if (host == nullptr || captured_host == m_hosts.end() || !execution_hosts::SameConnection(*host, captured_host->second) ||
			    provider == nullptr || !provider->supports_structured || !provider_availability::IsEnabled(app.settings, group->provider_id, group->execution_host_id))
			{
				group = m_pending_groups.erase(group);
				continue;
			}
			if (host->transport == "ssh" && host->runner_status != "ready") { ++group; continue; }
			std::erase_if(group->workspaces, [&](const std::string& workspace) {
				std::error_code path_error;
				return !RegisteredWorkspace(app, {group->provider_id, workspace, group->execution_host_id}) ||
				       (host->transport == "ssh" && !execution_hosts::IsAbsoluteRemotePath(host->platform, workspace)) ||
				       (host->transport == "local" && !std::filesystem::is_directory(paths::PathFromUtf8(workspace), path_error));
			});
			if (group->workspaces.empty())
			{
				group = m_pending_groups.erase(group);
				continue;
			}
			const std::string version_key = CliProviderVersionStateKey(group->provider_id, group->execution_host_id);
			const std::unordered_map<std::string, CliProviderVersionState>::const_iterator version = app.runtime_cli_versions_by_provider_id.find(version_key);
			if (version == app.runtime_cli_versions_by_provider_id.end())
			{
				std::string check_error;
				changed = ProviderCliCompatibilityService().StartProviderVersionCheck(app, group->provider_id, false, group->execution_host_id, &check_error) || changed;
				++group;
				continue;
			}
			if (!ProviderCliLaunchBlockReason(app, group->provider_id, group->execution_host_id).empty() || !version->second.checked) { ++group; continue; }
			const std::string compatibility_error = host->transport == "local" ? ProviderRuntimeRegistry::ResolveById(group->provider_id).LocalCliCompatibilityError(app) : std::string{};
			// A failed local check says nothing about the installation; launch applies the same rule.
			const bool local_check_failed = host->transport == "local" && !version->second.check_error.empty();
			if (!compatibility_error.empty() || (!version->second.supported && !local_check_failed))
			{
				diagnostics::Write("Startup model discovery skipped an incompatible provider: " + group->provider_id);
				group = m_pending_groups.erase(group);
				continue;
			}
			const ScopeGroup scopes = std::move(*group);
			group = m_pending_groups.erase(group);
			// A cache under seven days old that this CLI version fetched is kept; Refresh and Retry still ask the provider.
			const std::string checked_version = version->second.check_error.empty() ? strings::Trim(version->second.installed_version) : std::string{};
			std::vector<std::string> fresh;
			std::vector<std::string> stale;
			for (const std::string& workspace : scopes.workspaces)
				(app.provider_model_catalog->IsCacheFreshForCliVersion(scopes.provider_id, workspace, scopes.execution_host_id, checked_version) ? fresh : stale).push_back(workspace);
			if (stale.empty()) continue;
			if (!fresh.empty())
			{
				// Account-wide only: another workspace already holds a current list, so no process is needed.
				changed = app.provider_model_catalog->ShareAccountWideCatalog(scopes.provider_id, scopes.execution_host_id, fresh.front(), stale) > 0 || changed;
				continue;
			}
			const std::string& workspace = stale.front();
			const std::vector<std::string> targets(stale.begin() + 1, stale.end());
			const nlohmann::json source_before = app.provider_model_catalog->GetPersistentCatalogEntry(scopes.provider_id, workspace, scopes.execution_host_id);
			// Keep cached choices visible while the scope is attempted once; an already pending explicit refresh owns it,
			// and its result is shared the same way when it ends.
			if (!app.provider_model_catalog->BeginDiscovery(scopes.provider_id, workspace, scopes.execution_host_id))
			{
				if (!targets.empty() && app.provider_model_catalog->IsDiscoveryPending(scopes.provider_id, workspace, scopes.execution_host_id))
					m_pending_shares.push_back({scopes.provider_id, scopes.execution_host_id, workspace, targets, source_before});
				continue;
			}
			std::string error;
			bool launched = false;
			// One unusable saved workspace fails its own scope, never the whole startup poll.
			try { launched = m_launch(app, scopes.provider_id, workspace, scopes.execution_host_id, &error); }
			catch (const std::exception& exception) { error = std::string("unexpected error: ") + exception.what(); }
			catch (...) { error = "unexpected error."; }
			if (launched)
			{
				app.provider_model_catalog->MarkDiscoveryLaunchStarted(scopes.provider_id, workspace, scopes.execution_host_id);
				if (!targets.empty()) m_pending_shares.push_back({scopes.provider_id, scopes.execution_host_id, workspace, targets, source_before});
				++active;
			}
			else
			{
				app.provider_model_catalog->RememberRefreshFailure(scopes.provider_id, strings::NonEmptyOrFallback(error, "Provider model discovery failed to start."), workspace, scopes.execution_host_id);
				diagnostics::Write("Startup model discovery launch failed for " + scopes.provider_id + ": " + error);
			}
			changed = true;
		}
		// Query only the visible saved Antigravity model; native /effort is a read-only builtin, never a model turn.
		// The catalogue deduplicates attempts and retains scoped metadata so idle polling cannot spawn repeated probes.
		const ChatSession* selected = ChatDomainService().SelectedChat(app);
		if (active < kMaxConcurrentDiscoveries && selected != nullptr && !selected->imported_read_only &&
		    provider_ids::IsCliProviderAliasOf(selected->provider_id, provider_ids::kAntigravityCli))
		{
			const std::string host_id = HostId(selected->execution_host_id);
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, host_id);
			const ProviderProfile* provider = ProviderProfileStore::FindById(app.provider_profiles, provider_ids::kAntigravityCli);
			const std::string workspace = paths::PortablePathString(paths::ResolveWorkspaceRootPath(app, *selected));
			std::string model = selected->model_id;
			if (model.empty())
				for (const auto& entry : app.provider_model_catalog->GetCachedProviderModels(selected->provider_id, workspace, host_id))
					if (entry.value("isDefault", false)) { model = entry.value("id", ""); break; }
			if (host != nullptr && provider != nullptr && !model.empty() && provider_availability::IsEnabled(app.settings, selected->provider_id, host_id) &&
			    ProviderCliLaunchBlockReason(app, selected->provider_id, host_id).empty())
			{
				const auto before = app.provider_model_catalog->ActiveNativeModelDiscoveries();
				std::string error;
				(void)app.provider_model_catalog->StartAntigravityEffortDiscovery(*provider, *host, workspace, model, &error);
				changed = app.provider_model_catalog->ActiveNativeModelDiscoveries() > before || changed;
			}
		}
		return changed;
	}
}
