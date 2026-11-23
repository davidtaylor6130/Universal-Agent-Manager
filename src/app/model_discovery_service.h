#pragma once

#include "common/models/app_models.h"

#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace uam
{
	struct AppState;

	struct ModelDiscoveryScope
	{
		std::string provider_id;
		std::string workspace_directory;
		std::string execution_host_id;
		bool operator==(const ModelDiscoveryScope&) const = default;
	};

	/// <summary>
	/// Discovers registered startup scopes independently of user conversations, with bounded concurrency. A provider whose
	/// model list does not depend on the workspace is asked once per computer and the result is shared with its other
	/// workspaces; a workspace-dependent provider is asked per workspace. Caches under seven days old that the current CLI
	/// version fetched are kept without starting a process.
	/// </summary>
	class StartupModelDiscoveryService
	{
	  public:
		using LaunchFunction = std::function<bool(AppState&, const std::string&, const std::string&, const std::string&, std::string*)>;
		explicit StartupModelDiscoveryService(LaunchFunction launch = {});
		/// <summary>Captures registered scopes once; reopening a picker does not refill the startup queue.</summary>
		void Initialize(AppState& app);
		/// <summary>Refreshes stale startup scopes once per run; starts at most two processes and lets unavailable scopes wait.</summary>
		bool Poll(AppState& app);
		static std::vector<ModelDiscoveryScope> CollectScopes(const AppState& app);

	  private:
		/// <summary>Scopes answered by one discovery: every workspace of an account-wide provider on one computer, otherwise one workspace.</summary>
		struct ScopeGroup
		{
			std::string provider_id;
			std::string execution_host_id;
			std::vector<std::string> workspaces;
		};
		/// <summary>A started account-wide discovery whose result is shared with the other workspaces when it ends.</summary>
		struct PendingShare
		{
			std::string provider_id;
			std::string execution_host_id;
			std::string source_workspace;
			std::vector<std::string> target_workspaces;
			nlohmann::json source_before;
		};
		bool ShareFinishedDiscoveries(AppState& app);
		void NoteCheckedCliVersions(AppState& app);

		bool m_initialized = false;
		LaunchFunction m_launch;
		std::vector<ScopeGroup> m_pending_groups;
		std::vector<PendingShare> m_pending_shares;
		std::map<std::string, ExecutionHost> m_hosts;
		std::map<std::string, std::string> m_noted_cli_versions;
	};
}
