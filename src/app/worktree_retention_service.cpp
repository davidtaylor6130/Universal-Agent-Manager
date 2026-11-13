#include "app/worktree_retention_service.h"

#include "app/startup_native_import_service.h"
#include "common/chat/chat_repository.h"
#include "common/paths/path_utils.h"
#include "common/paths/app_paths.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/hash_utils.h"
#include "common/utils/shell_escape.h"
#include "common/vcs/git_command.h"
#include "common/utils/string_utils.h"

#include <deque>
#include <unordered_set>

namespace uam
{
	namespace
	{
		constexpr std::string_view kManagedSnapshotPrefix = "managed-repository-";

		std::string ManagedSnapshotName(const std::string& chat_id)
		{
			return std::string(kManagedSnapshotPrefix) + hashing::Hex64(hashing::Fnv1a64(chat_id));
		}

		bool IsManagedSnapshotName(const std::string& name)
		{
			return name.size() == kManagedSnapshotPrefix.size() + 16 && name.starts_with(kManagedSnapshotPrefix) &&
			    name.find_first_not_of("0123456789abcdef", kManagedSnapshotPrefix.size()) == std::string::npos;
		}
	}

	WorktreeRetentionService::~WorktreeRetentionService() { Stop(); }

	void WorktreeRetentionService::Stop()
	{
		m_stop.request_stop();
		m_policyStop.request_stop();
		if (m_worker.valid())
		{
			try { (void)m_worker.get(); }
			catch (const std::exception& exception) { diagnostics::Write("[retention] Cleanup stopped with an error: " + std::string(exception.what())); }
		}
		m_candidates.clear();
	}

	bool WorktreeRetentionService::Capture(const AppState& app, Snapshot& snapshot)
	{
		if (app.data_root.empty() || !app.settings.git_enabled || !app.settings.worktree_cleanup_enabled ||
		    app.settings.worktree_retention_days < 1 || app.settings.worktree_retention_days > 3650 || !app.worktree_operation_chat_ids.empty() ||
		    (app.startup_native_import_service && app.startup_native_import_service->ImportPending())) return false;
		snapshot.data_root = app.data_root;
		snapshot.retention_days = app.settings.worktree_retention_days;
		std::unordered_set<std::string> chat_ids;
		for (const ChatSession& chat : app.chats)
		{
			chat_ids.insert(chat.id);
			if (!paths::IsControllerLocalWorkspace(chat)) continue;
			for (const std::string* path : {&chat.workspace_directory, &chat.workspace_worktree_directory, &chat.workspace_source_directory})
				if (!path->empty()) snapshot.protected_paths.push_back(paths::PathFromUtf8(*path));
		}
		// Archived agent transcripts are deliberately excluded from the sidebar catalogue.
		for (const AgentRun& run : app.agent_runs)
		{
			if (run.transcript_chat_id.empty() || chat_ids.contains(run.transcript_chat_id)) continue;
			if (snapshot.agent_transcript_ids.size() >= 4096) return false;
			snapshot.agent_transcript_ids.push_back(run.transcript_chat_id);
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
		for (const std::string& id : chat_ids) snapshot.protected_snapshot_names.insert(ManagedSnapshotName(id));
		for (const std::string& id : snapshot.agent_transcript_ids) snapshot.protected_snapshot_names.insert(ManagedSnapshotName(id));
		snapshot.known_chat_ids = std::move(chat_ids);
		return true;
	}

	bool WorktreeRetentionService::ProtectStoredWorkspaces(const Snapshot& snapshot,
	    std::vector<std::filesystem::path>& protected_paths, std::unordered_set<std::string>& protected_snapshot_names, std::stop_token stop)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		const std::filesystem::path root = AppPaths::UamChatsRootPath(snapshot.data_root);
		if (paths::IsLinkOrReparsePointNoThrow(snapshot.data_root) || paths::IsLinkOrReparsePointNoThrow(root)) return false;
		std::unordered_set<std::string> unresolved(snapshot.agent_transcript_ids.begin(), snapshot.agent_transcript_ids.end());
		std::error_code error;
		std::filesystem::directory_iterator iterator(root, error), end;
		if (error == std::errc::no_such_file_or_directory) return unresolved.empty() && !stop.stop_requested();
		std::size_t visited = 0;
		for (; !error && iterator != end; iterator.increment(error))
		{
			if (++visited > 262144 || stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) return false;
			if (iterator->path().extension() != ".json") continue;
			if (paths::IsLinkOrReparsePointNoThrow(iterator->path()) || !iterator->is_regular_file(error) || error) return false;
			const std::string id = paths::Utf8PathString(iterator->path().stem());
			protected_snapshot_names.insert(ManagedSnapshotName(id));
			if (snapshot.known_chat_ids.contains(id)) continue;
			const std::optional<ChatSession> chat = ChatRepository::LoadLocalChatSummaryReadOnly(snapshot.data_root, id);
			if (!chat) return false;
			unresolved.erase(id);
			if (!paths::IsControllerLocalWorkspace(*chat)) continue;
			for (const std::string* path : {&chat->workspace_directory, &chat->workspace_worktree_directory, &chat->workspace_source_directory})
				if (!path->empty()) protected_paths.push_back(paths::PathFromUtf8(*path));
		}
		return !error && unresolved.empty() && !stop.stop_requested() && std::chrono::steady_clock::now() < deadline;
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

	std::size_t WorktreeRetentionService::PrunePortPatches(const Snapshot& snapshot, std::stop_token stop)
	{
		// A patch is written for every port; after a failed apply its path is shown so it can be applied by hand within the retention period.
		const std::filesystem::path patches = snapshot.data_root / "worktrees" / "patches";
		if (paths::IsLinkOrReparsePointNoThrow(snapshot.data_root / "worktrees") || paths::IsLinkOrReparsePointNoThrow(patches)) return 0;
		const std::filesystem::file_time_type cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * snapshot.retention_days);
		std::size_t removed = 0, visited = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(patches, error), end; !error && it != end && ++visited <= 4096 && !stop.stop_requested(); it.increment(error))
		{
			std::error_code entry_error;
			if (it->path().extension() != ".patch" || !std::filesystem::is_regular_file(it->symlink_status(entry_error)) || entry_error) continue;
			const std::filesystem::file_time_type modified = std::filesystem::last_write_time(it->path(), entry_error);
			if (entry_error || modified >= cutoff) continue;
			if (std::filesystem::remove(it->path(), entry_error)) ++removed;
		}
		return removed;
	}

	WorktreeRetentionService::Result WorktreeRetentionService::Discover(const Snapshot& snapshot, std::stop_token stop)
	{
		Result result;
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		std::size_t visited = 0;
		std::vector<std::filesystem::path> protected_paths = snapshot.protected_paths;
		std::unordered_set<std::string> protected_snapshot_names = snapshot.protected_snapshot_names;
		if (!ProtectProcessWorkspace(snapshot.data_root, protected_paths) || !ProtectStoredWorkspaces(snapshot, protected_paths, protected_snapshot_names, stop)) return result;
		if (const std::size_t patches = PrunePortPatches(snapshot, stop); patches > 0)
			diagnostics::Write("[retention] Removed " + std::to_string(patches) + " port patch file(s) older than the retention period.");
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
					const std::string name = paths::Utf8PathString(path.filename());
					if (directory.second == 0 && name.starts_with(kManagedSnapshotTombstonePrefix))
					{
						// A snapshot whose deletion was interrupted; nothing uses it once renamed.
						(void)paths::RemoveTreeWithoutFollowingLinksNoThrow(path);
						continue;
					}
					if (directory.second == 0 && IsManagedSnapshotName(name))
					{
						// A plain-folder snapshot of a chat that no longer exists, once its last linked worktree is gone.
						GitWorktreeRetentionCandidate candidate;
						if (!protected_snapshot_names.contains(name) &&
						    worktrees.InspectManagedSnapshotRetentionCandidate(snapshot.data_root, path, protected_paths, candidate, stop, snapshot.retention_days))
							result.candidates.push_back({path, std::move(candidate), true});
						continue;
					}
					if (!paths::PathExistsNoThrow(path / ".git"))
					{
						if (directory.second < 2) directories.emplace_back(path, directory.second + 1);
						continue;
					}
					const std::filesystem::file_status marker = std::filesystem::symlink_status(path / ".git", list_error);
					if (list_error || !std::filesystem::is_regular_file(marker)) { list_error.clear(); continue; }
					const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path / ".git", list_error);
					if (list_error || modified >= std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * snapshot.retention_days)) { list_error.clear(); continue; }
					// Recover the original source even after its last sidebar folder was deleted. NUL records preserve quoted/Unicode paths.
					const ProcessExecutionResult registration = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
					    vcs::HardenedGitShellCommandInDirectory(path, "--no-optional-locks worktree list --porcelain -z"), 2000, stop);
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
						// A plain-folder snapshot's repository lives in this bucket; its worktrees follow the same rules.
						const std::string source_name = paths::Utf8PathString(source.filename());
						const bool managed_source = IsManagedSnapshotName(source_name) && paths::Utf8PathString(source.parent_path().filename()) == key;
						if ((managed_source ? protected_snapshot_names.contains(source_name) : hashing::Hex64(hashing::Fnv1a64(paths::NormalizedPortablePathString(source))) != key) ||
						    paths::IsSameOrInsideRoot(path, source)) continue;
						GitWorktreeRetentionCandidate candidate;
						if (worktrees.InspectRetentionCandidate(snapshot.data_root, source, path, protected_paths, candidate, stop, snapshot.retention_days))
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
		if (m_policyRoot != app.data_root || m_policyDays != app.settings.worktree_retention_days ||
		    m_policyEnabled != app.settings.worktree_cleanup_enabled || m_policyGitEnabled != app.settings.git_enabled)
		{
			m_policyStop.request_stop();
			m_candidates.clear();
			m_policyRoot = app.data_root;
			m_policyDays = app.settings.worktree_retention_days;
			m_policyEnabled = app.settings.worktree_cleanup_enabled;
			m_policyGitEnabled = app.settings.git_enabled;
			++m_policyRevision;
			if (m_policyRevision > 1) m_nextScan = now;
			m_nextCapture = {};
		}
		if (m_worker.valid())
		{
			if (m_worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
			try
			{
				Result result = m_worker.get();
				if (!m_removing && m_workerRevision == m_policyRevision) m_candidates = std::move(result.candidates);
			}
			catch (const std::exception& exception) { diagnostics::Write("[retention] Cleanup failed: " + std::string(exception.what())); }
			m_removing = false;
		}
		if (!app.settings.git_enabled || !app.settings.worktree_cleanup_enabled) return;
		Snapshot snapshot;
		if (m_candidates.empty() && now < m_nextScan) return;
		const std::array<std::size_t, 5> capture_shape{app.chats.size(), app.acp_sessions.size(), app.cli_terminals.size(),
		    app.agent_runs.size(), app.worktree_operation_chat_ids.size()};
		if (now < m_nextCapture && capture_shape == m_refusedCaptureShape) return;
		std::shared_ptr<WorkspaceLeaseGate::Lease> lease;
		if (!m_candidates.empty())
		{
			lease = app.worktree_retention_gate.TryAcquireCleanup();
			if (!lease) return;
		}
		if (!Capture(app, snapshot))
		{
			m_nextCapture = now + std::chrono::seconds(30);
			m_refusedCaptureShape = capture_shape;
			return;
		}
		try
		{
			m_policyStop = std::stop_source();
			m_workerRevision = m_policyRevision;
			if (m_candidates.empty())
			{
				m_nextScan = now + std::chrono::hours(6);
				m_worker = std::async(std::launch::async, [snapshot = std::move(snapshot), stop = m_policyStop.get_token()]() { return Discover(snapshot, stop); });
			}
			else
			{
				Candidate candidate = std::move(m_candidates.back());
				m_candidates.pop_back();
				m_removing = true;
				m_worker = std::async(std::launch::async, [snapshot = std::move(snapshot), candidate = std::move(candidate), lease = std::move(lease), stop = m_policyStop.get_token()]() mutable
				{
					std::shared_ptr<WorkspaceLeaseGate::Lease> active = std::move(lease);
					Result result;
					std::unordered_set<std::string> protected_snapshot_names = snapshot.protected_snapshot_names;
					if (ProtectProcessWorkspace(snapshot.data_root, snapshot.protected_paths) && ProtectStoredWorkspaces(snapshot, snapshot.protected_paths, protected_snapshot_names, stop))
					{
						// A snapshot (or a snapshot's worktree) whose chat reappeared since discovery is kept.
						const std::filesystem::path& snapshot_path = candidate.managed_snapshot ? candidate.worktree.directory : candidate.source;
						const std::string snapshot_name = paths::Utf8PathString(snapshot_path.filename());
						if (IsManagedSnapshotName(snapshot_name) && protected_snapshot_names.contains(snapshot_name))
							result.removed = false;
						else if (!candidate.managed_snapshot)
							result.removed = GitWorktreeService().RemoveRetentionCandidate(snapshot.data_root, candidate.source, candidate.worktree, snapshot.protected_paths, *active, stop, snapshot.retention_days);
						else
							result.removed = GitWorktreeService().RemoveManagedSnapshotRetentionCandidate(snapshot.data_root, candidate.worktree, snapshot.protected_paths, *active, stop, snapshot.retention_days);
					}
					if (candidate.managed_snapshot)
						diagnostics::Write(std::string("[retention] ") + (result.removed ? "Removed plain-folder snapshot " : "Preserved plain-folder snapshot ") +
						    paths::Utf8PathString(candidate.worktree.directory) + (result.removed ? "." : " after validation or removal failed."));
					else
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
