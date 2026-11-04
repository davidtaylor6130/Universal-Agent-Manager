#include "test_harness.h"
#include "cef/uam_query_handler_internal.h"
#include "common/memory/memory_policy.h"

using namespace uam_test;

UAM_TEST(AutomaticRoutingDoesNotInheritSourceAccountModelOrTierDefaults)
{
	AppSettings settings;
	ProviderChatDefaults defaults;
	defaults.model_id = "source-account-model";
	defaults.reviewer_model_id = "source-account-reviewer";
	defaults.reasoning_effort = "high";
	defaults.service_tier = "fast";
	defaults.approval_mode = "plan";
	defaults.command_safety_tier = "acceptEdits";
	defaults.memory_enabled = false;
	settings.provider_chat_defaults["codex-cli"] = defaults;
	for (const bool automatic : {false, true})
	{
		ChatSession chat;
		chat.provider_id = "codex-cli";
		chat.routing_mode = automatic ? "automatic" : "explicit";
		const nlohmann::json payload{{"approvalMode", "plan"}};
		uam::query_handler_internal::ApplyProviderDefaultsToChat(settings, chat, &payload);
		UAM_ASSERT_EQ(chat.model_id, automatic ? std::string{} : defaults.model_id);
		UAM_ASSERT_EQ(chat.reviewer_model_id, automatic ? std::string{} : defaults.reviewer_model_id);
		UAM_ASSERT_EQ(chat.reasoning_effort, automatic ? std::string{} : defaults.reasoning_effort);
		UAM_ASSERT_EQ(chat.service_tier, automatic ? std::string{} : defaults.service_tier);
		UAM_ASSERT_EQ(chat.service_tier_explicit, !automatic);
		UAM_ASSERT_EQ(chat.approval_mode, defaults.approval_mode);
		UAM_ASSERT_EQ(chat.command_safety_tier, defaults.command_safety_tier);
		UAM_ASSERT_EQ(chat.memory_level, std::string(uam::memory_levels::kDefault));
		UAM_ASSERT(chat.memory_enabled);
		UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string(uam::memory_levels::kOff));
	}
}
