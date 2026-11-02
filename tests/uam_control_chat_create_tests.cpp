#include "test_harness.h"
#include "app/uam_control_service.h"
#include "cef/state_serializer.h"
#include "common/chat/chat_repository.h"
#include "common/paths/workspace_root.h"
#include "common/config/execution_host_config.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/utils/time_utils.h"

using namespace uam_test;

namespace
{
	/// Caller chat with a live structured session and one issued UAM control capability.
	struct ChatCreateFixture
	{
		TempDir temp{"uam-control-chat-create"};
		uam::AppState app;
		fs::path workspace;
		ChatCreateFixture()
		{
			workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			app.data_root = temp.root / "data";
			fs::create_directories(app.data_root);
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile(), ProviderProfileStore::DefaultClaudeProfile()};
			ChatSession chat;
			chat.id = "caller";
			chat.title = "Planning chat";
			chat.created_at = "2026-11-01 10:00:00";
			chat.provider_id = "codex-cli";
			chat.model_id = "parent-model";
			chat.approval_mode = "acceptEdits";
			chat.command_safety_tier = "yolo";
			chat.execution_host_id = "local";
			chat.workspace_directory = workspace.string();
			chat.uam_control_enabled = true;
			chat.uam_agent_id = "build";
			app.chats.push_back(chat);
			Issue("caller-capability", chat.id);
			Ready("codex-cli");
		}
		void Issue(const std::string& id, const std::string& chat_id, const std::string& run_id = {})
		{
			uam::UamControlCapability capability;
			capability.id = id;
			capability.directory = temp.root / id;
			fs::create_directories(capability.directory / "responses");
			capability.chat_id = chat_id;
			capability.session_chat_id = chat_id;
			capability.provider_id = "codex-cli";
			capability.agent_id = "build";
			capability.agent_run_id = run_id;
			capability.expires_at_epoch_ms = uam::time::SystemEpochMillisecondsNow() + 3600000;
			app.uam_control_capabilities.push_back(capability);
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = chat_id;
			session->provider_id = "codex-cli";
			session->running = true;
			session->uam_control_capability_id = id;
			app.acp_sessions.push_back(std::move(session));
		}
		void Ready(const std::string& provider)
		{
			uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey(provider, "local")];
			version.checked = true;
			version.supported = true;
			version.installed_version = "1.2.3";
			version.execution_host = app.settings.execution_hosts.front();
		}
		nlohmann::json Call(const std::string& request_id, nlohmann::json arguments, const std::string& capability = "caller-capability")
		{
			return uam::UamControlService::HandleRequestForTests(app, capability,
			    {{"requestId", request_id}, {"method", "chat_create"}, {"arguments", std::move(arguments)}}, uam::time::SystemEpochMillisecondsNow());
		}
		const ChatSession* Find(const std::string& id) const
		{
			const auto found = std::ranges::find(app.chats, id, &ChatSession::id);
			return found == app.chats.end() ? nullptr : &*found;
		}
		std::size_t CreatedCount() const
		{
			return static_cast<std::size_t>(std::ranges::count(app.chats, std::string("caller"), &ChatSession::created_by_chat_id));
		}
	};
}

UAM_TEST(UamControlChatCreateInheritsCallerDefaultsAndRecordsProvenance)
{
	ChatCreateFixture fixture;
	const nlohmann::json created = fixture.Call("create-1", {{"task", "Implement the parser."}, {"title", "Parser work"}});
	UAM_ASSERT(created.value("ok", false));
	const std::string chat_id = created["result"].value("chatId", "");
	const ChatSession* chat = fixture.Find(chat_id);
	UAM_ASSERT(chat != nullptr);
	UAM_ASSERT_EQ(chat->title, std::string("Parser work"));
	UAM_ASSERT_EQ(chat->provider_id, std::string("codex-cli"));
	UAM_ASSERT_EQ(chat->model_id, std::string("parent-model"));
	UAM_ASSERT_EQ(chat->approval_mode, std::string("acceptEdits"));
	UAM_ASSERT_EQ(chat->command_safety_tier, std::string("yolo"));
	UAM_ASSERT_EQ(chat->execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(chat->workspace_directory, fixture.workspace.string());
	UAM_ASSERT_EQ(chat->workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
	// An ordinary top-level chat: no subagent, branch or side-chat lineage.
	UAM_ASSERT(chat->agent_run_id.empty());
	UAM_ASSERT(chat->subagent_parent_chat_id.empty());
	UAM_ASSERT(chat->parent_chat_id.empty());
	UAM_ASSERT(chat->temporary_parent_chat_id.empty());
	UAM_ASSERT_EQ(chat->created_by_chat_id, std::string("caller"));
	// The first prompt is handed to the ordinary send path after the request completes.
	UAM_ASSERT_EQ(fixture.app.uam_chat_create_initial_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.app.uam_chat_create_initial_prompts.front().first, chat_id);
	UAM_ASSERT_EQ(fixture.app.uam_chat_create_initial_prompts.front().second, std::string("Implement the parser."));
	// Durable provenance and the React projection.
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat_id, false);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->created_by_chat_id, std::string("caller"));
	const nlohmann::json state = uam::StateSerializer::Serialize(fixture.app, true);
	const auto projected = std::ranges::find_if(state["chats"], [&](const nlohmann::json& value) { return value.value("id", "") == chat_id; });
	UAM_ASSERT(projected != state["chats"].end());
	UAM_ASSERT_EQ(projected->value("createdByChatId", ""), std::string("caller"));
	// Audited on the caller without the prompt body.
	const ChatSession* caller = fixture.Find("caller");
	UAM_ASSERT_EQ(caller->uam_control_audit.back().method, std::string("chat_create"));
	UAM_ASSERT(caller->uam_control_audit.back().reason.find("parser") == std::string::npos);
	// A cross-provider choice without a model uses that provider's default.
	fixture.Ready("claude-cli");
	const nlohmann::json other = fixture.Call("create-2", {{"task", "Review it."}, {"providerId", "claude-cli"}});
	UAM_ASSERT(other.value("ok", false));
	const ChatSession* claude = fixture.Find(other["result"].value("chatId", ""));
	UAM_ASSERT(claude != nullptr);
	UAM_ASSERT_EQ(claude->provider_id, std::string("claude-cli"));
	UAM_ASSERT(claude->model_id.empty());
	UAM_ASSERT_EQ(claude->approval_mode, std::string("acceptEdits"));
}

UAM_TEST(UamControlChatCreateRefusesPlanReadOnlyAndEscalation)
{
	{
		ChatCreateFixture fixture;
		fixture.app.chats.front().approval_mode = "plan";
		const nlohmann::json refused = fixture.Call("plan", {{"task", "Do work."}});
		UAM_ASSERT(!refused.value("ok", true));
		UAM_ASSERT(refused.value("error", "").find("Plan") != std::string::npos);
		UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(fixture.app, "caller").has_value());
		UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{0});
	}
	{
		ChatCreateFixture fixture;
		fixture.app.acp_sessions.front()->active_uam_agent_workspace_access = "read";
		const nlohmann::json refused = fixture.Call("read-only", {{"task", "Do work."}});
		UAM_ASSERT(!refused.value("ok", true));
		UAM_ASSERT(refused.value("error", "").find("read-only") != std::string::npos);
		UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{0});
	}
	{
		ChatCreateFixture fixture;
		fixture.app.uam_control_capabilities.front().agent_id = "plan";
		UAM_ASSERT(!fixture.Call("plan-agent", {{"task", "Do work."}}).value("ok", true));
		UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{0});
	}
	{
		// No argument can request a broader approval mode, host or workspace.
		ChatCreateFixture fixture;
		for (const char* key : {"approvalMode", "commandSafetyTier", "executionHostId", "workspaceDirectory"})
		{
			const nlohmann::json refused = fixture.Call(std::string("escalate-") + key, {{"task", "Do work."}, {key, "yolo"}});
			UAM_ASSERT(!refused.value("ok", true));
		}
		UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{0});
	}
	{
		// A non-YOLO caller waits for the user's explicit approval and inherits its own restricted modes.
		ChatCreateFixture fixture;
		fixture.app.chats.front().command_safety_tier = "off";
		fixture.app.chats.front().approval_mode = "default";
		const nlohmann::json pending = fixture.Call("approval", {{"task", "Do work."}, {"title", "Approved work"}});
		UAM_ASSERT(pending.value("pendingApproval", false));
		UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{0});
		const auto question = uam::UamControlService::PendingApprovalForChat(fixture.app, "caller");
		UAM_ASSERT(question.has_value());
		UAM_ASSERT_EQ(question->questions.front().id, std::string("chatCreate"));
		std::string error;
		UAM_ASSERT(uam::UamControlService::ResolveApproval(fixture.app, "caller", question->request_id_json, {{"chatCreate", {"Allow"}}}, &error));
		UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{1});
		const ChatSession& created = fixture.app.chats.back();
		UAM_ASSERT_EQ(created.title, std::string("Approved work"));
		UAM_ASSERT_EQ(created.approval_mode, std::string("default"));
		UAM_ASSERT_EQ(created.command_safety_tier, std::string("off"));
		const nlohmann::json denied = fixture.Call("approval-denied", {{"task", "Other work."}});
		UAM_ASSERT(denied.value("pendingApproval", false));
		const auto second = uam::UamControlService::PendingApprovalForChat(fixture.app, "caller");
		UAM_ASSERT(second.has_value());
		UAM_ASSERT(uam::UamControlService::ResolveApproval(fixture.app, "caller", second->request_id_json, {{"chatCreate", {"Deny"}}}, &error));
		UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{1});
	}
}

UAM_TEST(UamControlChatCreateBoundsPerTurnTotalAndDepth)
{
	ChatCreateFixture fixture;
	UAM_ASSERT(fixture.Call("turn-1-a", {{"task", "Task A"}}).value("ok", false));
	UAM_ASSERT(fixture.Call("turn-1-b", {{"task", "Task B"}}).value("ok", false));
	const nlohmann::json per_turn = fixture.Call("turn-1-c", {{"task", "Task C"}});
	UAM_ASSERT(!per_turn.value("ok", true));
	UAM_ASSERT(per_turn.value("error", "").find("turn") != std::string::npos);
	int turn = 1;
	int request = 0;
	while (fixture.CreatedCount() < 8)
	{
		fixture.app.acp_sessions.front()->turn_serial = ++turn;
		UAM_ASSERT(fixture.Call("next-" + std::to_string(++request), {{"task", "Task " + std::to_string(request)}}).value("ok", false));
	}
	fixture.app.acp_sessions.front()->turn_serial = ++turn;
	const nlohmann::json total = fixture.Call("over-total", {{"task", "One too many"}});
	UAM_ASSERT(!total.value("ok", true));
	UAM_ASSERT(total.value("error", "").find("at most 8") != std::string::npos);
	UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{8});

	// A chat created by the tool cannot itself create chats.
	const std::string created_id = std::ranges::find(fixture.app.chats, std::string("caller"), &ChatSession::created_by_chat_id)->id;
	fixture.Issue("created-capability", created_id);
	const nlohmann::json chained = fixture.Call("chain", {{"task", "Grandchild"}}, "created-capability");
	UAM_ASSERT(!chained.value("ok", true));
	UAM_ASSERT(chained.value("error", "").find("started by another chat") != std::string::npos);

	// Managed subagents cannot create top-level chats either.
	fixture.Issue("child-capability", "caller", "child-run");
	const nlohmann::json child = fixture.Call("child", {{"task", "From a child"}}, "child-capability");
	UAM_ASSERT(!child.value("ok", true));
	UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{8});
}

UAM_TEST(UamControlChatCreateRetryIsIdempotent)
{
	ChatCreateFixture fixture;
	const nlohmann::json first = fixture.Call("first", {{"task", "Same task"}, {"title", "Same"}});
	UAM_ASSERT(first.value("ok", false));
	const nlohmann::json retry = fixture.Call("retry", {{"task", "Same task"}, {"title", "Same"}});
	UAM_ASSERT(retry.value("ok", false));
	UAM_ASSERT_EQ(retry["result"].value("chatId", ""), first["result"].value("chatId", ""));
	UAM_ASSERT(retry["result"].value("reused", false));
	UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.app.uam_chat_create_initial_prompts.size(), std::size_t{1});
	// A retry does not consume the per-turn allowance.
	UAM_ASSERT(fixture.Call("second", {{"task", "Different task"}}).value("ok", false));
	// Replaying the same transport request ID is still rejected.
	UAM_ASSERT(!fixture.Call("second", {{"task", "Different task"}}).value("ok", true));
}

UAM_TEST(UamControlChatCreateRefusesUnavailableProviders)
{
	ChatCreateFixture fixture;
	// Claude is configured but its CLI has not been checked on this host.
	const nlohmann::json unchecked = fixture.Call("unchecked", {{"task", "Do work."}, {"providerId", "claude-cli"}});
	UAM_ASSERT(!unchecked.value("ok", true));
	UAM_ASSERT(unchecked.value("error", "").find("unavailable") != std::string::npos);
	UAM_ASSERT(!fixture.Call("unknown", {{"task", "Do work."}, {"providerId", "not-a-provider"}}).value("ok", true));
	fixture.app.runtime_cli_versions_by_provider_id.clear();
	UAM_ASSERT(!fixture.Call("caller-provider", {{"task", "Do work."}}).value("ok", true));
	UAM_ASSERT_EQ(fixture.CreatedCount(), std::size_t{0});
	UAM_ASSERT(fixture.app.uam_chat_create_initial_prompts.empty());
}
