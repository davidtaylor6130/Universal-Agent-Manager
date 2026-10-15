#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

struct ChatSession;

namespace uam
{

	struct AppState;
	struct AcpSessionState;
	class OpenCodeCompactionHistoryService;

	enum class OpenCodeCompactionGateState
	{
		NotApplicable,
		Pending,
		Ready,
		Failed
	};

	/// <summary>Classifies native OpenCode summaries in owned live turns and gates prior-turn reconciliation.</summary>
	class OpenCodeCompactionSyncService
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
		explicit OpenCodeCompactionSyncService(ExecuteQuery execute = {});
		~OpenCodeCompactionSyncService();
		OpenCodeCompactionSyncService(const OpenCodeCompactionSyncService&) = delete;
		OpenCodeCompactionSyncService& operator=(const OpenCodeCompactionSyncService&) = delete;

		/// <summary>Captures the previous unchanged assistant before a staged turn resets its stream.</summary>
		void ObserveBeforeTurnReset(AppState& app, AcpSessionState& session, const ChatSession& chat);
		/// <summary>Waits for the prior settlement and native baseline before any next prompt write.</summary>
		OpenCodeCompactionGateState BeforeDispatch(AppState& app, AcpSessionState& session,
		    ChatSession& chat, std::string& error);
		bool Poll(AppState& app);
		void Stop();

	  private:
		struct State;
		std::unique_ptr<State> m_state;
		std::unique_ptr<OpenCodeCompactionHistoryService> m_history;
	};
}
