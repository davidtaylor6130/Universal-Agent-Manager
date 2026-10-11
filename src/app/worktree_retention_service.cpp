#include "app/worktree_retention_service.h"

#include "app/startup_native_import_service.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/hash_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"

#include <deque>
#include <unordered_set>

namespace uam
{
	WorktreeRetentionService::~WorktreeRetentionService() { Stop(); }

	void WorktreeRetentionService::Stop()
	{
		m_stop.request_stop();
		if (m_worker.valid())
		{
			try { (void)m_worker.get(); }
			catch (const std::exception& exception) { diagnostics::Write("[retention] Cleanup stopped with an error: " + std::string(exception.what())); }
		}
		m_candidates.clear();
	}

	bool WorktreeRetentionService::Capture(const AppState& app, Snapshot& snapshot)
	{
		if (app.data_root.empty() || !app.settings.git_enabled || !app.worktree_operation_chat_ids.empty() ||
		    (app.startup_native_import_service && app.startup_native_import_service->ImportPending())) return false;
		snapshot.data_root = app.data_root;
		std::unordered_set<std::string> chat_ids;
		for (const ChatSession& chat : app.chats)
		{
			chat_ids.insert(chat.id);
			if (!paths::IsControllerLocalWorkspace(chat)) continue;
			for (const std::string* path : {&chat.workspace_directory, &chat.workspace_worktree_directory, &chat.workspace_source_directory})
				if (!path->empty()) snapshot.protected_paths.push_back(paths::PathFromUtf8(*path));
		}
		for (const ChatFolder& folder : app.folders)
		{
			if (!paths::IsControllerLocalWorkspace(folder) || folder.directory.empty()) continue;
			snapshot.protected_paths.push_back(paths::PathFromUtf8(folder.directory));
		}
		for (const std::unique_ptr<AcpSessionState>& session : app.acp_sessions)
		{
			if (!session) continue;
			// An orphaned provider may still own its cwd while shutdown is pending.
			if (!chat_ids.contains(session->chat_id)) return false;
			if (!session->provider_configuration_directory.empty()) snapshot.protected_paths.push_back(session->provider_configuration_directory);
			if (!session->active_uam_agent_adapter_directory.empty()) snapshot.protected_paths.push_back(session->active_uam_agent_adapter_directory);
		}
		for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
		{
			if (!terminal) continue;
			if (!chat_ids.contains(terminal->frontend_chat_id) && !chat_ids.contains(terminal->attached_chat_id)) return false;
			if (!terminal->codex_activity_cwd.empty()) snapshot.protected_paths.push_back(terminal->codex_activity_cwd);
		}
		if (!app.settings.markdown_store_directory.empty()) snapshot.protected_paths.push_back(paths::PathFromUtf8(app.settings.markdown_store_directory));
		for (const ResourceCollection& collection : app.resource_collections)
			for (const ResourceReference& reference : collection.references)
				if (reference.type == "file" || reference.type == "desktop-app") snapshot.protected_paths.push_back(paths::PathFromUtf8(reference.target));
		return true;
	}

	bool WorktreeRetentionService::ProtectProcessWorkspace(const std::filesystem::path& data_root,
	    std::vector<std::filesystem::path>& protected_paths)
	{
		std::error_code error;
		const std::filesystem::path cwd = std::filesystem::current_path(error);
		if (error) return false;
		const std::filesystem::path executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
		if (executable.empty()) return false;
		for (const std::filesystem::path& path : {cwd, executable})
			if (paths::IsSameOrInsideRoot(data_root / "worktrees", path)) protected_paths.push_back(path);
		return true;
	}

	WorktreeRetentionService::Result WorktreeRetentionService::Discover(const Snapshot& snapshot, std::stop_token stop)
	{
		Result result;
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		std::size_t visited = 0;
		std::vector<std::filesystem::path> protected_paths = snapshot.protected_paths;
		if (!ProtectProcessWorkspace(snapshot.data_root, protected_paths)) return result;
		const GitWorktreeService worktrees;
		const std::filesystem::path storage = snapshot.data_root / "worktrees";
		if (paths::IsLinkOrReparsePointNoThrow(storage) || paths::IsLinkOrReparsePointNoThrow(snapshot.data_root)) return result;
		std::error_code error;
		for (std::filesystem::directory_iterator bucket_iterator(storage, error), end; !error && bucket_iterator != end; bucket_iterator.increment(error))
		{
			if (++visited >= 4096 || stop.stop_requested() || std::chrono::steady_clock::now() >= deadline || result.candidates.size() >= 32) break;
			const std::filesystem::path bucket = bucket_iterator->path();
			const std::string key = paths::Utf8PathString(bucket.filename());
			if (key.empty() || key.size() > 16 || key.find_first_not_of("0123456789abcdef") != std::string::npos ||
			    paths::IsLinkOrReparsePointNoThrow(bucket) || !paths::IsDirectoryNoThrow(bucket)) continue;
			std::deque<std::pair<std::filesystem::path, int>> directories{{bucket, 0}};
			while (!directories.empty() && visited < 4096 && result.candidates.size() < 32 && !stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
			{
				const std::pair<std::filesystem::path, int> directory = std::move(directories.front());
				directories.pop_front();
				std::error_code list_error;
				for (std::filesystem::directory_iterator iterator(directory.first, list_error), directory_end; !list_error && iterator != directory_end; iterator.increment(list_error))
				{
					if (++visited >= 4096 || stop.stop_requested() || std::chrono::steady_clock::now() >= deadline || result.candidates.size() >= 32) break;
					const std::filesystem::path path = iterator->path();
					if (paths::IsLinkOrReparsePointNoThrow(path) || !paths::IsDirectoryNoThrow(path)) continue;
					if (!paths::PathExistsNoThrow(path / ".git"))
					{
						if (directory.second < 2) directories.emplace_back(path, directory.second + 1);
						continue;
					}
					const std::filesystem::file_status marker = std::filesystem::symlink_status(path / ".git", list_error);
					if (list_error || !std::filesystem::is_regular_file(marker)) { list_error.clear(); continue; }
					const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path / ".git", list_error);
					if (list_error || modified >= std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 14)) { list_error.clear(); continue; }
					// Recover the original source even after its last sidebar folder was deleted. NUL records preserve quoted/Unicode paths.
					const ProcessExecutionResult registration = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
					    "git --no-optional-locks -C " + shell::EscapeArg(paths::Utf8PathString(path)) + " worktree list --porcelain -z", 2000, stop);
					if (!registration.ok || registration.exit_code != 0 || registration.timed_out || registration.canceled || registration.output_truncated) continue;
					std::size_t offset = 0;
					while (offset < registration.output.size())
					{
						const std::size_t record_end = registration.output.find('\0', offset);
						if (record_end == std::string::npos) break;
						const std::string_view record(registration.output.data() + offset, record_end - offset);
						offset = record_end + 1;
						if (!record.starts_with("worktree ")) continue;
						const std::filesystem::path source = paths::PathFromUtf8(std::string(record.substr(9)));
						if (hashing::Hex64(hashing::Fnv1a64(paths::NormalizedPortablePathString(source))) != key ||
						    paths::IsSameOrInsideRoot(path, source)) continue;
						GitWorktreeRetentionCandidate candidate;
						if (worktrees.InspectRetentionCandidate(snapshot.data_root, source, path, protected_paths, candidate, stop))
							result.candidates.push_back({source, std::move(candidate)});
						break;
					}
				}
			}
		}
		return result;
	}

	void WorktreeRetentionService::Poll(const AppState& app, std::chrono::steady_clock::time_point now)
	{
		if (m_stop.stop_requested()) return;
		if (m_worker.valid())
		{
			if (m_worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
			try
			{
				Result result = m_worker.get();
				if (!m_removing) m_candidates = std::move(result.candidates);
			}
			catch (const std::exception& exception) { diagnostics::Write("[retention] Cleanup failed: " + std::string(exception.what())); }
			m_removing = false;
		}
		Snapshot snapshot;
		if (m_candidates.empty() && now < m_nextScan) return;
		std::shared_ptr<WorkspaceLeaseGate::Lease> lease;
		if (!m_candidates.empty())
		{
			lease = app.worktree_retention_gate.TryAcquireCleanup();
			if (!lease) return;
		}
		if (!Capture(app, snapshot)) return;
		try
		{
			if (m_candidates.empty())
			{
				m_nextScan = now + std::chrono::hours(6);
				m_worker = std::async(std::launch::async, [snapshot = std::move(snapshot), stop = m_stop.get_token()]() { return Discover(snapshot, stop); });
			}
			else
			{
				Candidate candidate = std::move(m_candidates.back());
				m_candidates.pop_back();
				m_removing = true;
				m_worker = std::async(std::launch::async, [snapshot = std::move(snapshot), candidate = std::move(candidate), lease = std::move(lease), stop = m_stop.get_token()]() mutable
				{
					std::shared_ptr<WorkspaceLeaseGate::Lease> active = std::move(lease);
					Result result;
					if (ProtectProcessWorkspace(snapshot.data_root, snapshot.protected_paths))
						result.removed = GitWorktreeService().RemoveRetentionCandidate(snapshot.data_root, candidate.source, candidate.worktree, snapshot.protected_paths, *active, stop);
					diagnostics::Write(std::string("[retention] ") + (result.removed ? "Removed " : "Preserved ") +
					    paths::Utf8PathString(candidate.worktree.directory) + "; branch " + candidate.worktree.branch_name +
					    (result.removed ? " remains." : " remains after validation or removal failed."));
					return result;
				});
			}
		}
		catch (const std::exception& exception)
		{
			m_removing = false;
			diagnostics::Write("[retention] Cleanup could not start: " + std::string(exception.what()));
		}
	}
}
