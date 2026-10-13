#include "test_harness.h"
#include "app/runtime_orchestration_services.h"

#include <nlohmann/json.hpp>

using namespace uam_test;

UAM_TEST(OpenCodeImportAcceptsMoreThanTenThousandMessages)
{
	nlohmann::json exported = {
	    {"info", {{"id", "ses_many_messages"}, {"directory", "/workspace"}}},
	    {"messages", nlohmann::json::array()},
	};
	const nlohmann::json message = {
	    {"info", {{"role", "user"}}},
	    {"parts", nlohmann::json::array({{{"type", "text"}, {"text", "Message"}}})},
	};
	for (std::size_t index = 0; index < 10001; ++index)
	{
		exported["messages"].push_back(message);
	}

	const ChatHistorySyncService::RemoteOpenCodeTranscript parsed =
	    ChatHistorySyncService::ParseRemoteOpenCodeTranscript(exported.dump());
	UAM_ASSERT(parsed.success);
	UAM_ASSERT_EQ(parsed.messages.size(), std::size_t{10001});
}

UAM_TEST(OpenCodeImportReportsInvalidRootAndMessagesSeparately)
{
	const ChatHistorySyncService::RemoteOpenCodeTranscript invalid_root =
	    ChatHistorySyncService::ParseRemoteOpenCodeTranscript(R"({"unexpected":true})");
	UAM_ASSERT(!invalid_root.success);
	UAM_ASSERT_EQ(invalid_root.error, std::string("OpenCode export is missing valid session information."));

	const ChatHistorySyncService::RemoteOpenCodeTranscript invalid_messages =
	    ChatHistorySyncService::ParseRemoteOpenCodeTranscript(
	        R"({"info":{"id":"ses_invalid_messages","directory":"/workspace"},"messages":{}})");
	UAM_ASSERT(!invalid_messages.success);
	UAM_ASSERT_EQ(invalid_messages.error, std::string("OpenCode export is missing a valid messages array."));
}
