#include "test_harness.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"

using namespace uam_test;

namespace
{
	void ConfigureOpenCodeChildFixture(uam::AcpSessionState& session)
	{
		session.provider_id = "opencode-cli";
		session.chat_id = "parent-chat";
		session.session_id = "ses_parent";
		session.last_process_id = "process-1";
		session.running = true;
		session.session_ready = true;
		session.lifecycle_state = "ready";
	}

	nlohmann::json ChildActivityFrame(const std::string& tool_id, const std::string& child_id,
		const std::string& status, const std::string& parent_id = "ses_parent")
	{
		nlohmann::json update = {{"sessionUpdate", "tool_call_update"}, {"toolCallId", tool_id}, {"kind", "think"}, {"status", status}};
		if (!child_id.empty()) update["rawOutput"] = {{"metadata", {{"sessionId", child_id}}}};
		return {{"method", "session/update"}, {"params", {{"sessionId", parent_id}, {"update", update}}}};
	}

	void ObserveChild(uam::AppState& app, uam::AcpSessionState& session, const nlohmann::json& frame)
	{
		ChatSession chat;
		UAM_ASSERT(!ProviderRuntimeRegistry::ResolveById("opencode-cli").OnAcpHandleMessage(app, session, chat, frame, nullptr));
	}
}

UAM_TEST(OpenCodeChildActivityRequiresCurrentParentProcessReceipts)
{
	uam::AppState app;
	uam::AcpSessionState session;
	ConfigureOpenCodeChildFixture(session);
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("opencode-cli");
	uam::AcpToolCallState historical;
	historical.is_sub_agent = true;
	historical.sub_agent_id = "ses_historical";
	historical.status = "in_progress";
	session.tool_calls.push_back(historical);
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
	for (const std::string child : {"", "../foreign", "invented-child", "ses_parent"})
		ObserveChild(app, session, ChildActivityFrame("tool-1", child, "in_progress"));
	ObserveChild(app, session, ChildActivityFrame("tool-1", "ses_child", "in_progress", "ses_other_parent"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
	ObserveChild(app, session, ChildActivityFrame("tool-1", "ses_child", "in_progress"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(1));
	session.processing = false;
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(1));
	session.last_process_id = "process-2";
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
	session.last_process_id = "process-1";
	session.session_id = "ses_other_parent";
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
	session.session_id = "ses_parent";
	session.running = false;
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
}

UAM_TEST(OpenCodeChildActivityDeduplicatesAndRequiresExplicitSettlement)
{
	uam::AppState app;
	uam::AcpSessionState session;
	ConfigureOpenCodeChildFixture(session);
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("opencode-cli");
	ObserveChild(app, session, ChildActivityFrame("tool-1", "ses_child", "in_progress"));
	ObserveChild(app, session, ChildActivityFrame("tool-2", "ses_child", "in_progress"));
	ObserveChild(app, session, ChildActivityFrame("tool-3", "ses_sibling", "in_progress"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(2));
	ObserveChild(app, session, ChildActivityFrame("tool-1", "", "invalid-status"));
	ObserveChild(app, session, ChildActivityFrame("tool-2", "", "completed", "ses_stale_parent"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(2));
	ObserveChild(app, session, ChildActivityFrame("tool-1", "", "completed"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(2));
	ObserveChild(app, session, ChildActivityFrame("tool-2", "", "completed"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(1));
	ObserveChild(app, session, ChildActivityFrame("tool-3", "", "failed"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
	UAM_ASSERT(session.opencode_child_activity_by_tool.empty());
}

UAM_TEST(OpenCodeChildActivityRejectsHistoryReplayAndReusedProcessInitialization)
{
	uam::AppState app;
	uam::AcpSessionState session;
	ConfigureOpenCodeChildFixture(session);
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("opencode-cli");
	session.pending_request_methods[5] = "session/load";
	ObserveChild(app, session, ChildActivityFrame("replayed-tool", "ses_history", "in_progress"));
	session.pending_request_methods.clear();
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
	session.manual_compaction_pending = true;
	ObserveChild(app, session, ChildActivityFrame("replayed-tool", "ses_history", "in_progress"));
	session.manual_compaction_pending = false;
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
	ObserveChild(app, session, ChildActivityFrame("live-tool", "ses_child", "in_progress"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(1));
	ChatSession chat;
	chat.id = session.chat_id;
	chat.provider_id = session.provider_id;
	for (const std::string replay : {"load", "fork", "compaction"})
	{
		if (replay == "load") session.pending_request_methods[5] = "session/load";
		if (replay == "fork") session.pending_request_methods[5] = "session/fork";
		if (replay == "compaction") session.manual_compaction_pending = true;
		ObserveChild(app, session, ChildActivityFrame("live-tool", "", "completed"));
		UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(1));
		UAM_ASSERT(!uam::AcpSessionCanIdleShutdown(session, chat));
		session.pending_request_methods.clear();
		session.manual_compaction_pending = false;
	}
	ObserveChild(app, session, ChildActivityFrame("live-tool", "", "completed"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
	UAM_ASSERT(session.opencode_child_activity_by_tool.empty());
	UAM_ASSERT(uam::AcpSessionCanIdleShutdown(session, chat));
	ObserveChild(app, session, ChildActivityFrame("fresh-tool", "ses_child", "paused"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
	UAM_ASSERT_EQ(session.opencode_child_activity_by_tool.size(), static_cast<std::size_t>(1));
	ObserveChild(app, session, ChildActivityFrame("fresh-tool", "", "in_progress"));
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(1));
	const std::string process_id = session.last_process_id;
	const nlohmann::json initialize = runtime.OnAcpBuildInitialize(session, 10);
	UAM_ASSERT(initialize["method"] == "initialize");
	UAM_ASSERT_EQ(session.last_process_id, process_id);
	UAM_ASSERT(session.opencode_child_activity_by_tool.empty());
	UAM_ASSERT_EQ(runtime.AcpBackgroundTaskCount(session), static_cast<std::size_t>(0));
}

UAM_TEST(ProviderChildActivityProjectionPreservesSiblingAndManagedCounts)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	for (const std::string provider : {"claude-code", "codex-cli", "opencode-cli"})
	{
		ChatSession chat;
		chat.id = provider;
		chat.provider_id = provider;
		app.chats.push_back(chat);
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		uam::AcpSessionState& session = *app.acp_sessions.back();
		session.provider_id = provider;
		session.chat_id = chat.id;
		session.running = true;
		session.session_ready = true;
		session.session_id = "ses_parent";
		session.last_process_id = provider + "-process";
		if (provider == "claude-code") session.claude_live_background_task_ids.insert("claude-child");
		if (provider == "codex-cli") session.codex_live_background_thread_ids.insert("codex-child");
		if (provider == "opencode-cli") ObserveChild(app, session, ChildActivityFrame("native-tool", "ses_child", "in_progress"));
		AgentRun managed;
		managed.id = provider + "-managed-child";
		managed.root_chat_id = chat.id;
		managed.status = "running";
		app.agent_runs.push_back(managed);
		UAM_ASSERT_EQ(ProviderRuntimeRegistry::ResolveById(provider).AcpBackgroundTaskCount(session), static_cast<std::size_t>(1));
	}
	const nlohmann::json state = uam::StateSerializer::Serialize(app, false);
	UAM_ASSERT_EQ(state["chats"].size(), static_cast<std::size_t>(3));
	for (const nlohmann::json& chat : state["chats"])
		UAM_ASSERT_EQ(chat["acpSession"]["backgroundTaskCount"].get<std::size_t>(), static_cast<std::size_t>(2));
}

UAM_TEST(OpenCodeLiveChildPreventsIdleShutdownUntilNativeSettlement)
{
	uam::AppState app;
	uam::AcpSessionState session;
	ConfigureOpenCodeChildFixture(session);
	ChatSession chat;
	chat.id = session.chat_id;
	chat.provider_id = session.provider_id;
	UAM_ASSERT(uam::AcpSessionCanIdleShutdown(session, chat));
	ObserveChild(app, session, ChildActivityFrame("task-1", "ses_child", "in_progress"));
	UAM_ASSERT(!uam::AcpSessionCanIdleShutdown(session, chat));
	session.processing = false;
	UAM_ASSERT(!uam::AcpSessionCanIdleShutdown(session, chat));
	ObserveChild(app, session, ChildActivityFrame("task-1", "", "completed", "ses_foreign_parent"));
	UAM_ASSERT(!uam::AcpSessionCanIdleShutdown(session, chat));
	ObserveChild(app, session, ChildActivityFrame("task-1", "", "completed"));
	UAM_ASSERT(uam::AcpSessionCanIdleShutdown(session, chat));
}

UAM_TEST(OpenCodeLocalChildStopObservesOnlyItsOwnedProcess)
{
	TempDir temp("opencode-child-local-stop");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "parent-chat";
	chat.provider_id = "opencode-cli";
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	ConfigureOpenCodeChildFixture(session);
	IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> command = {"cmd.exe", "/d", "/s", "/c", "more > NUL"};
#else
	const std::vector<std::string> command = {"/bin/sh", "-c", "cat > /dev/null"};
#endif
	UAM_ASSERT(process.StartStdioProcess(session, temp.root, command, &error));
	struct ProcessGuard
	{
		IPlatformProcessService& process;
		uam::AcpSessionState& session;
		~ProcessGuard() { process.StopStdioProcess(session, true); process.CloseStdioProcessHandles(session); }
	} guard{process, session};
	ObserveChild(app, session, ChildActivityFrame("task-1", "ses_child", "in_progress"));
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& sibling = *app.acp_sessions.back();
	ConfigureOpenCodeChildFixture(sibling);
	sibling.chat_id = "sibling-chat";
	sibling.session_id = "ses_sibling";
	UAM_ASSERT(!uam::StopAcpSession(app, chat.id));
	UAM_ASSERT_EQ(app.acp_process_stop_tasks.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.acp_process_stop_tasks.front().observe_exit);
	UAM_ASSERT_EQ(app.acp_process_stop_tasks.front().chat_id, chat.id);
	UAM_ASSERT(app.acp_process_stop_tasks.front().owned_process != nullptr);
	UAM_ASSERT(session.local_stop_pending);
	UAM_ASSERT(!session.running);
	UAM_ASSERT(sibling.running);
	UAM_ASSERT(!sibling.local_stop_pending);
	app.acp_process_stop_tasks.front().worker->join();
	UAM_ASSERT(app.acp_process_stop_tasks.front().finished->load());
	UAM_ASSERT(app.acp_process_stop_tasks.front().result->exit_confirmed);
}
