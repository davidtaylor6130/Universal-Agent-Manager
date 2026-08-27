#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <string>
#include <vector>

namespace uam
{
	struct AgentRunLoadResult
	{
		std::vector<AgentRun> runs;
		std::vector<std::string> errors;
	};

	class AgentRunLedger
	{
	  public:
		static std::string NewRunId();
		static bool Save(const std::filesystem::path& data_root, const AgentRun& run,
		                 std::string* error_out = nullptr);
		static AgentRunLoadResult LoadAll(const std::filesystem::path& data_root);
		static bool MarkNonterminalInterrupted(const std::filesystem::path& data_root,
		                                       std::vector<AgentRun>* runs,
		                                       std::string_view diagnostic_code,
		                                       std::vector<std::string>* errors = nullptr);
	};
} // namespace uam
