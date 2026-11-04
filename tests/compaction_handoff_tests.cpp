#include "test_harness.h"
#include "common/chat/conversation_handoff.h"
#include "app/opencode_compaction_sync_service.h"
#include "app/chat_lifecycle_service.h"

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

		explicit CompactedBranchFixture(const std::string& provider_id = "gemini-cli")
		{
			app.data_root = temp.root;
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.settings.git_enabled = app.settings.git_auto_worktree = false;
			app.settings.central_provider_configuration.enabled = false;
			app.settings.provider_chat_defaults[provider_id].feature_preference = "provider";
			ChatSession chat;
			chat.id = "private-compacted-branch";
			chat.provider_id = provider_id;
			chat.memory_enabled = chat.uam_control_enabled = false;
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

UAM_TEST(OpenCodeInterruptedCompactionRetryUsesCompletedBoundaryAfterReload)
{
	CompactedBranchFixture fixture("opencode-cli");
	ChatSession& chat = fixture.app.chats.front();
	Message& assistant = chat.messages[1];
	for (int index = 0; index < 24; ++index)
	{
		AppendBlock(assistant, "assistant_text", "OBSOLETE_REPLY_" + std::to_string(index));
		AppendBlock(assistant, "context_compaction", "COMPLETED_" + std::to_string(index) + ":" + std::string(12000, 's'));
		assistant.blocks.back().request_id_json = "acp-compaction:msg_completed_" + std::to_string(index);
	}
	AppendBlock(assistant, "assistant_text", "KEEP_AFTER_COMPLETION");
	AppendBlock(assistant, "context_compaction_interrupted", "TYPED_INTERRUPTION");
	assistant.blocks.back().request_id_json = "acp-compaction:msg_interrupted";
	AppendBlock(assistant, "context_compaction_summary", "UNFINISHED_NATIVE_SUMMARY:" + std::string(96000, 'p'));
	assistant.blocks.back().request_id_json = "opencode-message:msg_quarantined";
	assistant.interrupted = true;
	chat.messages.back().native_input_id = "10980000-0000-4000-8000-000000000024";
	chat.messages.back().origin_request_id = "owned-continuation-action";
	chat.messages.back().markdown_store_files = {"captured-continuation.md"};
	chat.messages.back().markdown_store_prompt_blocks = {"EXACT_CONTINUATION_SKILL"};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id);
	UAM_ASSERT(loaded);
	chat = *loaded;
	UAM_ASSERT_EQ(chat.messages.back().native_input_id, std::optional<std::string>{"10980000-0000-4000-8000-000000000024"});
	const std::vector<Message> original = chat.messages;
	fixture.Retry();
	const std::string& prompt = fixture.session->queued_prompt;
	UAM_ASSERT(prompt.find("Conversation summary: COMPLETED_23:") != std::string::npos);
	UAM_ASSERT(prompt.find("COMPLETED_22:") == std::string::npos);
	UAM_ASSERT(prompt.find("OBSOLETE_") == std::string::npos);
	UAM_ASSERT(prompt.find("KEEP_AFTER_COMPLETION") != std::string::npos);
	UAM_ASSERT(prompt.find("Interrupted conversation summary: TYPED_INTERRUPTION") != std::string::npos);
	UAM_ASSERT(prompt.find("UNFINISHED_NATIVE_SUMMARY") == std::string::npos);
	UAM_ASSERT(prompt.find("</UAM History>\n\ncontinue") != std::string::npos);
	UAM_ASSERT(prompt.find("EXACT_CONTINUATION_SKILL") != std::string::npos);
	UAM_ASSERT(prompt.size() < 14000);
	UAM_ASSERT_EQ(chat.native_session_id, std::string("private-native-session"));
	UAM_ASSERT_EQ(fixture.session->session_id, chat.native_session_id);
	UAM_ASSERT_EQ(fixture.session->turn_first_user_message_index, 2);
	UAM_ASSERT_EQ(fixture.session->turn_user_message_index, 2);
	UAM_ASSERT_EQ(fixture.session->turn_assistant_message_index, -1);
	UAM_ASSERT_EQ(chat.messages.size(), original.size());
	UAM_ASSERT_EQ(chat.messages[1], original[1]);
	UAM_ASSERT_EQ(chat.messages.back().content, original.back().content);
	UAM_ASSERT_EQ(chat.messages.back().native_input_id, original.back().native_input_id);
	UAM_ASSERT_EQ(chat.messages.back().origin_request_id, original.back().origin_request_id);
	UAM_ASSERT_EQ(chat.messages.back().markdown_store_prompt_blocks, original.back().markdown_store_prompt_blocks);
}

UAM_TEST(OpenCodeInterruptedCompactionRetryWithoutCompletedBoundaryKeepsOrdinaryContext)
{
	CompactedBranchFixture fixture("opencode-cli");
	Message& assistant = fixture.app.chats.front().messages[1];
	AppendBlock(assistant, "assistant_text", "KEEP_BEFORE_INTERRUPTION");
	AppendBlock(assistant, "context_compaction_summary", "DO_NOT_REPLAY_PARTIAL");
	assistant.blocks.back().request_id_json = "opencode-message:msg_unfinished";
	AppendBlock(assistant, "assistant_text", "KEEP_AFTER_INTERRUPTION");
	assistant.interrupted = true;
	const Message original = assistant;
	fixture.Retry();
	const std::string& prompt = fixture.session->queued_prompt;
	UAM_ASSERT(prompt.find("OBSOLETE_USER") != std::string::npos); // No completed boundary can replace it.
	UAM_ASSERT(prompt.find("Assistant: KEEP_BEFORE_INTERRUPTION") != std::string::npos);
	UAM_ASSERT(prompt.find("Assistant: KEEP_AFTER_INTERRUPTION") != std::string::npos);
	UAM_ASSERT(prompt.find("DO_NOT_REPLAY_PARTIAL") == std::string::npos);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages[1], original);
}

UAM_TEST(OpenCodeInterruptedCompactionRetryRefusesUnprovedQuarantineCoverage)
{
	CompactedBranchFixture fixture("opencode-cli");
	ChatSession& chat = fixture.app.chats.front();
	AppendBlock(chat.messages[1], "assistant_text", "KEEP_ORDINARY_REPLY");
	AppendBlock(chat.messages[1], "context_compaction_summary", "RETAIN_PARTIAL_SUMMARY");
	chat.messages[1].blocks.back().request_id_json = "opencode-message:msg_unproved";
	chat.messages[1].content += "UNREPRESENTED_SAVED_BYTES";
	chat.messages.back().acp_prompt_not_sent = true;
	const std::vector<Message> original = chat.messages;
	std::string error;
	UAM_ASSERT(!uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
	UAM_ASSERT(error.find("interrupted compaction history") != std::string::npos);
	UAM_ASSERT_EQ(chat.messages, original);
	UAM_ASSERT(fixture.session->queued_prompt.empty());
	UAM_ASSERT(fixture.session->queued_user_prompts.empty());
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	UAM_ASSERT_EQ(fixture.session->session_id, std::string("private-native-session"));
	UAM_ASSERT_EQ(chat.native_session_id, fixture.session->session_id);
}

UAM_TEST(OpenCodeQuarantinedSummaryHandoffUsesProvenBlockOrder)
{
	for (const bool content_includes_summaries : {false, true})
	{
		ChatSession chat;
		chat.provider_id = "opencode-cli";
		Message assistant{.role = MessageRole::Assistant};
		AppendBlock(assistant, "assistant_text", "Before.");
		AppendBlock(assistant, "context_compaction", "Completed.");
		AppendBlock(assistant, "context_compaction_summary", "Incomplete.");
		assistant.blocks.back().request_id_json = "opencode-message:msg_pending";
		AppendBlock(assistant, "assistant_text", "After.");
		if (!content_includes_summaries) assistant.content = "Before.After.";
		chat.messages.push_back(assistant);
		std::string context;
		UAM_ASSERT(uam::chat::BuildConversationHandoff(chat, context));
		UAM_ASSERT_EQ(context, std::string("Assistant: Before.\n\nConversation summary: Completed.\n\nAssistant: After.\n\n"));
		UAM_ASSERT_EQ(uam::BuildProviderHandoffContext(chat), context);
		UAM_ASSERT_EQ(chat.messages.front(), assistant);
	}
}

UAM_TEST(OpenCodeQuarantinedSummaryHandoffsRejectAmbiguousCoverage)
{
	ChatSession chat;
	chat.provider_id = "opencode-cli";
	Message assistant{.role = MessageRole::Assistant};
	AppendBlock(assistant, "assistant_text", "Preserve ordinary reply.");
	AppendBlock(assistant, "context_compaction_summary", "Preserve private partial summary.");
	assistant.blocks.back().request_id_json = "opencode-message:msg_ambiguous";
	assistant.content += "Unrepresented saved bytes.";
	chat.messages.push_back(assistant);
	std::string context = "Previously prepared context";
	std::string error;
	UAM_ASSERT(!uam::chat::BuildConversationHandoff(chat, context, &error));
	UAM_ASSERT_EQ(context, std::string("Previously prepared context"));
	UAM_ASSERT(error.find("interrupted compaction history") != std::string::npos);
	UAM_ASSERT(uam::BuildProviderHandoffContext(chat, {}, &error).empty());
	UAM_ASSERT(error.find("interrupted compaction history") != std::string::npos);
	UAM_ASSERT_EQ(chat.messages.front(), assistant);
}

UAM_TEST(OpenCodeQuarantinedSummaryProviderSwitchRetainsSessionAndInput)
{
	CompactedBranchFixture fixture("opencode-cli");
	ChatSession& chat = fixture.app.chats.front();
	AppendBlock(chat.messages[1], "context_compaction_summary", "Saved unfinished summary.");
	chat.messages[1].blocks.back().request_id_json = "opencode-message:msg_switch";
	chat.messages[1].content += "Unrepresented ordinary reply.";
	chat.messages.back().acp_prompt_not_sent = true;
	chat.provider_handoff_context = "Previously prepared context";
	const ChatSession original = chat;
	chat.execution_host_id = "synthetic-remote-no-transport";
	fixture.session->remote_stop_unconfirmed = true;
	// Refusal must precede every stop path; no process or remote transport is installed.
	UAM_ASSERT_EQ(uam::SwitchChatProvider(fixture.app, chat.id, "gemini-cli"), uam::ChatProviderSwitchResult::SaveFailed);
	UAM_ASSERT(fixture.app.status_line.find("interrupted compaction history") != std::string::npos);
	UAM_ASSERT_EQ(chat.provider_id, original.provider_id);
	UAM_ASSERT_EQ(chat.native_session_id, original.native_session_id);
	UAM_ASSERT_EQ(chat.provider_handoff_context, original.provider_handoff_context);
	UAM_ASSERT_EQ(chat.messages, original.messages);
	UAM_ASSERT(fixture.session->running);
	UAM_ASSERT(fixture.session->remote_stop_unconfirmed);
	UAM_ASSERT_EQ(fixture.session->session_id, original.native_session_id);
}

UAM_TEST(OpenCodeQuarantinedSummaryWorkspaceRetryRefusesBeforeStaging)
{
	CompactedBranchFixture fixture("opencode-cli");
	ChatSession& chat = fixture.app.chats.front();
	AppendBlock(chat.messages[1], "context_compaction_summary", "QUARANTINED_WORKSPACE_SUMMARY");
	chat.messages[1].content += "UNREPRESENTED_BYTES";
	chat.messages.back().created_at = "2026-10-23T00:00:00Z";
	chat.messages.back().acp_prompt_not_sent = true;
	chat.svn_update_pending = true;
	const ChatSession original = chat;
	std::string error;
	UAM_ASSERT(!uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
	UAM_ASSERT(error.find("interrupted compaction history") != std::string::npos);
	UAM_ASSERT_EQ(chat.messages, original.messages);
	UAM_ASSERT(chat.svn_update_pending);
	UAM_ASSERT(chat.acp_queued_prompts.empty());
	UAM_ASSERT(fixture.session->queued_user_prompts.empty());
	UAM_ASSERT(fixture.session->queued_prompt.empty());
	UAM_ASSERT(fixture.session->running);
	UAM_ASSERT_EQ(chat.native_session_id, original.native_session_id);
	UAM_ASSERT(!ChatRepository::LoadLocalChat(fixture.temp.root, chat.id, false));
}

UAM_TEST(OpenCodeQuarantinedSummarySideContextUsesSafeProjection)
{
	for (const bool detached : {false, true})
	{
		CompactedBranchFixture fixture("opencode-cli");
		ChatSession& chat = fixture.app.chats.front();
		AppendBlock(chat.messages[1], "assistant_text", "ORDINARY_BEFORE");
		AppendBlock(chat.messages[1], "context_compaction", "COMPLETED_SUMMARY");
		AppendBlock(chat.messages[1], "context_compaction_summary", "EXCLUDED_PARTIAL_SUMMARY");
		AppendBlock(chat.messages[1], "assistant_text", "ORDINARY_AFTER");
		chat.branch_root_chat_id.clear();
		const auto messages = chat.messages;
		std::optional<uam::TemporarySideChatContext> prepared;
		if (detached)
		{
			UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
			const auto metadata = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id, false);
			UAM_ASSERT(metadata.has_value());
			chat = *metadata;
			prepared = uam::PrepareTemporarySideChatContext(fixture.temp.root, chat);
			UAM_ASSERT(prepared.has_value());
		}
		const auto side = uam::BuildTemporarySideChatCandidate(fixture.app, chat.id, prepared ? &*prepared : nullptr);
		UAM_ASSERT(side.has_value());
		UAM_ASSERT(side->provider_handoff_context.find("EXCLUDED_PARTIAL_SUMMARY") == std::string::npos);
		UAM_ASSERT(side->provider_handoff_context.find("Conversation summary: COMPLETED_SUMMARY") != std::string::npos);
		UAM_ASSERT(side->provider_handoff_context.find("Assistant: ORDINARY_BEFORE") != std::string::npos);
		UAM_ASSERT(side->provider_handoff_context.find("Assistant: ORDINARY_AFTER") != std::string::npos);
		UAM_ASSERT(side->provider_handoff_context.size() <= 32768);
		if (detached) UAM_ASSERT(!chat.messages_loaded && chat.messages.empty());
		else UAM_ASSERT_EQ(chat.messages, messages);
	}
}

UAM_TEST(OpenCodeQuarantinedSummarySideContextHoldsAmbiguousHistory)
{
	CompactedBranchFixture fixture("opencode-cli");
	ChatSession& chat = fixture.app.chats.front();
	AppendBlock(chat.messages[1], "context_compaction_summary", "PARTIAL_SUMMARY");
	chat.messages[1].content += "UNREPRESENTED_BYTES";
	const auto original = chat.messages;
	UAM_ASSERT(!uam::BuildTemporarySideChatCandidate(fixture.app, chat.id));
	UAM_ASSERT_EQ(chat.messages, original);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
	const auto metadata = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id, false);
	UAM_ASSERT(metadata.has_value());
	UAM_ASSERT(!uam::PrepareTemporarySideChatContext(fixture.temp.root, *metadata));
}

UAM_TEST(OpenCodeSavedQuarantinedHandoffRefusesBeforeRetryAdmission)
{
	for (const bool ambiguous : {false, true})
	{
		CompactedBranchFixture fixture("opencode-cli");
		ChatSession& chat = fixture.app.chats.front();
		AppendBlock(chat.messages[1], "assistant_text", "ORDINARY_REPLY");
		AppendBlock(chat.messages[1], "context_compaction_summary", "CACHED_PARTIAL_SUMMARY");
		if (ambiguous) chat.messages[1].content += "UNREPRESENTED_BYTES";
		chat.provider_handoff_context = "Assistant: ORDINARY_REPLYCACHED_PARTIAL_SUMMARY\n\n";
		chat.svn_update_pending = true;
		const ChatSession original = chat;
		std::string error;
		UAM_ASSERT(!uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
		UAM_ASSERT(error.find("interrupted compaction history") != std::string::npos);
		UAM_ASSERT_EQ(chat.messages, original.messages);
		UAM_ASSERT_EQ(chat.provider_handoff_context, original.provider_handoff_context);
		UAM_ASSERT_EQ(chat.native_session_id, original.native_session_id);
		UAM_ASSERT(fixture.session->queued_user_prompts.empty());
		UAM_ASSERT(fixture.session->queued_prompt.empty());
	}
}

UAM_TEST(OpenCodeSavedSafeHandoffKeepsOrdinaryControlAndExcludesPartial)
{
	CompactedBranchFixture fixture("opencode-cli");
	ChatSession& chat = fixture.app.chats.front();
	AppendBlock(chat.messages[1], "assistant_text", "ORDINARY_REPLY");
	AppendBlock(chat.messages[1], "context_compaction_summary", "EXCLUDED_PARTIAL_SUMMARY");
	chat.provider_handoff_context = uam::BuildProviderHandoffContext(std::span<const Message>(chat.messages).first(2));
	std::string error;
	UAM_ASSERT(uam::ValidateProviderHandoffContext(chat, 2, &error));
	fixture.Retry();
	UAM_ASSERT(fixture.session->queued_prompt.find("ORDINARY_REPLY") != std::string::npos);
	UAM_ASSERT(fixture.session->queued_prompt.find("EXCLUDED_PARTIAL_SUMMARY") == std::string::npos);
	chat.messages[1].blocks.clear();
	chat.messages[1].content = "Ordinary historical answer";
	chat.provider_handoff_context = "Ordinary previously saved context";
	UAM_ASSERT(uam::ValidateProviderHandoffContext(chat, 2, &error));
}

UAM_TEST(OpenCodeSavedQuarantinedHandoffCliRefusesBeforeContextStaging)
{
	for (const bool detached : {false, true})
	{
		CompactedBranchFixture fixture("opencode-cli");
		ChatSession& chat = fixture.app.chats.front();
		AppendBlock(chat.messages[1], "assistant_text", "ORDINARY_REPLY");
		AppendBlock(chat.messages[1], "context_compaction_summary", "CACHED_PARTIAL_SUMMARY");
		chat.provider_handoff_context = "Assistant: ORDINARY_REPLYCACHED_PARTIAL_SUMMARY\n\n";
		if (detached)
		{
			UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
			const auto metadata = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id, false);
			UAM_ASSERT(metadata.has_value());
			chat = *metadata;
		}
		const std::string context = chat.provider_handoff_context;
		const auto retained_messages = chat.messages;
		std::string launch_error;
		UAM_ASSERT(!uam::PrepareAcpSessionForCliTerminalLaunch(fixture.app, chat, &launch_error));
		UAM_ASSERT(!launch_error.empty());
		UAM_ASSERT(launch_error.find(detached ? "Load the complete saved history" : "interrupted compaction history") != std::string::npos);
		UAM_ASSERT(fixture.session->running);
		UAM_ASSERT_EQ(fixture.session->session_id, std::string("private-native-session"));
		UAM_ASSERT_EQ(chat.messages, retained_messages);
		UAM_ASSERT_EQ(chat.messages_loaded, !detached);
		ExecutionHost host;
		host.id = "synthetic-remote-no-transport"; // Guard must return before any RunnerClient is created.
		std::vector<std::string> argv = {"opencode"};
		const auto original_argv = argv;
		std::vector<std::pair<std::string, std::string>> environment;
		std::string channel, error;
		UAM_ASSERT(!uam::PrepareCliProviderHandoff(fixture.app, chat, host, argv, environment, channel, error));
		UAM_ASSERT(error.find("interrupted compaction history") != std::string::npos);
		UAM_ASSERT_EQ(chat.provider_handoff_context, context);
		UAM_ASSERT_EQ(chat.native_session_id, std::string("private-native-session"));
		UAM_ASSERT_EQ(argv, original_argv);
		UAM_ASSERT(environment.empty() && channel.empty());
		UAM_ASSERT(chat.provider_handoff_cli_contexts.empty());
	}
}

UAM_TEST(OpenCodeSavedFullHistoryHandoffAllowsSafeRetryAndRejectsPartial)
{
	CompactedBranchFixture fixture("opencode-cli");
	ChatSession& chat = fixture.app.chats.front();
	AppendBlock(chat.messages[1], "assistant_text", "ORDINARY_REPLY");
	AppendBlock(chat.messages[1], "context_compaction_summary", "EXCLUDED_PARTIAL_SUMMARY");
	chat.provider_handoff_context = "Assistant: ORDINARY_REPLYEXCLUDED_PARTIAL_SUMMARY\n\n";
	std::string error;
	// Even an empty or shorter replay prefix cannot authorize a contaminated full-history cache.
	for (const std::size_t prefix : {std::size_t{0}, std::size_t{2}, chat.messages.size()})
		UAM_ASSERT(!uam::ValidateProviderHandoffContext(chat, prefix, &error));
	chat.provider_handoff_context = uam::BuildProviderHandoffContext(chat);
	for (const std::size_t prefix : {std::size_t{0}, std::size_t{2}, chat.messages.size()})
	{
		UAM_ASSERT(uam::ValidateProviderHandoffContext(chat, prefix, &error));
		UAM_ASSERT(error.empty());
	}
	const auto messages = chat.messages;
	fixture.Retry();
	UAM_ASSERT_EQ(chat.messages, messages);
	UAM_ASSERT(fixture.session->queued_prompt.find("EXCLUDED_PARTIAL_SUMMARY") == std::string::npos);
	UAM_ASSERT(fixture.session->queued_prompt.find("ORDINARY_REPLY") != std::string::npos);
	UAM_ASSERT_EQ(chat.native_session_id, std::string("private-native-session"));
}

UAM_TEST(OpenCodeRetainedRetryRechecksHistoryBeforeRuntimeMutation)
{
	CompactedBranchFixture fixture("opencode-cli");
	ChatSession& chat = fixture.app.chats.front();
	AppendBlock(chat.messages[1], "context_compaction_summary", "QUARANTINED_SUMMARY");
	chat.messages[1].content += "UNREPRESENTED_BYTES";
	chat.svn_update_pending = true;
	uam::AcpQueuedUserPromptState queued;
	queued.text = chat.messages.back().content;
	queued.append_user_message = false;
	queued.include_prior_context = true;
	queued.existing_user_message_index = 2;
	queued.id = "owned-retained-retry";
	queued.existing_user_message_created_at = chat.messages.back().created_at = "2026-10-23T00:00:00Z";
	chat.messages.back().acp_prompt_not_sent = true;
	fixture.session->queued_user_prompts.push_back(queued);
	chat.acp_queued_prompts = {queued};
	const auto messages = chat.messages;
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
	UAM_ASSERT(fixture.session->last_error.find("interrupted compaction history") != std::string::npos);
	UAM_ASSERT(fixture.session->running);
	UAM_ASSERT(fixture.session->queued_prompt.empty());
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts, std::deque<uam::AcpQueuedUserPromptState>{queued});
	UAM_ASSERT_EQ(chat.acp_queued_prompts, std::vector<uam::AcpQueuedUserPromptState>{queued});
	UAM_ASSERT(chat.svn_update_pending && chat.acp_queue_held && fixture.session->queue_held);
	UAM_ASSERT_EQ(chat.messages, messages);
	std::string error;
	UAM_ASSERT(!uam::ResumeQueuedAcpPrompts(fixture.app, chat.id, {{queued.id, queued.revision}}, &error));
	UAM_ASSERT(error.find("interrupted compaction history") != std::string::npos);
	UAM_ASSERT(fixture.session->queue_held && chat.acp_queue_held);
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts, std::deque<uam::AcpQueuedUserPromptState>{queued});
}

UAM_TEST(OpenCodeQuarantinedSideContextPreservesUtf8AndBudget)
{
	Message assistant{.role = MessageRole::Assistant};
	AppendBlock(assistant, "context_compaction_summary", "EXCLUDED_PARTIAL");
	std::string ordinary;
	for (int index = 0; index < 12000; ++index) ordinary += "🙂";
	AppendBlock(assistant, "assistant_text", ordinary + "RECENT_TAIL");
	std::string error;
	const std::string context = uam::BuildTemporarySideChatContext({assistant}, &error);
	UAM_ASSERT(error.empty());
	UAM_ASSERT(context.size() <= 32768);
	UAM_ASSERT(context.find("EXCLUDED_PARTIAL") == std::string::npos);
	UAM_ASSERT(context.find("RECENT_TAIL") != std::string::npos);
	UAM_ASSERT(!nlohmann::json(context).dump().empty());
}

UAM_TEST(OpenCodeBoundHandoffDoesNotReinjectSavedContext)
{
	CompactedBranchFixture fixture("opencode-cli");
	ChatSession& chat = fixture.app.chats.front();
	AppendBlock(chat.messages[1], "assistant_text", "ORDINARY_REPLY");
	AppendBlock(chat.messages[1], "context_compaction_summary", "EXCLUDED_PARTIAL");
	chat.provider_handoff_context = "OLD_BOUND_CONTEXT_EXCLUDED_PARTIAL";
	chat.provider_handoff_session_id = chat.native_session_id;
	fixture.Retry();
	UAM_ASSERT(fixture.session->queued_prompt.find("OLD_BOUND_CONTEXT") == std::string::npos);
	UAM_ASSERT(fixture.session->queued_prompt.find("EXCLUDED_PARTIAL") == std::string::npos);
	UAM_ASSERT_EQ(chat.native_session_id, std::string("private-native-session"));
}

namespace
{
	void CheckFailedMessageRetryRefusesCachedQuarantineBeforeAdmission(bool workspace)
	{
		CompactedBranchFixture fixture("opencode-cli");
		ChatSession& chat = fixture.app.chats.front();
		AppendBlock(chat.messages[1], "assistant_text", "ORDINARY_REPLY");
		AppendBlock(chat.messages[1], "context_compaction_summary", "EXCLUDED_PARTIAL_SUMMARY");
		chat.provider_handoff_context = "Assistant: ORDINARY_REPLYEXCLUDED_PARTIAL_SUMMARY\n\n";
		chat.messages.back().created_at = "2026-10-24T00:00:00Z";
		chat.messages.back().interrupted = chat.messages.back().acp_prompt_not_sent = true;
		chat.messages.back().markdown_store_files = {"captured-skill.md"};
		chat.messages.back().markdown_store_prompt_blocks = {"CAPTURED_SKILL"};
		chat.svn_update_pending = workspace;
		fixture.session->running = fixture.session->session_ready = workspace;
		fixture.session->lifecycle_state = workspace ? "idle" : "stopped";
		fixture.session->reconnect_not_before_time_s = 123.0;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
		const ChatSession original = chat;
		const std::string saved = ReadFile(AppPaths::UamChatFilePath(fixture.temp.root, chat.id));
		const std::string lifecycle = fixture.session->lifecycle_state;
		std::string error;
		UAM_ASSERT(!uam::RetryFailedAcpMessage(fixture.app, chat.id, 2, &error));
		UAM_ASSERT(error.find("interrupted compaction history") != std::string::npos);
		UAM_ASSERT_EQ(chat.messages, original.messages);
		UAM_ASSERT_EQ(chat.provider_handoff_context, original.provider_handoff_context);
		UAM_ASSERT_EQ(chat.native_session_id, original.native_session_id);
		UAM_ASSERT_EQ(chat.svn_update_pending, workspace);
		UAM_ASSERT(chat.acp_queued_prompts.empty() && fixture.session->queued_user_prompts.empty());
		UAM_ASSERT(!chat.acp_queue_held && !fixture.session->queue_held);
		UAM_ASSERT(!fixture.session->reconnect_pending);
		UAM_ASSERT_EQ(fixture.session->reconnect_not_before_time_s, 123.0);
		UAM_ASSERT_EQ(fixture.session->lifecycle_state, lifecycle);
		UAM_ASSERT_EQ(fixture.session->running, workspace);
		UAM_ASSERT_EQ(fixture.session->session_id, original.native_session_id);
		UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.temp.root, chat.id)), saved);
		UAM_ASSERT(fixture.app.worktree_launch_preparations.empty());
	}
}

UAM_TEST(OpenCodeStoppedNativeFailedRetryRefusesCachedQuarantineBeforeAdmission)
{
	CheckFailedMessageRetryRefusesCachedQuarantineBeforeAdmission(false);
}

UAM_TEST(OpenCodeWorkspaceFailedRetryRefusesCachedQuarantineBeforeAdmission)
{
	CheckFailedMessageRetryRefusesCachedQuarantineBeforeAdmission(true);
}
