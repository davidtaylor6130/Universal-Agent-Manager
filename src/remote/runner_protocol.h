#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>

namespace uam::remote
{
	class RunnerState;
	inline constexpr int kRunnerProtocolVersion = 2;
	inline constexpr std::size_t kMaxRunnerFrameBytes = 1024 * 1024;

	enum class FrameReadResult
	{
		Ok,
		EndOfStream,
		Error,
	};

	FrameReadResult ReadFrame(std::istream& input, nlohmann::json& message,
	                          std::string* error = nullptr);
	bool WriteFrame(std::ostream& output, const nlohmann::json& message,
	                std::string* error = nullptr);
	nlohmann::json HandleRunnerRequest(const nlohmann::json& request,
	                                   std::string_view runner_version,
	                                   RunnerState* state = nullptr);
}
