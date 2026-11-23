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
		std::string inputs;    // fingerprint of the chat's worker, provider, CLI and policy inputs when it failed
		int count = 0;
		bool paused = false;
		int message_count = 0;             // new messages end a pause; checked by the idle sweep on every poll
		int parked_manual_scan_start = -1; // a manual scan taken off the queue while paused, queued again on resume
	};
}

namespace uam::memory
{
	// ponytail: three identical failures (about 15 minutes of retries at the per-chat backoff) before
	// pausing; make it a setting only if a deterministic category shows failures that clear on their own.
	constexpr int kIdenticalWorkerFailuresBeforePause = 3;
	// Paused chats' provider, CLI and settings inputs are compared at most this often; new messages resume a chat on the next poll.
	constexpr double kPausedWorkerInputRecheckSeconds = 30.0;

	/// <summary>Failures that clear on their own (a quota reset, a host coming back, a slow provider) keep the
	/// ordinary backoff and never pause: nothing in the chat's inputs would change to resume it.</summary>
	inline bool IsTransientWorkerFailure(std::string_view category)
	{
		return category == "rate-limit" || category == "network" || category == "timeout";
	}

	/// <summary>Returns a fixed category for a failed worker process from its error and stderr text, which it
	/// reads but never returns. Standard output is model text derived from the chat, so it is not consulted.</summary>
	inline std::string_view ClassifyWorkerFailure(const ProcessExecutionResult& result)
	{
		if (result.timed_out) return "timeout";
		if (result.canceled) return "canceled";
		if (result.output_truncated) return "output-truncated";
		if (result.exit_code == 127) return "command-not-found";
		constexpr std::size_t kTailBytes = 4096;
		const auto tail = [](std::string_view text) { return text.size() > kTailBytes ? text.substr(text.size() - kTailBytes) : text; };
		const std::string_view sources[] = {tail(result.error), tail(result.diagnostic_tail)};
		const auto any = [&sources](std::initializer_list<std::string_view> needles)
		{
			for (const std::string_view source : sources)
				if (uam::strings::ContainsAnyCaseInsensitive(source, needles)) return true;
			return false;
		};
		// Transient categories come first, so an auth or version word in the same text cannot turn a passing outage into a pause.
		if (any({"rate limit", "rate_limit", "ratelimit", "too many requests", "quota exceeded", "exceeded your current quota", "insufficient_quota", "resource_exhausted", "resource exhausted", "usage limit"})) return "rate-limit";
		if (any({"econnrefused", "enotfound", "econnreset", "etimedout", "enetunreach", "eai_again", "connection refused", "connection reset", "could not resolve", "network error",
		         "fetch failed", "socket hang up", "timed out", "deadline exceeded", "internal server error", "bad gateway", "service unavailable", "gateway timeout", "overloaded"}))
			return "network";
		if (any({"unauthorized", "unauthenticated", "not logged in", "authenticat", "api key", "apikey", "forbidden", "login required"})) return "auth";
		if (any({"model not found", "unknown model", "invalid model", "model is not supported", "model not supported"})) return "model";
		if (any({"unsupported", "not supported", "minimum version", "unknown option", "unknown argument", "unrecognized option", "unexpected argument"})) return "unsupported-cli";
		return result.exit_code >= 0 ? "exit" : "unknown";
	}

	/// <summary>Identifies a failure by category and exit code so unrelated failures do not extend one streak.</summary>
	inline std::string WorkerFailureSignature(std::string_view category, int exit_code)
	{
		std::string signature(category);
		if (exit_code >= 0) signature.append(" code=").append(std::to_string(exit_code));
		return signature;
	}

	/// <summary>Counts one deterministic failure; returns true only when this failure starts a pause. The streak
	/// survives a resume caused by new messages, so the same failure on the new messages pauses again at once.</summary>
	inline bool RecordWorkerFailure(MemoryWorkerFailureStreak& streak, std::string signature, std::string inputs, int message_count)
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
		return true;
	}
}
