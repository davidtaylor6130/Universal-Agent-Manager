#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace uam::codex
{
	struct SetupInputState
	{
		std::string pending;
		bool configured = false;
	};

	/// <summary>Deliver complete central instructions through native thread setup, without OS argv limits.</summary>
	inline bool ApplySetupInstructions(nlohmann::json& request, const std::string& instructions)
	{
		if (instructions.empty() || !request.is_object()) return false;
		const nlohmann::json::const_iterator method = request.find("method");
		if (method == request.end() || !method->is_string() ||
		    (*method != "thread/start" && *method != "thread/resume")) return false;
		if (!request.contains("params") || !request["params"].is_object()) return false;
		request["params"]["developerInstructions"] = instructions;
		return true;
	}

	/// <summary>Prepare a transactional input update; the caller commits state only after successful delivery.</summary>
	inline bool PrepareSetupInput(const std::string& instructions, const SetupInputState& current,
	                              std::string_view input, std::string& output,
	                              SetupInputState& next, std::string& error)
	{
		next = current;
		output.clear();
		if (instructions.empty() || current.configured)
		{
			output.assign(input);
			return true;
		}
		// Only startup frames are buffered. After thread setup, arbitrary prompt/image input passes unchanged.
		constexpr std::size_t max_startup_frame_bytes = 32 * 1024 * 1024;
		if (input.size() > max_startup_frame_bytes - next.pending.size())
		{
			error = "Codex startup input exceeds 32 MiB.";
			return false;
		}
		next.pending.append(input);
		std::size_t consumed = 0;
		while (true)
		{
			const std::size_t end = next.pending.find('\n', consumed);
			if (end == std::string::npos) break;
			const std::string_view line(next.pending.data() + consumed, end - consumed);
			nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
			if (request.is_discarded() || !request.is_object())
			{
				error = "Codex startup input is not a JSON request.";
				return false;
			}
			const nlohmann::json::const_iterator method = request.find("method");
			if (method != request.end() && method->is_string() &&
			    (*method == "thread/start" || *method == "thread/resume") &&
			    (!request.contains("params") || !request["params"].is_object()))
			{
				error = "Codex thread setup parameters are invalid.";
				return false;
			}
			if (ApplySetupInstructions(request, instructions))
			{
				output += request.dump() + "\n";
				output.append(next.pending, end + 1, std::string::npos);
				next.pending.clear();
				next.configured = true;
				return true;
			}
			output.append(next.pending, consumed, end + 1 - consumed);
			consumed = end + 1;
		}
		next.pending.erase(0, consumed);
		return true;
	}
}
