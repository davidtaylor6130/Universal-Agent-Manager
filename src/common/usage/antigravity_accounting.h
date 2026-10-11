#pragma once

#include "common/usage/transcript_usage.h"

namespace uam::usage
{
	struct AntigravityUsageCandidate
	{
		Record record;
		std::vector<std::string> aliases;
		std::string location_id;
		int timestamp_quality = 0;
		bool is_step = false;
	};
	struct AntigravityMetadata
	{
		std::string model;
		std::optional<std::int64_t> timestamp_ms;
		std::vector<AntigravityUsageCandidate> candidates;
	};

	/// <summary>Decode accounting-only protobuf rows; keep request aliases for merging generation, step and retry copies.</summary>
	std::optional<AntigravityMetadata> DecodeAntigravityMetadata(std::string_view bytes, std::string_view session_id,
	    std::int64_t row_index, bool step, std::int64_t fallback_ms, std::optional<std::int64_t> trajectory_ms = {}, std::string_view generation_model = {});
	/// <summary>Merge aliases before date filtering so copied metadata does not become a second billed record.</summary>
	std::optional<std::vector<Record>> MergeAntigravityCandidates(const std::vector<const AntigravityUsageCandidate*>& candidates, std::vector<std::size_t>* group_owners = nullptr);
	/// <summary>Retain aliases and timestamp reliability inside the opaque native identity used by storage and runner transport.</summary>
	std::optional<Record> EncodeAntigravityAccountingRecord(const AntigravityUsageCandidate& candidate);
	std::optional<AntigravityUsageCandidate> DecodeAntigravityAccountingRecord(const Record& record);
	inline constexpr std::string_view kAntigravityAccountingPrefix = "[\"antigravity-accounting-v1\",";
	bool DecodeAntigravityTrajectoryTimestamp(std::string_view bytes, std::optional<std::int64_t>& timestamp_ms);
}
