#include "app/repository_route_match_service.h"
#include "common/utils/string_utils.h"
#include "app/execution_routing_integration.h"
#include "common/config/execution_host_config.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/runtime/app_time.h"
#include "common/utils/shell_escape.h"
#include "remote/runner_client.h"

namespace uam
{
	namespace
	{
		bool SafePath(const std::string& value, bool allow_empty)
		{
			if ((!allow_empty && value.empty()) || value.size() > 4096 || value.starts_with('/') || value.find('\\') != std::string::npos) return false;
			std::size_t start = 0;
			while (start < value.size())
			{
				const std::size_t end = value.find('/', start);
				const std::string part = value.substr(start, end == std::string::npos ? end : end - start);
				if (part.empty() || part == "." || part == "..") return false;
				if (end == std::string::npos) break;
				start = end + 1;
			}
			return std::ranges::none_of(value, [](unsigned char c) { return c <= 32 || c == 127 || c == '%' || c == '?' || c == '#'; });
		}
	}

	std::optional<std::string> NormalizeRepositoryRouteIdentity(const std::string& raw, bool git)
	{
		const std::string value = strings::Trim(raw);
		if (value.empty() || value.size() > 1024 || std::ranges::any_of(value, [](unsigned char c) { return c <= 32 || c == 127; })) return {};
		std::string authority;
		std::string path;
		std::string scheme;
		const std::size_t protocol = value.find("://");
		if (protocol != std::string::npos)
		{
			scheme = value.substr(0, protocol);
			if (scheme != "https" && scheme != "http" && scheme != "ssh" && scheme != "git" && scheme != "svn" && scheme != "svn+ssh") return {};
			const std::size_t slash = value.find('/', protocol + 3);
			if (slash == std::string::npos) return {};
			authority = value.substr(protocol + 3, slash - protocol - 3);
			path = value.substr(slash + 1);
			const std::size_t at = authority.find('@');
			if (at != std::string::npos)
			{
				// SSH usernames are transport details; HTTP credentials never enter the displayed identity.
				if ((scheme != "ssh" && scheme != "svn+ssh") || authority.substr(0, at).find(':') != std::string::npos || at == 0) return {};
				authority.erase(0, at + 1);
			}
		}
		else
		{
			if (!git) return {};
			const std::size_t colon = value.find(':');
			if (colon == std::string::npos) return {};
			authority = value.substr(0, colon);
			path = value.substr(colon + 1);
			const std::size_t at = authority.find('@');
			if (at != std::string::npos) authority.erase(0, at + 1);
			scheme = "ssh";
		}
		if (authority.empty() || authority.find('@') != std::string::npos || authority.find_first_of("/?#\\%") != std::string::npos) return {};
		const std::size_t port = authority.find(':');
		if (port != std::string::npos)
		{
			const std::string number = authority.substr(port + 1);
			if (port == 0 || number.empty() || number.size() > 5 || !std::ranges::all_of(number, [](unsigned char c) { return std::isdigit(c) != 0; })) return {};
			const int numeric = std::stoi(number);
			if (numeric < 1 || numeric > 65535) return {};
			if ((scheme == "https" && numeric == 443) || (scheme == "http" && numeric == 80) || ((scheme == "ssh" || scheme == "svn+ssh") && numeric == 22) || (scheme == "git" && numeric == 9418)) authority.erase(port);
		}
		const std::string host = authority.substr(0, authority.find(':'));
		if (host.find('.') == std::string::npos || !std::ranges::all_of(host, [](unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '.'; })) return {};
		std::ranges::transform(authority, authority.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		while (path.ends_with('/')) path.pop_back();
		if (git && path.ends_with(".git")) path.resize(path.size() - 4);
		if (!SafePath(path, false)) return {};
		return authority + "/" + path;
	}

	std::optional<RepositoryRouteIdentity> ReadRepositoryRouteIdentity(const RepositoryRouteReader& read)
	{
		std::string remote;
		std::string prefix;
		if (read({"git", "--no-optional-locks", "config", "--get", "remote.origin.url"}, remote))
		{
			const std::optional<std::string> identity = NormalizeRepositoryRouteIdentity(remote, true);
			if (!identity || !read({"git", "--no-optional-locks", "rev-parse", "--show-prefix"}, prefix)) return {};
			prefix = strings::Trim(prefix);
			while (prefix.ends_with('/')) prefix.pop_back();
			if (!SafePath(prefix, true)) return {};
			return RepositoryRouteIdentity{"git", *identity, prefix};
		}
		std::string root;
		std::string url;
		if (!read({"svn", "info", "--non-interactive", "--show-item", "repos-root-url", "--", "."}, root) ||
		    !read({"svn", "info", "--non-interactive", "--show-item", "url", "--", "."}, url)) return {};
		const std::optional<std::string> identity = NormalizeRepositoryRouteIdentity(root, false);
		const std::optional<std::string> checkout = NormalizeRepositoryRouteIdentity(url, false);
		if (!identity || !checkout || (*checkout != *identity && !checkout->starts_with(*identity + "/"))) return {};
		return RepositoryRouteIdentity{"svn", *identity, *checkout == *identity ? "" : checkout->substr(identity->size() + 1)};
	}

	std::vector<RepositoryRouteMatch> MatchRepositoryRoutes(const RepositoryRouteWorkspace& source,
	    const std::vector<RepositoryRouteWorkspace>& workspaces, const ExecutionRoutingSettings& settings)
	{
		std::vector<RepositoryRouteMatch> matches;
		for (const ExecutionRoutingHostPolicy& policy : settings.hosts)
		{
			bool manual = false;
			for (const ExecutionRoutingWorkspaceMapping& mapping : policy.workspace_mappings)
			{
				if (mapping.source_folder_id != source.folder_id || mapping.source_directory != source.directory) continue;
				manual = true;
				for (const RepositoryRouteWorkspace& destination : workspaces)
					if (destination.folder_id == mapping.destination_folder_id && destination.host_id == policy.host_id)
						matches.push_back({policy.host_id, destination.folder_id, destination.directory, true});
				break;
			}
			if (manual || !source.repository || source.host_id != "local") continue;
			const RepositoryRouteWorkspace* unique = nullptr;
			std::size_t count = 0;
			for (const RepositoryRouteWorkspace& destination : workspaces)
				if (destination.host_id == policy.host_id && destination.repository == source.repository)
				{
					unique = &destination;
					++count;
				}
			if (count == 1) matches.push_back({policy.host_id, unique->folder_id, unique->directory, false});
		}
		return matches;
	}

	std::vector<RepositoryRouteWorkspace> CaptureRepositoryRouteWorkspaces(const std::vector<ChatFolder>& folders, const std::vector<ExecutionHost>& hosts)
	{
		std::vector<RepositoryRouteWorkspace> result;
		for (const ChatFolder& folder : folders)
		{
			const std::string host_id = folder.execution_host_id.empty() ? "local" : folder.execution_host_id;
			const ExecutionHost* host = execution_hosts::Find(hosts, host_id);
			result.push_back({folder.id, host_id, folder.directory, host ? ExecutionRoutingConnectionIdentity(*host) + ":" + host->runner_status : "", {}});
		}
		return result;
	}

	bool RepositoryRouteSnapshotMatches(const std::vector<RepositoryRouteWorkspace>& snapshot,
	    const std::vector<RepositoryRouteWorkspace>& current)
	{
		if (snapshot.size() != current.size()) return false;
		for (std::size_t i = 0; i < snapshot.size(); ++i)
			if (snapshot[i].folder_id != current[i].folder_id || snapshot[i].host_id != current[i].host_id ||
			    snapshot[i].directory != current[i].directory || snapshot[i].connection_identity != current[i].connection_identity) return false;
		return true;
	}

	std::vector<RepositoryRouteWorkspace> ReadRepositoryRouteInventory(std::vector<RepositoryRouteWorkspace> workspaces,
	    const std::vector<ExecutionHost>& hosts, std::stop_token stop)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
		for (RepositoryRouteWorkspace& workspace : workspaces)
		{
			workspace.repository.reset();
			if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) break;
			const ExecutionHost* host = execution_hosts::Find(hosts, workspace.host_id);
			if (!host || workspace.directory.empty()) continue;
			std::unique_ptr<remote::RunnerClient> runner;
			if (workspace.host_id != "local")
			{
				if (host->transport != "ssh" || host->runner_status != "ready" || host->runner_protocol_version != remote::kRunnerProtocolVersion ||
				    !execution_hosts::IsAbsoluteRemotePath(host->platform, workspace.directory)) continue;
				runner = std::make_unique<remote::RunnerClient>(process, *host);
			}
			else if (!paths::PathFromUtf8(workspace.directory).is_absolute()) continue;
			workspace.repository = ReadRepositoryRouteIdentity([&](const std::vector<std::string>& argv, std::string& output)
			{
				const long long remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
				if (remaining <= 0 || stop.stop_requested()) return false;
				const int timeout = static_cast<int>(std::min(remaining, 2000LL));
				ProcessExecutionResult result;
				if (runner) result = runner->ExecuteCommand(process.GenerateUuid(), paths::PathFromUtf8(workspace.directory), argv, timeout, stop);
				else
				{
					std::vector<std::string> local_argv = argv;
					if (argv.front() == "git") local_argv.insert(local_argv.begin() + 2, {"-C", workspace.directory});
					else local_argv.back() = workspace.directory;
					std::string command;
					for (const std::string& argument : local_argv) command += shell::EscapeArg(argument) + " ";
					result = process.ExecuteCommand(command, timeout, stop);
				}
				if (!result.ok || result.exit_code != 0 || result.output_truncated || result.canceled || result.timed_out || result.output.size() > 4096) return false;
				output = result.output;
				return true;
			});
		}
		return workspaces;
	}

	bool RefreshRepositoryRoutes(AppState& app)
	{
		const double now = GetAppTimeSeconds();
		const std::vector<RepositoryRouteWorkspace> current = CaptureRepositoryRouteWorkspaces(app.folders, app.settings.execution_hosts);
		if (app.repository_route_refresh && app.repository_route_refresh->finished.load(std::memory_order_acquire))
		{
			const std::shared_ptr<RepositoryRouteTask> task = std::move(app.repository_route_refresh);
			if (task->data_root == app.data_root && RepositoryRouteSnapshotMatches(task->captured, current))
			{
				app.repository_route_workspaces = task->observed;
				app.repository_route_data_root = task->data_root;
				app.repository_route_observed_at_s = now;
			}
		}
		std::erase_if(app.repository_route_assignments, [&](const std::pair<const std::string, std::vector<RepositoryRouteWorkspace>>& assignment)
		{ return std::ranges::none_of(app.chats, [&](const ChatSession& chat) { return chat.id == assignment.first; }); });
		std::erase_if(app.repository_route_tasks, [now](const std::shared_ptr<RepositoryRouteTask>& task)
		{ return task->finished.load(std::memory_order_acquire) && now - task->started_at_s > 30.0; });
		const bool fresh = app.repository_route_data_root == app.data_root && app.repository_route_observed_at_s >= 0.0 &&
		    now >= app.repository_route_observed_at_s && now - app.repository_route_observed_at_s <= 30.0 && RepositoryRouteSnapshotMatches(app.repository_route_workspaces, current);
		if (fresh || app.repository_route_refresh || app.folders.size() > 512) return fresh;
		const std::shared_ptr<RepositoryRouteTask> task = std::make_shared<RepositoryRouteTask>();
		task->data_root = app.data_root;
		task->captured = current;
		task->started_at_s = now;
		app.repository_route_refresh = task;
		app.repository_route_tasks.push_back(task);
		task->worker = std::make_unique<std::jthread>([state = task.get(), hosts = app.settings.execution_hosts](std::stop_token stop)
		{
			try { state->observed = ReadRepositoryRouteInventory(state->captured, hosts, stop); }
			catch (...) { state->observed = state->captured; }
			state->completed_at_s = GetAppTimeSeconds();
			state->finished.store(true, std::memory_order_release);
		});
		return false;
	}

	void RememberRepositoryRouteAssignment(AppState& app, const ChatSession& chat)
	{
		std::vector<RepositoryRouteWorkspace> expected;
		for (const RepositoryRouteWorkspace& workspace : app.repository_route_workspaces)
			if ((workspace.folder_id == chat.routing_source_folder_id && workspace.directory == chat.routing_source_directory && workspace.host_id == "local") ||
			    (workspace.folder_id == chat.folder_id && workspace.directory == chat.workspace_directory && workspace.host_id == chat.execution_host_id)) expected.push_back(workspace);
		if (expected.size() == 2 && expected[0].repository && expected[0].repository == expected[1].repository)
			app.repository_route_assignments[chat.id] = std::move(expected);
	}

	RepositoryRouteLaunchState PrepareRepositoryRouteLaunch(AppState& app, const ChatSession& chat, std::string& error)
	{
		if (chat.routing_mode != "automatic" || chat.execution_host_id == "local") return RepositoryRouteLaunchState::Ready;
		for (const ExecutionRoutingHostPolicy& policy : app.settings.execution_routing.hosts)
			if (policy.host_id == chat.execution_host_id)
				for (const ExecutionRoutingWorkspaceMapping& mapping : policy.workspace_mappings)
					if (mapping.source_folder_id == chat.routing_source_folder_id && mapping.source_directory == chat.routing_source_directory && mapping.destination_folder_id == chat.folder_id &&
					    std::ranges::any_of(app.folders, [&](const ChatFolder& folder) { return folder.id == mapping.source_folder_id && folder.directory == mapping.source_directory; }) &&
					    std::ranges::any_of(app.folders, [&](const ChatFolder& folder) { return folder.id == mapping.destination_folder_id && folder.directory == chat.workspace_directory && folder.execution_host_id == chat.execution_host_id; }))
						return RepositoryRouteLaunchState::Ready;
		if (!RefreshRepositoryRoutes(app)) return RepositoryRouteLaunchState::Pending;
		const double now = GetAppTimeSeconds();
		if (!app.repository_route_assignments.contains(chat.id)) RememberRepositoryRouteAssignment(app, chat);
		const std::unordered_map<std::string, std::vector<RepositoryRouteWorkspace>>::const_iterator assignment = app.repository_route_assignments.find(chat.id);
		const std::vector<RepositoryRouteWorkspace> expected = assignment == app.repository_route_assignments.end()
		    ? std::vector<RepositoryRouteWorkspace>{} : assignment->second;
		if (expected.size() != 2 || !expected[0].repository || expected[0].repository != expected[1].repository)
		{ error = "The assigned checkout no longer matches the source repository. Review its routing mapping."; return RepositoryRouteLaunchState::Failed; }
		const std::vector<RepositoryRouteWorkspace> current = CaptureRepositoryRouteWorkspaces(app.folders, app.settings.execution_hosts);
		for (const RepositoryRouteWorkspace& workspace : expected)
			if (std::ranges::none_of(current, [&](const RepositoryRouteWorkspace& item)
			    { return RepositoryRouteSnapshotMatches({workspace}, {item}); }))
			{ error = "The assigned workspace or SSH connection changed before launch."; return RepositoryRouteLaunchState::Failed; }
		const std::string identity = nlohmann::json::array({chat.id, chat.routing_source_folder_id, chat.routing_source_directory,
		    chat.folder_id, chat.workspace_directory, chat.execution_host_id, chat.routing_assigned_connection_identity}).dump();
		for (const std::shared_ptr<RepositoryRouteTask>& task : app.repository_route_tasks)
		{
			if (task->chat_id != chat.id || task->chat_incarnation != chat.metadata_incarnation || task->chat_identity != identity || task->data_root != app.data_root ||
			    task->captured != expected) continue;
			if (!task->finished.load(std::memory_order_acquire)) return RepositoryRouteLaunchState::Pending;
			if (!task->matches) { error = "The assigned repository changed during launch preparation. Review its routing mapping."; return RepositoryRouteLaunchState::Failed; }
			if (now - task->completed_at_s <= 2.0) return RepositoryRouteLaunchState::Ready;
		}
		if (std::ranges::count_if(app.repository_route_tasks, [](const std::shared_ptr<RepositoryRouteTask>& active)
		    { return !active->finished.load(std::memory_order_acquire); }) >= 4) return RepositoryRouteLaunchState::Pending;
		const std::shared_ptr<RepositoryRouteTask> task = std::make_shared<RepositoryRouteTask>();
		task->data_root = app.data_root;
		task->captured = expected;
		task->chat_id = chat.id;
		task->chat_identity = identity;
		task->chat_incarnation = chat.metadata_incarnation;
		task->started_at_s = now;
		app.repository_route_tasks.push_back(task);
		task->worker = std::make_unique<std::jthread>([state = task.get(), hosts = app.settings.execution_hosts](std::stop_token stop)
		{
			try
			{
				state->observed = ReadRepositoryRouteInventory(state->captured, hosts, stop);
				state->matches = state->observed == state->captured;
			}
			catch (...) { state->matches = false; }
			state->completed_at_s = GetAppTimeSeconds();
			state->finished.store(true, std::memory_order_release);
		});
		return RepositoryRouteLaunchState::Pending;
	}
}
