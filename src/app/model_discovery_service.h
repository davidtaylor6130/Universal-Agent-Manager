#pragma once

#include "common/models/app_models.h"

#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
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
			// Sources tried this run; a failed source hands the remaining workspaces to another, a bounded number of times.
			int attempts = 0;
		};
		/// <summary>A started account-wide discovery whose result is shared with the other workspaces when it ends.</summary>
		struct PendingShare
		{
			std::string provider_id;
			std::string execution_host_id;
			std::string source_workspace;
			std::vector<std::string> target_workspaces;
			nlohmann::json source_before;
			int attempts = 0;
		};
		/// <summary>Puts workspaces whose source failed back in the queue under another source, until the attempt limit.</summary>
		void RequeueAfterFailedSource(const std::string& provider_id, const std::string& execution_host_id, std::vector<std::string> workspaces, int attempts);
		bool ShareFinishedDiscoveries(AppState& app);
		void NoteCheckedCliVersions(AppState& app);

		bool m_initialized = false;
		LaunchFunction m_launch;
		std::vector<ScopeGroup> m_pending_groups;
		std::vector<PendingShare> m_pending_shares;
		// Filled while m_pending_groups is being iterated, then appended to it.
		std::vector<ScopeGroup> m_requeued_groups;
		std::map<std::string, ExecutionHost> m_hosts;
		std::map<std::string, std::string> m_noted_cli_versions;
		/// Scopes already found registered before a version check; a refused check retries without re-testing every chat.
		std::unordered_set<std::string> m_registered_before_version_check;
	};
}
