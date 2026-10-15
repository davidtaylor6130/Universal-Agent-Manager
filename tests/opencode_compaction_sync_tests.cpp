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
