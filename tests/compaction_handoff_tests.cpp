#include "test_harness.h"
#include "common/chat/conversation_handoff.h"
#include "app/opencode_compaction_sync_service.h"

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

namespace
{
	/// <summary>Public retry assembles wire context; the existing admission callback stops before any process I/O.</summary>
	struct CompactedBranchFixture
	{
		TempDir temp{"uam-compacted-branch"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;

		CompactedBranchFixture()
		{
			app.data_root = temp.root;
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			app.settings.git_auto_worktree = false;
			app.settings.provider_chat_defaults["gemini-cli"].feature_preference = "provider";
			ChatSession chat;
			chat.id = "private-compacted-branch";
			chat.provider_id = "gemini-cli";
			chat.workspace_directory = temp.root.string();
			chat.native_session_id = "private-native-session";
			chat.branch_root_chat_id = "private-source";
			chat.messages = {{.role = MessageRole::User, .content = "OBSOLETE_USER"},
			    {.role = MessageRole::Assistant}, {.role = MessageRole::User, .content = "continue"}};
			app.chats.push_back(std::move(chat));
			std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
			owned->chat_id = app.chats.front().id;
			owned->provider_id = app.chats.front().provider_id;
			owned->session_id = app.chats.front().native_session_id;
			owned->running = owned->session_ready = true;
			session = owned.get();
			app.acp_sessions.push_back(std::move(owned));
			app.opencode_compaction_before_dispatch = [](uam::AcpSessionState&, ChatSession&, std::string&)
			{ return uam::OpenCodeCompactionGateState::Pending; };
		}

		void Retry()
		{
			std::string error;
			UAM_ASSERT(uam::RetryLastAcpPrompt(app, app.chats.front().id, &error));
			UAM_ASSERT(error.empty());
			UAM_ASSERT(session->prompt_request_id == 0);
			UAM_ASSERT(!session->queued_prompt.empty());
		}
	};

	void AppendBlock(Message& message, const std::string& type, const std::string& text)
	{
		message.blocks.push_back({.type = type, .text = text});
		message.content += text;
	}
}

UAM_TEST(CompactedBranchPublicRetryUsesLatestCompletedSummaryOnly)
{
	CompactedBranchFixture fixture;
	Message& assistant = fixture.app.chats.front().messages[1];
	for (int index = 0; index < 24; ++index)
	{
		AppendBlock(assistant, "assistant_text", "OBSOLETE_ASSISTANT_" + std::to_string(index));
		AppendBlock(assistant, "context_compaction", "SUMMARY_" + std::to_string(index) + ":" + std::string(12000, 's'));
	}
	AppendBlock(assistant, "assistant_text", "AFTER_LATEST_SUMMARY");
	const Message original = assistant;
	fixture.Retry();
	const std::string& prompt = fixture.session->queued_prompt;
	UAM_ASSERT(prompt.find("Conversation summary: SUMMARY_23:") != std::string::npos);
	UAM_ASSERT(prompt.find("SUMMARY_22:") == std::string::npos);
	UAM_ASSERT(prompt.find("OBSOLETE_USER") == std::string::npos);
	UAM_ASSERT(prompt.find("OBSOLETE_ASSISTANT") == std::string::npos);
	UAM_ASSERT(prompt.find("AFTER_LATEST_SUMMARY") != std::string::npos);
	const std::size_t authored = prompt.find("</UAM History>\n\ncontinue");
	UAM_ASSERT(authored != std::string::npos);
	UAM_ASSERT_EQ(prompt.find("continue"), authored + std::string("</UAM History>\n\n").size());
	UAM_ASSERT_EQ(prompt.find("continue"), prompt.rfind("continue"));
	UAM_ASSERT(prompt.size() < 14000);
	UAM_ASSERT(fixture.app.chats.front().messages[1] == original);
}

UAM_TEST(CompactedBranchPublicRetryKeepsInterruptedTailAndCapturedUserResources)
{
	CompactedBranchFixture fixture;
	Message& assistant = fixture.app.chats.front().messages[1];
	AppendBlock(assistant, "context_compaction", "COMPLETE_BOUNDARY");
	AppendBlock(assistant, "assistant_text", "AFTER_COMPLETION");
	AppendBlock(assistant, "context_compaction_interrupted", "PARTIAL_NOT_A_BOUNDARY");
	AppendBlock(assistant, "assistant_text", "AFTER_INTERRUPTION");
	Message later{.role = MessageRole::User, .content = "POST_BOUNDARY_USER"};
	later.markdown_store_files = {"private-prior-skill.md"};
	later.markdown_store_prompt_blocks = {"EXACT_PRIOR_SKILL"};
	later.attachments = {{.id = "private-prior-file", .kind = "file", .path = "private-prior.txt"}};
	fixture.app.chats.front().messages.insert(fixture.app.chats.front().messages.end() - 1, later);
	fixture.app.chats.front().messages.back().markdown_store_files = {"private-current-skill.md"};
	fixture.app.chats.front().messages.back().markdown_store_prompt_blocks = {"EXACT_CURRENT_SKILL"};
	const std::vector<Message> original = fixture.app.chats.front().messages;
	fixture.Retry();
	const std::string& prompt = fixture.session->queued_prompt;
	UAM_ASSERT(prompt.find("Conversation summary: COMPLETE_BOUNDARY") != std::string::npos);
	UAM_ASSERT(prompt.find("AFTER_COMPLETION") != std::string::npos);
	UAM_ASSERT(prompt.find("Interrupted conversation summary: PARTIAL_NOT_A_BOUNDARY") != std::string::npos);
	UAM_ASSERT(prompt.find("AFTER_INTERRUPTION") != std::string::npos);
	UAM_ASSERT(prompt.find("POST_BOUNDARY_USER") != std::string::npos);
	UAM_ASSERT(prompt.find("EXACT_PRIOR_SKILL") != std::string::npos);
	UAM_ASSERT(prompt.find("private-prior.txt") != std::string::npos);
	UAM_ASSERT(prompt.find("EXACT_CURRENT_SKILL") != std::string::npos);
	for (std::size_t index = 0; index + 1 < original.size(); ++index)
		UAM_ASSERT(fixture.app.chats.front().messages[index] == original[index]);
	UAM_ASSERT(fixture.app.chats.front().messages.back().content == original.back().content);
	UAM_ASSERT(fixture.app.chats.front().messages.back().markdown_store_prompt_blocks == original.back().markdown_store_prompt_blocks);
}

UAM_TEST(CompactedBranchPublicRetryDoesNotTrustInterruptedOrUnorderedBoundary)
{
	for (const bool unordered : {false, true})
	{
		CompactedBranchFixture fixture;
		Message& assistant = fixture.app.chats.front().messages[1];
		AppendBlock(assistant, "assistant_text", "MUST_KEEP_PRIOR_TEXT");
		AppendBlock(assistant, unordered ? "context_compaction" : "context_compaction_interrupted", "UNTRUSTED_BOUNDARY");
		if (unordered) assistant.content += "UNREPRESENTED_TEXT";
		fixture.Retry();
		UAM_ASSERT(fixture.session->queued_prompt.find("OBSOLETE_USER") != std::string::npos);
		UAM_ASSERT(fixture.session->queued_prompt.find("MUST_KEEP_PRIOR_TEXT") != std::string::npos);
		UAM_ASSERT(fixture.session->queued_prompt.find("UNTRUSTED_BOUNDARY") != std::string::npos);
	}
}
