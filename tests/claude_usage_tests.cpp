#include "test_harness.h"
#include "common/usage/transcript_usage.h"
#include "common/runtime/acp/acp_polling.h"

#include <limits>

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
namespace
{
	void ConsumeClaudeUsage(uam::AcpSessionState& session, const nlohmann::json& message)
	{
		TempDir temp("uam-claude-usage-dispatch");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = session.chat_id.empty() ? "claude-usage-dispatch" : session.chat_id;
		chat.provider_id = "claude-cli";
		chat.workspace_directory = temp.root.string();
		app.chats.push_back(chat);
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = "claude-code-stream-json";
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), message.dump()));
	}

	nlohmann::json ClaudeUsageResult(const std::string& session_id = "claude-usage-session")
	{
		return {{"type", "result"}, {"session_id", session_id}, {"subtype", "success"},
		    {"modelUsage", {{"main", {{"inputTokens", 100}, {"cacheReadInputTokens", 20}, {"cacheCreationInputTokens", 5}, {"outputTokens", 10}}},
		        {"child", {{"inputTokens", 20}, {"cacheReadInputTokens", 2}, {"cacheCreationInputTokens", 1}, {"outputTokens", 3}}}}},
		    {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}};
	}
}

UAM_TEST(ClaudeUsageSnapshotsCountWholeTreeCachesWithoutAddingCumulativeResults)
{
	uam::AcpSessionState session;
	session.session_id = "claude-usage-session";
	nlohmann::json event = ClaudeUsageResult();
	ConsumeClaudeUsage(session, event);
	const uam::AcpTokenUsageState& usage = session.provider_usage.token_usage;
	UAM_ASSERT(usage.available);
	UAM_ASSERT(!usage.last_available);
	UAM_ASSERT_EQ(usage.total.input_tokens, std::int64_t{148});
	UAM_ASSERT_EQ(usage.total.cached_input_tokens, std::int64_t{22});
	UAM_ASSERT_EQ(usage.total.cache_write_input_tokens, std::int64_t{6});
	UAM_ASSERT_EQ(usage.total.output_tokens, std::int64_t{13});
	UAM_ASSERT_EQ(usage.total.total_tokens, std::int64_t{161});
	ConsumeClaudeUsage(session, event);
	UAM_ASSERT_EQ(usage.total.total_tokens, std::int64_t{161});
	event["modelUsage"]["main"]["inputTokens"] = 104;
	ConsumeClaudeUsage(session, event);
	UAM_ASSERT_EQ(usage.total.total_tokens, std::int64_t{165});
	UAM_ASSERT(!usage.last_available);
}

UAM_TEST(ClaudeUsageRejectsMalformedCountersAndPreservesKnownCrashTotals)
{
	uam::AcpSessionState session;
	session.session_id = "claude-usage-session";
	ConsumeClaudeUsage(session, ClaudeUsageResult());
	for (const nlohmann::json& value : std::vector<nlohmann::json>{-1, 1.5, true, uam::usage::kMaxExactTokenCount + 1, std::numeric_limits<std::uint64_t>::max()})
	{
		nlohmann::json event = ClaudeUsageResult();
		event["modelUsage"]["main"]["inputTokens"] = value;
		ConsumeClaudeUsage(session, event);
		UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, std::int64_t{161});
	}
	nlohmann::json event = ClaudeUsageResult();
	event["modelUsage"]["main"]["inputTokens"] = uam::usage::kMaxExactTokenCount;
	ConsumeClaudeUsage(session, event);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, std::int64_t{161});
	event = ClaudeUsageResult();
	event["modelUsage"]["main"].erase("outputTokens");
	ConsumeClaudeUsage(session, event);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, std::int64_t{161});
	event = ClaudeUsageResult();
	event["modelUsage"] = nlohmann::json::object();
	for (int index = 0; index < 129; ++index) event["modelUsage"][std::to_string(index)] = ClaudeUsageResult()["modelUsage"]["main"];
	ConsumeClaudeUsage(session, event);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, std::int64_t{161});
	event = ClaudeUsageResult();
	for (nlohmann::json& model : event["modelUsage"]) for (nlohmann::json& count : model) count = 0;
	event["is_error"] = true;
	ConsumeClaudeUsage(session, event);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, std::int64_t{161});
	event["is_error"] = false;
	ConsumeClaudeUsage(session, event);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, std::int64_t{0});
}

UAM_TEST(ClaudeUsageScopesResumedTotalsAndSerializesUnknownLastTurn)
{
	TempDir temp("uam-claude-usage-wire");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "claude-usage-chat";
	chat.provider_id = "claude-cli";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);
	std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
	uam::AcpSessionState& session = *owned;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "claude-code-stream-json";
	session.running = true;
	session.session_ready = true;
	session.processing = true;
	session.session_id = "claude-usage-session";
	app.acp_sessions.push_back(std::move(owned));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), ClaudeUsageResult().dump()));
	const nlohmann::json serialized = uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["providerUsage"]["tokenUsage"];
	UAM_ASSERT_EQ(serialized["total"]["totalTokens"], nlohmann::json(161));
	UAM_ASSERT(serialized["last"].is_null());
	nlohmann::json child = ClaudeUsageResult();
	child["parent_tool_use_id"] = "child-tool";
	child["modelUsage"]["main"]["inputTokens"] = 500;
	ConsumeClaudeUsage(session, child);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, std::int64_t{161});
	ConsumeClaudeUsage(session, ClaudeUsageResult("resumed-session"));
	UAM_ASSERT_EQ(session.provider_usage.token_usage.session_id, std::string("resumed-session"));
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, std::int64_t{161});
	session.session_id = "123";
	nlohmann::json malformed_scope = ClaudeUsageResult();
	malformed_scope["session_id"] = 123;
	ConsumeClaudeUsage(session, malformed_scope);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, std::int64_t{161});
	session.provider_usage.rate_limits.available = true;
	ConsumeClaudeUsage(session, {{"type", "system"}, {"subtype", "init"}, {"session_id", "reset-session"}});
	UAM_ASSERT(!session.provider_usage.token_usage.available);
	UAM_ASSERT(!session.provider_usage.rate_limits.available);
	const nlohmann::json reset_serialized = uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"];
	UAM_ASSERT(reset_serialized.is_object());
	UAM_ASSERT(reset_serialized["providerUsage"].is_null());
	nlohmann::json reset = ClaudeUsageResult("reset-session");
	reset.erase("modelUsage");
	ConsumeClaudeUsage(session, reset);
	UAM_ASSERT(!session.provider_usage.token_usage.available);
	ConsumeClaudeUsage(session, ClaudeUsageResult("reset-session"));
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, std::int64_t{161});
	UAM_ASSERT(!session.provider_usage.token_usage.last_available);
}

#endif
