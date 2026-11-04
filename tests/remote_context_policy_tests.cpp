#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
using namespace uam_test;

UAM_TEST(RemotePromptContextCancelsPolicyChangesBeforeStartupWait)
{
	const std::vector<std::function<void(uam::AppState&, uam::AcpSessionState&, ChatSession&)>> changes = {
		[](uam::AppState& app, uam::AcpSessionState&, ChatSession&) { app.settings.memory_recall_budget_bytes = 128; },
		[](uam::AppState&, uam::AcpSessionState&, ChatSession& chat) { chat.memory_enabled = false; },
		[](uam::AppState&, uam::AcpSessionState&, ChatSession& chat) { chat.memory_level = "off"; },
		[](uam::AppState&, uam::AcpSessionState& session, ChatSession&) { session.turn_first_user_message_index = 2; },
		[](uam::AppState&, uam::AcpSessionState& session, ChatSession&) { session.queued_prompt += "changed"; },
		[](uam::AppState&, uam::AcpSessionState&, ChatSession& chat) { chat.workspace_directory = "/other"; },
		[](uam::AppState&, uam::AcpSessionState&, ChatSession& chat) { chat.execution_host_id = "local"; },
		[](uam::AppState& app, uam::AcpSessionState&, ChatSession&) { app.settings.execution_hosts.clear(); },
		[](uam::AppState& app, uam::AcpSessionState&, ChatSession&) { app.settings.execution_hosts.front().instruction_file = "/changed"; }
	};
	for (std::size_t index = 0; index < changes.size() * 2; ++index)
	{
		uam::AppState app;
		app.settings.memory_recall_budget_bytes = 2048;
		ExecutionHost host;
		host.id = "owned-fixture";
		host.transport = "ssh";
		host.platform = "linux";
		host.ssh_alias = "never-connect";
		app.settings.execution_hosts.push_back(host);
		ChatSession chat;
		chat.id = "policy-fixture";
		chat.execution_host_id = host.id;
		chat.workspace_directory = "/fixture";
		chat.memory_enabled = true;
		chat.memory_level = "normal";
		uam::AcpSessionState session;
		session.queued_prompt = "accepted prompt";
		session.turn_first_user_message_index = 0;
		session.startup_model_request_id = 1;
		const std::shared_ptr<uam::RemoteContextPreparation> state = std::make_shared<uam::RemoteContextPreparation>();
		state->host = host;
		state->workspace = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat));
		state->prompt = session.queued_prompt;
		state->memory_budget = 2048;
		state->context = "Previously prepared memory";
		state->finished.store(index % 2 != 0, std::memory_order_release);
		session.remote_context_preparation = state;
		UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
		UAM_ASSERT(!state->cancel.stop_requested());
		changes[index / 2](app, session, chat);
		UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
		UAM_ASSERT(state->cancel.stop_requested());
		UAM_ASSERT(session.remote_context_preparation == nullptr);
		UAM_ASSERT(app.remote_context_tasks.empty());
	}
}
