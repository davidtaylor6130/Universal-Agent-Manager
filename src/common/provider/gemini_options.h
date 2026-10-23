#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace uam::gemini
{
	/// <summary>Package-inspected named thinking levels; no routed aliases, budgets or inferred future versions.</summary>
	inline std::vector<std::string> SupportedReasoningEfforts(std::string_view model, std::string_view version)
	{
		if (version != "0.55.1" && version != "0.64.0") return {};
		if (model == "gemini-3-pro-preview") return {"low", "high"};
		if (model == "gemini-3.1-pro-preview" || model == "gemini-3.1-pro-preview-customtools") return {"low", "medium", "high"};
		if (model == "gemini-3-flash-preview" || model == "gemini-3.1-flash-lite" || model == "gemini-3.5-flash" ||
		    (version == "0.64.0" && model == "gemini-3.5-flash-lite")) return {"minimal", "low", "medium", "high"};
		if (version == "0.64.0" && model == "gemini-3.8-flash") return {"low", "medium", "high"};
		return {};
	}

	inline bool SupportsReasoningEffort(std::string_view model, std::string_view effort, std::string_view version)
	{
		const auto levels = SupportedReasoningEfforts(model, version);
		return std::find(levels.begin(), levels.end(), effort) != levels.end();
	}
}
