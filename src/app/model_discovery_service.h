#pragma once

#include "common/models/app_models.h"

#include <functional>
#include <map>
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

	/// <summary>Discovers registered startup scopes independently of user conversations, with bounded concurrency.</summary>
	class StartupModelDiscoveryService
	{
	  public:
		using LaunchFunction = std::function<bool(AppState&, const std::string&, const std::string&, const std::string&, std::string*)>;
		explicit StartupModelDiscoveryService(LaunchFunction launch = {});
		/// <summary>Captures registered scopes once; reopening a picker does not refill the startup queue.</summary>
		void Initialize(AppState& app);
		/// <summary>Starts at most two discovery processes; unavailable scopes wait without blocking ready scopes.</summary>
		bool Poll(AppState& app);
		static std::vector<ModelDiscoveryScope> CollectScopes(const AppState& app);

	  private:
		bool m_initialized = false;
		LaunchFunction m_launch;
		std::vector<ModelDiscoveryScope> m_pending_scopes;
		std::map<std::string, ExecutionHost> m_hosts;
	};
}
