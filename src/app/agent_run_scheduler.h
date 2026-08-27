#pragma once

#include "common/state/app_state.h"

#include <cstdint>
#include <string>

namespace uam
{
	struct AgentRunResult
	{
		std::string delivery_id;
		std::string run_id;
		std::string status;
		std::string agent_id;
		std::string output;
		std::string diagnostic_code;
		std::string diagnostic;
	};

	class AgentRunScheduler
	{
	  public:
		static bool Enqueue(AppState& app, const std::string& root_chat_id,
		                    const std::string& parent_run_id, const std::string& agent_id,
		                    const std::string& task, std::string* run_id_out = nullptr,
		                    std::string* error_out = nullptr, const std::string& provider_id = {},
		                    const std::string& model_id = {});
		static bool TryEnqueueMention(AppState& app, const std::string& root_chat_id,
		                              const std::string& text, bool* handled_out,
		                              std::string* run_id_out = nullptr,
		                              std::string* error_out = nullptr);
		static bool ResumeInterrupted(AppState& app, const std::string& run_id,
		                              std::string* new_run_id_out = nullptr,
		                              std::string* error_out = nullptr);
		static bool Poll(AppState& app);
		static bool GetResultForParent(const AppState& app, const std::string& run_id,
		                               const std::string& root_chat_id,
		                               const std::string& parent_run_id,
		                               AgentRunResult* result_out,
		                               std::string* error_out = nullptr);
		static bool CancelTree(AppState& app, const std::string& run_id,
		                       std::string* error_out = nullptr);
		static bool InterruptForShutdown(AppState& app);
		static bool PollAtForTests(AppState& app, int64_t now_epoch_ms);
	};
} // namespace uam
