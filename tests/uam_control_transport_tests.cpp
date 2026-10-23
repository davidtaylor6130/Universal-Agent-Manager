#include "app/managed_parent_continuation_service.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_internal.h"
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

namespace
{
	struct ControlAgentWaitFixture
	{
		uam_test::TempDir temp{"uam-control-manager-wait"};
		uam_test::ScopedEnvVar codex{"CODEX_HOME", (temp.root / "codex").string()};
		uam_test::ScopedEnvVar claude{"CLAUDE_CONFIG_DIR", (temp.root / "claude").string()};
		uam_test::ScopedEnvVar config{"XDG_CONFIG_HOME", (temp.root / "config").string()};
		uam::AppState app;
		std::string root_id, capability_id;
		fs::path capability_directory;
		ControlAgentWaitFixture()
		{
			app.data_root = temp.root / "data";
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			fs::create_directories(temp.root / "workspace");
			fs::create_directories(app.data_root / "agents");
			UAM_ASSERT(uam::io::WriteTextFile(app.data_root / "agents" / "wait-worker.md",
			    "---\nversion: 1\nname: wait-worker\ndescription: Wait graph fixture\nmode: subagent\nworkspaceAccess: write\nskills: []\ndelegates: [plan]\n---\nInspect the fixture.\n"));
			ChatSession root = ChatDomainService().CreateNewChat("", "codex-cli");
			root_id = root.id;
			root.workspace_directory = (temp.root / "workspace").string();
			root.execution_host_id = "local";
			root.uam_control_enabled = true;
			root.command_safety_tier = "yolo";
			root.approval_mode = "default";
			root.native_session_id = "fixture-native-parent";
			app.chats.push_back(root);
			UAM_ASSERT(uam::UamControlService::Initialize(app));
			app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
			auto& session = *app.acp_sessions.front();
			session.chat_id = root.id;
			session.provider_id = root.provider_id;
			session.process_execution_host_id = root.execution_host_id;
			session.session_id = root.native_session_id;
			session.codex_thread_id = session.session_id;
			session.codex_turn_id = "fixture-native-turn";
			session.last_process_id = "fixture-native-process";
			session.running = session.processing = session.initialized = session.session_ready = true;
			session.turn_serial = 1;
			nlohmann::json setup{{"params", {{"mcpServers", nlohmann::json::array()}}}};
			UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, app.chats.front(), "session/new", setup));
			capability_id = session.uam_control_capability_id;
			capability_directory = app.uam_control_capabilities.front().directory;
			UAM_ASSERT(Call("connection", "_mcp_initialized", {{"connectionId", "manager-test-connection"}}).value("ok", false));
		}
		~ControlAgentWaitFixture() { uam::UamControlService::Shutdown(app); }
		nlohmann::json Call(const std::string& id, const std::string& method, const nlohmann::json& arguments)
		{
			return uam::UamControlService::HandleRequestForTests(app, capability_id,
			    {{"requestId", id}, {"method", method}, {"arguments", arguments}}, uam::time::SystemEpochMillisecondsNow());
		}
		std::string Delegate(const std::string& agent)
		{
			const auto response = Call("delegate-" + agent, "agent_delegate", {{"agentId", agent}, {"task", "Inspect the private fixture."}});
			UAM_ASSERT(response.value("ok", false));
			const std::string id = response.at("result").at("runId");
			UAM_ASSERT(app.uam_control_capabilities.front().owned_agent_run_ids.contains(id));
			return id;
		}
		AgentRun& Run(const std::string& id)
		{
			const auto run = std::ranges::find(app.agent_runs, id, &AgentRun::id);
			UAM_ASSERT(run != app.agent_runs.end());
			return *run;
		}
	};
}

UAM_TEST(UamControlAgentWaitCyclicAncestryRetiresWithoutOwnerHang)
{
	// Contain the pre-fix infinite owner loop using the existing exact-test subprocess entry point.
	// This adds no native fixture mode, provider process, detached thread, or production timeout.
	if (uam::env::GetNonEmptyString("UAM_TEST_WAIT_CYCLE_CHILD") != std::optional<std::string>{"1"})
	{
		uam_test::TempDir temp("uam-control-wait-cycle-watchdog");
		auto& service = PlatformServicesFactory::Instance().process_service;
		uam::platform::StdioProcessPlatformFields process;
		std::string error;
		UAM_ASSERT(service.StartStdioProcess(process, temp.root,
		    {service.ResolveCurrentExecutablePath().string(), "UamControlAgentWaitCyclicAncestryRetiresWithoutOwnerHang"},
		    &error, {{"UAM_TEST_WAIT_CYCLE_CHILD", "1"}}));
		struct Stop
		{
			uam::platform::StdioProcessPlatformFields& process;
			~Stop()
			{
				auto& service = PlatformServicesFactory::Instance().process_service;
				service.StopStdioProcess(process, true);
				service.CloseStdioProcessHandles(process);
			}
		} stop{process};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		std::string diagnostics;
		std::array<char, 4096> buffer{};
		int exit_code = -1;
		while (std::chrono::steady_clock::now() < deadline)
		{
			for (const bool stderr_stream : {false, true})
			{
				const auto size = stderr_stream ? service.ReadStdioProcessStderr(process, buffer.data(), buffer.size(), &error)
				                                : service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &error);
				if (size > 0 && diagnostics.size() < 8192) diagnostics.append(buffer.data(), std::min<std::size_t>(size, 8192 - diagnostics.size()));
			}
			if (service.PollStdioProcessExited(process, &exit_code))
			{
				if (exit_code != 0) throw std::runtime_error("Cycle fixture failed: " + diagnostics);
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		throw std::runtime_error("Cyclic wait ancestry hung the owner test subprocess past its 15-second containment deadline.");
	}

	ControlAgentWaitFixture fixture;
	const std::string ancestor_id = fixture.Delegate("wait-worker");
	fixture.Run(ancestor_id).status = "running";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.Run(ancestor_id)));
	std::string child_id, error;
	UAM_ASSERT(uam::AgentRunScheduler::Enqueue(fixture.app, fixture.root_id, ancestor_id,
	    "plan", "Inspect one descendant.", &child_id, &error));
	UAM_ASSERT(!fixture.app.uam_control_capabilities.front().owned_agent_run_ids.contains(child_id));
	UAM_ASSERT(fixture.Call("pending-cycle-wait", "agent_wait", {{"runId", child_id}}).value("pendingAgentWait", false));
	UAM_ASSERT(fixture.app.uam_control_capabilities.front().pending_agent_wait);
	const auto audit_count = fixture.app.chats.front().uam_control_audit.size();
	// Corrupt only this test graph: a two-node cycle excludes the genuinely granted ancestor.
	AgentRun other = fixture.Run(child_id);
	other.id = "cycle-other";
	other.parent_run_id = child_id;
	fixture.app.agent_runs.push_back(other);
	fixture.Run(child_id).parent_run_id = other.id;
	const auto started = std::chrono::steady_clock::now();
	(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
	UAM_ASSERT(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
	UAM_ASSERT(!fixture.app.uam_control_capabilities.front().pending_agent_wait);
	UAM_ASSERT_EQ(fixture.app.chats.front().uam_control_audit.size(), audit_count);
	UAM_ASSERT(!fs::exists(fixture.capability_directory / "responses" / "pending-cycle-wait.json"));
	UAM_ASSERT_EQ(fixture.Run(child_id).status, std::string("queued"));
	UAM_ASSERT_EQ(fixture.Run(ancestor_id).status, std::string("running"));
	const auto retry_started = std::chrono::steady_clock::now();
	const auto rejected = fixture.Call("cyclic-admission", "agent_wait", {{"runId", child_id}});
	UAM_ASSERT(std::chrono::steady_clock::now() - retry_started < std::chrono::milliseconds(500));
	UAM_ASSERT(!rejected.value("ok", true));
	UAM_ASSERT(!rejected.contains("result"));
	UAM_ASSERT(!fixture.app.uam_control_capabilities.front().pending_agent_wait);
	UAM_ASSERT_EQ(fixture.Run(child_id).parent_run_id, other.id);
	UAM_ASSERT_EQ(fixture.Run(other.id).parent_run_id, child_id);
}

UAM_TEST(UamControlAgentWaitPostReplaceFailureNeverRepublishesConsumedReply)
{
	ControlAgentWaitFixture fixture;
	const std::string child_id = fixture.Delegate("plan");
	fixture.Run(child_id).status = "running";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.Run(child_id)));
	UAM_ASSERT(fixture.Call("post-replace-wait", "agent_wait", {{"runId", child_id}}).value("pendingAgentWait", false));
	fixture.Run(child_id).status = "completed";
	fixture.Run(child_id).result_excerpt = "Exact terminal result before response directory sync.";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.Run(child_id)));
	struct Gate
	{
		std::atomic<int> replacements{0};
		std::atomic<bool> release{false};
	};
	const auto gate = std::make_shared<Gate>();
	struct Release
	{
		std::shared_ptr<Gate> gate;
		~Release() { gate->release.store(true, std::memory_order_release); }
	} release{gate};
	const std::function<bool(uam::io::AtomicWriteStage)> hook = [gate](uam::io::AtomicWriteStage stage)
	{
		if (stage != uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync) return true;
		gate->replacements.fetch_add(1, std::memory_order_release);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!gate->release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return false; // Actual rename already happened; reject its directory-sync completion.
	};
	const fs::path response_path = fixture.capability_directory / "responses" / "post-replace-wait.json";
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (gate->replacements.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::UamControlService::ProcessPendingRequests(fixture.app, hook);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT_EQ(gate->replacements.load(std::memory_order_acquire), 1);
	UAM_ASSERT(fixture.app.uam_control_capabilities.front().pending_agent_wait);
	UAM_ASSERT(fs::exists(response_path));
	const auto first = nlohmann::json::parse(uam::io::ReadTextFile(response_path));
	UAM_ASSERT(first.value("ok", false));
	UAM_ASSERT_EQ(first["result"]["runId"], nlohmann::json(child_id));
	UAM_ASSERT_EQ(first["result"]["status"], nlohmann::json("completed"));
	UAM_ASSERT_EQ(first["result"]["result"], nlohmann::json(fixture.Run(child_id).result_excerpt));
	// Consume the real first response BEFORE publisher harvest, exactly the hazardous ordering.
	UAM_ASSERT(fs::remove(response_path));
	gate->release.store(true, std::memory_order_release);
	const auto retirement_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (fixture.app.uam_control_capabilities.front().pending_agent_wait && std::chrono::steady_clock::now() < retirement_deadline)
	{
		(void)uam::UamControlService::ProcessPendingRequests(fixture.app, hook);
		UAM_ASSERT_EQ(gate->replacements.load(std::memory_order_acquire), 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(!fixture.app.uam_control_capabilities.front().pending_agent_wait);
	UAM_ASSERT_EQ(fixture.app.uam_wait_retirements.size(), std::size_t{1});
	// Longer than the unchanged one-second retry interval, with owner/lease epoch still current.
	const auto observe_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1600);
	while (std::chrono::steady_clock::now() < observe_until)
	{
		(void)uam::UamControlService::ProcessPendingRequests(fixture.app, hook);
		UAM_ASSERT(!fs::exists(response_path));
		UAM_ASSERT_EQ(gate->replacements.load(std::memory_order_acquire), 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(fixture.app.acp_sessions.front()->running && fixture.app.acp_sessions.front()->processing);
	UAM_ASSERT(fixture.app.uam_control_capabilities.front().mcp_initialized);
	UAM_ASSERT_EQ(fixture.app.uam_control_capabilities.front().mcp_connection_id, std::string("manager-test-connection"));
	UAM_ASSERT_EQ(fixture.Run(child_id).status, std::string("completed"));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(std::ranges::count_if(saved->uam_control_audit, [](const auto& record)
	    { return record.method == "agent_wait" && record.request_id == "post-replace-wait"; }), 1);
}


UAM_TEST(UamControlAgentWaitReturnsResultAfterRealSchedulerRetiresChildRuntime)
{
	ControlAgentWaitFixture fixture;
	const std::string child_id = fixture.Delegate("plan");
	auto& child = fixture.Run(child_id);
	const std::string transcript_id = child.transcript_chat_id;
	const std::string result_text = "Original owned child result survives normal scheduler retirement.";
	child.status = "running";
	child.expected_turn_serial = 1;
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, child));
	ChatSession* transcript = ChatDomainService().FindChatById(fixture.app, transcript_id);
	UAM_ASSERT(transcript);
	const auto transcript_birth = transcript->metadata_incarnation;
	transcript->messages.push_back(Message{MessageRole::Assistant, result_text});
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *transcript));
	// Public delegation and real admission establish the original read's authority.
	const std::string request_id = "wait-through-scheduler-retirement";
	UAM_ASSERT(fixture.Call(request_id, "agent_wait", {{"runId", child_id}}).value("pendingAgentWait", false));
	UAM_ASSERT(fixture.app.uam_control_capabilities.front().pending_agent_wait);
	// Settle one provider turn through the existing native-session seam. The scheduler,
	// not this test, must publish terminal status/result and retire the runtime transcript.
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = transcript_id;
	session->managed_agent_run_id = child_id;
	session->provider_id = child.provider_id;
	session->last_settled_turn_serial = 1;
	session->last_turn_outcome = "completed";
	fixture.app.acp_sessions.push_back(std::move(session));
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(fixture.app, uam::time::SystemEpochMillisecondsNow()));
	UAM_ASSERT_EQ(fixture.Run(child_id).status, std::string("completed"));
	UAM_ASSERT_EQ(fixture.Run(child_id).result_excerpt, result_text);
	UAM_ASSERT(!ChatDomainService().FindChatById(fixture.app, transcript_id));
	UAM_ASSERT(!uam::FindAcpSessionForChat(fixture.app, transcript_id));
	UAM_ASSERT(fixture.app.acp_sessions.front()->running && fixture.app.acp_sessions.front()->processing);
	const auto ledger = uam::AgentRunLedger::LoadAll(fixture.app.data_root);
	UAM_ASSERT(ledger.errors.empty());
	UAM_ASSERT_EQ(ledger.runs.size(), std::size_t{1});
	UAM_ASSERT_EQ(ledger.runs.front().status, std::string("completed"));
	UAM_ASSERT_EQ(ledger.runs.front().result_excerpt, result_text);
	UAM_ASSERT(transcript_birth); // Original birth existed; no replacement transcript is fabricated.
	const fs::path response_path = fixture.capability_directory / "responses" / (request_id + ".json");
	(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
	// Normal terminal cleanup must not silently abandon the original admitted read.
	UAM_ASSERT(fixture.app.uam_control_capabilities.front().pending_agent_wait || fs::exists(response_path));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!fs::exists(response_path) && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(fs::exists(response_path));
	const auto response = nlohmann::json::parse(uam::io::ReadTextFile(response_path));
	UAM_ASSERT(response.value("ok", false));
	UAM_ASSERT_EQ(response["result"]["runId"], nlohmann::json(child_id));
	UAM_ASSERT_EQ(response["result"]["status"], nlohmann::json("completed"));
	UAM_ASSERT_EQ(response["result"]["result"], nlohmann::json(result_text));
	UAM_ASSERT_EQ(response["result"]["timedOut"], nlohmann::json(false));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(std::ranges::count_if(saved->uam_control_audit, [&](const auto& record)
	    { return record.method == "agent_wait" && record.request_id == request_id; }), 1);
}


namespace
{
	void RetireWaitFixtureChildThroughScheduler(ControlAgentWaitFixture& fixture, const std::string& child_id)
	{
		auto& run = fixture.Run(child_id);
		run.status = "running";
		run.expected_turn_serial = 1;
		UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
		const std::string transcript_id = run.transcript_chat_id;
		ChatSession* transcript = ChatDomainService().FindChatById(fixture.app, transcript_id);
		UAM_ASSERT(transcript);
		const auto original_birth = transcript->metadata_incarnation;
		transcript->messages.push_back(Message{MessageRole::Assistant, "Exact already-retired owned result."});
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *transcript));
		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = transcript_id;
		session->managed_agent_run_id = child_id;
		session->provider_id = run.provider_id;
		session->last_settled_turn_serial = 1;
		session->last_turn_outcome = "completed";
		fixture.app.acp_sessions.push_back(std::move(session));
		UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(fixture.app, uam::time::SystemEpochMillisecondsNow()));
		UAM_ASSERT_EQ(fixture.Run(child_id).status, std::string("completed"));
		UAM_ASSERT(!ChatDomainService().FindChatById(fixture.app, transcript_id));
		UAM_ASSERT_EQ(fixture.app.hidden_agent_chat_ownership.chats.size(), std::size_t{1});
		UAM_ASSERT(fixture.app.hidden_agent_chat_ownership.chats.front().managed_terminal_retirement_witness);
		UAM_ASSERT_EQ(fixture.app.hidden_agent_chat_ownership.chats.front().managed_terminal_retirement_witness->original_birth, original_birth);
		UAM_ASSERT(fixture.app.acp_sessions.front()->running && fixture.app.acp_sessions.front()->processing);
	}
}

UAM_TEST(UamControlAgentWaitReadsAlreadyCompletedOwnedRetirement)
{
	ControlAgentWaitFixture fixture;
	const std::string child_id = fixture.Delegate("plan");
	RetireWaitFixtureChildThroughScheduler(fixture, child_id);
	UAM_ASSERT(fixture.Call("late-terminal-wait", "agent_wait", {{"runId", child_id}}).value("pendingAgentWait", false));
	const auto response_path = fixture.capability_directory / "responses" / "late-terminal-wait.json";
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!fs::exists(response_path) && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(fs::exists(response_path));
	const auto response = nlohmann::json::parse(uam::io::ReadTextFile(response_path));
	UAM_ASSERT(response.value("ok", false));
	UAM_ASSERT_EQ(response["result"]["runId"], nlohmann::json(child_id));
	UAM_ASSERT_EQ(response["result"]["status"], nlohmann::json("completed"));
	UAM_ASSERT_EQ(response["result"]["result"], nlohmann::json("Exact already-retired owned result."));
	UAM_ASSERT_EQ(response["result"]["timedOut"], nlohmann::json(false));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(std::ranges::count_if(saved->uam_control_audit, [](const auto& record)
	    { return record.method == "agent_wait" && record.request_id == "late-terminal-wait"; }), 1);
}

UAM_TEST(UamControlAgentWaitRejectsUnprovenOrReplacedTerminalRetirement)
{
	for (int mode = 0; mode < 13; ++mode)
	{
		ControlAgentWaitFixture fixture;
		const std::string child_id = fixture.Delegate("plan");
		const std::string transcript_id = fixture.Run(child_id).transcript_chat_id;
		UAM_ASSERT(fixture.Call("guarded-terminal-wait", "agent_wait", {{"runId", child_id}}).value("pendingAgentWait", false));
		const auto audits_before = fixture.app.chats.front().uam_control_audit.size();
		RetireWaitFixtureChildThroughScheduler(fixture, child_id);
		auto& hidden = fixture.app.hidden_agent_chat_ownership.chats;
		if (mode == 0)
		{
			ChatSession replacement;
			replacement.id = transcript_id;
			replacement.agent_run_id = child_id;
			replacement.provider_id = fixture.Run(child_id).provider_id;
			replacement.execution_host_id = "local";
			UAM_ASSERT(replacement.metadata_incarnation != hidden.front().metadata_incarnation);
			fixture.app.chats.push_back(std::move(replacement)); // Old matching hidden birth remains.
		}
		if (mode == 1) hidden.front().managed_terminal_retirement_witness.reset();
		if (mode == 2) hidden.clear();
		if (mode == 3) fixture.Run(child_id).status = "running";
		if (mode == 4) hidden.front().agent_run_id = "foreign-run";
		if (mode == 5) hidden.front().execution_host_id = "foreign-host";
		if (mode == 6) hidden.front().provider_id = "opencode-cli";
		if (mode == 7) hidden.front().created_at += "-foreign";
		if (mode == 8) fixture.app.hidden_agent_chat_ownership.data_root = fixture.temp.root / "foreign-data";
		if (mode == 9) { const auto duplicate = hidden.front(); hidden.push_back(duplicate); }
		if (mode == 10)
		{
			AgentRun duplicate = fixture.Run(child_id);
			duplicate.id = "different-run-same-transcript";
			fixture.app.agent_runs.push_back(std::move(duplicate));
		}
		if (mode == 11)
		{
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, transcript_id);
			UAM_ASSERT(saved.has_value());
			hidden.clear();
			ChatHistorySyncService().RetainHiddenAgentChatOwnership(fixture.app, *saved);
			UAM_ASSERT(!hidden.front().managed_terminal_retirement_witness); // Catalogue cannot mint retirement.
		}
		if (mode == 12) fixture.Run(child_id).root_chat_id = "foreign-root";
		(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
		UAM_ASSERT(!fixture.app.uam_control_capabilities.front().pending_agent_wait);
		UAM_ASSERT(!fs::exists(fixture.capability_directory / "responses" / "guarded-terminal-wait.json"));
		UAM_ASSERT_EQ(fixture.app.chats.front().uam_control_audit.size(), audits_before);
		UAM_ASSERT(fixture.app.acp_sessions.front()->running && fixture.app.acp_sessions.front()->processing);
		// A fresh wait cannot adopt unproven hidden metadata either. A genuinely replaced
		// live row is governed by a new capture, so mode0 tests only the original read.
		if (mode != 0)
			UAM_ASSERT(!fixture.Call("reject-late-lookalike", "agent_wait", {{"runId", child_id}}).value("ok", true));
	}
}


UAM_TEST(UamControlAgentWaitRetirementSurvivesUnrelatedSidebarMerge)
{
	for (int audit_pending = 0; audit_pending < 2; ++audit_pending)
	{
		ControlAgentWaitFixture fixture;
		const std::string child_id = fixture.Delegate("plan");
		UAM_ASSERT(fixture.Call("wait-through-sidebar-refresh", "agent_wait", {{"runId", child_id}}).value("pendingAgentWait", false));
		RetireWaitFixtureChildThroughScheduler(fixture, child_id);
		const auto proof = fixture.app.hidden_agent_chat_ownership.chats.front().managed_terminal_retirement_witness;
		const auto inspection_birth = fixture.app.hidden_agent_chat_ownership.chats.front().metadata_incarnation;
		if (audit_pending)
		{
			// One real manager pass seals the result and schedules its owned audit save;
			// the next pass has not yet advanced the asynchronous publication pipeline.
			(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
			UAM_ASSERT(fixture.app.uam_control_capabilities.front().pending_agent_wait);
			UAM_ASSERT_EQ(std::ranges::count_if(fixture.app.chats.front().uam_control_audit, [](const auto& record)
			    { return record.method == "agent_wait" && record.request_id == "wait-through-sidebar-refresh"; }), 1);
		}
		ChatSession unrelated = ChatDomainService().CreateNewChat("Unrelated native import", "codex-cli");
		unrelated.workspace_directory = (fixture.temp.root / "workspace").string();
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, unrelated));
		ChatHistorySyncService().MergeSidebarChatsPreservingCurrent(fixture.app, {unrelated.id});
		UAM_ASSERT(ChatDomainService().FindChatById(fixture.app, unrelated.id));
		UAM_ASSERT_EQ(fixture.app.hidden_agent_chat_ownership.chats.size(), std::size_t{1});
		const auto& refreshed = fixture.app.hidden_agent_chat_ownership.chats.front();
		UAM_ASSERT(refreshed.metadata_incarnation != inspection_birth); // Fresh inspection metadata stays fresh.
		UAM_ASSERT_EQ(refreshed.managed_terminal_retirement_witness, proof); // Never minted from the new summary.
		const auto response_path = fixture.capability_directory / "responses" / "wait-through-sidebar-refresh.json";
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!fs::exists(response_path) && std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT(fs::exists(response_path));
		const auto response = nlohmann::json::parse(uam::io::ReadTextFile(response_path));
		UAM_ASSERT(response.value("ok", false));
		UAM_ASSERT_EQ(response["result"]["runId"], nlohmann::json(child_id));
		UAM_ASSERT_EQ(response["result"]["status"], nlohmann::json("completed"));
		UAM_ASSERT_EQ(response["result"]["result"], nlohmann::json("Exact already-retired owned result."));
		UAM_ASSERT_EQ(response["result"]["timedOut"], nlohmann::json(false));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(std::ranges::count_if(saved->uam_control_audit, [](const auto& record)
		    { return record.method == "agent_wait" && record.request_id == "wait-through-sidebar-refresh"; }), 1);
	}
}

UAM_TEST(UamControlAgentWaitSidebarMergeRejectsChangedOrUnprovenRetirement)
{
	for (int mode = 0; mode < 11; ++mode)
	{
		ControlAgentWaitFixture fixture;
		const std::string child_id = fixture.Delegate("plan");
		const std::string transcript_id = fixture.Run(child_id).transcript_chat_id;
		UAM_ASSERT(fixture.Call("reject-changed-sidebar-binding", "agent_wait", {{"runId", child_id}}).value("pendingAgentWait", false));
		RetireWaitFixtureChildThroughScheduler(fixture, child_id);
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, transcript_id);
		UAM_ASSERT(saved.has_value());
		ChatSession summary_source = *saved;
		if (mode == 0) summary_source.provider_id = "opencode-cli";
		if (mode == 1) summary_source.execution_host_id = "foreign-host";
		if (mode == 2) summary_source.workspace_directory += "-different";
		if (mode == 3) summary_source.workspace_worktree_directory += "-different";
		if (mode == 4) summary_source.native_session_id = "11111111-2222-4333-8444-555555555555";
		if (mode == 5) summary_source.created_at += "-different";
		if (mode == 6) summary_source.agent_run_id = "foreign-run";
		if (mode == 7) fixture.Run(child_id).definition_snapshot += "-different";
		if (mode == 8) fixture.app.hidden_agent_chat_ownership.chats.front().managed_terminal_retirement_witness.reset();
		if (mode == 9)
		{
			const auto duplicate = fixture.app.hidden_agent_chat_ownership.chats.front();
			fixture.app.hidden_agent_chat_ownership.chats.push_back(duplicate);
		}
		if (mode == 10)
		{
			ChatSession replacement = summary_source;
			replacement.metadata_incarnation = std::make_shared<const std::uint8_t>(0);
			fixture.app.chats.push_back(std::move(replacement));
		}
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, summary_source));
		if (mode == 4)
		{
			const auto readback = ChatRepository::LoadLocalChat(fixture.app.data_root, transcript_id);
			UAM_ASSERT(readback.has_value());
			UAM_ASSERT_EQ(readback->native_session_id, summary_source.native_session_id);
			UAM_ASSERT(readback->native_session_id != saved->native_session_id);
		}
		ChatHistorySyncService().MergeSidebarChatsPreservingCurrent(fixture.app);
		std::cerr << "terminal-retirement sidebar mode=" << mode << '\n';
		UAM_ASSERT(std::ranges::none_of(fixture.app.hidden_agent_chat_ownership.chats, [](const auto& chat)
		    { return bool(chat.managed_terminal_retirement_witness); }));
		const auto audit_count = ChatDomainService().FindChatById(fixture.app, fixture.root_id)->uam_control_audit.size();
		(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
		UAM_ASSERT(!fixture.app.uam_control_capabilities.front().pending_agent_wait);
		UAM_ASSERT(!fs::exists(fixture.capability_directory / "responses" / "reject-changed-sidebar-binding.json"));
		UAM_ASSERT_EQ(ChatDomainService().FindChatById(fixture.app, fixture.root_id)->uam_control_audit.size(), audit_count);
	}
}

#if UAM_ENABLE_RUNTIME_CODEX_CLI
namespace
{
	/// Public accepted delegation, actual repository publication and real scheduler retirement.
	std::string OrdinaryContinuationChild(ControlAgentWaitFixture& fixture)
	{
		auto& root = fixture.app.chats.front();
		auto& session = *fixture.app.acp_sessions.front();
		root.native_session_id = "caac7903-01e1-48e9-bf3f-e75ae01d6cc1";
		session.session_id = session.codex_thread_id = root.native_session_id;
		session.protocol_kind = "codex-app-server";
		Message input{MessageRole::User, "Delegate a bounded child and continue my original task."};
		input.created_at = uam::time::TimestampNow(); root.messages.push_back(std::move(input));
		session.turn_first_user_message_index = session.turn_user_message_index = 0;
		// Drive the real common response dispatcher, not a manufactured continuation proof.
		session.prompt_request_id = 91; session.pending_request_methods[91] = "turn/start";
		uam::acp_detail::HandleAcpResponse(fixture.app, session, root,
		    {{"jsonrpc", "2.0"}, {"id", 91}, {"result", {{"turn", {{"id", session.codex_turn_id}, {"status", "inProgress"}}}}}});
		UAM_ASSERT_EQ(session.codex_origin_rpc_turn_id, session.codex_turn_id);
		UAM_ASSERT_EQ(session.codex_origin_rpc_turn_serial, session.turn_serial);
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, root));
		const auto delegated = fixture.Call("ordinary-continuation-child", "agent_delegate",
		    {{"agentId", "plan"}, {"task", "Return the exact fixture result without tools."}});
		UAM_ASSERT(delegated.value("ok", false));
		UAM_ASSERT_EQ(delegated["result"]["resultDelivery"], nlohmann::json("automatic-pending"));
		const std::string child_id = delegated["result"]["runId"];
		RetireWaitFixtureChildThroughScheduler(fixture, child_id);
		return child_id;
	}
	void EndOrdinaryOrigin(ControlAgentWaitFixture& fixture, const std::string& status = "completed")
	{
		auto& session = *fixture.app.acp_sessions.front();
		auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
		UAM_ASSERT(root);
		ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpHandleMessage(fixture.app, session, *root,
		    {{"method", "turn/completed"}, {"params", {{"threadId", session.codex_thread_id},
		    {"turn", {{"id", session.codex_turn_id}, {"status", status}, {"error", nullptr}}}}}}, nullptr);
	}
	void AssertNoContinuationPacket(ControlAgentWaitFixture& fixture)
	{
		for (unsigned index = 0; index < 8; ++index) (void)uam::ManagedParentContinuationService::Poll(fixture.app);
		UAM_ASSERT(!fixture.app.acp_sessions.front()->pending_prompt_delivery);
		const auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
		UAM_ASSERT(root && root->managed_parent_result);
		UAM_ASSERT(root->managed_parent_result->stage != "admitted" && root->managed_parent_result->stage != "write-unconfirmed");
	}
}

UAM_TEST(ManagedParentContinuationRequiresExactNormalOriginCompletion)
{
	for (int mode = 0; mode < 4; ++mode)
	{
		ControlAgentWaitFixture fixture; (void)OrdinaryContinuationChild(fixture);
		auto& session = *fixture.app.acp_sessions.front();
		if (mode == 0) EndOrdinaryOrigin(fixture, "failed");
		if (mode == 1) { session.cancel_requested = true; EndOrdinaryOrigin(fixture); }
		if (mode == 2) { session.manual_compaction_pending = true; EndOrdinaryOrigin(fixture); }
		if (mode == 3) uam::acp_detail::CompletePromptTurn(session, uam::acp_detail::kAcpLifecycleReady);
		AssertNoContinuationPacket(fixture); // Ready/failed/maintenance/cancel never mints normal completion.
	}
}

UAM_TEST(ManagedParentContinuationNewHumanAndHeldInputsTakePrecedence)
{
	for (int mode = 0; mode < 7; ++mode)
	{
		ControlAgentWaitFixture fixture; (void)OrdinaryContinuationChild(fixture); EndOrdinaryOrigin(fixture);
		auto& session = *fixture.app.acp_sessions.front();
		auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id); UAM_ASSERT(root);
		if (mode == 0)
		{
			// Actual public new input revokes FIRST; an existing transport guard then refuses its wire attempt.
			session.transport_ownership_error = "fixture existing transport guard";
			std::string error; UAM_ASSERT(!uam::SendAcpPrompt(fixture.app, fixture.root_id, "New human work wins.", &error));
			UAM_ASSERT(!error.empty());
		}
		if (mode == 1) root->acp_queue_held = true;
		if (mode == 2) session.queue_held = true;
		if (mode == 3) session.pending_permission.request_id_json = "17";
		if (mode == 4) fixture.app.worktree_operation_chat_ids.insert(fixture.root_id);
		if (mode == 5) root->acp_queue_storage_error = true;
		if (mode == 6) session.pending_user_input.request_id_json = "18";
		const auto messages = root->messages;
		AssertNoContinuationPacket(fixture);
		UAM_ASSERT_EQ(root->messages, messages);
	}
}

UAM_TEST(ManagedParentContinuationRejectsRecreatedOrReboundAuthority)
{
	for (int mode = 0; mode < 10; ++mode)
	{
		ControlAgentWaitFixture fixture; (void)OrdinaryContinuationChild(fixture); EndOrdinaryOrigin(fixture);
		auto& session = *fixture.app.acp_sessions.front();
		auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id); UAM_ASSERT(root);
		if (mode == 0) root->metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (mode == 1) session.permission_review_incarnation = std::make_shared<const char>(0);
		if (mode == 2) session.last_process_id += "-replacement";
		if (mode == 3) session.codex_thread_id = "7857bcef-e364-46b4-8812-48fbc99f2bf1";
		if (mode == 4) root->workspace_directory += "/different";
		if (mode == 5) root->model_id = "different-model";
		if (mode == 6) root->uam_control_explicitly_disabled = true;
		if (mode == 7) fixture.app.managed_subagent_control_incarnation = std::make_shared<const std::uint8_t>(0);
		if (mode == 8) root->managed_parent_result->payload = "Reloaded DATA cannot change the current delivery state.";
		if (mode == 9) fixture.app.settings.provider_availability.push_back({"codex-cli", "local", false});
		AssertNoContinuationPacket(fixture);
		UAM_ASSERT(session.managed_parent_continuation->revoked);
	}
}

UAM_TEST(ManagedParentContinuationPublicationRefusalCreatesNoHumanInput)
{
	ControlAgentWaitFixture fixture; (void)OrdinaryContinuationChild(fixture); EndOrdinaryOrigin(fixture);
	auto& session = *fixture.app.acp_sessions.front();
	auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id); UAM_ASSERT(root);
	const auto original = root->messages;
	const auto primary = fixture.app.data_root / "chats" / (fixture.root_id + ".json");
	const auto retained = primary.string() + ".owned-test-retained";
	fs::rename(primary, retained); fs::create_directory(primary);
	const int before = session.next_request_id;
	(void)uam::ManagedParentContinuationService::Poll(fixture.app);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (session.managed_parent_continuation->admission_pending && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(!session.pending_prompt_delivery && session.next_request_id == before);
	UAM_ASSERT_EQ(root->managed_parent_result->stage, std::string("held"));
	UAM_ASSERT_EQ(root->messages, original);
	fs::remove(primary); fs::rename(retained, primary); // Restore only our exact generated obstruction.
}

UAM_TEST(ManagedParentContinuationUnknownWriteNeverReplaysOrCreatesUnsent)
{
	ControlAgentWaitFixture fixture; (void)OrdinaryContinuationChild(fixture); EndOrdinaryOrigin(fixture);
	auto& session = *fixture.app.acp_sessions.front();
	auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id); UAM_ASSERT(root);
	const auto original = root->messages;
#if defined(__APPLE__) || defined(__linux__)
	UAM_ASSERT(session.stdin_write_fd < 0); // Real platform write fails: no fabricated transport result.
#endif
	UAM_ASSERT(uam::ManagedParentContinuationService::Poll(fixture.app));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while ((session.pending_prompt_delivery || session.managed_parent_continuation->admission_pending) && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(!session.pending_prompt_delivery);
	UAM_ASSERT_EQ(root->managed_parent_result->stage, std::string("held"));
	UAM_ASSERT(session.managed_parent_continuation->revoked);
	const int allocated = session.next_request_id;
	for (unsigned index = 0; index < 20; ++index) (void)uam::ManagedParentContinuationService::Poll(fixture.app);
	UAM_ASSERT_EQ(session.next_request_id, allocated);
	UAM_ASSERT_EQ(root->messages, original);
	UAM_ASSERT(root->acp_queued_prompts.empty() && root->acp_unsent_prompt_recoveries.empty());
}

UAM_TEST(ManagedParentContinuationSavedDataNeverReconstructsRestartAuthority)
{
	ControlAgentWaitFixture fixture; (void)OrdinaryContinuationChild(fixture); EndOrdinaryOrigin(fixture);
	auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id); UAM_ASSERT(root);
	for (const std::string stage : {"collecting", "admitted", "write-unconfirmed", "native-accepted", "settled", "held"})
	{
		root->managed_parent_result->stage = stage;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *root));
		const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
		UAM_ASSERT(loaded && loaded->managed_parent_result && loaded->managed_parent_result->version == 1);
		UAM_ASSERT_EQ(loaded->managed_parent_result->stage, stage);
		uam::AppState restart; restart.data_root = fixture.app.data_root; restart.chats.push_back(*loaded);
		restart.provider_profiles = fixture.app.provider_profiles; restart.settings = fixture.app.settings;
		restart.agent_runs = fixture.app.agent_runs;
		auto runtime = std::make_unique<uam::AcpSessionState>();
		runtime->chat_id = root->id; runtime->provider_id = root->provider_id; runtime->process_execution_host_id = "local";
		runtime->session_id = runtime->codex_thread_id = root->native_session_id;
		runtime->last_process_id = fixture.app.acp_sessions.front()->last_process_id;
		runtime->running = runtime->initialized = runtime->session_ready = true; runtime->lifecycle_state = "ready";
		restart.acp_sessions.push_back(std::move(runtime));
		UAM_ASSERT(!uam::ManagedParentContinuationService::Poll(restart));
		UAM_ASSERT(!restart.acp_sessions.front()->managed_parent_continuation && !restart.acp_sessions.front()->pending_prompt_delivery);
		UAM_ASSERT_EQ(restart.chats.front().messages, loaded->messages);
	}
}
UAM_TEST(ManagedParentContinuationCoalescesReadyChildrenAndDoesNotReplayBatch)
{
	ControlAgentWaitFixture fixture;
	const std::string first = OrdinaryContinuationChild(fixture);
	const std::string second = fixture.Delegate("plan");
	// Same real scheduler FinishRun/EraseRuntimeChat path, now with a second retirement.
	auto& run = fixture.Run(second); run.status = "running"; run.expected_turn_serial = 1;
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	const std::string child_chat_id = run.transcript_chat_id;
	auto* child = ChatDomainService().FindChatById(fixture.app, child_chat_id); UAM_ASSERT(child);
	child->messages.push_back(Message{MessageRole::Assistant, "Second exact child result."});
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *child));
	auto child_session = std::make_unique<uam::AcpSessionState>();
	child_session->chat_id = child_chat_id; child_session->managed_agent_run_id = second; child_session->provider_id = run.provider_id;
	child_session->last_settled_turn_serial = 1; child_session->last_turn_outcome = "completed";
	fixture.app.acp_sessions.push_back(std::move(child_session));
	(void)uam::AgentRunScheduler::Poll(fixture.app);
	UAM_ASSERT_EQ(fixture.Run(second).status, std::string("completed"));
	UAM_ASSERT(!ChatDomainService().FindChatById(fixture.app, child_chat_id));
	EndOrdinaryOrigin(fixture);
	auto& parent = *fixture.app.acp_sessions.front();
	for (unsigned index = 0; index < 4 && !parent.managed_parent_continuation->admission_pending; ++index)
		(void)uam::ManagedParentContinuationService::Poll(fixture.app);
	UAM_ASSERT(parent.managed_parent_continuation->admission_pending);
	const auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id); UAM_ASSERT(root);
	UAM_ASSERT_EQ(root->managed_parent_result->batch_ids, (std::vector<std::string>{first, second}));
	UAM_ASSERT(root->managed_parent_result->payload.find(first) != std::string::npos);
	UAM_ASSERT(root->managed_parent_result->payload.find(second) != std::string::npos);
	const int allocated = parent.next_request_id;
	for (unsigned index = 0; index < 20; ++index) (void)uam::ManagedParentContinuationService::Poll(fixture.app);
	UAM_ASSERT_EQ(parent.next_request_id, allocated); // One pending input, not one new turn per terminal poll/child.
}

#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI
UAM_TEST(ManagedParentContinuationCapacityRefusalPreservesEveryEarlierReceipt)
{
	ControlAgentWaitFixture fixture;
	auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id); UAM_ASSERT(root);
	ManagedParentInputReceipt prior;
	prior.id = "prior-input"; prior.origin_id = "prior-origin"; prior.root_id = root->id;
	prior.provider_id = "codex-cli"; prior.host_id = "local"; prior.workspace = fixture.temp.root.string();
	prior.native_thread = "11111111-2222-4333-8444-555555555555";
	prior.origin_turn = "prior-origin-turn"; prior.payload = "PRIVATE-RETAINED-INPUT-DATA";
	prior.native_request_id = "1"; prior.accepted_turn_id = "prior-accepted-turn"; prior.run_ids = {"prior-child"};
	root->managed_parent_input_receipts = {prior}; root->managed_parent_input_receipts_present = true;
	(void)OrdinaryContinuationChild(fixture); EndOrdinaryOrigin(fixture);
	root = ChatDomainService().FindChatById(fixture.app, fixture.root_id); UAM_ASSERT(root);
	auto& session = *fixture.app.acp_sessions.front();
	const auto primary = fixture.app.data_root / "chats" / (fixture.root_id + ".json");
	const auto before_bytes = fs::file_size(primary);
	constexpr std::uintmax_t limit = 256ULL * 1024ULL * 1024ULL; // Actual canonical serializer agreement.
	UAM_ASSERT(before_bytes + 1024 < limit);
	Message retained{MessageRole::Assistant, std::string(static_cast<std::size_t>(limit - before_bytes - 1024), 'x')};
	root->messages.push_back(std::move(retained));
	UAM_ASSERT(uam::acp_detail::SaveChatQuietly(fixture.app, *root)); // Genuine successful near-capacity full primary.
	const auto retained_size = fs::file_size(primary);
	UAM_ASSERT(retained_size < limit);
	const auto before_receipts = root->managed_parent_input_receipts;
	const auto before_payload = root->managed_parent_result->payload;
	const auto before_batch = root->managed_parent_result->batch_ids;
	const int next_request = session.next_request_id;
	(void)uam::ManagedParentContinuationService::Poll(fixture.app);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (session.managed_parent_continuation->admission_pending && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(!session.managed_parent_continuation->admission_pending);
	UAM_ASSERT(session.managed_parent_continuation->revoked && !session.pending_prompt_delivery);
	UAM_ASSERT_EQ(session.next_request_id, next_request); // First full-save capacity refusal occurs before request allocation/wire.
	UAM_ASSERT_EQ(root->managed_parent_result->stage, std::string("held"));
	UAM_ASSERT_EQ(root->managed_parent_input_receipts, before_receipts);
	UAM_ASSERT_EQ(root->managed_parent_result->payload, before_payload);
	UAM_ASSERT_EQ(root->managed_parent_result->batch_ids, before_batch);
	UAM_ASSERT_EQ(fs::file_size(primary), retained_size); // Existing primary/proofs remain; no eviction to fit.
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id, false);
	UAM_ASSERT(saved && saved->managed_parent_input_receipts == std::vector<ManagedParentInputReceipt>{prior});
	UAM_ASSERT(root->acp_queued_prompts.empty() && root->acp_unsent_prompt_recoveries.empty());
	Message tiny{MessageRole::User, "tiny-human-after-capacity-refusal"}; tiny.created_at = "2026-01-01T00:10:00Z";
	root->messages.push_back(tiny);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *root)); // Never-published RAM DATA must not poison ordinary saves.
	const auto resaved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
	UAM_ASSERT(resaved && resaved->managed_parent_input_receipts == before_receipts);
	UAM_ASSERT_EQ(resaved->messages.back(), tiny);
	UAM_ASSERT_EQ(session.next_request_id, next_request);
}
#endif


namespace
{
	// Public control boundary only: no Poll, ACP pump, native identity, processing
	// flag, process handle, provider command, subprocess fixture or background work.
	struct ControlDelegationPermissionFixture
	{
		uam_test::TempDir temp{"uam-control-delegation-permission"};
		uam::AppState app;
		std::string root_id, capability_id;
		ControlDelegationPermissionFixture(const std::string& approval, const std::string& tier, bool captured_read = false)
		{
			app.data_root = temp.root / "data";
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			fs::create_directories(temp.root / "workspace");
			fs::create_directories(app.data_root / "agents");
			for (const std::string agent : {"policy-worker", "policy-leaf", "policy-other"})
				UAM_ASSERT(uam::io::WriteTextFile(app.data_root / "agents" / (agent + ".md"),
				    "---\nversion: 1\nname: " + agent + "\ndescription: Private permission boundary\nmode: subagent\nworkspaceAccess: write\nskills: []\ndelegates: " + (agent == "policy-worker" ? "[policy-leaf]" : "[]") + "\n---\nInspect the private fixture.\n"));
			ChatSession root = ChatDomainService().CreateNewChat("", "codex-cli", false);
			root_id = root.id;
			root.workspace_directory = (temp.root / "workspace").string();
			root.execution_host_id = "local";
			root.uam_control_enabled = true;
			root.approval_mode = approval;
			root.command_safety_tier = tier;
			root.uam_agent_id = captured_read ? "plan" : "build";
			app.chats.push_back(root);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			UAM_ASSERT(uam::UamControlService::Initialize(app));
			capability_id = Attach(root_id, captured_read ? "read" : "write", root.uam_agent_id, {});
		}
		~ControlDelegationPermissionFixture() { uam::UamControlService::Shutdown(app); }
		std::string Attach(const std::string& chat_id, const std::string& access, const std::string& agent, const std::vector<std::string>& delegates)
		{
			const auto* chat = ChatDomainService().FindChatById(app, chat_id);
			UAM_ASSERT(chat != nullptr);
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = chat_id;
			session->provider_id = chat->provider_id;
			session->running = true; // HasAuthority's in-memory liveness prerequisite; no OS process exists.
			session->active_uam_agent_id = agent;
			session->active_uam_agent_workspace_access = access;
			session->active_uam_agent_delegates = delegates;
			app.acp_sessions.push_back(std::move(session));
			nlohmann::json setup{{"params", {{"mcpServers", nlohmann::json::array()}}}};
			UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, *app.acp_sessions.back(), *chat, "session/new", setup));
			UAM_ASSERT(!app.acp_sessions.back()->uam_control_capability_id.empty());
			return app.acp_sessions.back()->uam_control_capability_id;
		}
		nlohmann::json Call(const std::string& cap, const std::string& id, const std::string& method, const nlohmann::json& args)
		{
			return uam::UamControlService::HandleRequestForTests(app, cap,
			    {{"requestId", id}, {"method", method}, {"arguments", args}}, uam::time::SystemEpochMillisecondsNow());
		}
		std::string Delegate(const std::string& cap, const std::string& request, const std::string& agent)
		{
			const auto response = Call(cap, request, "agent_delegate", {{"agentId", agent}, {"task", "Inspect the private workspace."}});
			UAM_ASSERT(response.value("ok", false));
			UAM_ASSERT(!response.contains("pendingApproval"));
			return response.at("result").at("runId").get<std::string>();
		}
		AgentRun& Run(const std::string& id)
		{
			const auto found = std::ranges::find(app.agent_runs, id, &AgentRun::id);
			UAM_ASSERT(found != app.agent_runs.end());
			return *found;
		}
	};
}

UAM_TEST(UamControlPublicDelegationInheritsPermissionWithoutCommandApproval)
{
	struct Policy { const char* approval; const char* tier; bool captured_read; };
	for (const auto policy : {Policy{"default", "off", false}, Policy{"default", "acceptEdits", false},
	                         Policy{"default", "aiReview", false}, Policy{"default", "yolo", false},
	                         Policy{"plan", "yolo", false}, Policy{"default", "yolo", true}})
	{
		ControlDelegationPermissionFixture fixture(policy.approval, policy.tier, policy.captured_read);
		const auto id = fixture.Delegate(fixture.capability_id, "public-delegate", "policy-worker");
		const AgentRun run = fixture.Run(id);
		const bool read = policy.captured_read || std::string(policy.approval) == "plan";
		UAM_ASSERT_EQ(run.status, std::string("queued"));
		UAM_ASSERT_EQ(run.depth, 1);
		UAM_ASSERT_EQ(run.effective_workspace_access, std::string(read ? "read" : "write"));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, run.transcript_chat_id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->approval_mode, std::string(read ? "plan" : policy.approval));
		UAM_ASSERT_EQ(saved->command_safety_tier, std::string(policy.tier));
		UAM_ASSERT_EQ(saved->workspace_directory, (fixture.temp.root / "workspace").string());
		UAM_ASSERT(saved->native_session_id.empty());
		const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
		UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
		UAM_ASSERT_EQ(ledger.runs.front().id, id);
		UAM_ASSERT_EQ(ledger.runs.front().effective_workspace_access, run.effective_workspace_access);
		const auto root = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
		UAM_ASSERT(root && !root->uam_control_audit.empty());
		UAM_ASSERT_EQ(root->approval_mode, std::string(policy.approval));
		UAM_ASSERT_EQ(root->command_safety_tier, std::string(policy.tier));
		UAM_ASSERT_EQ(root->uam_control_audit.back().method, std::string("agent_delegate"));
		UAM_ASSERT_EQ(root->uam_control_audit.back().result, std::string("ok"));
		UAM_ASSERT_EQ(fixture.app.queued_agent_run_ids.size(), std::size_t{1});
		UAM_ASSERT_EQ(fixture.app.queued_agent_run_ids.front(), id);
		UAM_ASSERT_EQ(fixture.app.acp_sessions.size(), std::size_t{1}); // No child launch or session.
		UAM_ASSERT(!fixture.app.acp_sessions.front()->processing);
		UAM_ASSERT(!fixture.app.uam_control_capabilities.front().pending_approval);
		UAM_ASSERT(fixture.app.uam_control_capabilities.front().owned_agent_run_ids.contains(id));
		UAM_ASSERT(!fixture.Call(fixture.capability_id, "public-delegate", "agent_delegate", {{"agentId", "policy-worker"}, {"task", "Replay."}}).value("ok", true));
		UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{1});
		uam::UamControlService::RevokeForChat(fixture.app, fixture.root_id);
		UAM_ASSERT(!fixture.Call(fixture.capability_id, "revoked-delegate", "agent_delegate", {{"agentId", "policy-worker"}, {"task", "Stale."}}).value("ok", true));
		UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{1});
	}
}

UAM_TEST(UamControlPublicDelegationKeepsGoalCancelAndComputerUseGates)
{
	for (const bool captured_read : {false, true})
	{
		ControlDelegationPermissionFixture fixture("default", "off", captured_read);
		const auto id = fixture.Delegate(fixture.capability_id, "delegate-before-other-gates", "policy-worker");
		UAM_ASSERT(!fixture.Call(fixture.capability_id, "cancel", "agent_cancel", {{"runId", id}}).value("ok", true));
		UAM_ASSERT_EQ(fixture.Run(id).status, std::string("queued"));
		const auto computer = fixture.Call(fixture.capability_id, "computer", "computer_use_request", {{"reason", "Private fixture request"}});
		UAM_ASSERT(!computer.value("ok", true));
		if (captured_read)
			UAM_ASSERT(computer.value("error", "").find("read-only") != std::string::npos);
		UAM_ASSERT(!fixture.app.uam_control_capabilities.front().pending_approval);
		const auto goal = fixture.Call(fixture.capability_id, "goal", "goal_create", {{"objective", "Inspect the fixture."}, {"idempotencyKey", "private-policy-goal"}});
		UAM_ASSERT(fixture.app.chats.front().goals.empty());
		if (captured_read)
		{
			UAM_ASSERT(!goal.value("ok", true));
			UAM_ASSERT(!fixture.app.uam_control_capabilities.front().pending_approval);
		}
		else
		{
			UAM_ASSERT(goal.value("pendingApproval", false));
			UAM_ASSERT(fixture.app.uam_control_capabilities.front().pending_approval);
			UAM_ASSERT_EQ(fixture.app.uam_control_capabilities.front().pending_approval->method, std::string("goal_create"));
		}
	}
}

UAM_TEST(UamControlPublicReadParentDelegationKeepsSavedListAndDepthCeilings)
{
	ControlDelegationPermissionFixture fixture("default", "off", true);
	const auto parent_id = fixture.Delegate(fixture.capability_id, "parent", "policy-worker");
	// Phase setup only: persist the real public acceptance as running, without
	// claiming provider dispatch or using a synthetic processing/native session.
	fixture.Run(parent_id).status = "running";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.Run(parent_id)));
	const AgentRun parent = fixture.Run(parent_id);
	const auto parent_cap = fixture.Attach(parent.transcript_chat_id, parent.effective_workspace_access, parent.agent_id, parent.delegates_snapshot);
	const auto unsaved = fixture.Call(parent_cap, "not-in-saved-list", "agent_delegate", {{"agentId", "policy-other"}, {"task", "Refuse unsaved delegate."}});
	UAM_ASSERT(!unsaved.value("ok", true));
	UAM_ASSERT(unsaved.value("error", "").find("saved definition") != std::string::npos);
	UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{1});
	const auto child_id = fixture.Delegate(parent_cap, "leaf", "policy-leaf");
	const AgentRun child = fixture.Run(child_id);
	UAM_ASSERT_EQ(child.parent_run_id, parent_id);
	UAM_ASSERT_EQ(child.depth, 2);
	UAM_ASSERT_EQ(child.effective_workspace_access, std::string("read"));
	const auto transcript = ChatRepository::LoadLocalChat(fixture.app.data_root, child.transcript_chat_id);
	UAM_ASSERT(transcript && transcript->approval_mode == "plan" && transcript->command_safety_tier == "off");
	fixture.Run(child_id).status = "running";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.Run(child_id)));
	const auto child_cap = fixture.Attach(child.transcript_chat_id, "read", child.agent_id, child.delegates_snapshot);
	const auto too_deep = fixture.Call(child_cap, "depth-three", "agent_delegate", {{"agentId", "policy-worker"}, {"task", "Refuse depth three."}});
	UAM_ASSERT(!too_deep.value("ok", true));
	UAM_ASSERT(too_deep.value("error", "").find("two levels") != std::string::npos);
	UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{2});
}

UAM_TEST(UamControlPublicDelegationSelectsPlanOrBuildWithinParentCeiling)
{
	for (const std::string tier : {"off", "acceptEdits", "aiReview", "yolo"})
		for (const bool captured_read : {false, true})
			for (const std::string role : {"plan", "build"})
			{
				ControlDelegationPermissionFixture fixture("default", tier, captured_read);
				const auto id = fixture.Delegate(fixture.capability_id, "explicit-role", role);
				const AgentRun run = fixture.Run(id);
				const bool read = captured_read || role == "plan";
				UAM_ASSERT_EQ(run.agent_id, role);
				UAM_ASSERT_EQ(run.definition_hash, "builtin-" + role + "-v2");
				UAM_ASSERT_EQ(run.effective_workspace_access, std::string(read ? "read" : "write"));
				const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, run.transcript_chat_id);
				UAM_ASSERT(saved);
				UAM_ASSERT_EQ(saved->uam_agent_id, role);
				UAM_ASSERT_EQ(saved->approval_mode, std::string(read ? "plan" : "default"));
				UAM_ASSERT_EQ(saved->command_safety_tier, tier);
				UAM_ASSERT(saved->native_session_id.empty());
				UAM_ASSERT_EQ(run.status, std::string("queued"));
				UAM_ASSERT_EQ(fixture.app.queued_agent_run_ids.size(), std::size_t{1});
				UAM_ASSERT_EQ(fixture.app.acp_sessions.size(), std::size_t{1});
				UAM_ASSERT(!fixture.app.acp_sessions.front()->processing);
				UAM_ASSERT(!fixture.app.uam_control_capabilities.front().pending_approval);
			}
}

UAM_TEST(UamControlPublicTerminalDelegationRetainsCapturedReadOnlyCeiling)
{
	struct Policy { const char* launched_agent; const char* saved_agent; bool read; };
	for (const auto policy : {Policy{"plan", "build", true}, Policy{"policy-reader", "build", true},
	                         Policy{"plan", "plan", true}, Policy{"build", "build", false}})
	{
		ControlDelegationPermissionFixture fixture("default", "off");
		UAM_ASSERT(uam::io::WriteTextFile(fixture.app.data_root / "agents" / "policy-reader.md",
		    "---\nversion: 1\nname: policy-reader\ndescription: Private captured read role\nmode: both\nworkspaceAccess: read\nskills: []\ndelegates: [policy-worker]\n---\nRead the private fixture.\n"));
		uam::UamControlService::RevokeForChat(fixture.app, fixture.root_id);
		fixture.app.acp_sessions.clear(); // Inert context only; no native process was created.
		auto& root = fixture.app.chats.front();
		root.uam_agent_id = policy.launched_agent;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, root));
		auto terminal = std::make_unique<uam::CliTerminalState>();
		nlohmann::json setup;
		std::string error;
		UAM_ASSERT(uam::UamControlService::PrepareTerminalMcpServer(fixture.app, *terminal, root, setup, error));
		UAM_ASSERT(terminal->uam_control_session != nullptr);
		UAM_ASSERT_EQ(terminal->uam_control_session->active_uam_agent_id, std::string(policy.launched_agent));
		UAM_ASSERT_EQ(terminal->uam_control_session->active_uam_agent_workspace_access, std::string(policy.read ? "read" : "write"));
		terminal->running = true; // Public ownership predicate only; platform handles remain absent.
		const auto capability = terminal->uam_control_session->uam_control_capability_id;
		fixture.app.cli_terminals.push_back(std::move(terminal));
		root.uam_agent_id = policy.saved_agent; // Same saved-agent transition as HandleSetChatUamAgent.
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, root));
		const auto id = fixture.Delegate(capability, "terminal-delegate", "policy-worker");
		const AgentRun run = fixture.Run(id);
		UAM_ASSERT_EQ(run.effective_workspace_access, std::string(policy.read ? "read" : "write"));
		UAM_ASSERT_EQ(run.status, std::string("queued"));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, run.transcript_chat_id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->approval_mode, std::string(policy.read ? "plan" : "default"));
		UAM_ASSERT_EQ(saved->command_safety_tier, std::string("off"));
		UAM_ASSERT(saved->native_session_id.empty());
		UAM_ASSERT(fixture.app.acp_sessions.empty());
		UAM_ASSERT_EQ(fixture.app.cli_terminals.size(), std::size_t{1});
		UAM_ASSERT_EQ(fixture.app.queued_agent_run_ids.size(), std::size_t{1});
	}
}

UAM_TEST(UamControlPublicGoalDiscoverySeparatesDelegationFromMutationApproval)
{
	for (const bool read : {false, true})
	{
		ControlDelegationPermissionFixture fixture("default", "off", read);
		const auto response = fixture.Call(fixture.capability_id, "policy-discovery", "goal_get", nlohmann::json::object());
		UAM_ASSERT(response.value("ok", false));
		const auto& policy = response.at("result");
		UAM_ASSERT(!policy.value("mutationToolsEnabled", true));
		UAM_ASSERT_EQ(policy.value("mutationPolicy", ""), std::string("readOnly"));
		UAM_ASSERT_EQ(policy.value("delegationPolicy", ""), std::string("inheritParentPermissions"));
		const auto id = fixture.Delegate(fixture.capability_id, "discovered-delegate", "policy-worker");
		UAM_ASSERT_EQ(fixture.Run(id).effective_workspace_access, std::string(read ? "read" : "write"));
		UAM_ASSERT(!fixture.Call(fixture.capability_id, "still-no-cancel", "agent_cancel", {{"runId", id}}).value("ok", true));
	}
}
