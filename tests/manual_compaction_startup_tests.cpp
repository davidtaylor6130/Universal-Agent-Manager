#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_polling.h"

#include <cstdio>
#if defined(_WIN32)
#include <io.h>
#endif

using namespace uam_test;

namespace
{
	class CompactionTransport
	{
	public:
		explicit CompactionTransport(uam::AcpSessionState& session) : m_session(session), m_file(std::tmpfile())
		{
			UAM_ASSERT(m_file != nullptr);
#if defined(_WIN32)
			m_session.stdin_write = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(m_file)));
#else
			m_session.stdin_write_fd = fileno(m_file);
#endif
			m_session.stdin_writer = std::make_shared<uam::platform::AsyncByteWriter>(
			    [this](const char* bytes, std::size_t count, std::string&) -> std::ptrdiff_t
			    { captured.append(bytes, count); return static_cast<std::ptrdiff_t>(count); }, [] {});
		}
		~CompactionTransport()
		{
			m_session.stdin_writer.reset();
#if defined(_WIN32)
			m_session.stdin_write = INVALID_HANDLE_VALUE;
#else
			m_session.stdin_write_fd = -1;
#endif
			std::fclose(m_file);
		}
		std::string captured;
	private:
		uam::AcpSessionState& m_session;
		std::FILE* m_file;
	};
}

UAM_TEST(ManualCompactionAcceptsStartingSessionWithoutPromptDelivery)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-compact-starting");
	uam::AppState app;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "compact-starting";
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "local";
	chat.native_session_id = "existing-thread";
	Message message;
	message.content = "Preserved conversation";
	chat.messages.push_back(message);
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.running = true;
	session.session_ready = false;
	session.pending_request_methods[17] = "initialize";
	std::string error;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(session.queued_prompt.empty());
	UAM_ASSERT(session.queued_user_prompts.empty());
	UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t(1));
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, message.content);
	UAM_ASSERT_EQ(session.pending_request_methods.size(), std::size_t(1));
#endif
}

UAM_TEST(ManualCompactionUnsupportedProviderDoesNotCreateRuntime)
{
	uam::AppState app;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
	ChatSession chat;
	chat.id = "compact-unsupported";
	chat.provider_id = "claude-cli";
	app.chats.push_back(chat);
	std::string error;
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(error.find("does not support") != std::string::npos);
	UAM_ASSERT(app.acp_sessions.empty());
	UAM_ASSERT(app.chats.front().messages.empty());
}

UAM_TEST(ManualCompactionDefersAbsentAndStoppedSessionsDuringOwnedVersionProbe)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (bool existing : {false, true})
	{
		TempDir temp("uam-compact-deferred");
		uam::AppState app;
		app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "compact-deferred";
		chat.provider_id = "codex-cli";
		chat.execution_host_id = "local";
		chat.native_session_id = "preserved-thread";
		app.chats.push_back(chat);
		if (existing)
		{
			app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
			app.acp_sessions.back()->chat_id = chat.id;
			app.acp_sessions.back()->provider_id = chat.provider_id;
		}
		app.runtime_cli_version_check_task.running = true;
		app.runtime_cli_version_provider_id = chat.provider_id;
		std::string error;
		UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
		UAM_ASSERT_EQ(app.acp_sessions.size(), std::size_t(1));
		UAM_ASSERT(!app.acp_sessions.front()->running);
		UAM_ASSERT(app.acp_sessions.front()->queued_prompt.empty());
		UAM_ASSERT(app.chats.front().messages.empty());
		UAM_ASSERT_EQ(app.chats.front().native_session_id, chat.native_session_id);
		UAM_ASSERT(uam::StopAcpSession(app, chat.id));
		UAM_ASSERT(!app.chats.front().acp_queue_held);
	}
#endif
}

UAM_TEST(ManualCompactionDeferredOwnershipChangesRejectWithoutPromptDelivery)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (int mutation = 0; mutation < 5; ++mutation)
	{
		TempDir temp("uam-compact-owned");
		uam::AppState app;
		app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "compact-owned";
		chat.provider_id = "codex-cli";
		chat.execution_host_id = "local";
		chat.native_session_id = "owned-thread";
		app.chats.push_back(chat);
		app.runtime_cli_version_check_task.running = true;
		app.runtime_cli_version_provider_id = chat.provider_id;
		std::string error;
		UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
		if (mutation == 0) app.chats.front().native_session_id = "foreign-thread";
		if (mutation == 1) app.chats.front().model_id = "foreign-model";
		if (mutation == 2) app.chats.front().workspace_directory = "/foreign-workspace";
		if (mutation == 3) app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (mutation == 4) app.data_root = temp.root / "foreign-root";
		UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
		UAM_ASSERT(error.find("ownership") != std::string::npos);
		UAM_ASSERT(!app.acp_sessions.front()->running);
		UAM_ASSERT(app.acp_sessions.front()->queued_prompt.empty());
		UAM_ASSERT(app.chats.front().messages.empty());
		UAM_ASSERT(!app.chats.front().acp_queue_held);
	}
#endif
}

UAM_TEST(ManualCompactionStartingIntentDispatchesOnceAfterReady)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-compact-ready");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
	ChatSession chat;
	chat.id = "compact-ready";
	chat.provider_id = "codex-cli";
	chat.native_session_id = "thread-ready";
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.running = true;
	session.session_ready = false;
	CompactionTransport transport(session);
	std::string error;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(transport.captured.empty());
	session.session_ready = true;
	session.session_id = chat.native_session_id;
	session.codex_thread_id = chat.native_session_id;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
	const nlohmann::json request = nlohmann::json::parse(transport.captured);
	UAM_ASSERT_EQ(request["method"].get<std::string>(), std::string("thread/compact/start"));
	UAM_ASSERT_EQ(request["params"]["threadId"].get<std::string>(), chat.native_session_id);
	const std::string sent = transport.captured;
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
	UAM_ASSERT_EQ(transport.captured, sent);
	UAM_ASSERT(app.chats.front().messages.empty());
	UAM_ASSERT(session.queued_prompt.empty());
	UAM_ASSERT(session.queued_user_prompts.empty());
#endif
}

UAM_TEST(ManualCompactionPendingIntentBlocksNewPromptAdmission)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-compact-input-guard");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
	ChatSession chat;
	chat.id = "compact-input-guard";
	chat.provider_id = "codex-cli";
	app.chats.push_back(chat);
	app.runtime_cli_version_check_task.running = true;
	app.runtime_cli_version_provider_id = chat.provider_id;
	std::string error;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	// A second admission gate prevents a real launch if the maintenance guard regresses.
	app.worktree_operation_chat_ids.insert(chat.id);
	UAM_ASSERT(!uam::SendAcpPrompt(app, chat.id, "Keep this input", &error));
	UAM_ASSERT(error.find("compaction") != std::string::npos);
	UAM_ASSERT(!app.acp_sessions.front()->running);
	UAM_ASSERT(app.acp_sessions.front()->queued_user_prompts.empty());
	UAM_ASSERT(app.chats.front().messages.empty());
#endif
}
