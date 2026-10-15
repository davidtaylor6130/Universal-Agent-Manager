#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace uam
{
	struct AppState;

	/// <summary>Classifies bounded, unchanged OpenCode history using native summary metadata.</summary>
	class OpenCodeCompactionHistoryService
	{
	  public:
		struct Query
		{
			std::vector<std::string> argv;
			std::filesystem::path workspace;
			std::optional<ExecutionHost> host;
			std::string sql;
		};
		using ExecuteQuery = std::function<ProcessExecutionResult(const Query&, std::stop_token)>;
		/// <summary>Recovers successful OpenCode output truncated by its forced exit, using bounded native SQL pages.</summary>
		static ProcessExecutionResult CollectBoundedPages(const Query& query, const ExecuteQuery& execute,
		    std::stop_token stop = {});
		explicit OpenCodeCompactionHistoryService(ExecuteQuery execute = {});
		~OpenCodeCompactionHistoryService();
		OpenCodeCompactionHistoryService(const OpenCodeCompactionHistoryService&) = delete;
		OpenCodeCompactionHistoryService& operator=(const OpenCodeCompactionHistoryService&) = delete;
		bool Poll(AppState& app);
		void Stop();

	  private:
		struct State;
		std::unique_ptr<State> m_state;
	};
}
