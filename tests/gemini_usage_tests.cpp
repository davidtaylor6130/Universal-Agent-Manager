#include "test_harness.h"
#include "common/usage/transcript_usage.h"

using namespace uam_test;

namespace
{
	void ObserveGeminiUsage(uam::AcpSessionState& session, const nlohmann::json& result)
	{
		uam::AppState app;
		ChatSession chat;
		UAM_ASSERT(!ProviderRuntimeRegistry::ResolveById("gemini-cli").OnAcpHandleResult(app, session, chat, "session/prompt", "owned", result));
	}
}

UAM_TEST(GeminiPromptUsageNormalizesReasoningAndCacheWithoutDoubleCounting)
{
	uam::AcpSessionState session;
	session.session_id = "native";
	const nlohmann::json reply = {{"usage", {{"inputTokens", 100}, {"outputTokens", 20}, {"totalTokens", 120}, {"cachedReadTokens", 70}, {"thoughtTokens", 5}}}};
	ObserveGeminiUsage(session, reply);
	const uam::AcpTokenUsageState& usage = session.provider_usage.token_usage;
	UAM_ASSERT(usage.available && usage.last_available);
	UAM_ASSERT_EQ(usage.total.total_tokens, 125LL);
	UAM_ASSERT_EQ(usage.total.input_tokens, 100LL);
	UAM_ASSERT_EQ(usage.total.cached_input_tokens, 70LL);
	UAM_ASSERT_EQ(usage.total.output_tokens, 25LL);
	UAM_ASSERT_EQ(usage.total.reasoning_output_tokens, 5LL);
	ObserveGeminiUsage(session, {{"usage", {{"inputTokens", 2}, {"outputTokens", 3}, {"totalTokens", 5}}}});
	UAM_ASSERT_EQ(usage.total.total_tokens, 130LL);
	UAM_ASSERT_EQ(usage.last.total_tokens, 5LL);
	ObserveGeminiUsage(session, {{"usage", {{"inputTokens", 0}, {"outputTokens", 0}, {"totalTokens", 0}}}});
	UAM_ASSERT(usage.last_available);
	UAM_ASSERT_EQ(usage.last.total_tokens, 0LL);
	UAM_ASSERT_EQ(usage.total.total_tokens, 130LL);
}

UAM_TEST(GeminiPromptUsageRejectsMalformedCountsAndResetsNativeSessionScope)
{
	uam::AcpSessionState session;
	session.session_id = "native";
	const nlohmann::json reply = {{"usage", {{"inputTokens", 10}, {"outputTokens", 2}, {"totalTokens", 12}}}};
	ObserveGeminiUsage(session, reply);
	for (const nlohmann::json bad : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json(nullptr), nlohmann::json("10"), nlohmann::json(18446744073709551615ULL)})
	{
		nlohmann::json malformed = reply;
		malformed["usage"]["inputTokens"] = bad;
		ObserveGeminiUsage(session, malformed);
		UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, 12LL);
		UAM_ASSERT(!session.provider_usage.token_usage.last_available);
	}
	for (const nlohmann::json bad : {nlohmann::json{{"cachedReadTokens", 11}}, nlohmann::json{{"totalTokens", 99}}, nlohmann::json{{"thoughtTokens", uam::usage::kMaxExactTokenCount}}})
	{
		nlohmann::json malformed = reply;
		malformed["usage"].update(bad);
		ObserveGeminiUsage(session, malformed);
		UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, 12LL);
	}
	session.provider_usage.token_usage.total.total_tokens = uam::usage::kMaxExactTokenCount;
	ObserveGeminiUsage(session, reply);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, uam::usage::kMaxExactTokenCount);
	session.session_id = "other";
	ObserveGeminiUsage(session, nlohmann::json::object());
	UAM_ASSERT(!session.provider_usage.token_usage.available);
	ObserveGeminiUsage(session, {{"_meta", {{"quota", {{"token_count", {{"input_tokens", 4}, {"output_tokens", 3}}}}}}}});
	UAM_ASSERT_EQ(session.provider_usage.token_usage.session_id, std::string("other"));
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, 7LL);
	// Standard and legacy copies in the same response describe the same work.
	nlohmann::json both = reply;
	both["_meta"] = {{"quota", {{"token_count", {{"input_tokens", 10}, {"output_tokens", 2}}}}}};
	ObserveGeminiUsage(session, both);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, 19LL);
}

UAM_TEST(GeminiPromptUsageOnlyConsumesCorrelatedSuccessfulPromptReplies)
{
	TempDir temp("uam-gemini-live-usage");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat";
	chat.provider_id = "gemini-cli";
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
