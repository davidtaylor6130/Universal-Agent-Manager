#include "test_harness.h"
#include "common/chat/conversation_handoff.h"

using namespace uam_test;

UAM_TEST(CompactionHandoffDoesNotDuplicateReclassifiedSummary)
{
	ChatSession chat;
	Message message;
	message.role = MessageRole::Assistant;
	message.content = "Keep the workspace.Keep the workspace.";
	message.blocks = {{"context_compaction", "Keep the workspace.", "", "native-summary"},
	    {"thought", "Do not copy reasoning", "", ""},
	    {"assistant_text", "Keep the workspace.", "", ""}};
	chat.messages.push_back(message);
	std::string context;
	UAM_ASSERT(uam::chat::BuildConversationHandoff(chat, context));
	UAM_ASSERT_EQ(context, std::string("Conversation summary: Keep the workspace.\n\nAssistant: Keep the workspace.\n\n"));
	UAM_ASSERT_EQ(chat.messages.front(), message);
	UAM_ASSERT_EQ(uam::BuildProviderHandoffContext(chat), context);
}

UAM_TEST(CompactionHandoffPreservesOrdinaryContentAndSizeGuard)
{
	ChatSession chat;
	Message message;
	message.role = MessageRole::Assistant;
	message.content = "Ordinary saved response";
	chat.messages.push_back(message);
	std::string context;
	UAM_ASSERT(uam::chat::BuildConversationHandoff(chat, context));
	UAM_ASSERT_EQ(context, std::string("Assistant: Ordinary saved response\n\n"));
	chat.messages.front().blocks = {{"context_compaction", "A separate summary", "", "native-summary"}};
	UAM_ASSERT(uam::chat::BuildConversationHandoff(chat, context));
	UAM_ASSERT_EQ(context, std::string("Conversation summary: A separate summary\n\nAssistant: Ordinary saved response\n\n"));
	UAM_ASSERT_EQ(uam::BuildProviderHandoffContext(chat), context);
	chat.messages.front().blocks = {{"context_compaction", std::string(256 * 1024, 'x'), "", "native-summary"}};
	std::string error;
	UAM_ASSERT(!uam::chat::BuildConversationHandoff(chat, context, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(context, std::string("Conversation summary: A separate summary\n\nAssistant: Ordinary saved response\n\n"));
}

UAM_TEST(CompactionHandoffRetainsInterleavedTimelineOrder)
{
	for (const bool raw_content_includes_summary : {false, true})
	{
		ChatSession chat;
		Message message;
		message.role = MessageRole::Assistant;
		message.content = raw_content_includes_summary ? "BeforeSummaryAfter" : "BeforeAfter";
		message.blocks = {{"assistant_text", "Before", "", ""},
		    {"context_compaction", "Summary", "", "native-summary"}, {"assistant_text", "After", "", ""}};
		chat.messages.push_back(message);
		std::string context;
		UAM_ASSERT(uam::chat::BuildConversationHandoff(chat, context));
		UAM_ASSERT_EQ(context, std::string("Assistant: Before\n\nConversation summary: Summary\n\nAssistant: After\n\n"));
		UAM_ASSERT_EQ(uam::BuildProviderHandoffContext(chat), context);
	}
}
