#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>

namespace uam::remote
{
	class RunnerState;
	inline constexpr int kRunnerProtocolVersion = 3;
	/// Set by the helper on every process it starts and inherited by that process's children, so a process that outlives
	/// its helper can still be found from the host's process table by the session it belongs to.
	inline constexpr std::string_view kRemoteSessionEnvironment = "UAM_REMOTE_SESSION_ID";
	inline constexpr std::size_t kMaxRunnerFrameBytes = 1024 * 1024;
	/// Largest file a helper accepts through file.begin. Helpers from before the `maxUploadBytes` capability accept the legacy size.
	inline constexpr std::uintmax_t kMaxUploadBytes = 4ull * 1024ull * 1024ull * 1024ull;
	inline constexpr std::uintmax_t kLegacyMaxUploadBytes = 25ull * 1024ull * 1024ull;

	inline std::string RunnerEndpointName(std::string_view runner_version,
	                                     int protocol_version = kRunnerProtocolVersion)
	{
		// Protocol 2 runners predate versioned endpoints and always serve uam.sock.
		if (protocol_version == 2) return "uam";
		return "uam-" + std::string(runner_version) + "-p" +
		       std::to_string(protocol_version);
	}

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
