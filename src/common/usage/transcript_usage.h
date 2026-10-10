#pragma once

#include "common/provider/provider_ids.h"

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <optional>
#include <map>
#include <vector>
#include <string>
#include <string_view>

namespace uam::usage
{
	inline constexpr std::int64_t kMaxExactTokenCount = 9007199254740991LL;

	struct TokenCounts
	{
		std::int64_t input = 0;
		std::int64_t cached_input = 0;
		std::int64_t cache_write = 0;
		std::int64_t output = 0;
		// Reasoning is included in output, never added to the billed-token total.
		std::int64_t reasoning = 0;
		std::int64_t Total() const { return input + cached_input + cache_write + output; }
	};

	struct Record
	{
		std::string provider_id;
		std::string model_id;
		std::string session_id;
		std::string speed = "standard";
		std::string dedupe_key;
		std::int64_t timestamp_ms = 0;
		TokenCounts tokens;
		std::optional<double> reported_cost_usd;
	};

	struct CodexScanState
	{
		std::string model_id;
		std::string session_id;
		std::string speed = "standard";
		std::string last_signature;
		bool saw_session_meta = false;
		bool suppressing_fork_copies = false;
		std::int64_t fork_anchor_ms = 0;
		std::size_t suppressed_fork_records = 0;
	};

	struct CopilotScanState
	{
		std::map<std::string, TokenCounts> model_totals;
		bool is_subagent = false;
		std::size_t rejected_snapshots = 0;
	};

	/// <summary>Emit increments from durable Copilot accounting snapshots; premium request multipliers are not USD.</summary>
	std::vector<Record> CopilotRecords(const nlohmann::json& event, std::string_view session_id, CopilotScanState& state);

	/// <summary>Parse a timezone-qualified ISO timestamp without using local timezone state.</summary>
	std::optional<std::int64_t> TimestampMilliseconds(std::string_view text);

	/// <summary>Normalize Claude usage; the collector deduplicates message/request identities across files.</summary>
	std::optional<Record> ClaudeRecord(const nlohmann::json& event);

	/// <summary>Normalize Gemini saved message counts, with cached input and reasoning kept as subsets.</summary>
	std::optional<Record> GeminiRecord(const nlohmann::json& message, std::string_view session_id);

	/// <summary>Track Codex model/tier changes and emit per-turn usage, never cumulative totals.</summary>
	std::optional<Record> CodexRecord(const nlohmann::json& event, CodexScanState& state);
}
