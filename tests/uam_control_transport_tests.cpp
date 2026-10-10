#include "test_harness.h"
#include "app/uam_control_service.h"
#include "common/config/central_provider_configuration.h"
#include "common/chat/chat_repository.h"
#include "cef/cef_push.h"

UAM_TEST(UamControlProviderOverrideAuthorizesLaunchWithoutChangingChatPermissions)
{
	uam_test::TempDir temp("control-provider-override");
	uam::AppState app; app.data_root = temp.root;
	ChatSession chat; chat.id = "override-chat"; chat.provider_id = "opencode-cli"; chat.execution_host_id = "local"; chat.uam_control_enabled = false;
	app.chats.push_back(chat);
	app.settings.central_provider_configuration.mcp_overrides.push_back({chat.provider_id, "local", "uam-mcp", true});
	UAM_ASSERT(uam::UamControlService::Initialize(app));
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back(); session.chat_id = chat.id; session.provider_id = chat.provider_id; session.running = true;
	nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, app.chats.front(), "session/new", setup));
	UAM_ASSERT(!app.chats.front().uam_control_enabled);
	const std::string capability_id = session.uam_control_capability_id;
	const auto call = [&](const std::string& id) {
		return uam::UamControlService::HandleRequestForTests(app, capability_id, {{"requestId", id}, {"method", "goal_get"}, {"arguments", nlohmann::json::object()}}, uam::time::SystemEpochMillisecondsNow());
	};
	UAM_ASSERT(call("granted").value("ok", false));
	app.settings.central_provider_configuration.mcp_overrides.front().enabled = false;
	UAM_ASSERT(call("next-launch-only").value("ok", false));
	uam::UamControlService::RevokeForChat(app, chat.id);
	UAM_ASSERT(!call("revoked").value("ok", true));
	UAM_ASSERT(session.uam_control_capability_id.empty());
	app.settings.central_provider_configuration.mcp_overrides.front().enabled = true;
	app.chats.front().uam_control_explicitly_disabled = true;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(restored.has_value() && restored->uam_control_explicitly_disabled);
	app.chats.front() = *restored;
	setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, app.chats.front(), "session/new", setup));
	UAM_ASSERT(setup["params"]["mcpServers"].empty());
	UAM_ASSERT(session.uam_control_capability_id.empty());
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(UamControlHeartbeatsPreserveToolQuotaAndRootRevocationRemovesDescendants)
{
	uam_test::TempDir temp("control-heartbeat-quota");
	uam::AppState app; app.data_root = temp.root;
	ChatSession chat; chat.id = "root-chat"; chat.provider_id = "opencode-cli"; chat.execution_host_id = "local"; chat.uam_control_enabled = true;
	app.chats.push_back(chat);
	UAM_ASSERT(uam::UamControlService::Initialize(app));
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back(); session.chat_id = chat.id; session.provider_id = chat.provider_id; session.running = true;
	nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, app.chats.front(), "session/new", setup));
	const int64_t now = uam::time::SystemEpochMillisecondsNow();
	const auto call = [&](const std::string& id, const std::string& method, const nlohmann::json& arguments) {
		return uam::UamControlService::HandleRequestForTests(app, session.uam_control_capability_id, {{"requestId", id}, {"method", method}, {"arguments", arguments}}, now);
	};
	UAM_ASSERT(call("initialize", "_mcp_initialized", {{"connectionId", "connection"}}).value("ok", false));
	for (int index = 0; index < 60; ++index)
		UAM_ASSERT(call("heartbeat-" + std::to_string(index), "_mcp_heartbeat", {{"connectionId", "connection"}}).value("ok", false));
	for (int index = 0; index < 30; ++index)
		UAM_ASSERT(call("tool-" + std::to_string(index), "goal_get", nlohmann::json::object()).value("ok", false));
	UAM_ASSERT(!call("tool-over-limit", "goal_get", nlohmann::json::object()).value("ok", true));
	UAM_ASSERT(call("heartbeat-after-limit", "_mcp_heartbeat", {{"connectionId", "connection"}}).value("ok", false));
	const std::string old_capability = session.uam_control_capability_id;
	const std::filesystem::path directory = app.uam_control_capabilities.front().directory;
	ChatSession child = chat; child.id = "child-chat"; child.agent_run_id = "child-run";
	app.chats.push_back(child);
	AgentRun child_run; child_run.id = child.agent_run_id; child_run.root_chat_id = chat.id; child_run.transcript_chat_id = child.id; child_run.status = "running";
	app.agent_runs.push_back(child_run);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& child_session = *app.acp_sessions.back(); child_session.chat_id = child.id; child_session.provider_id = child.provider_id; child_session.running = true;
	setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, child_session, app.chats.back(), "session/new", setup));
	UAM_ASSERT_EQ(app.uam_control_capabilities.size(), std::size_t{2});
	const std::filesystem::path child_directory = app.uam_control_capabilities.back().directory;
	uam::UamControlService::RevokeForChat(app, chat.id);
	UAM_ASSERT(app.uam_control_capabilities.empty());
	UAM_ASSERT(session.uam_control_capability_id.empty());
	UAM_ASSERT(child_session.uam_control_capability_id.empty());
	UAM_ASSERT(!uam::paths::IsDirectoryNoThrow(directory));
	UAM_ASSERT(!uam::paths::IsDirectoryNoThrow(child_directory));
	UAM_ASSERT(!uam::UamControlService::HandleRequestForTests(app, old_capability, {{"requestId", "stale"}, {"method", "_mcp_heartbeat"}, {"arguments", {{"connectionId", "connection"}}}}, now).value("ok", true));
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(FullFrontendStatePushDoesNotReloadAnEntireLongTranscript)
{
	uam::AppState app;
	ChatSession chat; chat.id = "long-history"; chat.provider_id = "codex-cli";
	for (int index = 0; index < 1000; ++index) chat.messages.push_back(Message{MessageRole::User, "historical sentinel " + std::string(1024, 'x')});
	app.chats.push_back(chat); app.selected_chat_index = 0;
	const nlohmann::json push = nlohmann::json::parse(uam::StateUpdateForTests(app));
	UAM_ASSERT_EQ(push["type"], nlohmann::json("stateUpdate"));
	UAM_ASSERT(!push["data"]["chats"][0].contains("messages"));
	UAM_ASSERT_EQ(push["data"]["chats"][0]["messageCount"], nlohmann::json(1000));
	UAM_ASSERT(push.dump().find("historical sentinel") == std::string::npos);
	UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{1000});
}

UAM_TEST(UamControlLaunchIntentHonorsProviderHostOverridesAndChatOptOut)
{
	AppSettings settings;
	for (const std::string provider : {"codex-cli", "claude-cli", "antigravity-cli", "opencode-cli", "gemini-cli", "copilot-cli"})
	{
		ChatSession chat; chat.provider_id = provider; chat.execution_host_id = "local";
		settings.central_provider_configuration.mcp_overrides = {{provider, "local", "uam-mcp", true}};
		UAM_ASSERT(uam::UamControlService::EnabledForChat(settings, chat));
		chat.execution_host_id = "lab";
		UAM_ASSERT(!uam::UamControlService::EnabledForChat(settings, chat));
		chat.execution_host_id = "local"; chat.uam_control_explicitly_disabled = true;
		UAM_ASSERT(!uam::UamControlService::EnabledForChat(settings, chat));
		chat.uam_control_explicitly_disabled = false; chat.uam_control_enabled = true;
		settings.central_provider_configuration.mcp_overrides.front().enabled = false;
		UAM_ASSERT(!uam::UamControlService::EnabledForChat(settings, chat));
		settings.central_provider_configuration.mcp_overrides.clear();
		UAM_ASSERT(uam::UamControlService::EnabledForChat(settings, chat));
	}
}
