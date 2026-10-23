#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uam::io
{
	enum class AtomicWriteStage;
}

namespace uam
{
	struct AgentRunLoadResult
	{
		std::vector<AgentRun> runs;
		std::vector<std::string> errors;
	};

	struct PreparedAgentRunSave;

	enum class AgentRunPublishState
	{
		Rejected, Committed, Uncertain
	};

	struct AgentRunPublishResult
	{
		AgentRunPublishState state = AgentRunPublishState::Rejected;
		std::string error;
	};

	class AgentRunLedger
	{
	  public:
		static std::string NewRunId();
		static bool Save(const std::filesystem::path& data_root, const AgentRun& run,
		                 std::string* error_out = nullptr);
		/// Inert initial creation only; requires absent primary and backup, never restore or adoption.
		static std::shared_ptr<PreparedAgentRunSave> PrepareInitialSave(
		    const std::filesystem::path& data_root, const AgentRun& candidate,
		    std::string* error_out = nullptr);
		/// Conditional persistence only; does not admit execution.
		static std::shared_ptr<PreparedAgentRunSave> PrepareSave(
		    const std::filesystem::path& data_root, const AgentRun& expected,
		    const AgentRun& candidate, std::string* error_out = nullptr);
		static AgentRunPublishResult PublishPreparedSave(
		    const std::shared_ptr<PreparedAgentRunSave>& prepared,
		    const std::function<bool()>& still_owned);
		/// Per-call deterministic IO-stage faults; no global hook or callback reentrancy.
		static AgentRunPublishResult PublishPreparedSave(
		    const std::shared_ptr<PreparedAgentRunSave>& prepared,
		    const std::function<bool()>& still_owned,
		    const std::function<bool(io::AtomicWriteStage)>& stage_hook_for_tests);
		/// Startup/recovery loader: v1 backup recovery only; recognizable v2 records stay inert on disk.
		static AgentRunLoadResult LoadAll(const std::filesystem::path& data_root);
		/// Primary-only read: validates v1/v2 for inspection, without execution authority or writes.
		/// The optional per-call observer is for deterministic concurrent-read regression fixtures.
		static AgentRunLoadResult LoadAllReadOnly(const std::filesystem::path& data_root,
		    const std::function<void(const std::filesystem::path&)>& after_primary_size_for_tests = {});
		static bool MarkNonterminalInterrupted(const std::filesystem::path& data_root,
		                                       std::vector<AgentRun>* runs,
		                                       std::string_view diagnostic_code,
		                                       std::vector<std::string>* errors = nullptr);
	  private:
		static AgentRunLoadResult LoadAllImpl(const std::filesystem::path& data_root, bool recover,
		    const std::function<void(const std::filesystem::path&)>& after_primary_size_for_tests);
	};
} // namespace uam
