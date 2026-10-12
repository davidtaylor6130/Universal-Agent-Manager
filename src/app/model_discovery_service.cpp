#include "app/model_discovery_service.h"

#include "app/provider_model_catalog_service.h"
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
#include <filesystem>

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
				return paths::NormalizeExistingPath(paths::AbsolutePathNoThrow(path)).generic_string();
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
				if (!chat.imported_read_only && chat.temporary_parent_chat_id.empty() && HostId(chat.execution_host_id) == scope.execution_host_id && WorkspaceIdentity(app, paths::ResolveWorkspaceRootPath(app, chat).generic_string(), scope.execution_host_id) == WorkspaceIdentity(app, scope.workspace_directory, scope.execution_host_id)) return true;
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
		const std::function<void(const std::string&, const std::string&, const std::string&)> add = [&](const std::string& provider_id, const std::string& directory, const std::string& host_id)
		{
			if (strings::IsBlank(directory)) return;
			ModelDiscoveryScope scope{provider_ids::NormalizeCliProviderAliasOrSelf(provider_id), {}, HostId(host_id)};
			scope.workspace_directory = Workspace(app, directory, scope.execution_host_id);
			const ProviderProfile* provider = ProviderProfileStore::FindById(app.provider_profiles, scope.provider_id);
			if (provider == nullptr || !provider->supports_structured || scope.provider_id == provider_ids::kAntigravityCli || !RegisteredWorkspace(app, scope)) return;
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
				add(chat.provider_id, paths::ResolveWorkspaceRootPath(app, chat).generic_string(), chat.execution_host_id);
		for (const ChatFolder& folder : app.folders)
			add(app.settings.default_new_chat_provider_id, folder.directory, folder.execution_host_id);
		return scopes;
	}

	void StartupModelDiscoveryService::Initialize(AppState& app)
	{
		if (m_initialized) return;
		m_initialized = true;
		m_pending_scopes = CollectScopes(app);
		for (const ExecutionHost& host : app.settings.execution_hosts) m_hosts.emplace(host.id, host);
	}

	bool StartupModelDiscoveryService::Poll(AppState& app)
	{
		if (!m_initialized || app.provider_model_catalog == nullptr) return false;
		std::size_t active = std::ranges::count_if(app.acp_sessions, [](const std::unique_ptr<AcpSessionState>& session) {
			return session != nullptr && session->ephemeral_model_discovery && session->running;
		});
		bool changed = false;
		for (std::vector<ModelDiscoveryScope>::iterator item = m_pending_scopes.begin(); item != m_pending_scopes.end() && active < kMaxConcurrentDiscoveries;)
		{
			const ModelDiscoveryScope scope = *item;
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, scope.execution_host_id);
			const std::map<std::string, ExecutionHost>::const_iterator captured_host = m_hosts.find(scope.execution_host_id);
			const ProviderProfile* provider = ProviderProfileStore::FindById(app.provider_profiles, scope.provider_id);
			if (host == nullptr || captured_host == m_hosts.end() || !execution_hosts::SameConnection(*host, captured_host->second) ||
			    provider == nullptr || !provider->supports_structured || !RegisteredWorkspace(app, scope) || !provider_availability::IsEnabled(app.settings, scope.provider_id, scope.execution_host_id))
			{
				item = m_pending_scopes.erase(item);
				continue;
			}
			if (host->transport == "ssh" && host->runner_status != "ready") { ++item; continue; }
			std::error_code path_error;
			if ((host->transport == "ssh" && !execution_hosts::IsAbsoluteRemotePath(host->platform, scope.workspace_directory)) ||
			    (host->transport == "local" && !std::filesystem::is_directory(scope.workspace_directory, path_error)))
			{
				item = m_pending_scopes.erase(item);
				continue;
			}
			const std::string version_key = CliProviderVersionStateKey(scope.provider_id, scope.execution_host_id);
			const std::unordered_map<std::string, CliProviderVersionState>::const_iterator version = app.runtime_cli_versions_by_provider_id.find(version_key);
			if (version == app.runtime_cli_versions_by_provider_id.end())
			{
				std::string check_error;
				changed = ProviderCliCompatibilityService().StartProviderVersionCheck(app, scope.provider_id, false, scope.execution_host_id, &check_error) || changed;
				++item;
				continue;
			}
			if (!ProviderCliLaunchBlockReason(app, scope.provider_id, scope.execution_host_id).empty() || (version != app.runtime_cli_versions_by_provider_id.end() && !version->second.checked)) { ++item; continue; }
			const std::string compatibility_error = host->transport == "local" ? ProviderRuntimeRegistry::ResolveById(scope.provider_id).LocalCliCompatibilityError(app) : std::string{};
			if (!compatibility_error.empty() || (version != app.runtime_cli_versions_by_provider_id.end() && !version->second.supported))
			{
				diagnostics::Write("Startup model discovery skipped an incompatible provider: " + scope.provider_id);
				item = m_pending_scopes.erase(item);
				continue;
			}
			item = m_pending_scopes.erase(item);
			// Restart refreshes even a recent cache. Keep its choices visible while the
			// startup scope is attempted once; an already pending explicit refresh owns it.
			if (!app.provider_model_catalog->BeginDiscovery(scope.provider_id, scope.workspace_directory, scope.execution_host_id)) continue;
			std::string error;
			if (m_launch(app, scope.provider_id, scope.workspace_directory, scope.execution_host_id, &error))
			{
				app.provider_model_catalog->MarkDiscoveryLaunchStarted(scope.provider_id, scope.workspace_directory, scope.execution_host_id);
				++active;
			}
			else
			{
				app.provider_model_catalog->RememberRefreshFailure(scope.provider_id, strings::NonEmptyOrFallback(error, "Provider model discovery failed to start."), scope.workspace_directory, scope.execution_host_id);
				diagnostics::Write("Startup model discovery launch failed for " + scope.provider_id + ": " + error);
			}
			changed = true;
		}
		return changed;
	}
}
