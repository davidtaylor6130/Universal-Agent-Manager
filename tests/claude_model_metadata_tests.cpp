#include "test_harness.h"

using namespace uam_test;

UAM_TEST(ClaudeDiscoveryPreservesOnlyReportedModelTokenCapacities)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	uam::AcpSessionState session;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli");
	runtime.OnAcpInitializeResult(session, {{"models", {{{"value", "fixture-claude"}, {"contextWindow", 200000}, {"maxOutputTokens", 64000}}, {{"value", "unknown-claude"}}, {{"value", "invalid-claude"}, {"contextWindow", -1}, {"maxOutputTokens", "64000"}}}}});
	UAM_ASSERT_EQ(session.available_models.size(), std::size_t{3});
	UAM_ASSERT_EQ(session.available_models[0].context_window_tokens.value_or(-1), std::int64_t{200000});
	UAM_ASSERT_EQ(session.available_models[0].max_output_tokens.value_or(-1), std::int64_t{64000});
	UAM_ASSERT(!session.available_models[1].context_window_tokens.has_value());
	UAM_ASSERT(!session.available_models[1].max_output_tokens.has_value());
	UAM_ASSERT(!session.available_models[2].context_window_tokens.has_value());
	UAM_ASSERT(!session.available_models[2].max_output_tokens.has_value());
#endif
}
