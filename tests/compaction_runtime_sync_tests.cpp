#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

namespace
{
	struct CompactionFixture
	{
		TempDir temp{"uam-compaction-sync"};
		uam::AppState app;
		uam::AcpSessionState session;
		CompactionFixture()
		{
			app.data_root = temp.root;
			ChatSession chat;
			chat.id = "compaction-owner";
			chat.provider_id = "opencode-cli";
			chat.native_session_id = "ses_parent";
			app.chats.push_back(std::move(chat));
			session.chat_id = app.chats.front().id;
			session.provider_id = "opencode-cli";
			session.session_id = "ses_parent";
			session.last_process_id = "private-process";
			session.running = session.session_ready = session.processing = true;
			session.turn_serial = 7;
			session.prompt_request_id = 13;
		}
		void Send(nlohmann::json update, nlohmann::json native_session = "ses_parent")
		{
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
			    nlohmann::json{{"method", "session/update"}, {"params", {{"sessionId", native_session}, {"update", update}}}}.dump()));
		}
		void Status(std::string status, std::string id = "owned")
		{
			Send({{"sessionUpdate", "compaction_update"}, {"compactionId", id}, {"status", status}});
		}
	};
}

UAM_TEST(AcpOwnedCompactionUpdatesMaintainLiveState)
{
	for (const std::string terminal : {"completed", "failed", "cancelled"})
	{
		CompactionFixture fixture;
		fixture.Status("in_progress");
		UAM_ASSERT(fixture.session.context_compaction_in_progress);
		fixture.Status(terminal);
		UAM_ASSERT(!fixture.session.context_compaction_in_progress);
		UAM_ASSERT(fixture.session.processing);
	}
}

UAM_TEST(AcpCompactionRejectsForeignNativeSessionIdentity)
{
	for (const nlohmann::json identity : {nlohmann::json("ses_foreign"), nlohmann::json(123), nlohmann::json(nullptr)})
	{
		CompactionFixture fixture;
		fixture.Send({{"sessionUpdate", "compaction_update"}, {"compactionId", "owned"}, {"status", "started"}}, identity);
		UAM_ASSERT(fixture.app.chats.front().messages.empty());
		UAM_ASSERT(fixture.session.turn_events.empty());
		UAM_ASSERT(!fixture.session.context_compaction_in_progress);
		fixture.Status("started");
		fixture.Send({{"sessionUpdate", "compaction_update"}, {"compactionId", "owned"}, {"status", "completed"}}, identity);
		UAM_ASSERT(fixture.app.chats.front().messages.empty());
		UAM_ASSERT(fixture.session.context_compaction_in_progress);
	}
}

UAM_TEST(AcpLateOwnedCompactionCompletesOnlyItsSettledMessage)
{
	CompactionFixture fixture;
	fixture.Send({{"sessionUpdate", "agent_message_chunk"}, {"content", {{"type", "text"}, {"text", "Authored answer"}}}});
	fixture.Status("started");
	uam::acp_detail::CompletePromptTurn(fixture.session, "ready");
	fixture.session.queued_user_prompts.push_back({.id = "future", .text = "Future prompt"});
	const std::size_t message_count = fixture.app.chats.front().messages.size();
	fixture.Send({{"sessionUpdate", "compaction_update"}, {"compactionId", "owned"}, {"status", "completed"},
	    {"summary", {{"type", "text"}, {"text", "Retain workspace facts"}}}});
	const ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT_EQ(chat.messages.size(), message_count);
	UAM_ASSERT_EQ(chat.messages.back().blocks.size(), std::size_t{2});
	UAM_ASSERT_EQ(chat.messages.back().blocks.back().type, std::string("context_compaction"));
	UAM_ASSERT_EQ(chat.messages.back().blocks.back().text, std::string("Retain workspace facts"));
	UAM_ASSERT_EQ(chat.messages.back().content, std::string("Authored answer"));
	UAM_ASSERT(!fixture.session.processing && !fixture.session.context_compaction_in_progress);
	UAM_ASSERT_EQ(fixture.session.current_assistant_message_index, -1);
	UAM_ASSERT_EQ(fixture.session.queued_user_prompts.front().text, std::string("Future prompt"));
	UAM_ASSERT_EQ(fixture.session.turn_serial, 7);
	fixture.Status("completed", "unknown-late");
	fixture.Status("completed");
	UAM_ASSERT_EQ(chat.messages.size(), message_count);
	UAM_ASSERT_EQ(chat.messages.back().blocks.size(), std::size_t{2});
}

UAM_TEST(OpenCodeCompactionMetadataMaintainsOwnedLiveState)
{
	CompactionFixture fixture;
	fixture.Send({{"sessionUpdate", "session_info_update"}, {"_meta", {{"opencode/compaction", {{"messageId", "owned"}, {"status", "started"}}}}}});
	UAM_ASSERT(fixture.session.context_compaction_in_progress);
	fixture.Send({{"sessionUpdate", "compaction_summary_chunk"}, {"compactionId", "owned"}, {"content", {{"type", "text"}, {"text", "Native summary"}}}});
	fixture.Send({{"sessionUpdate", "session_info_update"}, {"_meta", {{"opencode/compaction", {{"messageId", "owned"}, {"status", "completed"}}}}}});
	UAM_ASSERT(!fixture.session.context_compaction_in_progress);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.back().blocks.front().text, std::string("Native summary"));
}

UAM_TEST(AcpCompactionKeepsOverlappingChildAndReplayStatesSeparate)
{
	CompactionFixture fixture;
	fixture.Status("completed", "unowned");
	fixture.Status("unknown", "unowned");
	UAM_ASSERT(fixture.session.turn_events.empty());
	fixture.Status("started");
	fixture.Send({{"sessionUpdate", "session_info_update"}, {"_meta", {
	    {"opencode/compaction", {{"messageId", "child"}, {"status", "completed"}}},
	    {"opencode/child-session", {{"id", "ses_child"}}}}}});
	UAM_ASSERT(fixture.session.context_compaction_in_progress);
	UAM_ASSERT(fixture.app.chats.front().messages.empty());
	fixture.Status("in_progress", "second");
	fixture.Status("completed");
	UAM_ASSERT(fixture.session.context_compaction_in_progress);
	fixture.Status("failed", "unknown");
	UAM_ASSERT(fixture.session.context_compaction_in_progress);
	fixture.Status("cancelled", "second");
	UAM_ASSERT(!fixture.session.context_compaction_in_progress);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.back().blocks.size(), std::size_t{1});
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.front()));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.session.chat_id)->messages.back().blocks.size(), std::size_t{1});

	CompactionFixture replay;
	replay.session.ignore_session_updates_until_ready = true;
	replay.Status("completed", "historical");
	UAM_ASSERT(!replay.session.context_compaction_in_progress && replay.session.turn_events.empty());
	UAM_ASSERT_EQ(replay.app.chats.front().messages.back().blocks.front().type, std::string("context_compaction"));
}

UAM_TEST(AcpCompactionLateCompletionRejectsNewTurnsStoppedAndFailedOwners)
{
	for (const int replacement : {0, 1, 2, 3, 4, 5})
	{
		CompactionFixture fixture;
		fixture.Send({{"sessionUpdate", "agent_message_chunk"}, {"content", {{"type", "text"}, {"text", "Prior answer"}}}});
		fixture.Status("started");
		uam::acp_detail::CompletePromptTurn(fixture.session, replacement == 2 ? "error" : "ready");
		if (replacement == 0)
		{
			uam::acp_detail::ResetAcpTurnStreamState(fixture.session);
			++fixture.session.turn_serial;
			fixture.session.processing = true;
		}
		if (replacement == 1) uam::acp_detail::ResetAcpRuntimeState(fixture.app, fixture.session, fixture.app.chats.front());
		if (replacement == 3) fixture.session.running = false;
		if (replacement == 4) fixture.session.cancel_request_id = 99;
		if (replacement == 5) fixture.app.chats.front().messages.back().blocks.front().text = "Edited message";
		const std::vector<Message> original = fixture.app.chats.front().messages;
		fixture.Status("completed");
		UAM_ASSERT(fixture.app.chats.front().messages == original);
		UAM_ASSERT(!fixture.session.context_compaction_in_progress);
	}
}
