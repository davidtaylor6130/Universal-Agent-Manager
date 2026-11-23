#pragma once

#include "common/models/app_models.h"
#include "common/utils/string_utils.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace uam
{
	/// <summary>Consecutive identical memory worker failures for one chat, and the retry pause they caused.</summary>
	struct MemoryWorkerFailureStreak
	{
		std::string signature; // failure category and exit code; never worker or chat text
		std::string inputs;    // fingerprint of the chat's extraction inputs when it failed
		int count = 0;
		bool paused = false;
		int message_count = 0;   // cheap per-poll check for new chat content while paused
		double recheck_at = 0.0; // next full input comparison while paused
	};
}

namespace uam::memory
{
	// ponytail: three identical failures (about 15 minutes of retries at the per-chat backoff) before
	// pausing; make it a setting only if a provider shows legitimate failures that clear on their own.
	constexpr int kIdenticalWorkerFailuresBeforePause = 3;
	// A paused chat's provider, CLI and settings inputs are compared at most this often; new messages resume it on the next poll.
	constexpr double kPausedWorkerInputRecheckSeconds = 30.0;

	/// <summary>Returns a fixed category for a failed worker process. Worker text is read, never returned.</summary>
	inline std::string_view ClassifyWorkerFailure(const ProcessExecutionResult& result)
	{
		if (result.timed_out) return "timeout";
		if (result.canceled) return "canceled";
		if (result.output_truncated) return "output-truncated";
		if (result.exit_code == 127) return "command-not-found";
		constexpr std::size_t kTailBytes = 4096;
		const auto tail = [](std::string_view text) { return text.size() > kTailBytes ? text.substr(text.size() - kTailBytes) : text; };
		const std::string_view sources[] = {tail(result.error), tail(result.diagnostic_tail), tail(result.output)};
		const auto any = [&sources](std::initializer_list<std::string_view> needles)
		{
			for (const std::string_view source : sources)
				if (uam::strings::ContainsAnyCaseInsensitive(source, needles)) return true;
			return false;
		};
		if (any({"rate limit", "rate_limit", "ratelimit", "too many requests", "quota", "usage limit"})) return "rate-limit";
		if (any({"unauthorized", "unauthenticated", "not logged in", "authenticat", "api key", "apikey", "forbidden", "login required"})) return "auth";
		if (any({"model not found", "unknown model", "invalid model", "model is not supported", "model not supported"})) return "model";
		if (any({"unsupported", "not supported", "minimum version", "unknown option", "unknown argument", "unrecognized option", "unexpected argument"})) return "unsupported-cli";
		if (any({"econnrefused", "enotfound", "econnreset", "connection refused", "connection reset", "could not resolve", "network error"})) return "network";
		return result.exit_code >= 0 ? "exit" : "unknown";
	}

	/// <summary>Identifies a failure by category and exit code so unrelated failures do not extend one streak.</summary>
	inline std::string WorkerFailureSignature(std::string_view category, int exit_code)
	{
		std::string signature(category);
		if (exit_code >= 0) signature.append(" exit=").append(std::to_string(exit_code));
		return signature;
	}

	/// <summary>Counts one deterministic failure; returns true only when this failure starts the pause.</summary>
	inline bool RecordWorkerFailure(MemoryWorkerFailureStreak& streak, std::string signature, std::string inputs, int message_count, double now)
	{
		if (streak.signature != signature || streak.inputs != inputs)
		{
			streak = {};
			streak.signature = std::move(signature);
			streak.inputs = std::move(inputs);
		}
		streak.message_count = message_count;
		if (++streak.count < kIdenticalWorkerFailuresBeforePause || streak.paused) return false;
		streak.paused = true;
		streak.recheck_at = now + kPausedWorkerInputRecheckSeconds;
		return true;
	}

	/// <summary>Whether a paused chat's inputs changed. The full fingerprint is built only when new content cannot decide it and the recheck is due.</summary>
	template <typename Fingerprint>
	inline bool PausedWorkerInputsChanged(MemoryWorkerFailureStreak& streak, int message_count, double now, Fingerprint&& fingerprint)
	{
		if (message_count != streak.message_count) return true;
		if (now < streak.recheck_at) return false;
		streak.recheck_at = now + kPausedWorkerInputRecheckSeconds;
		return fingerprint() != streak.inputs;
	}
}
