#include "test_harness.h"
#include "common/usage/transcript_usage.h"
#include "common/usage/usage_aggregation.h"
#include <cmath>
#include <limits>

using namespace uam_test;

namespace
{
	nlohmann::json CodexUsage(const std::string& timestamp, std::int64_t cumulative_input = 120)
	{
		return {{"type", "event_msg"}, {"timestamp", timestamp}, {"payload", {
		    {"type", "token_count"}, {"info", {
		        {"last_token_usage", {{"input_tokens", 120}, {"cached_input_tokens", 50}, {"cache_write_input_tokens", 10}, {"output_tokens", 30}, {"reasoning_output_tokens", 20}}},
		        {"total_token_usage", {{"input_tokens", cumulative_input}, {"output_tokens", 30}}}
		    }}}}};
	}

	nlohmann::json ClaudeUsage()
	{
		return {{"type", "assistant"}, {"timestamp", "2026-10-10T12:00:00.125Z"}, {"requestId", "request"}, {"sessionId", "session"}, {"costUSD", 0.125},
		    {"message", {{"id", "message"}, {"model", "claude-sonnet"}, {"usage", {
		        {"input_tokens", 60}, {"cache_read_input_tokens", 50}, {"cache_creation_input_tokens", 10}, {"output_tokens", 30}, {"speed", "fast"}
		    }}}}};
	}
}

UAM_TEST(UsageTimestampParsingUsesExplicitTimezoneAndRejectsInvalidDates)
{
	UAM_ASSERT_EQ(*uam::usage::TimestampMilliseconds("2026-10-10T12:00:00.125Z"), *uam::usage::TimestampMilliseconds("2026-10-10T14:30:00.125000+02:30"));
	UAM_ASSERT_EQ(*uam::usage::TimestampMilliseconds("1970-01-01T00:00:00.1Z"), 100LL);
	UAM_ASSERT_EQ(*uam::usage::TimestampMilliseconds("1970-01-01T00:00:00.12Z"), 120LL);
	UAM_ASSERT_EQ(*uam::usage::TimestampMilliseconds("1970-01-01T00:00:00Z"), 0LL);
	UAM_ASSERT(uam::usage::TimestampMilliseconds("2024-02-29T12:00:00Z"));
	for (const std::string invalid : {"2026-02-29T12:00:00Z", "2026-13-10T12:00:00Z", "2026-10-10T24:00:00Z", "2026-10-10T12:60:00Z", "2026-10-10T12:00:00", "2026-10-10T12:00:00.Z", "2026-10-10T12:00:00+24:00", "2026-10-10T12:00:00Zjunk", "2026-10-10T12:00:00.1234567890Z"})
		UAM_ASSERT(!uam::usage::TimestampMilliseconds(invalid));
}

UAM_TEST(UsageCodexTracksModelAndTierWithoutDoubleCountingCumulativeOrReasoningTokens)
{
	uam::usage::CodexScanState state;
	nlohmann::json event = CodexUsage("2026-10-10T12:00:10Z");
	UAM_ASSERT(!uam::usage::CodexRecord(event, state));
	UAM_ASSERT(state.last_signature.empty());
	uam::usage::CodexRecord({{"type", "session_meta"}, {"payload", {{"id", "native-session"}}}}, state);
	uam::usage::CodexRecord({{"type", "turn_context"}, {"payload", {{"model", "gpt-6"}}}}, state);
	const std::optional<uam::usage::Record> first = uam::usage::CodexRecord(event, state);
	UAM_ASSERT(first);
	UAM_ASSERT_EQ(first->tokens.input, 60LL);
	UAM_ASSERT_EQ(first->tokens.Total(), 150LL);
	UAM_ASSERT_EQ(first->tokens.reasoning, 20LL);
	UAM_ASSERT_EQ(first->model_id, std::string("gpt-6"));
	UAM_ASSERT(!first->reported_cost_usd);
	// A stream boundary may repeat the same totals with a newer timestamp.
	event["timestamp"] = "2026-10-10T12:00:11Z";
	UAM_ASSERT(!uam::usage::CodexRecord(event, state));
	uam::usage::CodexRecord({{"type", "turn_context"}, {"payload", {{"model", "gpt-6-mini"}}}}, state);
	uam::usage::CodexRecord({{"payload", {{"type", "thread_settings_applied"}, {"thread_settings", {{"service_tier", "priority"}}}}}}, state);
	const std::optional<uam::usage::Record> second = uam::usage::CodexRecord(CodexUsage("2026-10-10T12:01:10Z", 240), state);
	UAM_ASSERT(second);
	UAM_ASSERT_EQ(second->tokens.Total(), 150LL);
	UAM_ASSERT_EQ(second->model_id, std::string("gpt-6-mini"));
	UAM_ASSERT_EQ(second->speed, std::string("fast"));
	UAM_ASSERT(first->dedupe_key != second->dedupe_key);
}

UAM_TEST(UsageCodexForkHistorySuppressionPreservesChildIdentityAndCountsRealTurns)
{
	uam::usage::CodexScanState state;
	uam::usage::CodexRecord({{"type", "session_meta"}, {"timestamp", "2026-10-10T12:00:00Z"}, {"payload", {{"id", "child"}, {"forked_from_id", "parent"}}}}, state);
	uam::usage::CodexRecord({{"type", "session_meta"}, {"payload", {{"id", "parent"}}}}, state);
	uam::usage::CodexRecord({{"type", "turn_context"}, {"payload", {{"model", "gpt-6"}}}}, state);
	UAM_ASSERT(!uam::usage::CodexRecord(CodexUsage("2026-10-10T12:00:00.040Z"), state));
	UAM_ASSERT(!uam::usage::CodexRecord(CodexUsage("2026-10-10T12:00:00.080Z", 240), state));
	UAM_ASSERT_EQ(state.suppressed_fork_records, std::size_t(2));
	const std::optional<uam::usage::Record> child = uam::usage::CodexRecord(CodexUsage("2026-10-10T12:00:05Z", 360), state);
	UAM_ASSERT(child);
	UAM_ASSERT_EQ(child->session_id, std::string("child"));
	UAM_ASSERT(!state.suppressing_fork_copies);
	UAM_ASSERT(uam::usage::CodexRecord(CodexUsage("2026-10-10T12:00:05.010Z", 480), state));
}

UAM_TEST(UsageClaudeIncludesCacheAndReportedCostWithStableCrossFileIdentity)
{
	nlohmann::json event = ClaudeUsage();
	const std::optional<uam::usage::Record> first = uam::usage::ClaudeRecord(event);
	UAM_ASSERT(first);
	UAM_ASSERT_EQ(first->tokens.Total(), 150LL);
	UAM_ASSERT_EQ(first->tokens.reasoning, 0LL);
	UAM_ASSERT_EQ(first->speed, std::string("fast"));
	UAM_ASSERT_EQ(*first->reported_cost_usd, 0.125);
	event["sessionId"] = "branched-session";
	event["message"]["content"] = nlohmann::json::array({{{"type", "text"}, {"text", "Another block of the same response"}}});
	UAM_ASSERT_EQ(uam::usage::ClaudeRecord(event)->dedupe_key, first->dedupe_key);
	event["costUSD"] = -1;
	UAM_ASSERT(!uam::usage::ClaudeRecord(event)->reported_cost_usd);
	event["message"].erase("id"); event.erase("requestId");
	UAM_ASSERT(uam::usage::ClaudeRecord(event)->dedupe_key.empty());
}

UAM_TEST(UsageParsersRejectUnsafeCountsAndMalformedRecordsWithoutPoisoningState)
{
	for (const nlohmann::json invalid : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("10"), nlohmann::json(true), nlohmann::json(9007199254740992ULL)})
	{
		nlohmann::json event = ClaudeUsage();
		event["message"]["usage"]["input_tokens"] = invalid;
		UAM_ASSERT(!uam::usage::ClaudeRecord(event));
	}
	for (const nlohmann::json invalid : {nlohmann::json::array(), nlohmann::json(nullptr), nlohmann::json("text"), nlohmann::json::object()})
	{
		UAM_ASSERT(!uam::usage::ClaudeRecord(invalid));
		uam::usage::CodexScanState state;
		UAM_ASSERT(!uam::usage::CodexRecord(invalid, state));
	}
	uam::usage::CodexScanState state;
	state.model_id = "gpt-6";
	nlohmann::json event = CodexUsage("2026-10-10T12:00:10Z");
	event["payload"]["info"]["last_token_usage"]["cached_input_tokens"] = 121;
	UAM_ASSERT(!uam::usage::CodexRecord(event, state));
	UAM_ASSERT(state.last_signature.empty());
	event = CodexUsage("2026-10-10T12:00:10Z");
	event["payload"]["info"]["last_token_usage"]["reasoning_output_tokens"] = 31;
	UAM_ASSERT(!uam::usage::CodexRecord(event, state));
	UAM_ASSERT(uam::usage::CodexRecord(CodexUsage("2026-10-10T12:00:10Z"), state));
}

UAM_TEST(UsageAggregationDeduplicatesAcrossFilesAndSeparatesReportedEstimatedAndUnpricedCost)
{
	const std::int64_t start = *uam::usage::TimestampMilliseconds("2026-10-10T00:00:00Z");
	uam::usage::Record reported = *uam::usage::ClaudeRecord(ClaudeUsage());
	uam::usage::Record estimated = reported;
	estimated.provider_id = uam::provider_ids::kCodexCli;
	estimated.model_id = "gpt-6";
	estimated.speed = "standard";
	estimated.reported_cost_usd.reset();
	uam::usage::Record unknown = estimated;
	unknown.model_id = "unknown-model";
	unknown.dedupe_key = "unknown";
	unknown.session_id.clear();
	unknown.timestamp_ms += 3600000;
	uam::usage::Record duplicate = reported;
	duplicate.session_id = "fork";
	uam::usage::Record outside = estimated;
	outside.dedupe_key = "outside";
	outside.timestamp_ms = start + 86400000;
	const std::map<uam::usage::ModelKey, uam::usage::ModelPrices> prices{{{uam::provider_ids::kCodexCli, "gpt-6", "standard"}, {2, 0.5, 3, 4}}};
	const std::optional<uam::usage::Summary> summary = uam::usage::Aggregate({reported, duplicate, estimated, unknown, outside}, start, start + 86400000, prices);
	UAM_ASSERT(summary);
	UAM_ASSERT_EQ(summary->total.tokens.Total(), 450LL);
	UAM_ASSERT_EQ(summary->total.sessions, std::size_t(2));
	UAM_ASSERT_EQ(summary->duplicates, std::size_t(1));
	UAM_ASSERT_EQ(summary->total.records, std::size_t(3));
	UAM_ASSERT_EQ(summary->total.reported_cost_tokens, 150LL);
	UAM_ASSERT_EQ(summary->total.estimated_cost_tokens, 150LL);
	UAM_ASSERT_EQ(summary->total.unpriced_tokens, 150LL);
	UAM_ASSERT_EQ(summary->total.reported_cost_usd, 0.125);
	UAM_ASSERT(std::abs(summary->total.estimated_cost_usd - 0.000295) < 1e-12);
	UAM_ASSERT(std::abs(summary->total.estimated_cache_savings_usd - 0.000075) < 1e-12);
	UAM_ASSERT_EQ(summary->models.size(), std::size_t(3));
	UAM_ASSERT_EQ(summary->days.size(), std::size_t(1));
	UAM_ASSERT_EQ(summary->hours.size(), std::size_t(2));
	UAM_ASSERT_EQ(summary->providers.at(uam::provider_ids::kClaudeCli).sessions, std::size_t(1));
	UAM_ASSERT_EQ(summary->provider_days.size(), std::size_t(2));
	UAM_ASSERT_EQ(summary->provider_hours.size(), std::size_t(3));
	const uam::usage::Totals& claude_day = summary->provider_days.at({start / 86400000 * 86400000, uam::provider_ids::kClaudeCli});
	UAM_ASSERT_EQ(claude_day.tokens.Total(), 150LL);
	UAM_ASSERT_EQ(claude_day.sessions, std::size_t(1));
	UAM_ASSERT_EQ(claude_day.reported_cost_usd, 0.125);
	const uam::usage::Totals& codex_day = summary->provider_days.at({start / 86400000 * 86400000, uam::provider_ids::kCodexCli});
	UAM_ASSERT_EQ(codex_day.tokens.Total(), 300LL);
	UAM_ASSERT_EQ(codex_day.sessions, std::size_t(1));
	UAM_ASSERT_EQ(codex_day.unpriced_tokens, 150LL);
	std::map<uam::usage::ModelKey, uam::usage::ModelPrices> incomplete_prices = prices;
	incomplete_prices.begin()->second.output_per_million.reset();
	const std::optional<uam::usage::Summary> incomplete = uam::usage::Aggregate({estimated}, start, start + 86400000, incomplete_prices);
	UAM_ASSERT(incomplete);
	UAM_ASSERT_EQ(incomplete->total.unpriced_tokens, 150LL);
	UAM_ASSERT_EQ(incomplete->total.estimated_cost_usd, 0.0);
}

UAM_TEST(UsageAggregationRejectsOverflowAndInvalidPricesAndDoesNotGuessMissingIdentity)
{
	uam::usage::Record record = *uam::usage::ClaudeRecord(ClaudeUsage());
	const std::int64_t start = record.timestamp_ms;
	UAM_ASSERT(!uam::usage::Aggregate({record}, start, start));
	UAM_ASSERT(!uam::usage::Aggregate({record}, start, start + 1, {{{record.provider_id, record.model_id, record.speed}, {-1, 0, 0, 0}}}));
	UAM_ASSERT(!uam::usage::Aggregate({record}, start, start + 1, {{{record.provider_id, record.model_id, record.speed}, {std::numeric_limits<double>::infinity(), 0, 0, 0}}}));
	record.tokens = {uam::usage::kMaxExactTokenCount, 0, 0, 0, 0};
	record.dedupe_key.clear(); record.session_id.clear();
	UAM_ASSERT(!uam::usage::Aggregate({record, record}, start, start + 1));
	const std::optional<uam::usage::Summary> summary = uam::usage::Aggregate({record}, start, start + 1);
	UAM_ASSERT(summary);
	UAM_ASSERT_EQ(summary->unidentified_records, std::size_t(1));
	UAM_ASSERT_EQ(summary->total.sessions, std::size_t(0));
}

UAM_TEST(UsageAggregationDoesNotMoveCopiedParentUsageIntoANewDateWindow)
{
	uam::usage::Record original = *uam::usage::ClaudeRecord(ClaudeUsage());
	uam::usage::Record copied = original;
	copied.timestamp_ms += 86400000;
	copied.session_id = "fork";
	for (const std::vector<uam::usage::Record> records : {std::vector<uam::usage::Record>{original, copied}, std::vector<uam::usage::Record>{copied, original}})
	{
		const std::optional<uam::usage::Summary> summary = uam::usage::Aggregate(records, copied.timestamp_ms, copied.timestamp_ms + 1);
		UAM_ASSERT(summary);
		UAM_ASSERT_EQ(summary->total.tokens.Total(), 0LL);
		UAM_ASSERT_EQ(summary->total.reported_cost_usd, 0.0);
		UAM_ASSERT_EQ(summary->duplicates, std::size_t(1));
	}
}

UAM_TEST(UsageGeminiSeparatesCachedInputAndIncludesThoughtsOnlyOnce)
{
	nlohmann::json message{{"type", "gemini"}, {"id", "reply"}, {"model", "gemini-test"}, {"timestamp", "2026-10-10T12:00:00Z"},
	    {"tokens", {{"input", 100}, {"cached", 40}, {"output", 20}, {"thoughts", 10}, {"tool", 5}, {"total", 135}}}};
	const std::optional<uam::usage::Record> record = uam::usage::GeminiRecord(message, "session");
	UAM_ASSERT(record);
	UAM_ASSERT_EQ(record->tokens.input, 65LL);
	UAM_ASSERT_EQ(record->tokens.cached_input, 40LL);
	UAM_ASSERT_EQ(record->tokens.output, 30LL);
	UAM_ASSERT_EQ(record->tokens.reasoning, 10LL);
	UAM_ASSERT_EQ(record->tokens.Total(), 135LL);
	UAM_ASSERT_EQ(record->provider_id, std::string(uam::provider_ids::kGeminiCli));
	UAM_ASSERT(!uam::usage::GeminiRecord(message, ""));
	for (const nlohmann::json invalid : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("100"), nlohmann::json(9007199254740992ULL)})
	{
		message["tokens"]["input"] = invalid;
		UAM_ASSERT(!uam::usage::GeminiRecord(message, "session"));
	}
	message["tokens"]["input"] = 100;
	message["tokens"]["cached"] = 101;
	UAM_ASSERT(!uam::usage::GeminiRecord(message, "session"));
	message["tokens"]["cached"] = 40;
	message["tokens"]["total"] = 140;
	UAM_ASSERT(!uam::usage::GeminiRecord(message, "session"));
}

UAM_TEST(UsageCopilotDurableSnapshotsEmitOnlyNewTokensAndNeverPremiumRequestDollars)
{
	uam::usage::CopilotScanState state;
	nlohmann::json event{{"type", "session.shutdown"}, {"timestamp", "2026-10-10T12:00:00Z"},
	    {"data", {{"modelMetrics", {{"claude-sonnet", {{"requests", {{"cost", 1}}}, {"usage", {{"inputTokens", 100}, {"cacheReadTokens", 30}, {"cacheWriteTokens", 10}, {"outputTokens", 20}, {"reasoningTokens", 5}}}}}}}}}};
	std::vector<uam::usage::Record> records = uam::usage::CopilotRecords(event, "session", state);
	UAM_ASSERT_EQ(records.size(), std::size_t(1));
	UAM_ASSERT_EQ(records[0].tokens.input, 60LL);
	UAM_ASSERT_EQ(records[0].tokens.Total(), 120LL);
	UAM_ASSERT(!records[0].reported_cost_usd);
	UAM_ASSERT(uam::usage::CopilotRecords(event, "session", state).empty());
	event["type"] = "session.usage_checkpoint";
	event["data"]["accountingSnapshot"] = event["data"];
	event["data"].erase("modelMetrics");
	nlohmann::json& usage = event["data"]["accountingSnapshot"]["modelMetrics"]["claude-sonnet"]["usage"];
	usage["inputTokens"] = 150; usage["cacheReadTokens"] = 50; usage["outputTokens"] = 30;
	records = uam::usage::CopilotRecords(event, "session", state);
	UAM_ASSERT_EQ(records.size(), std::size_t(1));
	UAM_ASSERT_EQ(records[0].tokens.input, 30LL);
	UAM_ASSERT_EQ(records[0].tokens.cached_input, 20LL);
	UAM_ASSERT_EQ(records[0].tokens.Total(), 60LL);
	const std::string first_key = records[0].dedupe_key;
	for (const nlohmann::json bad : {nlohmann::json(nullptr), nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json(9007199254740992ULL)})
	{
		usage["inputTokens"] = bad;
		UAM_ASSERT(uam::usage::CopilotRecords(event, "session", state).empty());
	}
	usage["inputTokens"] = 140;
	UAM_ASSERT(uam::usage::CopilotRecords(event, "session", state).empty());
	UAM_ASSERT_EQ(state.rejected_snapshots, std::size_t(5));
	usage["inputTokens"] = 150;
	UAM_ASSERT(uam::usage::CopilotRecords(event, "session", state).empty());
	usage["inputTokens"] = 160;
	records = uam::usage::CopilotRecords(event, "session", state);
	UAM_ASSERT_EQ(records.size(), std::size_t(1));
	UAM_ASSERT_EQ(records[0].tokens.Total(), 10LL);
	UAM_ASSERT(records[0].dedupe_key != first_key);
	event["type"] = "assistant.usage";
	UAM_ASSERT(uam::usage::CopilotRecords(event, "session", state).empty());
	event["type"] = "session.shutdown"; event["agentId"] = "child";
	UAM_ASSERT(uam::usage::CopilotRecords(event, "session", state).empty());
}

UAM_TEST(UsageOpenCodeAddsDisjointReasoningAndKeepsProviderEstimatesSeparateFromBills)
{
	nlohmann::json message{{"type", "assistant"}, {"model", {{"id", "model"}, {"providerID", "openai"}}}, {"cost", 0.03},
	    {"tokens", {{"input", 10}, {"output", 5}, {"reasoning", 3}, {"cache", {{"read", 20}, {"write", 7}}}}}};
	std::optional<uam::usage::Record> record = uam::usage::OpenCodeRecord(message, "session", "message", 100);
	UAM_ASSERT(record);
	UAM_ASSERT_EQ(record->tokens.output, 8LL);
	UAM_ASSERT_EQ(record->tokens.reasoning, 3LL);
	UAM_ASSERT_EQ(record->tokens.Total(), 45LL);
	UAM_ASSERT_EQ(record->model_id, std::string("openai/model"));
	UAM_ASSERT(record->estimated_cost_usd && !record->reported_cost_usd);
	const std::optional<uam::usage::Summary> summary = uam::usage::Aggregate({*record}, 0, 200);
	UAM_ASSERT(summary && summary->total.estimated_cost_usd == 0.03);
	const std::map<uam::usage::ModelKey, uam::usage::ModelPrices> incomplete_prices{{{record->provider_id, record->model_id, "standard"}, {}}};
	const std::optional<uam::usage::Summary> overridden = uam::usage::Aggregate({*record}, 0, 200, incomplete_prices);
	UAM_ASSERT(overridden && overridden->total.unpriced_tokens == 45);
	UAM_ASSERT_EQ(overridden->total.estimated_cost_tokens, 0LL);
	message["cost"] = 0;
	record = uam::usage::OpenCodeRecord(message, "session", "message", 100);
	UAM_ASSERT(record && !record->estimated_cost_usd);
	for (const nlohmann::json bad : {nlohmann::json(nullptr), nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json(9007199254740992ULL)})
	{
		message["tokens"]["input"] = bad;
		UAM_ASSERT(!uam::usage::OpenCodeRecord(message, "session", "message", 100));
	}
}


#include "common/utils/history_timestamp_order.h"

namespace
{
	struct HistoryTimestampTestZone
	{
		std::unique_ptr<ScopedEnvVar> zone;
		static void Reload()
		{
#if defined(_WIN32)
			_tzset();
#else
			tzset();
#endif
		}
		explicit HistoryTimestampTestZone(const std::string& value) : zone(std::make_unique<ScopedEnvVar>("TZ", value))
		{
			Reload();
		}
		~HistoryTimestampTestZone()
		{
			zone.reset();
			Reload();
		}
	};
}

UAM_TEST(HistoryTimestampOffsetOrderingPreservesNanosecondsAndEquivalentInstants)
{
	using namespace uam::time;
	const auto equivalent = CompareHistoryTimestamps("2026-10-16T12:00:00.123456789Z", "2026-10-16T14:30:00.123456789+02:30");
	UAM_ASSERT_EQ(equivalent.order, HistoryTimestampOrder::Equal);
	UAM_ASSERT_EQ(equivalent.provenance, HistoryTimestampProvenance::ExplicitOffset);
	UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16T12:00:00.1231Z", "2026-10-16T12:00:00.1239Z").order, HistoryTimestampOrder::Before);
	UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16T12:00:00.000000001Z", "2026-10-16T12:00:00Z").order, HistoryTimestampOrder::After);
	UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16T12:00:00.1Z", "2026-10-16T12:00:00.100000000+00:00").order, HistoryTimestampOrder::Equal);
	UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16T00:00:00-02:00", "2026-10-16T01:59:59.999999999Z").order, HistoryTimestampOrder::After);
	UAM_ASSERT_EQ(CompareHistoryTimestamps("2024-02-29T12:00:00Z", "2024-03-01T12:00:00Z").order, HistoryTimestampOrder::Before);
	UAM_ASSERT_EQ(CompareHistoryTimestamps("9999-12-31T23:59:59Z", "9999-12-31T23:59:58.999999999Z").order, HistoryTimestampOrder::After);
}

UAM_TEST(HistoryTimestampMalformedAndUnqualifiedInstantsRemainUnknown)
{
	using namespace uam::time;
	for (const std::string invalid : {"", "recorded", "2026-02-29T12:00:00Z", "2026-13-10T12:00:00Z", "2026-10-16T24:00:00Z", "2026-10-16T12:60:00Z", "2026-10-16T12:00:60Z", "2026-10-16T12:00:00", "2026-10-16T12:00:00.Z", "2026-10-16T12:00:00.1234567890Z", "2026-10-16T12:00:00Zjunk", "2026-10-16T12:00:00+24:00", "2026-10-16T12:00:00+01:60", "2026-10-16T12:00:00-00:00", "2026-10-16 12:00:00Z", "2026-02-29 12:00:00", "2026-10-16 12:00:00 ", "1969-12-31T23:59:59Z"})
	{
		UAM_ASSERT_EQ(CompareHistoryTimestamps(invalid, "2026-10-16T12:00:00Z").order, HistoryTimestampOrder::Unknown);
		UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16T12:00:00Z", invalid).order, HistoryTimestampOrder::Unknown);
		UAM_ASSERT_EQ(CompareHistoryTimestamps(invalid, invalid).order, HistoryTimestampOrder::Unknown);
	}
}

UAM_TEST(HistoryTimestampLegacyExplicitlyAssumesCurrentControllerZone)
{
	using namespace uam::time;
	{
		HistoryTimestampTestZone zone("UTC0");
		const auto epoch = std::chrono::sys_days{std::chrono::year{2026}/10/16} + std::chrono::hours{12};
		const auto civil = detail::LocalTimeSnapshot(std::chrono::system_clock::to_time_t(epoch));
		UAM_ASSERT_EQ(civil.tm_hour, 12);
		UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16T13:00:00Z", "2026-10-16 12:00:00").order, HistoryTimestampOrder::After);
		UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16T11:00:00Z", "2026-10-16 12:00:00").order, HistoryTimestampOrder::Before);
		UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16 12:00:00", "2026-10-16 12:00:00").order, HistoryTimestampOrder::Equal);
		UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16 11:00:00", "2026-10-16 12:00:00").order, HistoryTimestampOrder::Before);
	}
	{
		HistoryTimestampTestZone zone("JST-9");
		const auto epoch = std::chrono::sys_days{std::chrono::year{2026}/10/16} + std::chrono::hours{3};
		const auto civil = detail::LocalTimeSnapshot(std::chrono::system_clock::to_time_t(epoch));
		UAM_ASSERT_EQ(civil.tm_hour, 12);
		const auto equivalent = CompareHistoryTimestamps("2026-10-16 12:00:00", "2026-10-16T03:00:00Z");
		UAM_ASSERT_EQ(equivalent.order, HistoryTimestampOrder::Equal);
		UAM_ASSERT_EQ(equivalent.provenance, HistoryTimestampProvenance::AssumedCurrentLocalZone);
		UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16T04:00:00Z", "2026-10-16 12:00:00").order, HistoryTimestampOrder::After);
		UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-10-16T02:00:00Z", "2026-10-16 12:00:00").order, HistoryTimestampOrder::Before);
	}
}

UAM_TEST(HistoryTimestampLegacyRejectsExposedDstFoldAndGap)
{
	using namespace uam::time;
	HistoryTimestampTestZone zone("EST5EDT");
	// Real C-runtime observations establish these US-rule transition premises.
	const auto first_epoch = std::chrono::sys_days{std::chrono::year{2026}/11/1} + std::chrono::hours{5} + std::chrono::minutes{30};
	const auto first = detail::LocalTimeSnapshot(std::chrono::system_clock::to_time_t(first_epoch));
	const auto second = detail::LocalTimeSnapshot(std::chrono::system_clock::to_time_t(first_epoch + std::chrono::hours{1}));
	UAM_ASSERT(first.tm_year == 126 && first.tm_mon == 10 && first.tm_mday == 1 && first.tm_hour == 1 && first.tm_min == 30 && first.tm_isdst > 0);
	UAM_ASSERT(second.tm_year == 126 && second.tm_mon == 10 && second.tm_mday == 1 && second.tm_hour == 1 && second.tm_min == 30 && second.tm_isdst == 0);
	UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-11-01 01:30:00", "2026-11-01T06:30:00Z").order, HistoryTimestampOrder::Unknown);
	UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-03-08 02:30:00", "2026-03-08T07:30:00Z").order, HistoryTimestampOrder::Unknown);
	UAM_ASSERT_EQ(CompareHistoryTimestamps("2026-03-08 03:30:00", "2026-03-08T07:30:00Z").order, HistoryTimestampOrder::Equal);
}


UAM_TEST(HistoryTimestampUnknownOverlayDoesNotInventLoadedSummaryMessages)
{
	TempDir temporary("uam-history-timestamp-summary");
	ChatSession saved;
	saved.id = "timestamp-summary-owner";
	saved.provider_id = "gemini-cli";
	saved.native_session_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
	saved.created_at = "2026-10-16T09:00:00Z";
	saved.updated_at = "legacy-with-unknown-time";
	saved.messages = {{MessageRole::User, "Saved question"}, {MessageRole::Assistant, "Saved answer"}};
	UAM_ASSERT(ChatRepository::SaveChat(temporary.root, saved));
	const auto cold = ChatRepository::LoadLocalChat(temporary.root, saved.id, false);
	const auto loaded = ChatRepository::LoadLocalChat(temporary.root, saved.id, true);
	UAM_ASSERT(cold && !cold->messages_loaded && cold->messages.empty() && cold->persisted_message_count == 2);
	UAM_ASSERT(loaded && loaded->messages_loaded && loaded->messages.size() == 2);
	ChatSession incoming = *loaded;
	incoming.updated_at = "2026-10-16T13:00:00Z";
	incoming.messages.back().content = "Different native answer";
	const auto native_messages = incoming.messages;
	ChatHistorySyncService().OverlayPreparedLocalHistory(*cold, incoming);
	UAM_ASSERT(incoming.messages_loaded);
	UAM_ASSERT_EQ(incoming.messages, native_messages);
	UAM_ASSERT_EQ(incoming.updated_at, std::string("2026-10-16T13:00:00Z"));
	ChatHistorySyncService().OverlayPreparedLocalHistory(*loaded, incoming);
	UAM_ASSERT_EQ(incoming.messages, loaded->messages);
	UAM_ASSERT_EQ(incoming.updated_at, loaded->updated_at);
	incoming.messages = native_messages;
	incoming.messages.push_back({MessageRole::Assistant, "Additional native message"});
	incoming.updated_at = "2026-10-16T13:00:00Z";
	const auto longer_native = incoming.messages;
	ChatHistorySyncService().OverlayPreparedLocalHistory(*loaded, incoming);
	UAM_ASSERT_EQ(incoming.messages, longer_native);
	UAM_ASSERT_EQ(incoming.updated_at, std::string("2026-10-16T13:00:00Z"));
}
