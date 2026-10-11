#pragma once

#include "app/git_worktree_service.h"

#include <chrono>
#include <future>
#include <stop_token>
#include <vector>

namespace uam
{
	/// Discovers old orphan worktrees off-thread, then acquires the UI-owned mutation gate before fresh removal checks.
	class WorktreeRetentionService
	{
	  public:
		~WorktreeRetentionService();
		void Poll(const AppState& app, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
		void Stop();
		bool Pending() const { return m_worker.valid() || !m_candidates.empty(); }
		bool WorkerPending() const { return m_worker.valid(); }

	  private:
		struct Snapshot
		{
			std::filesystem::path data_root;
			std::vector<std::filesystem::path> protected_paths;
		};
		struct Candidate
		{
			std::filesystem::path source;
			GitWorktreeRetentionCandidate worktree;
		};
		struct Result
		{
			std::vector<Candidate> candidates;
			bool removed = false;
		};
		static bool Capture(const AppState& app, Snapshot& snapshot);
		static bool ProtectProcessWorkspace(const std::filesystem::path& data_root, std::vector<std::filesystem::path>& protected_paths);
		static Result Discover(const Snapshot& snapshot, std::stop_token stop);
		std::future<Result> m_worker;
		std::vector<Candidate> m_candidates;
		std::stop_source m_stop;
		bool m_removing = false;
		std::chrono::steady_clock::time_point m_nextScan = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	};
}
