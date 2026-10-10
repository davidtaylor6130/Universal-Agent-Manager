#include "common/usage/transcript_usage.h"
#include "common/utils/nlohmann_json_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <nlohmann/json.hpp>

namespace uam::usage
{
	namespace
	{
		int DecimalField(std::string_view text, std::size_t offset, std::size_t length)
		{
			int number = 0;
			for (std::size_t index = offset; index < offset + length; ++index)
			{
				if (text[index] < '0' || text[index] > '9') return -1;
				number = number * 10 + (text[index] - '0');
			}
			return number;
		}
	}

	/// <summary>Parse a timezone-qualified ISO timestamp without using the machine's local timezone.</summary>
	std::optional<std::int64_t> TimestampMilliseconds(std::string_view text)
	{
		if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') return std::nullopt;
		const int year = DecimalField(text, 0, 4), month = DecimalField(text, 5, 2), day = DecimalField(text, 8, 2);
		const int hour = DecimalField(text, 11, 2), minute = DecimalField(text, 14, 2), second = DecimalField(text, 17, 2);
		if (year < 1970 || month < 1 || day < 1 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return std::nullopt;
		const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
		if (!date.ok()) return std::nullopt;
		std::size_t cursor = 19;
		int milliseconds = 0;
		if (text[cursor] == '.')
		{
			const std::size_t start = ++cursor;
			while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9')
			{
				if (cursor - start < 3) milliseconds = milliseconds * 10 + (text[cursor] - '0');
				++cursor;
			}
			if (cursor == start || cursor - start > 9) return std::nullopt;
			for (std::size_t digits = cursor - start; digits < 3; ++digits) milliseconds *= 10;
		}
		int offset_minutes = 0;
		if (cursor + 1 == text.size() && text[cursor] == 'Z') {}
		else if (cursor + 6 == text.size() && (text[cursor] == '+' || text[cursor] == '-') && text[cursor + 3] == ':')
		{
			const int offset_hour = DecimalField(text, cursor + 1, 2), offset_minute = DecimalField(text, cursor + 4, 2);
			if (offset_hour < 0 || offset_hour > 23 || offset_minute < 0 || offset_minute > 59) return std::nullopt;
			offset_minutes = (offset_hour * 60 + offset_minute) * (text[cursor] == '+' ? 1 : -1);
		}
		else return std::nullopt;
		const std::int64_t days = std::chrono::sys_days{date}.time_since_epoch().count();
		const std::int64_t result = days * 86400000 + (hour * 3600LL + minute * 60LL + second - offset_minutes * 60LL) * 1000 + milliseconds;
		return result >= 0 ? std::optional<std::int64_t>{result} : std::nullopt;
	}

	namespace detail
	{
		inline std::optional<std::int64_t> Count(const nlohmann::json& object, std::string_view key)
		{
			const nlohmann::json* value = nlohmann_json::FindField(object, key);
			if (!value || value->is_null()) return 0;
			if (!value->is_number_integer()) return std::nullopt;
			if (value->is_number_unsigned())
			{
				const std::uint64_t count = value->get<std::uint64_t>();
				return count <= static_cast<std::uint64_t>(kMaxExactTokenCount) ? std::optional<std::int64_t>{static_cast<std::int64_t>(count)} : std::nullopt;
			}
			const std::int64_t count = value->get<std::int64_t>();
			return count >= 0 && count <= kMaxExactTokenCount ? std::optional<std::int64_t>{count} : std::nullopt;
		}

		inline std::string Text(const nlohmann::json& object, std::string_view key)
		{
			const std::string_view value = nlohmann_json::StringViewOrEmpty(object, key);
			return value.size() <= 512 ? std::string(value) : std::string{};
		}

		inline std::optional<TokenCounts> Tokens(const nlohmann::json& usage, bool codex)
		{
			if (!usage.is_object()) return std::nullopt;
			const std::optional<std::int64_t> input = Count(usage, "input_tokens");
			const std::optional<std::int64_t> cached = Count(usage, codex ? "cached_input_tokens" : "cache_read_input_tokens");
			const std::optional<std::int64_t> written = Count(usage, codex ? "cache_write_input_tokens" : "cache_creation_input_tokens");
			const std::optional<std::int64_t> output = Count(usage, "output_tokens");
			const std::optional<std::int64_t> reasoning = codex ? Count(usage, "reasoning_output_tokens") : std::optional<std::int64_t>{0};
			if (!input || !cached || !written || !output || !reasoning || *reasoning > *output) return std::nullopt;
			if (codex && *cached + *written > *input) return std::nullopt;
			const TokenCounts tokens{codex ? *input - *cached - *written : *input, *cached, *written, *output, *reasoning};
			if (tokens.Total() <= 0 || tokens.Total() > kMaxExactTokenCount) return std::nullopt;
			return tokens;
		}
	}

	/// <summary>Normalize Claude transcript usage; callers deduplicate message/request identities across files.</summary>
	std::optional<Record> ClaudeRecord(const nlohmann::json& event)
	{
		if (detail::Text(event, "type") != "assistant") return std::nullopt;
		const nlohmann::json* message = nlohmann_json::FindObjectField(event, "message");
		if (!message) return std::nullopt;
		const nlohmann::json* usage = nlohmann_json::FindObjectField(*message, "usage");
		if (!usage) return std::nullopt;
		const std::optional<TokenCounts> tokens = detail::Tokens(*usage, false);
		const std::optional<std::int64_t> timestamp = TimestampMilliseconds(detail::Text(event, "timestamp"));
		const std::string model = detail::Text(*message, "model");
		if (!tokens || !timestamp || model.empty()) return std::nullopt;
		Record record;
		record.provider_id = provider_ids::kClaudeCli;
		record.model_id = model;
		record.session_id = detail::Text(event, "sessionId");
		record.timestamp_ms = *timestamp;
		record.tokens = *tokens;
		record.speed = detail::Text(*usage, "speed") == "fast" ? "fast" : "standard";
		const std::string message_id = detail::Text(*message, "id"), request_id = detail::Text(event, "requestId");
		if (!message_id.empty() || !request_id.empty()) record.dedupe_key = nlohmann::json::array({message_id, request_id}).dump();
		const nlohmann::json* cost = nlohmann_json::FindField(event, "costUSD");
		if (cost && cost->is_number())
		{
			const double amount = cost->get<double>();
			if (std::isfinite(amount) && amount >= 0) record.reported_cost_usd = amount;
		}
		return record;
	}

	std::optional<Record> GeminiRecord(const nlohmann::json& message, std::string_view session_id)
	{
		if (detail::Text(message, "type") != "gemini" || session_id.empty() || session_id.size() > 512) return std::nullopt;
		const nlohmann::json* usage = nlohmann_json::FindObjectField(message, "tokens");
		if (!usage || !usage->contains("input") || !usage->contains("output")) return std::nullopt;
		const std::optional<std::int64_t> input = detail::Count(*usage, "input"), cached = detail::Count(*usage, "cached");
		const std::optional<std::int64_t> output = detail::Count(*usage, "output"), thoughts = detail::Count(*usage, "thoughts"), tool = detail::Count(*usage, "tool"), total = detail::Count(*usage, "total");
		const std::optional<std::int64_t> timestamp = TimestampMilliseconds(detail::Text(message, "timestamp"));
		const std::string model = detail::Text(message, "model"), identity = detail::Text(message, "id");
		if (!input || !cached || !output || !thoughts || !tool || !total || *cached > *input || !timestamp || model.empty()) return std::nullopt;
		const TokenCounts tokens{*input - *cached + *tool, *cached, 0, *output + *thoughts, *thoughts};
		if (tokens.Total() <= 0 || tokens.Total() > kMaxExactTokenCount || (*total > 0 && *total != tokens.Total())) return std::nullopt;
		Record record;
		record.provider_id = provider_ids::kGeminiCli;
		record.model_id = model;
		record.session_id = session_id;
		record.timestamp_ms = *timestamp;
		record.tokens = tokens;
		if (!identity.empty()) record.dedupe_key = nlohmann::json::array({session_id, identity}).dump();
		return record;
	}

	std::vector<Record> CopilotRecords(const nlohmann::json& event, std::string_view session_id, CopilotScanState& state)
	{
		const std::string type = detail::Text(event, "type");
		const nlohmann::json* data = nlohmann_json::FindObjectField(event, "data");
		if (!data) return {};
		if (type == "session.start")
		{
			state.is_subagent = state.is_subagent || !detail::Text(*data, "detachedFromSpawningParentSessionId").empty();
			return {};
		}
		if (state.is_subagent || !detail::Text(event, "agentId").empty() || session_id.empty() || session_id.size() > 512 ||
		    (type != "session.shutdown" && type != "session.usage_checkpoint")) return {};
		const std::function<std::vector<Record>()> reject = [&state]() { ++state.rejected_snapshots; return std::vector<Record>{}; };
		const std::optional<std::int64_t> timestamp = TimestampMilliseconds(detail::Text(event, "timestamp"));
		if (!timestamp) return reject();
		const nlohmann::json* snapshot = nlohmann_json::FindObjectField(*data, "accountingSnapshot");
		if (!snapshot) snapshot = data;
		const nlohmann::json* models = nlohmann_json::FindObjectField(*snapshot, "modelMetrics");
		if (!models || models->size() > 200) return reject();
		std::vector<Record> records;
		std::map<std::string, TokenCounts> next_totals = state.model_totals;
		for (nlohmann::json::const_iterator model = models->begin(); model != models->end(); ++model)
		{
			if (model.key().empty() || model.key().size() > 512) return reject();
			const nlohmann::json* usage = nlohmann_json::FindObjectField(model.value(), "usage");
			if (!usage || !usage->contains("inputTokens") || !usage->contains("outputTokens") || !usage->contains("cacheReadTokens") || !usage->contains("cacheWriteTokens")) return reject();
			for (const char* key : {"inputTokens", "outputTokens", "cacheReadTokens", "cacheWriteTokens", "reasoningTokens"})
			{
				const nlohmann::json* value = nlohmann_json::FindField(*usage, key);
				if (value && !value->is_number_integer()) return reject();
			}
			const std::optional<std::int64_t> input = detail::Count(*usage, "inputTokens"), cached = detail::Count(*usage, "cacheReadTokens");
			const std::optional<std::int64_t> written = detail::Count(*usage, "cacheWriteTokens"), output = detail::Count(*usage, "outputTokens"), reasoning = detail::Count(*usage, "reasoningTokens");
			if (!input || !cached || !written || !output || !reasoning || *cached + *written > *input || *reasoning > *output) return reject();
			const TokenCounts cumulative{*input - *cached - *written, *cached, *written, *output, *reasoning};
			if (cumulative.Total() > kMaxExactTokenCount) return reject();
			const TokenCounts previous = next_totals.contains(model.key()) ? next_totals.at(model.key()) : TokenCounts{};
			const TokenCounts delta{cumulative.input - previous.input, cumulative.cached_input - previous.cached_input,
			    cumulative.cache_write - previous.cache_write, cumulative.output - previous.output, cumulative.reasoning - previous.reasoning};
			// Durable checkpoints subsume previous usage. Reject regressions rather than treating restored totals as a fresh bill.
			if (delta.input < 0 || delta.cached_input < 0 || delta.cache_write < 0 || delta.output < 0 || delta.reasoning < 0 || delta.reasoning > delta.output) return reject();
			if (delta.Total() == 0) continue;
			Record record;
			record.provider_id = provider_ids::kCopilotCli;
			record.model_id = model.key(); record.session_id = session_id; record.timestamp_ms = *timestamp; record.tokens = delta;
			record.dedupe_key = nlohmann::json::array({session_id, model.key(), cumulative.input, cumulative.cached_input, cumulative.cache_write, cumulative.output, cumulative.reasoning}).dump();
			records.push_back(std::move(record)); next_totals[model.key()] = cumulative;
		}
		if (next_totals.size() > 200) return reject();
		state.model_totals = std::move(next_totals);
		return records;
	}

	/// <summary>Track model/tier changes in a Codex rollout and emit per-turn usage, never cumulative totals.</summary>
	std::optional<Record> CodexRecord(const nlohmann::json& event, CodexScanState& state)
	{
		const nlohmann::json* payload = nlohmann_json::FindObjectField(event, "payload");
		if (!payload) return std::nullopt;
		const std::string type = detail::Text(event, "type");
		if (type == "session_meta")
		{
			if (state.saw_session_meta) return std::nullopt;
			state.saw_session_meta = true;
			state.session_id = detail::Text(*payload, "id");
			if (state.session_id.empty()) state.session_id = detail::Text(*payload, "session_id");
			bool forked = !detail::Text(*payload, "forked_from_id").empty();
			const nlohmann::json* source = nlohmann_json::FindObjectField(*payload, "source");
			const nlohmann::json* subagent = source ? nlohmann_json::FindObjectField(*source, "subagent") : nullptr;
			const nlohmann::json* spawn = subagent ? nlohmann_json::FindObjectField(*subagent, "thread_spawn") : nullptr;
			forked = forked || (spawn && !detail::Text(*spawn, "parent_thread_id").empty());
			const std::optional<std::int64_t> timestamp = TimestampMilliseconds(detail::Text(event, "timestamp"));
			state.suppressing_fork_copies = forked && timestamp.has_value();
			if (timestamp) state.fork_anchor_ms = *timestamp;
			return std::nullopt;
		}
		if (type == "turn_context")
		{
			const std::string model = detail::Text(*payload, "model");
			if (!model.empty()) state.model_id = model;
			return std::nullopt;
		}
		if (detail::Text(*payload, "type") == "thread_settings_applied")
		{
			const nlohmann::json* settings = nlohmann_json::FindObjectField(*payload, "thread_settings");
			const std::string tier = settings ? detail::Text(*settings, "service_tier") : std::string{};
			state.speed = tier == "ultrafast" ? "ultrafast" : tier == "priority" || tier == "fast" ? "fast" : "standard";
			return std::nullopt;
		}
		if (detail::Text(*payload, "type") != "token_count") return std::nullopt;
		const nlohmann::json* info = nlohmann_json::FindObjectField(*payload, "info");
		const nlohmann::json* last = info ? nlohmann_json::FindObjectField(*info, "last_token_usage") : nullptr;
		if (!last || state.model_id.empty()) return std::nullopt;
		const std::optional<TokenCounts> tokens = detail::Tokens(*last, true);
		const std::optional<std::int64_t> timestamp = TimestampMilliseconds(detail::Text(event, "timestamp"));
		if (!tokens || !timestamp) return std::nullopt;
		// Cumulative totals distinguish legitimate turns with identical per-turn counts.
		const nlohmann::json* cumulative = nlohmann_json::FindObjectField(*info, "total_token_usage");
		const std::optional<TokenCounts> cumulative_tokens = cumulative ? detail::Tokens(*cumulative, true) : std::nullopt;
		const std::string signature = cumulative_tokens
		    ? nlohmann::json::array({cumulative_tokens->input, cumulative_tokens->cached_input, cumulative_tokens->cache_write, cumulative_tokens->output, cumulative_tokens->reasoning}).dump()
		    : nlohmann::json::array({*timestamp, tokens->input, tokens->cached_input, tokens->cache_write, tokens->output, tokens->reasoning}).dump();
		if (signature == state.last_signature) return std::nullopt;
		state.last_signature = signature;
		if (state.suppressing_fork_copies)
		{
			// shortcut: copied fork history is re-stamped in a startup burst; replace this heuristic if Codex exposes original event identities.
			if (*timestamp - state.fork_anchor_ms < 1000)
			{
				state.fork_anchor_ms = std::max(state.fork_anchor_ms, *timestamp);
				++state.suppressed_fork_records;
				return std::nullopt;
			}
			state.suppressing_fork_copies = false;
		}
		Record record;
		record.provider_id = provider_ids::kCodexCli;
		record.model_id = state.model_id;
		record.session_id = state.session_id;
		record.speed = state.speed;
		record.timestamp_ms = *timestamp;
		record.tokens = *tokens;
		if (!record.session_id.empty()) record.dedupe_key = nlohmann::json::array({record.session_id, signature}).dump();
		return record;
	}
}
