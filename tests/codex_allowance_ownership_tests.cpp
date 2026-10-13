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

UAM_TEST(CodexRateLimitWindowMergesPartialUpdatesByField)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	uam::AppState app;
	ChatSession chat;
	uam::AcpSessionState session;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("codex-cli");
	const auto update_limits = [&](const nlohmann::json& params)
	{
		UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat,
		    {{"method", "account/rateLimits/updated"}, {"params", params}}, nullptr));
	};
	const uam::AcpRateLimitWindowState& secondary = session.provider_usage.rate_limits.secondary;
	update_limits(
	    {{"rateLimits", {{"secondary", {{"resetsAt", 1000}, {"windowDurationMins", 300}}}}}});
	UAM_ASSERT(!secondary.available);
	UAM_ASSERT(!session.provider_usage.rate_limits.available);
	UAM_ASSERT_EQ(secondary.resets_at, int64_t{1000});
	UAM_ASSERT_EQ(secondary.window_duration_minutes, int64_t{300});
	update_limits(
	    {{"rateLimits", {{"secondary", {{"resetsAt", nullptr}, {"windowDurationMins", nullptr}}}}}});
	UAM_ASSERT(!secondary.available);
	UAM_ASSERT_EQ(secondary.resets_at, int64_t{0});
	UAM_ASSERT_EQ(secondary.window_duration_minutes, int64_t{0});
	update_limits(
	    {{"rateLimits", {{"secondary", {{"usedPercent", "bad"}, {"resetsAt", 2000}}}}}});
	UAM_ASSERT(!secondary.available);
	UAM_ASSERT_EQ(secondary.resets_at, int64_t{2000});
	update_limits(
	    {{"rateLimits", {{"secondary", {{"usedPercent", 20}}}}}});
	UAM_ASSERT(secondary.available);
	UAM_ASSERT_EQ(secondary.used_percent, 20);
	UAM_ASSERT_EQ(secondary.resets_at, int64_t{2000});
	update_limits(
	    {{"rateLimits", {{"primary", {{"usedPercent", 42}, {"resetsAt", 1000}, {"windowDurationMins", 300}}}}}});
	const uam::AcpRateLimitWindowState& primary = session.provider_usage.rate_limits.primary;
	UAM_ASSERT(primary.available);
	UAM_ASSERT_EQ(primary.used_percent, 42);
	UAM_ASSERT_EQ(primary.resets_at, int64_t{1000});
	UAM_ASSERT_EQ(primary.window_duration_minutes, int64_t{300});

	update_limits(
	    {{"rateLimits", {{"primary", {{"resetsAt", 2000}}}}}});
	UAM_ASSERT_EQ(primary.used_percent, 42);
	UAM_ASSERT_EQ(primary.resets_at, int64_t{2000});
	UAM_ASSERT_EQ(primary.window_duration_minutes, int64_t{300});

	update_limits(
	    {{"rateLimits", {{"primary", {{"usedPercent", 55}}}}}});
	UAM_ASSERT_EQ(primary.used_percent, 55);
	UAM_ASSERT_EQ(primary.resets_at, int64_t{2000});
	UAM_ASSERT_EQ(primary.window_duration_minutes, int64_t{300});

	update_limits(
	    {{"rateLimits", {{"primary", {{"resetsAt", nullptr}, {"windowDurationMins", nullptr}}}}}});
	UAM_ASSERT(primary.available);
	UAM_ASSERT_EQ(primary.used_percent, 55);
	UAM_ASSERT_EQ(primary.resets_at, int64_t{0});
	UAM_ASSERT_EQ(primary.window_duration_minutes, int64_t{0});

	update_limits(
	    {{"rateLimits", {{"primary", {{"usedPercent", "bad"}, {"resetsAt", -1}, {"windowDurationMins", 1.5}}}}}});
	UAM_ASSERT(primary.available);
	UAM_ASSERT_EQ(primary.used_percent, 55);
	UAM_ASSERT_EQ(primary.resets_at, int64_t{0});
	UAM_ASSERT_EQ(primary.window_duration_minutes, int64_t{0});
#endif
}
