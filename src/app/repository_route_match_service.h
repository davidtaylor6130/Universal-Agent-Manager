#pragma once

#include "common/models/app_models.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>
#include <optional>
#include <string>
#include <vector>

namespace uam
{
	struct AppState;
	struct RepositoryRouteIdentity
	{
		std::string kind;
		std::string identity;
		std::string relative_workspace;
		bool operator==(const RepositoryRouteIdentity&) const = default;
	};

	struct RepositoryRouteWorkspace
	{
		std::string folder_id;
		std::string host_id;
		std::string directory;
		std::string connection_identity;
		std::optional<RepositoryRouteIdentity> repository;
		bool operator==(const RepositoryRouteWorkspace&) const = default;
	};

	struct RepositoryRouteMatch
	{
		std::string host_id;
		std::string destination_folder_id;
		std::string directory;
		bool manual = false;
	};

	struct RepositoryRouteTask
	{
		std::filesystem::path data_root;
		std::vector<RepositoryRouteWorkspace> captured;
		std::vector<RepositoryRouteWorkspace> observed;
		std::string chat_id;
		std::string chat_identity;
		std::shared_ptr<const std::uint8_t> chat_incarnation;
		double started_at_s = 0.0;
		double completed_at_s = 0.0;
		bool matches = false;
		std::atomic<bool> finished{false};
		std::unique_ptr<std::jthread> worker;
	};

	enum class RepositoryRouteLaunchState { Pending, Ready, Failed };
	std::vector<RepositoryRouteWorkspace> ReadRepositoryRouteInventory(std::vector<RepositoryRouteWorkspace> workspaces,
	    const std::vector<ExecutionHost>& hosts, std::stop_token stop = {});
	bool RefreshRepositoryRoutes(AppState& app);
	void RememberRepositoryRouteAssignment(AppState& app, const ChatSession& chat);
	RepositoryRouteLaunchState PrepareRepositoryRouteLaunch(AppState& app, const ChatSession& chat, std::string& error);
	/// Executes fixed read-only VCS arguments in a captured workspace; failure leaves manual routing available.
	using RepositoryRouteReader = std::function<bool(const std::vector<std::string>&, std::string&)>;
	std::optional<std::string> NormalizeRepositoryRouteIdentity(const std::string& value, bool git);
	std::optional<RepositoryRouteIdentity> ReadRepositoryRouteIdentity(const RepositoryRouteReader& read);
	std::vector<RepositoryRouteMatch> MatchRepositoryRoutes(const RepositoryRouteWorkspace& source,
	    const std::vector<RepositoryRouteWorkspace>& workspaces, const ExecutionRoutingSettings& settings);
	std::vector<RepositoryRouteWorkspace> CaptureRepositoryRouteWorkspaces(const std::vector<ChatFolder>& folders, const std::vector<ExecutionHost>& hosts);
	bool RepositoryRouteSnapshotMatches(const std::vector<RepositoryRouteWorkspace>& snapshot,
	    const std::vector<RepositoryRouteWorkspace>& current);
}
