#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/platform/platform_services.h"

#include <chrono>
#include <thread>

using namespace uam_test;

namespace
{
	const std::string kPermissionThread = "08f433b8-6172-4ef6-b3b7-6c2846412b17";

	class PermissionWire
	{
	public:
		PermissionWire(uam::AcpSessionState& session, const std::filesystem::path& directory) : m_session(session)
		{
			const std::vector<std::string> sink = StdioCatArgv();
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, directory, sink, &error));
		}
		~PermissionWire()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(m_session, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(m_session);
		}
		nlohmann::json Read()
		{
			IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
			process.CloseStdioProcessInput(m_session);
			std::string wire;
			std::string error;
			char bytes[8192];
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (wire.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline)
			{
				const std::ptrdiff_t count = process.ReadStdioProcessStdout(m_session, bytes, sizeof(bytes), &error);
				if (count > 0) wire.append(bytes, static_cast<std::size_t>(count));
				else std::this_thread::yield();
			}
			UAM_ASSERT(wire.find('\n') != std::string::npos);
			return nlohmann::json::parse(wire);
		}
	private:
		uam::AcpSessionState& m_session;
	};

	nlohmann::json ReviewerNotification(const nlohmann::json& reviewer, const std::string& thread)
	{
		return {{"jsonrpc", "2.0"}, {"method", "thread/settings/updated"},
		    {"params", {{"threadId", thread}, {"threadSettings", {{"approvalsReviewer", reviewer}}}}}};
	}

	nlohmann::json NativeCommandPermission()
	{
		return {{"jsonrpc", "2.0"}, {"id", 71}, {"method", "item/commandExecution/requestApproval"},
		    {"params", {{"threadId", kPermissionThread}, {"itemId", "command"}, {"command", "git status"}, {"cwd", "/fixture"}}}};
	}
}

UAM_TEST(CodexNativeReviewerChoicesApplyToFreshAndResumedThreads)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("codex-cli");
	for (const bool resume : {false, true})
	{
		for (const std::string tier : {"aiReview", "acceptEdits", "off", "yolo"})
		{
			for (const std::string mode : {"default", "plan"})
			{
				ChatSession chat;
				chat.provider_id = "codex-cli";
				chat.command_safety_tier = tier;
				chat.approval_mode = mode;
				if (resume) chat.native_session_id = kPermissionThread;
				std::string method;
				const nlohmann::json request = runtime.OnAcpBuildSetupRequest(1, chat, "/fixture", true, method);
				UAM_ASSERT_EQ(method, resume ? std::string("thread/resume") : std::string("thread/start"));
				UAM_ASSERT_EQ(request["params"].value("approvalsReviewer", ""), tier == "aiReview" && mode != "plan" ? std::string("auto_review") : std::string("user"));
				UAM_ASSERT_EQ(request["params"]["sandbox"], mode == "plan" ? nlohmann::json("read-only") : nlohmann::json("workspace-write"));
				UAM_ASSERT_EQ(request["params"]["approvalPolicy"], nlohmann::json("on-request"));
			}
		}
	}
#endif
}

UAM_TEST(CodexNativeTurnReviewerPreservesManagedReadOnlyBounds)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("codex-cli");
	for (const std::string bound : {"automatic", "plan", "read-agent", "goal-review"})
	{
		ChatSession chat;
		chat.command_safety_tier = "aiReview";
		chat.model_id = "fixture-model";
		chat.workspace_directory = "/fixture";
		chat.approval_mode = bound == "plan" ? "plan" : "default";
		uam::AcpSessionState session;
		session.session_id = kPermissionThread;
		if (bound == "read-agent") session.active_uam_agent_workspace_access = "read";
		session.goal_review_turn = bound == "goal-review";
		std::string method;
		const nlohmann::json request = runtime.OnAcpBuildPrompt(session, 2, "Fixture", chat, method);
		const bool read_only = bound != "automatic";
		UAM_ASSERT_EQ(request["params"].value("approvalsReviewer", ""), read_only ? std::string("user") : std::string("auto_review"));
		UAM_ASSERT_EQ(request["params"]["sandboxPolicy"]["type"], read_only ? nlohmann::json("readOnly") : nlohmann::json("workspaceWrite"));
		UAM_ASSERT_EQ(request["params"]["sandboxPolicy"]["networkAccess"], nlohmann::json(false));
		UAM_ASSERT_EQ(request["params"]["approvalPolicy"], nlohmann::json("on-request"));
		UAM_ASSERT_EQ(request["params"]["collaborationMode"]["mode"], read_only ? nlohmann::json("plan") : nlohmann::json("default"));
	}
#endif
}

UAM_TEST(CodexLifecyclePropagatesAutomaticTierAndReadOnlySetup)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (const std::string phase : {"turn", "read-agent-setup", "goal-review-setup"})
	{
		TempDir temp("uam-codex-permission-wire");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "permission-wire";
		chat.provider_id = "codex-cli";
		chat.command_safety_tier = "aiReview";
		chat.workspace_directory = temp.root.string();
		ChatSession discovery;
		discovery.id = chat.id;
		app.model_discovery_chats.push_back(discovery);
		uam::AcpSessionState session;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = "codex-app-server";
		session.running = true;
		session.initialized = true;
		PermissionWire wire(session, temp.root);
		if (phase == "turn")
		{
			session.session_ready = true;
			session.processing = true;
			session.session_id = kPermissionThread;
			session.queued_prompt = "Fixture";
			UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
		}
		else
		{
			if (phase == "read-agent-setup") session.active_uam_agent_workspace_access = "read";
			session.goal_review_turn = phase == "goal-review-setup";
			UAM_ASSERT(uam::acp_detail::SendSessionSetupIfReady(app, session, chat));
		}
		const nlohmann::json request = wire.Read();
		UAM_ASSERT_EQ(request["params"].value("approvalsReviewer", ""), phase == "turn" ? std::string("auto_review") : std::string("user"));
		if (phase != "turn") UAM_ASSERT_EQ(request["params"]["sandbox"], nlohmann::json("read-only"));
		UAM_ASSERT_EQ(chat.command_safety_tier, std::string("aiReview"));
		UAM_ASSERT(chat.approval_mode != "plan");
	}
#endif
}

UAM_TEST(CodexEffectiveReviewerNotificationsStayThreadScopedAndFailClosed)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	struct Case
	{
		nlohmann::json reviewer;
		std::string thread;
		bool known_thread;
		bool parent_review;
	};
	for (const Case& input : std::vector<Case>{{"auto_review", kPermissionThread, true, false}, {"guardian_subagent", kPermissionThread, true, false},
	         {"user", kPermissionThread, true, true}, {"unsupported", kPermissionThread, true, true}, {true, kPermissionThread, true, true},
	         {"auto_review", "another-thread", true, true}, {"auto_review", "", true, true}, {"auto_review", kPermissionThread, false, true}})
	{
		TempDir temp("uam-codex-effective-reviewer");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "effective-reviewer";
		chat.provider_id = "codex-cli";
		chat.command_safety_tier = "aiReview";
		uam::AcpSessionState session;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = "codex-app-server";
		session.running = true;
		session.session_id = kPermissionThread;
		if (input.known_thread) session.codex_thread_id = kPermissionThread;
		// Settings notifications must be accepted while idle, before the next turn's permission.
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, chat, ReviewerNotification(input.reviewer, input.thread).dump()));
		session.processing = true;
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, chat, NativeCommandPermission().dump()));
		UAM_ASSERT(session.waiting_for_permission);
		UAM_ASSERT(session.last_error.empty());
		UAM_ASSERT_EQ(std::ranges::any_of(session.diagnostics, [](const uam::AcpDiagnosticEntryState& entry) { return entry.event == "permission_review"; }), input.parent_review);
	}
#endif
}

UAM_TEST(CodexNativeResidualEditPermissionCannotBeParentAutoGranted)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-codex-residual-edit");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "residual-codex-edit";
	chat.provider_id = "codex-cli";
	chat.command_safety_tier = "acceptEdits";
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "codex-app-server";
	session.running = true;
	session.processing = true;
	session.session_id = kPermissionThread;
	session.codex_thread_id = kPermissionThread;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, chat, ReviewerNotification("auto_review", kPermissionThread).dump()));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, chat,
	    nlohmann::json{{"jsonrpc", "2.0"}, {"id", 72}, {"method", "item/fileChange/requestApproval"},
	     {"params", {{"threadId", kPermissionThread}, {"itemId", "edit"}, {"reason", "Native approval required"}}}}.dump()));
	UAM_ASSERT(session.waiting_for_permission);
	UAM_ASSERT(session.last_error.empty());
#endif
}

UAM_TEST(CodexSetupReviewerObservationResetsOnFreshInitialize)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-codex-reviewer-reset");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "reviewer-reset";
	chat.provider_id = "codex-cli";
	chat.command_safety_tier = "aiReview";
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "codex-app-server";
	session.running = true;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "thread/start", "1",
	    {{"thread", {{"id", kPermissionThread}}}, {"approvalsReviewer", "auto_review"}}));
	session.processing = true;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, chat, NativeCommandPermission().dump()));
	UAM_ASSERT(!std::ranges::any_of(session.diagnostics, [](const uam::AcpDiagnosticEntryState& entry) { return entry.event == "permission_review"; }));
	session.pending_permission = {};
	session.waiting_for_permission = false;
	session.diagnostics.clear();
	(void)runtime.OnAcpBuildInitialize(session, 2);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, chat, NativeCommandPermission().dump()));
	UAM_ASSERT(session.waiting_for_permission);
	UAM_ASSERT(std::ranges::any_of(session.diagnostics, [](const uam::AcpDiagnosticEntryState& entry) { return entry.event == "permission_review"; }));
#endif
}

UAM_TEST(CodexNativeReviewerWarningsAreScopedAndDoNotLeakSensitiveContent)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (const std::string thread : {kPermissionThread, std::string("another-thread"), std::string{}})
	{
		TempDir temp("uam-codex-review-warning");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "review-warning";
		chat.provider_id = "codex-cli";
		uam::AcpSessionState session;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = "codex-app-server";
		session.running = true;
		session.codex_thread_id = kPermissionThread;
		const std::string warning = "Native reviewer rejected an action outside the task scope.";
		nlohmann::json frame = {{"jsonrpc", "2.0"}, {"method", "guardianWarning"}, {"params", {{"threadId", thread}, {"message", warning}}}};
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, chat, frame.dump()));
		UAM_ASSERT_EQ(session.diagnostics.size(), thread == kPermissionThread ? std::size_t{1} : std::size_t{0});
		if (thread == kPermissionThread)
		{
			UAM_ASSERT_EQ(session.diagnostics.back().reason, std::string("native_review_warning"));
			UAM_ASSERT_EQ(session.diagnostics.back().message, warning);
			frame["params"]["message"] = "https://example.test/mcp?token=fixture-private";
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, chat, frame.dump()));
			UAM_ASSERT_EQ(session.diagnostics.back().reason, std::string("native_review_warning_redacted"));
			UAM_ASSERT(session.diagnostics.back().message.empty());
			UAM_ASSERT(session.diagnostics.back().detail.empty());
			frame["params"]["message"] = std::string(511, 'x') + "\xE2\x82\xAC" + "tail";
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, chat, frame.dump()));
			const std::string encoded = nlohmann::json{{"message", session.diagnostics.back().message}}.dump();
			UAM_ASSERT(!encoded.empty());
		}
		UAM_ASSERT(!session.processing);
		UAM_ASSERT(session.last_error.empty());
		UAM_ASSERT(session.pending_permission.request_id_json.empty());
	}
#endif
}
