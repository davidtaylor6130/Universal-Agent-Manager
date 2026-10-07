#include "test_harness.h"

using namespace uam_test;

UAM_TEST(CodexFreshInitializeDropsPreviousAccountLimitsWithoutDroppingTranscriptTokens)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	uam::AppState app;
	ChatSession chat;
	uam::AcpSessionState session;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("codex-cli");
	session.provider_usage.token_usage.available = true;
	session.provider_usage.token_usage.total.total_tokens = 1234;
	session.provider_usage.token_usage.last.input_tokens = 321;
	runtime.OnAcpHandleResult(app, session, chat, "account/rateLimits/read", "1",
	    {{"rateLimits", {{"limitId", "old-account"}, {"planType", "plus"},
	     {"primary", {{"usedPercent", 90}, {"resetsAt", 2000000000}}},
	     {"credits", {{"hasCredits", true}, {"balance", "4.50"}}}}}});
	UAM_ASSERT(session.provider_usage.rate_limits.available);
	const nlohmann::json initialize = runtime.OnAcpBuildInitialize(session, 7);
	UAM_ASSERT_EQ(initialize.value("method", ""), std::string("initialize"));
	// An unavailable/malformed account read cannot resurrect the former process's account.
	UAM_ASSERT(!session.provider_usage.rate_limits.available);
	UAM_ASSERT_EQ(session.provider_usage.rate_limits.updated_at_ms, int64_t{0});
	UAM_ASSERT(session.provider_usage.rate_limits.limit_id.empty());
	UAM_ASSERT(!session.provider_usage.rate_limits.primary.available);
	UAM_ASSERT(!session.provider_usage.rate_limits.credits.available);
	runtime.OnAcpHandleResult(app, session, chat, "account/rateLimits/read", "2", nlohmann::json::object());
	UAM_ASSERT(!session.provider_usage.rate_limits.available);
	UAM_ASSERT(session.provider_usage.token_usage.available);
	UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, int64_t{1234});
	UAM_ASSERT_EQ(session.provider_usage.token_usage.last.input_tokens, int64_t{321});
	runtime.OnAcpHandleResult(app, session, chat, "account/rateLimits/read", "3",
	    {{"rateLimits", {{"limitId", "new-account"}, {"secondary", {{"usedPercent", 20}}}}}});
	UAM_ASSERT(session.provider_usage.rate_limits.available);
	UAM_ASSERT_EQ(session.provider_usage.rate_limits.limit_id, std::string("new-account"));
	UAM_ASSERT_EQ(session.provider_usage.rate_limits.secondary.used_percent, 20);
	UAM_ASSERT(!session.provider_usage.rate_limits.primary.available);
	UAM_ASSERT(session.provider_usage.rate_limits.plan_type.empty());
	UAM_ASSERT(!session.provider_usage.rate_limits.credits.available);
#endif
}
