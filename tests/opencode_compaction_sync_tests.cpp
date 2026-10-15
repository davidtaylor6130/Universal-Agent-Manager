#include "test_harness.h"
#include "app/opencode_compaction_sync_service.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

namespace
{
	struct NativeCompactionGateFixture
	{
		TempDir temp{"uam-opencode-compaction-gate"};
		uam::AppState app;
		uam::AcpSessionState session;

		NativeCompactionGateFixture()
		{
			app.data_root = temp.root;
			app.settings.git_enabled = false;
			app.settings.git_auto_worktree = false;
			ChatSession chat;
			chat.id = "owned-compaction-gate";
			chat.provider_id = "opencode-cli";
			chat.execution_host_id = "local";
			chat.native_session_id = "ses_owned";
			chat.workspace_directory = uam::paths::Utf8PathString(temp.root);
			Message user;
			user.role = MessageRole::User;
			user.content = "Private unsent fixture input";
			user.acp_prompt_not_sent = true;
			chat.messages.push_back(std::move(user));
			app.chats.push_back(std::move(chat));
			session.chat_id = app.chats.front().id;
			session.provider_id = "opencode-cli";
			session.process_execution_host_id = "local";
			session.session_id = "ses_owned";
			session.agent_version = "1.18.35";
			session.last_process_id = "private-unlaunched-process";
			session.running = session.session_ready = session.processing = true;
			session.turn_serial = 1;
			session.next_request_id = 17;
			session.turn_first_user_message_index = session.turn_user_message_index = 0;
			session.queued_prompt = "Private unsent fixture input";
		}
	};
}

UAM_TEST(OpenCodeNativeCompactionBaselineHoldsFirstResumedDispatch)
{
	NativeCompactionGateFixture fixture;
	int calls = 0;
	fixture.app.opencode_compaction_before_dispatch = [&](uam::AcpSessionState& session, ChatSession& chat, std::string&)
	{
		++calls;
		UAM_ASSERT_EQ(session.session_id, std::string("ses_owned"));
		UAM_ASSERT_EQ(chat.id, fixture.app.chats.front().id);
		return uam::OpenCodeCompactionGateState::Pending;
	};
	const bool changed = uam::acp_detail::SendQueuedPromptIfReady(fixture.app, fixture.session, fixture.app.chats.front());
	UAM_ASSERT_EQ(calls, 1);
	UAM_ASSERT(!changed);
	UAM_ASSERT_EQ(fixture.session.next_request_id, 17);
	UAM_ASSERT_EQ(fixture.session.prompt_request_id, 0);
	UAM_ASSERT(fixture.app.chats.front().messages.front().acp_prompt_not_sent);
	UAM_ASSERT_EQ(fixture.session.queued_prompt, std::string("Private unsent fixture input"));
}

UAM_TEST(OpenCodeNativeCompactionBaselineFailurePreservesUnsentInput)
{
	NativeCompactionGateFixture fixture;
	int calls = 0;
	fixture.app.opencode_compaction_before_dispatch = [&](uam::AcpSessionState&, ChatSession&, std::string& error)
	{
		++calls;
		error = "Native compaction metadata schema could not be verified.";
		return uam::OpenCodeCompactionGateState::Failed;
	};
	(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, fixture.session, fixture.app.chats.front());
	UAM_ASSERT_EQ(calls, 1);
	UAM_ASSERT_EQ(fixture.session.next_request_id, 17);
	UAM_ASSERT_EQ(fixture.session.prompt_request_id, 0);
	UAM_ASSERT(fixture.app.chats.front().messages.front().acp_prompt_not_sent);
	UAM_ASSERT(fixture.session.last_error.find("metadata schema") != std::string::npos);
}

namespace
{
	void SendNativeAssistantText(NativeCompactionGateFixture& fixture, const std::string& text,
	                             const nlohmann::json& message_id, const nlohmann::json& native_session = "ses_owned")
	{
		fixture.session.prompt_request_id = 13;
		fixture.session.queued_prompt.clear();
		UAM_ASSERT(uam::ProcessAcpLineForTests(fixture.app, fixture.session, fixture.app.chats.front(),
		    nlohmann::json{{"method", "session/update"}, {"params", {{"sessionId", native_session},
		        {"update", {{"sessionUpdate", "agent_message_chunk"}, {"messageId", message_id},
		            {"content", {{"type", "text"}, {"text", text}}}}}}}}.dump()));
	}
}

UAM_TEST(OpenCodeAssistantMessageIdentityMergesOnlyMatchingNativeChunks)
{
	NativeCompactionGateFixture fixture;
	SendNativeAssistantText(fixture, "Same", "msg_first");
	SendNativeAssistantText(fixture, " text", "msg_first");
	SendNativeAssistantText(fixture, "Same text", "msg_second");
	SendNativeAssistantText(fixture, "Same text extended", "msg_third");
	const std::vector<uam::AcpTurnEventState>& events = fixture.session.turn_events;
	UAM_ASSERT_EQ(events.size(), std::size_t{3});
	UAM_ASSERT_EQ(events[0].text, std::string("Same text"));
	UAM_ASSERT_EQ(events[1].text, std::string("Same text"));
	UAM_ASSERT_EQ(events[2].text, std::string("Same text extended"));
	const std::vector<MessageBlock>& blocks = fixture.app.chats.front().messages.back().blocks;
	UAM_ASSERT_EQ(blocks.size(), std::size_t{3});
	for (std::size_t index = 0; index < events.size(); ++index)
	{
		UAM_ASSERT_EQ(blocks[index].text, events[index].text);
		UAM_ASSERT_EQ(blocks[index].request_id_json, events[index].request_id_json);
	}
	UAM_ASSERT_EQ(blocks[0].request_id_json, std::string("opencode-message:msg_first"));
	UAM_ASSERT_EQ(blocks[1].request_id_json, std::string("opencode-message:msg_second"));
	UAM_ASSERT_EQ(blocks[2].request_id_json, std::string("opencode-message:msg_third"));
	NativeCompactionGateFixture prefix;
	SendNativeAssistantText(prefix, "Prefix", "msg_before");
	SendNativeAssistantText(prefix, "Prefix extended", "msg_after");
	UAM_ASSERT_EQ(prefix.session.turn_events.back().text, std::string("Prefix extended"));
	NativeCompactionGateFixture incremental;
	SendNativeAssistantText(incremental, "a", "msg_incremental");
	SendNativeAssistantText(incremental, "ab", "msg_incremental");
	UAM_ASSERT_EQ(incremental.session.turn_events.front().text, std::string("aab"));
	SendNativeAssistantText(incremental, "aaa", "msg_incremental");
	SendNativeAssistantText(incremental, "aaa", "msg_incremental");
	UAM_ASSERT_EQ(incremental.session.turn_events.size(), std::size_t{1});
	UAM_ASSERT_EQ(incremental.session.turn_events.front().text, std::string("aabaaaaaa"));
	UAM_ASSERT_EQ(incremental.app.chats.front().messages.back().content, std::string("aabaaaaaa"));
	UAM_ASSERT_EQ(incremental.app.chats.front().messages.back().blocks.front().text, std::string("aabaaaaaa"));
}

UAM_TEST(OpenCodeAssistantMessageIdentityRejectsInvalidNativeIds)
{
	for (const nlohmann::json& invalid : {nlohmann::json(""), nlohmann::json("msg bad"),
	     nlohmann::json("../msg"), nlohmann::json("-msg"), nlohmann::json(std::string(513, 'x')),
	     nlohmann::json(42), nlohmann::json(nullptr)})
	{
		NativeCompactionGateFixture fixture;
		SendNativeAssistantText(fixture, "First", invalid);
		SendNativeAssistantText(fixture, " second", invalid);
		UAM_ASSERT_EQ(fixture.session.turn_events.size(), std::size_t{1});
		UAM_ASSERT(fixture.session.turn_events.front().request_id_json.empty());
		UAM_ASSERT_EQ(fixture.app.chats.front().messages.back().blocks.size(), std::size_t{1});
		UAM_ASSERT(fixture.app.chats.front().messages.back().blocks.front().request_id_json.empty());
	}
	NativeCompactionGateFixture boundary;
	SendNativeAssistantText(boundary, "Bounded", std::string(512, 'x'));
	UAM_ASSERT_EQ(boundary.session.turn_events.front().request_id_json, "opencode-message:" + std::string(512, 'x'));
	for (const nlohmann::json& foreign : {nlohmann::json("ses_foreign"), nlohmann::json(7), nlohmann::json(nullptr)})
	{
		NativeCompactionGateFixture fixture;
		SendNativeAssistantText(fixture, "Foreign scope", "msg_known", foreign);
		for (const uam::AcpTurnEventState& event : fixture.session.turn_events)
			UAM_ASSERT(event.request_id_json.empty());
		for (const Message& message : fixture.app.chats.front().messages)
			for (const MessageBlock& block : message.blocks)
				UAM_ASSERT(block.request_id_json.empty());
	}
}

UAM_TEST(AssistantMessageIdentityLeavesOtherProvidersAndDefaultHelperUnchanged)
{
	NativeCompactionGateFixture fixture;
	fixture.session.provider_id = fixture.app.chats.front().provider_id = "gemini-cli";
	SendNativeAssistantText(fixture, "First", "msg_first");
	SendNativeAssistantText(fixture, " second", "msg_second");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.app.chats.front(), fixture.session, " third");
	UAM_ASSERT_EQ(fixture.session.turn_events.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.session.turn_events.front().text, std::string("First second third"));
	UAM_ASSERT(fixture.session.turn_events.front().request_id_json.empty());
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.back().blocks.size(), std::size_t{1});
	UAM_ASSERT(fixture.app.chats.front().messages.back().blocks.front().request_id_json.empty());
}
