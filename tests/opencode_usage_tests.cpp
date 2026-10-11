#include "test_harness.h"
#include "common/usage/transcript_usage.h"

using namespace uam_test;

namespace
{
	void ObserveOpenCodeUsage(uam::AcpSessionState& session, const nlohmann::json& result)
	{
		uam::AppState app;
		ChatSession chat;
		UAM_ASSERT(!ProviderRuntimeRegistry::ResolveById("opencode-cli").OnAcpHandleResult(app, session, chat, "session/prompt", "owned", result));
	}
}

UAM_TEST(OpenCodePromptUsageNormalizesDisjointBucketsAcrossToolSteps)
{
	uam::AcpSessionState session;
	session.session_id = "native";
	// Upstream's two-step fixture sums ordinary input/output separately from cache and reasoning.
	ObserveOpenCodeUsage(session, {{"usage", {{"inputTokens", 120}, {"outputTokens", 15}, {"thoughtTokens", 3}, {"cachedReadTokens", 150}, {"cachedWriteTokens", 50}, {"totalTokens", 338}}}});
	const uam::AcpTokenUsageState& usage = session.provider_usage.token_usage;
	UAM_ASSERT(usage.available && usage.last_available);
	UAM_ASSERT_EQ(usage.total.input_tokens, 320LL);
	UAM_ASSERT_EQ(usage.total.cached_input_tokens, 150LL);
	UAM_ASSERT_EQ(usage.total.cache_write_input_tokens, 50LL);
	UAM_ASSERT_EQ(usage.total.output_tokens, 18LL);
	UAM_ASSERT_EQ(usage.total.reasoning_output_tokens, 3LL);
	UAM_ASSERT_EQ(usage.total.total_tokens, 338LL);
	ObserveOpenCodeUsage(session, {{"usage", {{"inputTokens", 2}, {"outputTokens", 3}, {"totalTokens", 5}}}});
	UAM_ASSERT_EQ(usage.total.total_tokens, 343LL);
	UAM_ASSERT_EQ(usage.last.total_tokens, 5LL);
	ObserveOpenCodeUsage(session, {{"usage", {{"inputTokens", 0}, {"outputTokens", 0}, {"totalTokens", 0}}}});
	UAM_ASSERT(usage.last_available);
	UAM_ASSERT_EQ(usage.last.total_tokens, 0LL);
	UAM_ASSERT_EQ(usage.total.total_tokens, 343LL);
}

UAM_TEST(OpenCodePromptUsageRejectsMalformedCountsAndResetsNativeSessionScope)
{
	uam::AcpSessionState session;
	session.session_id = "native";
	const nlohmann::json reply = {{"usage", {{"inputTokens", 10}, {"outputTokens", 2}, {"totalTokens", 12}}}};
	ObserveOpenCodeUsage(session, reply);
	for (const nlohmann::json bad : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json(nullptr), nlohmann::json("10"), nlohmann::json(18446744073709551615ULL)})
	{
		nlohmann::json malformed = reply;
		malformed["usage"]["inputTokens"] = bad;
		ObserveOpenCodeUsage(session, malformed);
		UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, 12LL);
		UAM_ASSERT(!session.provider_usage.token_usage.last_available);
	}
	for (const nlohmann::json bad : {nlohmann::json{{"cachedReadTokens", -1}}, nlohmann::json{{"totalTokens", 99}}, nlohmann::json{{"thoughtTokens", uam::usage::kMaxExactTokenCount}}})
	{
		nlohmann::json malformed = reply;
		malformed["usage"].update(bad);
		ObserveOpenCodeUsage(session, malformed);
		UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, 12LL);
	}
	session.provider_usage.token_usage.total.total_tokens = uam::usage::kMaxExactTokenCount;
	ObserveOpenCodeUsage(session, reply);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, uam::usage::kMaxExactTokenCount);
	session.session_id = "other";
	ObserveOpenCodeUsage(session, nlohmann::json::object());
	UAM_ASSERT(!session.provider_usage.token_usage.available);
	ObserveOpenCodeUsage(session, reply);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.session_id, std::string("other"));
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, 12LL);
	ObserveOpenCodeUsage(session, {{"usage", {{"inputTokens", uam::usage::kMaxExactTokenCount}, {"cachedWriteTokens", 1}, {"outputTokens", 0}, {"totalTokens", uam::usage::kMaxExactTokenCount}}}});
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, 12LL);
	UAM_ASSERT(!session.provider_usage.token_usage.last_available);
}

UAM_TEST(OpenCodePromptUsageOnlyConsumesCorrelatedSuccessfulPromptReplies)
{
	TempDir temp("uam-opencode-live-usage");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat";
	chat.provider_id = "opencode-cli";
	app.chats.push_back(chat);
	std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = chat.provider_id;
	session->session_id = "native";
	session->prompt_request_id = 42;
	session->pending_request_methods[42] = "session/prompt";
	uam::AcpSessionState* raw = session.get();
	app.acp_sessions.push_back(std::move(session));
	const std::string reply = R"({"jsonrpc":"2.0","id":42,"result":{"stopReason":"end_turn","usage":{"inputTokens":10,"outputTokens":2,"totalTokens":12}}})";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw, app.chats.front(), reply));
	UAM_ASSERT_EQ(raw->provider_usage.token_usage.total.total_tokens, 12LL);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw, app.chats.front(), reply));
	UAM_ASSERT_EQ(raw->provider_usage.token_usage.total.total_tokens, 12LL);
	raw->pending_request_methods[43] = "session/set_model";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw, app.chats.front(), R"({"jsonrpc":"2.0","id":43,"result":{"usage":{"inputTokens":10,"outputTokens":2,"totalTokens":12}}})"));
	UAM_ASSERT_EQ(raw->provider_usage.token_usage.total.total_tokens, 12LL);
}
