#include "test_harness.h"
#include "app/uam_control_service.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "app/provider_model_catalog_service.h"
#include "common/config/execution_host_config.h"
#include "common/utils/shell_escape.h"
#include <chrono>
#include <thread>
#include <limits>
#include <cstdio>
#include <mutex>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
UAM_TEST(AntigravityNativeModelListRejectsMalformedOrDuplicateCatalogues)
{
	const auto parsed = uam::ProviderModelCatalogService::ParseAntigravityModels("gemini-id\tGemini Name\r\nclaude-id\tClaude Name\n");
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->size(), std::size_t{2});
	UAM_ASSERT_EQ((*parsed)[0]["id"], nlohmann::json("gemini-id"));
	UAM_ASSERT_EQ((*parsed)[1]["name"], nlohmann::json("Claude Name"));
	UAM_ASSERT(!(*parsed)[0].contains("supportedReasoningEfforts"));
	for (const std::string output : {"", "warning\nmodel\tName\n", "model\tName\nmodel\tOther\n", "-flag\tName\n", "model\t\n", "model\tName\textra\n", "model\tName\x1b[0m\n"})
		UAM_ASSERT(!uam::ProviderModelCatalogService::ParseAntigravityModels(output).has_value());
	UAM_ASSERT(!uam::ProviderModelCatalogService::ParseAntigravityModels(std::string(256 * 1024 + 1, 'x')).has_value());
	UAM_ASSERT(!uam::ProviderModelCatalogService::ParseAntigravityModels(std::string("model\tInvalid ") + char(0xff)).has_value());
}

UAM_TEST(AntigravitySavedEffortReachesNativeLaunchWithoutInventedOptions)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	const ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
	for (const std::string effort : {"low", "medium", "high", "xhigh", "max"})
	{
		ChatSession chat;
		chat.reasoning_effort = effort;
		for (const auto& argv : {runtime.BuildInteractiveArgv(profile, chat, {}), runtime.BuildStructuredLaunchArgv(profile, chat)})
		{
			const auto flag = std::ranges::find(argv, "--effort");
			UAM_ASSERT(flag != argv.end() && std::next(flag) != argv.end());
			UAM_ASSERT_EQ(*std::next(flag), effort);
		}
	}
	for (const std::string effort : {"none", "ultra", "--dangerously-skip-permissions", ""})
	{
		ChatSession chat;
		chat.reasoning_effort = effort;
		const auto argv = runtime.BuildStructuredLaunchArgv(profile, chat);
		UAM_ASSERT(std::ranges::find(argv, "--effort") == argv.end());
	}
	ChatSession chat;
	chat.reasoning_effort = "high";
	AppSettings settings;
	settings.provider_extra_flags = "--effort low -- literal --effort max";
	UAM_ASSERT_EQ(runtime.BuildInteractiveArgv(profile, chat, settings),
	    (std::vector<std::string>{"agy", "--effort", "high", "--", "literal", "--effort", "max"}));
}

UAM_TEST(AntigravityResumeIdentityNormalizesWhitespaceAndRejectsPassiveReferences)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	ChatSession chat;
	chat.native_session_id = " \t11111111-2222-4333-8444-555555555555\r\n";
	UAM_ASSERT_EQ(runtime.OnAcpValidateResumeId(chat), std::string("11111111-2222-4333-8444-555555555555"));
	for (const std::string invalid : {"antigravity-agent-11111111-2222-4333-8444-555555555555", "gemini-agent-aa-bb-cc", "not-a-conversation", "11111111-2222-4333-8444-555555555555 extra"})
	{
		chat.native_session_id = invalid;
		UAM_ASSERT(runtime.OnAcpValidateResumeId(chat).empty());
	}
}

#if !defined(_WIN32)
UAM_TEST(AntigravityNativeDiscoveryPublishesOnlySuccessfulCurrentScope)
{
	TempDir temp("uam-agy-models");
	ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
	const fs::path script = temp.root / "models.sh";
	UAM_ASSERT(uam::io::WriteTextFile(script, "test \"$1\" = models || exit 9\nprintf 'real-model\\tReal Model\\n'\n"));
	profile.interactive_command = "/bin/sh " + uam::shell::EscapeArg(script.string());
	uam::ProviderModelCatalogService catalog;
	catalog.Initialize(temp.root / "data", {profile});
	const ExecutionHost local = uam::execution_hosts::LocalHost();
	catalog.SetExecutionHostContext({local});
	UAM_ASSERT(catalog.BeginDiscovery(profile.id, temp.root.string()));
	std::string error;
	UAM_ASSERT(catalog.StartAntigravityModelDiscovery(profile, local, temp.root.string(), &error));
	UAM_ASSERT(catalog.StartAntigravityModelDiscovery(profile, local, temp.root.string(), &error));
	UAM_ASSERT_EQ(catalog.ActiveNativeModelDiscoveries(), std::size_t{1});
	const auto settle = [&]()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (catalog.ActiveNativeModelDiscoveries() > 0 && std::chrono::steady_clock::now() < deadline)
		{
			(void)catalog.Poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT_EQ(catalog.ActiveNativeModelDiscoveries(), std::size_t{0});
	};
	settle();
	UAM_ASSERT_EQ(catalog.GetCachedProviderModels(profile.id, temp.root.string())[0]["id"], nlohmann::json("real-model"));
	UAM_ASSERT(uam::io::WriteTextFile(script, "printf 'partial\\tPartial\\n'; exit 7\n"));
	UAM_ASSERT(catalog.BeginDiscovery(profile.id, temp.root.string()));
	UAM_ASSERT(catalog.StartAntigravityModelDiscovery(profile, local, temp.root.string(), &error));
	settle();
	UAM_ASSERT_EQ(catalog.GetCachedProviderModels(profile.id, temp.root.string())[0]["id"], nlohmann::json("real-model"));
	UAM_ASSERT(!catalog.GetProviderRefreshError(profile.id, temp.root.string()).empty());
	UAM_ASSERT(uam::io::WriteTextFile(script, "sleep 0.1\nprintf 'old-scope\\tOld Scope\\n'\n"));
	UAM_ASSERT(catalog.BeginDiscovery(profile.id, temp.root.string()));
	UAM_ASSERT(catalog.StartAntigravityModelDiscovery(profile, local, temp.root.string(), &error));
	profile.interactive_command += " changed-configuration";
	catalog.Initialize(temp.root / "data", {profile});
	settle();
	UAM_ASSERT(catalog.GetCachedProviderModels(profile.id, temp.root.string()).empty());
}
#endif

UAM_TEST(AntigravityStructuredLaunchUsesNativeStreamAndSavedConversation)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	ChatSession chat;
	chat.native_session_id = "11111111-2222-4333-8444-555555555555";
	chat.model_id = "user-model";
	const std::vector<std::string> args = runtime.BuildStructuredLaunchArgv(ProviderProfileStore::DefaultAntigravityProfile(), chat);
	UAM_ASSERT_EQ(args, (std::vector<std::string>{"agy", "--input-format", "stream-json", "--output-format", "stream-json", "--conversation", chat.native_session_id, "--model", chat.model_id}));
	for (const std::string selectors : {"--conversation foreign --continue", "--conversation=foreign -c", "-conversation foreign -continue=true"})
	{
		ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
		profile.interactive_command = "agy " + selectors;
		UAM_ASSERT_EQ(runtime.BuildStructuredLaunchArgv(profile, chat), args);
		ChatSession reviewer = chat;
		reviewer.native_session_id.clear();
		UAM_ASSERT_EQ(runtime.BuildStructuredLaunchArgv(profile, reviewer), (std::vector<std::string>{"agy", "--input-format", "stream-json", "--output-format", "stream-json", "--model", chat.model_id}));
		profile.interactive_command += " -- literal --conversation literal-value";
		auto literal = args;
		literal.insert(literal.end(), {"--", "literal", "--conversation", "literal-value"});
		UAM_ASSERT_EQ(runtime.BuildStructuredLaunchArgv(profile, chat), literal);
	}
	uam::AcpSessionState session;
	UAM_ASSERT(runtime.OnAcpBuildInitialize(session, 1).is_null());
	UAM_ASSERT(session.initialized);
	std::string method;
	UAM_ASSERT(runtime.OnAcpBuildSetupRequest(2, chat, "/tmp", true, method).is_null());
	UAM_ASSERT(runtime.OnAcpBuildCancel(session, 3, method).is_null());
	UAM_ASSERT_EQ(runtime.OnAcpBuildPrompt(session, 4, "hello", chat, method), (nlohmann::json{{"event", "user"}, {"message", {{"content", "hello"}}}}));
}

UAM_TEST(AntigravityStructuredGoalCommandsRefuseBeforeNativeEnvelope)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	ChatSession chat;
	for (const std::string command : {"/goal", "/goal Keep all history", " \t/goal\r\nKeep all history", "/goal\tKeep all history"})
	{
		for (const bool prefixed : {false, true})
		{
			uam::AcpSessionState session;
			session.queued_prompt = prefixed ? command : "";
			session.antigravity_input_tokens = 17;
			session.antigravity_output_tokens = 9;
			session.antigravity_completed_steps = {4};
			std::string method = "stale-method";
			const std::string prompt = prefixed ? "Captured workspace context\n" + command : command;
			UAM_ASSERT(runtime.OnAcpBuildPrompt(session, 4, prompt, chat, method).is_null());
			UAM_ASSERT(method.empty());
			UAM_ASSERT(!session.last_error.empty());
			UAM_ASSERT(session.goal_resume_suppressed);
			UAM_ASSERT_EQ(session.antigravity_input_tokens, std::int64_t{17});
			UAM_ASSERT_EQ(session.antigravity_output_tokens, std::int64_t{9});
			UAM_ASSERT_EQ(session.antigravity_completed_steps.size(), std::size_t{1});
			UAM_ASSERT(session.antigravity_completed_steps.contains(4));
		}
	}
}

UAM_TEST(AntigravityProviderOwnedGoalCannotFallThroughToOrdinaryPrompt)
{
	TempDir temp("uam-antigravity-native-goal-refusal");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	ChatSession chat;
	chat.id = "native-goal-refusal";
	chat.provider_id = "antigravity-cli";
	chat.active_goal_id = "native-goal";
	chat.approval_mode = "plan";
	chat.command_safety_tier = "off";
	Goal goal;
	goal.id = chat.active_goal_id;
	goal.objective = "Preserve history";
	goal.execution_owner = "provider";
	goal.provider_command = "/goal";
	goal.token_budget = 100;
	goal.tokens_used = 20;
	goal.loop_count = 3;
	goal.status = GoalStatus::Paused;
	chat.goals.push_back(goal);
	uam::AppState app;
	app.data_root = temp.root;
	app.chats.push_back(chat);
	for (const bool restart : {false, true})
	{
		std::string error;
		UAM_ASSERT(!uam::acp_detail::ResumeGoal(app, chat.id, goal.id, &error, restart));
		UAM_ASSERT_EQ(error, std::string("Antigravity native Goal control is unavailable in structured Chat. Use a UAM-managed goal for bounded continuation."));
		const ChatSession& retained = app.chats.front();
		UAM_ASSERT_EQ(retained.active_goal_id, chat.active_goal_id);
		UAM_ASSERT_EQ(retained.goal_command_revision, chat.goal_command_revision);
		UAM_ASSERT_EQ(retained.goals.front().status, GoalStatus::Paused);
		UAM_ASSERT_EQ(retained.goals.front().tokens_used, std::int64_t{20});
		UAM_ASSERT_EQ(retained.goals.front().loop_count, 3);
		UAM_ASSERT(retained.messages.empty());
		UAM_ASSERT(app.acp_sessions.empty());
		UAM_ASSERT(fs::is_empty(temp.root));
	}
	uam::AcpSessionState session;
	std::string method;
	UAM_ASSERT(runtime.OnAcpBuildPrompt(session, 4, "Context\n/goal Preserve history", chat, method).is_null());
	UAM_ASSERT_EQ(chat.goals.front().status, GoalStatus::Paused);
	UAM_ASSERT_EQ(chat.goals.front().execution_owner, std::string("provider"));
	UAM_ASSERT_EQ(chat.goals.front().provider_command, std::string("/goal"));
	UAM_ASSERT_EQ(chat.goals.front().token_budget, std::int64_t{100});
	UAM_ASSERT_EQ(chat.goals.front().tokens_used, std::int64_t{20});
	UAM_ASSERT_EQ(chat.goals.front().loop_count, 3);
	UAM_ASSERT(!chat.goals.front().native_binding.has_value());
	UAM_ASSERT_EQ(chat.approval_mode, std::string("plan"));
	UAM_ASSERT_EQ(chat.command_safety_tier, std::string("off"));
}

UAM_TEST(AntigravityUamGoalContinuationRetainsAuthorityAndLiteralMentions)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	UAM_ASSERT(ProviderProfileStore::DefaultAntigravityProfile().native_goal_command.empty());
	ChatSession chat;
	chat.active_goal_id = "uam-goal";
	chat.approval_mode = "plan";
	chat.command_safety_tier = "off";
	Goal goal;
	goal.id = chat.active_goal_id;
	goal.objective = "Document /goal without starting native Goal mode";
	goal.token_budget = 100;
	goal.tokens_used = 20;
	goal.loop_count = 3;
	chat.goals.push_back(goal);
	std::string resume_error;
	// Legacy inferred counters are retained, but cannot prove a remaining native budget.
	UAM_ASSERT(!runtime.OnAcpValidateGoalResume(chat, goal, AppSettings{}, false, resume_error));
	UAM_ASSERT(!resume_error.empty());
	UAM_ASSERT(runtime.OnAcpValidateGoalResume(chat, goal, AppSettings{}, true, resume_error));
	const std::string continuation = uam::GoalService::BuildContinuationPrompt(goal, goal.tokens_used, goal.token_budget);
	UAM_ASSERT(!continuation.empty());
	Goal earlier_native = goal;
	earlier_native.id = "earlier-native-goal";
	earlier_native.execution_owner = "provider";
	earlier_native.provider_command = "/goal";
	earlier_native.status = GoalStatus::Paused;
	chat.goals.push_back(earlier_native);
	for (const std::string prompt : {continuation, std::string("Explain /goal"), std::string("`/goal` is a command"), std::string("/goals"), std::string("/goalkeeper")})
	{
		uam::AcpSessionState session;
		session.queued_prompt = prompt;
		std::string method;
		UAM_ASSERT_EQ(runtime.OnAcpBuildPrompt(session, 4, prompt, chat, method), (nlohmann::json{{"event", "user"}, {"message", {{"content", prompt}}}}));
		UAM_ASSERT(!session.goal_resume_suppressed);
		UAM_ASSERT(method.empty());
		UAM_ASSERT(runtime.OnAcpBuildCancel(session, 5, method).is_null());
	}
	UAM_ASSERT_EQ(chat.goals.front().execution_owner, std::string("uam"));
	UAM_ASSERT_EQ(chat.goals.front().token_budget, std::int64_t{100});
	UAM_ASSERT_EQ(chat.goals.front().tokens_used, std::int64_t{20});
	UAM_ASSERT_EQ(chat.goals.front().loop_count, 3);
	UAM_ASSERT(!chat.goals.front().native_binding.has_value());
	UAM_ASSERT_EQ(chat.approval_mode, std::string("plan"));
	UAM_ASSERT_EQ(chat.command_safety_tier, std::string("off"));
}

UAM_TEST(AntigravityUnknownGoalEventsCannotClaimNativeLifecycleState)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	uam::AppState app;
	ChatSession chat;
	chat.active_goal_id = "uam-goal";
	Goal goal;
	goal.id = chat.active_goal_id;
	goal.token_budget = 100;
	goal.tokens_used = 20;
	goal.status = GoalStatus::Paused;
	chat.goals.push_back(goal);
	uam::AcpSessionState session;
	for (const std::string event : {"goal", "goal_update", "control_response"})
	{
		UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"event", event}, {"goal", {{"status", "complete"}, {"tokensUsed", 0}}}}, nullptr));
		UAM_ASSERT_EQ(chat.goals.front().status, GoalStatus::Paused);
		UAM_ASSERT_EQ(chat.goals.front().tokens_used, std::int64_t{20});
		UAM_ASSERT_EQ(chat.goals.front().execution_owner, std::string("uam"));
		UAM_ASSERT(!chat.goals.front().native_binding.has_value());
		UAM_ASSERT(session.session_id.empty());
		UAM_ASSERT(chat.messages.empty());
	}
}

namespace
{
	/// Actual adapter/services with private files and decoded events, never an executable or native profile.
	struct AntigravityUamGoalFixture
	{
		TempDir temp{"uam-antigravity-goal-data"};
		uam::AppState app;
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
		AntigravityUamGoalFixture(int64_t budget = 100)
		{
			app.data_root = temp.root;
			app.settings.git_enabled = false;
			app.settings.git_auto_worktree = false;
			ChatSession chat;
			chat.id = "antigravity-uam-goal";
			chat.provider_id = "antigravity-cli";
			chat.execution_host_id = "local";
			chat.native_session_id = "11111111-2222-4333-8444-555555555555";
			chat.workspace_directory = temp.root.string();
			chat.active_goal_id = "uam-goal";
			chat.goal_command_revision = "goal-revision";
			Goal goal;
			goal.id = chat.active_goal_id;
			goal.objective = "Verify the bounded native-conversation goal route";
			goal.token_budget = budget;
			chat.goals.push_back(goal);
			app.chats.push_back(chat);
			app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
			auto& session = Session();
			session.chat_id = chat.id;
			session.provider_id = chat.provider_id;
			session.protocol_kind = "antigravity-stream-json";
			session.session_id = chat.native_session_id;
			session.processing = true;
			session.session_ready = true;
			session.turn_serial = 1;
			session.goal_command_revision = chat.goal_command_revision;
			session.queued_prompt = "Continue the explicitly requested goal";
		}
		ChatSession& Chat()
		{
			return app.chats.front();
		}
		Goal& CurrentGoal()
		{
			return Chat().goals.front();
		}
		uam::AcpSessionState& Session()
		{
			return *app.acp_sessions.front();
		}
		void Prepare()
		{
			std::string error;
			UAM_ASSERT(runtime.OnAcpPreparePrompt(app, Session(), Chat(), error));
			std::string method;
			UAM_ASSERT(!runtime.OnAcpBuildPrompt(Session(), 1, Session().queued_prompt, Chat(), method).is_null());
			UAM_ASSERT(!Session().running);
		}
		void Step(int index, nlohmann::json usage, std::string text = "Observed work", std::string kind = "agent_response")
		{
			nlohmann::json payload{{"conversation_id", Session().session_id}, {"step_index", index}, {"state", "DONE"}, {"step_type", kind}, {"text_delta", text}};
			if (!usage.is_null())
				payload["usage"] = std::move(usage);
			UAM_ASSERT(runtime.OnAcpHandleMessage(app, Session(), Chat(), {{"event", "step_update"}, {"step_update", payload}}, nullptr));
		}
		void Result(std::string response = "Observed work", int64_t completed_turns = 9)
		{
			UAM_ASSERT(runtime.OnAcpHandleMessage(app, Session(), Chat(), {{"event", "result"}, {"result", {{"conversation_id", Session().session_id}, {"status", "SUCCESS"}, {"num_turns", completed_turns}, {"response", response}, {"usage", {{"input_tokens", 90000}, {"output_tokens", 10000}}}}}}, nullptr));
		}
	};

	/// A real private file handle satisfies the platform sink boundary; no child process exists.
	struct ControlledNativeInput
	{
		struct Capture { std::mutex mutex; std::string bytes; };
		uam::AcpSessionState& session;
		std::shared_ptr<Capture> capture = std::make_shared<Capture>();
		explicit ControlledNativeInput(uam::AcpSessionState& value) : session(value)
		{
			FILE* file = std::tmpfile();
			UAM_ASSERT(file != nullptr);
#if defined(_WIN32)
			HANDLE handle = INVALID_HANDLE_VALUE;
			UAM_ASSERT(DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file))), GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS));
			session.stdin_write = handle;
#else
			session.stdin_write_fd = dup(fileno(file));
			UAM_ASSERT(session.stdin_write_fd >= 0);
#endif
			std::fclose(file);
			session.stdin_writer = std::make_shared<uam::platform::AsyncByteWriter>(
			    [state = capture](const char* bytes, std::size_t size, std::string&)
			    {
				    std::lock_guard lock(state->mutex);
				    state->bytes.append(bytes, size);
				    return static_cast<std::ptrdiff_t>(size);
			    }, [] {});
		}
		~ControlledNativeInput()
		{
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
			session.running = false;
		}
		std::string Bytes() const { std::lock_guard lock(capture->mutex); return capture->bytes; }
	};

	class UnconfirmedGoalStopService final : public IPlatformProcessService
	{
	  public:
		std::atomic<bool> exited{false};
		mutable std::atomic<int> terminations{0}, legacy_stops{0}, closes{0};
		bool SupportsDetachedProcesses() const override { return false; }
		bool PopulateLocalTime(std::time_t, std::tm*) const override { return false; }
		std::string BuildShellCommandWithWorkingDirectory(const fs::path&, const std::string&) const override { return {}; }
		bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override { return false; }
		int NormalizeCapturedCommandExitCode(int status) const override { return status; }
		ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override { return {}; }
		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&, std::string*, const std::vector<std::pair<std::string,std::string>>&) const override { return false; }
		bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&, std::string_view, std::string*, const std::vector<std::pair<std::string,std::string>>&) const override { return false; }
		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields&) const override { ++closes; }
		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields&, const char*, std::size_t, std::string*) const override { return false; }
		void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields&) const override {}
		void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override { ++terminations; }
		void StopStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override { ++legacy_stops; }
		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override { return -2; }
		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override { return -2; }
		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields&, int* code) const override { if (code) *code = 0; return exited.load(); }
		fs::path ResolveCurrentExecutablePath() const override { return {}; }
		std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const fs::path&, std::string*) const override { return {}; }
		uintmax_t NativeGeminiSessionMaxFileBytes() const override { return 0; }
		std::size_t NativeGeminiSessionMaxMessages() const override { return 0; }
		std::string GenerateUuid() const override { return {}; }
		bool LaunchShellAt(const fs::path&, std::string*) const override { return false; }
		void SetKeepSystemAwake(bool) const override {}
		bool EmbeddedBrowserUsesMockKeychain() const override { return false; }
	};

	void CheckFallbackSteeringAdmission(bool promote)
	{
		AntigravityUamGoalFixture fixture;
		auto& chat = fixture.Chat(); auto& session = fixture.Session();
		chat.provider_id = session.provider_id = "gemini-cli";
		session.protocol_kind = "gemini-acp";
		chat.active_goal_id.clear(); chat.goals.clear();
		chat.memory_enabled = false;
		fixture.app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
		fixture.app.settings.central_provider_configuration.enabled = false;
		fixture.app.settings.central_provider_configuration.uam_control_enabled = false;
		fixture.app.settings.computer_use_globally_enabled = false;
		chat.uam_control_enabled = chat.computer_use_enabled = false;
		chat.messages = {{MessageRole::User, "Original input", "fixture"}, {MessageRole::Assistant, "Interrupted output", "fixture"}};
		session.turn_user_message_index = session.turn_first_user_message_index = 0;
		session.current_assistant_message_index = session.turn_assistant_message_index = 1;
		session.running = session.initialized = session.session_ready = true;
		session.queued_prompt.clear();
		session.prompt_request_id = 10; session.pending_request_methods[10] = "session/prompt";
		session.next_request_id = 20;
		ControlledNativeInput input(session);
		const std::string text = promote ? "Promoted steering reaches actual admission" : "Fresh steering reaches actual admission";
		if (promote)
		{
			uam::AcpQueuedUserPromptState queued; queued.id = "promoted-steer"; queued.text = text;
			session.queued_user_prompts.push_back(queued); chat.acp_queued_prompts.push_back(queued);
		}
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
		std::string error;
		UAM_ASSERT(promote ? uam::SteerQueuedAcpPrompt(fixture.app, chat.id, 0, &error) : uam::SteerAcpPrompt(fixture.app, chat.id, text, {}, {}, false, &error));
		UAM_ASSERT(session.cancel_requested);
		UAM_ASSERT(!session.queue_held && !chat.acp_queue_held);
		// Gemini cancels with a notification; the original prompt's terminal response
		// supplies retirement evidence. Do not invent an RPC acknowledgement.
		UAM_ASSERT_EQ(session.cancel_request_id, 0);
		UAM_ASSERT(uam::ProcessAcpLineForTests(fixture.app, session, chat, R"({"jsonrpc":"2.0","id":10,"result":{"stopReason":"cancelled"}})"));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (input.Bytes().find(text) == std::string::npos && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(fixture.app);
			(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, session, chat);
			std::this_thread::yield();
		}
		UAM_ASSERT(input.Bytes().find(text) != std::string::npos);
		UAM_ASSERT(session.queued_user_prompts.empty());
		UAM_ASSERT(!session.queue_held && !chat.acp_queue_held);
		UAM_ASSERT(session.prompt_request_id != 0 && session.prompt_request_id != 10);
		UAM_ASSERT(std::ranges::any_of(chat.messages, [&](const Message& message) { return message.role == MessageRole::User && message.content == text; }));
	}
} // namespace

UAM_TEST(AntigravityUamGoalAccountsNativeStepsOnceWithoutCumulativeOrTextEstimates)
{
	for (const bool fresh : {false, true})
	{
		AntigravityUamGoalFixture fixture;
		if (fresh)
		{
			fixture.Chat().native_session_id.clear();
			fixture.Session().session_id.clear();
		}
		fixture.Prepare();
		if (fresh)
			UAM_ASSERT(fixture.runtime.OnAcpHandleMessage(fixture.app, fixture.Session(), fixture.Chat(), {{"event", "init"}, {"conversation_id", "11111111-2222-4333-8444-555555555555"}}, nullptr));
		UAM_ASSERT_EQ(fixture.Session().antigravity_goal_turn.native_conversation_id, fixture.Chat().native_session_id);
		fixture.Step(0, nullptr, "", "user_input");
		fixture.Step(1, {{"input_tokens", 12}, {"output_tokens", 3}});
		fixture.Step(1, {{"input_tokens", 12}, {"output_tokens", 3}});
		UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{15});
		fixture.Result();
		UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{15});
		UAM_ASSERT_EQ(fixture.CurrentGoal().last_diagnostic, std::string("antigravity_goal_usage_observed"));
		UAM_ASSERT(fixture.Session().antigravity_goal_turn.settled);
		UAM_ASSERT(fixture.Session().provider_usage.token_usage.last_available);
		const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.Chat().id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->native_session_id, fixture.Chat().native_session_id);
		UAM_ASSERT_EQ(saved->goals.front().tokens_used, int64_t{15});
	}
}

UAM_TEST(AntigravityUamGoalBudgetStopsAtObservedStepAndPreservesNativeResume)
{
	AntigravityUamGoalFixture fixture(10);
	fixture.Prepare();
	fixture.Step(1, {{"input_tokens", 8}, {"output_tokens", 4}});
	UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Blocked);
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{12});
	UAM_ASSERT_EQ(fixture.CurrentGoal().last_blocker_kind, std::string("token_budget"));
	UAM_ASSERT(fixture.Chat().active_goal_id.empty());
	UAM_ASSERT(!fixture.Session().processing);
	UAM_ASSERT(fixture.Session().goal_resume_suppressed);
	UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
	fixture.Step(2, {{"input_tokens", 8}, {"output_tokens", 4}});
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{12});
	const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.Chat().id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT_EQ(saved->goals.front().tokens_used, int64_t{12});
}

UAM_TEST(AntigravityUamGoalUnknownUsageHoldsInsteadOfInventingRemainingBudget)
{
	AntigravityUamGoalFixture fixture;
	fixture.Prepare();
	fixture.Step(1, nullptr);
	fixture.Result();
	UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Blocked);
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{0});
	UAM_ASSERT_EQ(fixture.CurrentGoal().last_diagnostic, std::string("antigravity_goal_usage_unknown"));
	std::string error;
	UAM_ASSERT(!fixture.runtime.OnAcpValidateGoalResume(fixture.Chat(), fixture.CurrentGoal(), fixture.app.settings, false, error));
	// FlushPendingGoalContinuations records its failure text in last_diagnostic.
	// That replacement must preserve the budget hold, including an observed zero.
	fixture.CurrentGoal().last_diagnostic = error;
	UAM_ASSERT(!fixture.runtime.OnAcpValidateGoalResume(fixture.Chat(), fixture.CurrentGoal(), fixture.app.settings, false, error));
	UAM_ASSERT(fixture.runtime.OnAcpValidateGoalResume(fixture.Chat(), fixture.CurrentGoal(), fixture.app.settings, true, error));
}

UAM_TEST(AntigravityUamGoalInterruptedUsageSurvivesRestartWithoutReplayAuthority)
{
	AntigravityUamGoalFixture fixture;
	fixture.Prepare();
	const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.Chat().id);
	UAM_ASSERT(saved.has_value());
	// Reusing a runtime's turn serial for a replacement process must not reuse
	// the previous process's successful preflight and bypass its saved usage hold.
	fixture.runtime.OnAcpBuildInitialize(fixture.Session(), 2);
	std::string replacement_error;
	UAM_ASSERT(!fixture.runtime.OnAcpPreparePrompt(fixture.app, fixture.Session(), fixture.Chat(), replacement_error));
	UAM_ASSERT(!replacement_error.empty());
	uam::AppState restarted;
	restarted.data_root = fixture.temp.root;
	restarted.chats.push_back(*saved);
	UAM_ASSERT_EQ(uam::GoalService::PauseActiveGoalsAfterRestart(restarted), std::size_t{1});
	const Goal& goal = restarted.chats.front().goals.front();
	UAM_ASSERT_EQ(goal.status, GoalStatus::Paused);
	UAM_ASSERT_EQ(goal.last_diagnostic, std::string("antigravity_goal_usage_pending"));
	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(restarted, saved->id, goal.id, &error));
	UAM_ASSERT(restarted.acp_sessions.empty());
	UAM_ASSERT_EQ(restarted.chats.front().native_session_id, saved->native_session_id);
	UAM_ASSERT(restarted.chats.front().messages.empty());
}

UAM_TEST(AntigravityUamGoalPauseCancelsOwnedTurnAndPreservesHeldInput)
{
	AntigravityUamGoalFixture fixture;
	fixture.Prepare();
	uam::AcpQueuedUserPromptState held;
	held.text = "Keep this unrelated authored input held";
	fixture.Session().queued_user_prompts.push_back(held);
	fixture.Chat().acp_queued_prompts.push_back(held);
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(fixture.app, fixture.Chat().id, fixture.CurrentGoal().id, GoalStatus::Paused, true));
	UAM_ASSERT(!fixture.Session().processing);
	UAM_ASSERT(fixture.Session().goal_resume_suppressed);
	UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Paused);
	UAM_ASSERT_EQ(fixture.CurrentGoal().last_diagnostic, std::string("antigravity_goal_usage_unknown"));
	UAM_ASSERT_EQ(fixture.Session().queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT(fixture.Session().queue_held);
	UAM_ASSERT(fixture.Chat().acp_queue_held);
	UAM_ASSERT_EQ(fixture.Chat().acp_queued_prompts.front().text, held.text);
	UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
}

UAM_TEST(AntigravityUamGoalDirectControlsRequireSavedStateBeforeCancelling)
{
	for (int action = 0; action < 3; ++action)
	{
		AntigravityUamGoalFixture fixture;
		Goal replacement;
		replacement.id = "saved-replacement";
		replacement.objective = "Replace only after the control is saved";
		replacement.status = GoalStatus::Paused;
		fixture.Chat().goals.push_back(replacement);
		fixture.Prepare();
		const std::string goal_id = fixture.CurrentGoal().id;
		const std::string original_revision = fixture.Chat().goal_command_revision;
		const fs::path chats = fixture.temp.root / "chats";
		const fs::path retained = fixture.temp.root / "retained-private-chats";
		UAM_ASSERT(fs::is_directory(chats));
		fs::rename(chats, retained);
		UAM_ASSERT(uam::io::WriteTextFile(chats, "Private full-save refusal fixture"));
		const auto control = [&]()
		{
			if (action == 0)
				return uam::GoalService::UpdateGoalStatus(fixture.app, fixture.Chat().id, goal_id, GoalStatus::Paused, true);
			if (action == 1)
				return uam::GoalService::ClearActiveGoal(fixture.app, fixture.Chat().id, true);
			std::string error;
			return uam::GoalService::SetActiveGoal(fixture.app, fixture.Chat().id, replacement.id, &error);
		};
		UAM_ASSERT(!control());
		UAM_ASSERT(fixture.Session().processing);
		UAM_ASSERT(!fixture.Session().running);
		UAM_ASSERT(fixture.app.acp_process_stop_tasks.empty());
		UAM_ASSERT(!fixture.Session().antigravity_goal_turn.settled);
		UAM_ASSERT(fixture.Session().goal_resume_suppressed);
		UAM_ASSERT_EQ(fixture.Chat().active_goal_id, goal_id);
		UAM_ASSERT_EQ(fixture.Chat().goal_command_revision, original_revision);
		UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Active);
		UAM_ASSERT_EQ(fixture.CurrentGoal().last_diagnostic, std::string("antigravity_goal_usage_pending"));
		UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
		UAM_ASSERT(fs::remove(chats));
		fs::rename(retained, chats);
		const auto before_retry = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.Chat().id);
		UAM_ASSERT(before_retry.has_value());
		UAM_ASSERT_EQ(before_retry->active_goal_id, goal_id);
		UAM_ASSERT_EQ(before_retry->goals.front().status, GoalStatus::Active);
		UAM_ASSERT(control());
		UAM_ASSERT(!fixture.Session().processing);
		UAM_ASSERT(fixture.Session().antigravity_goal_turn.settled);
		const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.Chat().id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->goals.front().status, GoalStatus::Paused);
		UAM_ASSERT_EQ(saved->active_goal_id, action == 2 ? replacement.id : std::string{});
		UAM_ASSERT_EQ(saved->native_session_id, fixture.Chat().native_session_id);
	}
}

UAM_TEST(AntigravityUamGoalNativeStopFallbackDurablyHoldsQueuedInput)
{
	for (const bool has_native_session : {false, true})
	{
		AntigravityUamGoalFixture fixture;
		fixture.Prepare();
		uam::AcpQueuedUserPromptState queued;
		queued.id = "held-authored-input";
		queued.text = "Preserve this authored input without dispatch";
		fixture.Session().queued_user_prompts.push_back(queued);
		fixture.Chat().acp_queued_prompts.push_back(queued);
		// Exercise Cancel -> Stop -> finalization with a running-state marker only.
		// Every OS process field remains invalid: no executable starts and no PID/handle can be signalled.
		UAM_ASSERT(!fixture.Session().stdin_writer);
#if defined(_WIN32)
		UAM_ASSERT(fixture.Session().process_info.hProcess == INVALID_HANDLE_VALUE);
		UAM_ASSERT(fixture.Session().process_info.hThread == INVALID_HANDLE_VALUE);
		UAM_ASSERT(fixture.Session().job_object == nullptr);
		UAM_ASSERT(fixture.Session().stdin_write == INVALID_HANDLE_VALUE);
		UAM_ASSERT(fixture.Session().stdout_read == INVALID_HANDLE_VALUE);
		UAM_ASSERT(fixture.Session().stderr_read == INVALID_HANDLE_VALUE);
#else
		UAM_ASSERT_EQ(fixture.Session().child_pid, -1);
		UAM_ASSERT_EQ(fixture.Session().watchdog_pid, -1);
		UAM_ASSERT_EQ(fixture.Session().stdin_write_fd, -1);
		UAM_ASSERT_EQ(fixture.Session().stdout_read_fd, -1);
		UAM_ASSERT_EQ(fixture.Session().stderr_read_fd, -1);
#endif
		fixture.Session().running = true;
		if (!has_native_session)
			fixture.Session().session_id.clear();
		UAM_ASSERT(uam::GoalService::UpdateGoalStatus(fixture.app, fixture.Chat().id, fixture.CurrentGoal().id, GoalStatus::Paused, true));
		UAM_ASSERT_EQ(fixture.app.acp_process_stop_tasks.size(), std::size_t{1});
		fixture.app.acp_process_stop_tasks.front().worker->join();
		UAM_ASSERT(fixture.app.acp_process_stop_tasks.front().finished->load());
		UAM_ASSERT(!fixture.Session().running);
		UAM_ASSERT(!fixture.Session().processing);
		UAM_ASSERT(fixture.Session().queue_held);
		UAM_ASSERT(fixture.Chat().acp_queue_held);
		UAM_ASSERT_EQ(fixture.Session().queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(fixture.Session().queued_user_prompts.front().text, queued.text);
		UAM_ASSERT(fixture.Chat().messages.empty());
		const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.Chat().id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT(saved->acp_queue_held);
		UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(saved->acp_queued_prompts.front().text, queued.text);
		UAM_ASSERT_EQ(saved->native_session_id, fixture.Chat().native_session_id);
		UAM_ASSERT_EQ(saved->goals.front().status, GoalStatus::Paused);
		UAM_ASSERT_EQ(saved->goals.front().last_diagnostic, std::string("antigravity_goal_usage_unknown"));
	}
}

UAM_TEST(AntigravityUamGoalDirectReplacementStopsPriorOwnedTurn)
{
	AntigravityUamGoalFixture fixture;
	fixture.Prepare();
	Goal replacement;
	replacement.id = "replacement";
	replacement.objective = "New explicitly requested goal";
	fixture.Chat().goals.push_back(replacement);
	std::string error;
	UAM_ASSERT(uam::GoalService::SetActiveGoal(fixture.app, fixture.Chat().id, replacement.id, &error));
	UAM_ASSERT(!fixture.Session().processing);
	UAM_ASSERT(fixture.Session().goal_resume_suppressed);
	UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Paused);
	UAM_ASSERT_EQ(fixture.CurrentGoal().last_diagnostic, std::string("antigravity_goal_usage_unknown"));
	UAM_ASSERT_EQ(fixture.Chat().active_goal_id, replacement.id);
	UAM_ASSERT_EQ(fixture.Chat().goals.back().status, GoalStatus::Active);
	UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
}

UAM_TEST(AntigravityGoalCommandRefusalChecksAuthoredBatchBeforeContext)
{
	AntigravityUamGoalFixture fixture;
	Message first;
	first.role = MessageRole::User;
	first.content = "Continue the explicitly requested work";
	Message command = first;
	command.content = "/goal Start a competing native loop";
	fixture.Chat().messages = {first, command};
	fixture.Session().turn_first_user_message_index = 0;
	fixture.Session().turn_user_message_index = 1;
	fixture.Session().queued_prompt = "Captured goal/agent context\nMessage 1: Continue work\nMessage 2: /goal Start a competing native loop";
	std::string error;
	UAM_ASSERT(!fixture.runtime.OnAcpPreparePrompt(fixture.app, fixture.Session(), fixture.Chat(), error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(fixture.CurrentGoal().last_diagnostic.empty());
	UAM_ASSERT(fixture.Session().antigravity_goal_turn.goal_id.empty());
	UAM_ASSERT(fs::is_empty(fixture.temp.root));
	// An earlier command in history cannot block a later ordinary authored turn.
	fixture.Chat().messages = {command, first};
	fixture.Session().turn_first_user_message_index = 1;
	fixture.Session().queued_prompt = "Captured context\nContinue the explicitly requested work";
	UAM_ASSERT(fixture.runtime.OnAcpPreparePrompt(fixture.app, fixture.Session(), fixture.Chat(), error));
}

UAM_TEST(AcpFreshFallbackSteeringRetainsDispatchAfterCancellation)
{
	CheckFallbackSteeringAdmission(false);
}

UAM_TEST(AcpPromotedFallbackSteeringRetainsDispatchAfterCancellation)
{
	CheckFallbackSteeringAdmission(true);
}

UAM_TEST(AntigravityPrivateReviewerRepairRetainsIdentityAndAccountsUsage)
{
	AntigravityUamGoalFixture fixture;
	auto& session = fixture.Session();
	session.goal_internal_session = true;
	session.session_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
	session.goal_review_turn = true;
	session.goal_turn_kind = std::string(uam::acp_detail::kGoalTurnKindReview);
	session.goal_review_goal_id = fixture.CurrentGoal().id;
	session.current_mode_id = "plan";
	fixture.app.settings.central_provider_configuration.enabled = false;
	fixture.app.settings.central_provider_configuration.uam_control_enabled = false;
	fixture.app.settings.computer_use_globally_enabled = false;
	fixture.Chat().uam_control_enabled = fixture.Chat().computer_use_enabled = false;
	fixture.Chat().memory_enabled = false;
	fixture.Prepare();
	ControlledNativeInput input(session);
	session.running = session.initialized = true;
	session.queued_prompt.clear();
	fixture.Step(1, {{"input_tokens",10},{"output_tokens",5}}, "not valid review JSON");
	fixture.Result("not valid review JSON");
	UAM_ASSERT(session.goal_internal_session);
	UAM_ASSERT(session.goal_review_turn && session.goal_review_repair_attempts == 1);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (input.Bytes().find("only repair attempt") == std::string::npos && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(fixture.app);
		(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, session, fixture.Chat());
		std::this_thread::yield();
	}
	UAM_ASSERT(input.Bytes().find("only repair attempt") != std::string::npos);
	UAM_ASSERT_EQ(session.session_id, std::string("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"));
	UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
	UAM_ASSERT_EQ(session.current_mode_id, std::string("plan"));
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{15});
	const std::string complete = R"({"decision":"complete","reason":"Repaired bounded review","evidence":["Private repair delivered and verified"]})";
	fixture.Step(1, {{"input_tokens",2},{"output_tokens",3}}, complete);
	fixture.Result(complete, 10);
	UAM_ASSERT(session.antigravity_goal_turn.settled);
	UAM_ASSERT_EQ(session.antigravity_completed_turns, int64_t{10});
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{20});
	UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Complete);
	const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.Chat().id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
	UAM_ASSERT_EQ(saved->goals.front().tokens_used, int64_t{20});
}

UAM_TEST(AntigravityLocalGoalRestartRequiresObservedProcessExit)
{
	AntigravityUamGoalFixture fixture;
	fixture.Prepare(); fixture.Step(1, {{"input_tokens",12},{"output_tokens",3}});
	fixture.CurrentGoal().loop_count = 2;
	fixture.CurrentGoal().current_step = "Retain metered progress until actual exit";
	uam::FlushPendingChatSaves(fixture.app);
	fixture.Session().running = true;
	fixture.Session().queued_prompt.clear();
	auto service = std::make_shared<UnconfirmedGoalStopService>();
	UAM_ASSERT(!uam::StopAcpSession(fixture.app, fixture.Chat().id, uam::AcpStopPurpose::Interrupt, true, service));
	UAM_ASSERT_EQ(fixture.app.acp_process_stop_tasks.size(), std::size_t{1});
	auto& task = fixture.app.acp_process_stop_tasks.front();
	UAM_ASSERT(task.observe_exit && fixture.Session().local_stop_pending);
	task.worker->join();
	UAM_ASSERT(task.finished->load() && !task.result->exit_confirmed);
	UAM_ASSERT_EQ(service->terminations.load(), 1);
	UAM_ASSERT_EQ(service->legacy_stops.load(), 0);
	UAM_ASSERT_EQ(service->closes.load(), 0);
	UAM_ASSERT(!uam::PollCompletedAcpProcessStops(fixture.app, nullptr));
	UAM_ASSERT(uam::AcpStopInProgress(fixture.app, fixture.Chat().id));
	const std::string revision = fixture.Chat().goal_command_revision;
	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(fixture.app, fixture.Chat().id, fixture.CurrentGoal().id, &error, true));
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{15});
	UAM_ASSERT_EQ(fixture.CurrentGoal().loop_count, 2);
	UAM_ASSERT_EQ(fixture.Chat().goal_command_revision, revision);
	UAM_ASSERT_EQ(fixture.CurrentGoal().current_step, std::string("Retain metered progress until actual exit"));
	service->exited.store(true);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!fixture.app.acp_process_stop_tasks.empty() && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(fixture.app);
		(void)uam::PollCompletedAcpProcessStops(fixture.app, nullptr);
		std::this_thread::yield();
	}
	UAM_ASSERT(fixture.app.acp_process_stop_tasks.empty());
	UAM_ASSERT(!uam::AcpStopInProgress(fixture.app, fixture.Chat().id));
	UAM_ASSERT_EQ(service->closes.load(), 1);
	UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{15});
	UAM_ASSERT_EQ(fixture.CurrentGoal().loop_count, 2);
}

UAM_TEST(AntigravityUamGoalReviewerUsageAndCompletionRemainUamOwned)
{
	AntigravityUamGoalFixture fixture;
	fixture.Session().goal_review_turn = true;
	fixture.Session().goal_turn_kind = std::string(uam::acp_detail::kGoalTurnKindReview);
	fixture.Session().goal_review_goal_id = fixture.CurrentGoal().id;
	fixture.Prepare();
	const std::string review = R"({"decision":"complete","reason":"Verified the requested scope","evidence":["All bounded fixture assertions passed"]})";
	fixture.Step(1, {{"input_tokens", 10}, {"output_tokens", 5}}, review);
	fixture.Result(review);
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{15});
	UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Complete);
	UAM_ASSERT_EQ(fixture.CurrentGoal().execution_owner, std::string("uam"));
	UAM_ASSERT(!fixture.CurrentGoal().native_binding.has_value());
}

UAM_TEST(AntigravityUamGoalResumeChecksIterationAndNativeIdentityBeforeMutation)
{
	for (const bool invalid_identity : {false, true})
	{
		AntigravityUamGoalFixture fixture;
		fixture.Session().processing = false;
		fixture.CurrentGoal().status = GoalStatus::Paused;
		fixture.Chat().active_goal_id.clear();
		if (invalid_identity)
			fixture.Chat().native_session_id = "unverified-id";
		else
			fixture.CurrentGoal().loop_count = fixture.app.settings.goal_max_loop_iterations;
		std::string error;
		UAM_ASSERT(!uam::acp_detail::ResumeGoal(fixture.app, fixture.Chat().id, fixture.CurrentGoal().id, &error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Paused);
		UAM_ASSERT(fixture.Chat().active_goal_id.empty());
		UAM_ASSERT(fs::is_empty(fixture.temp.root));
	}
}

UAM_TEST(AntigravityUamGoalRestartWaitsForMeteredTurnRetirement)
{
	for (const bool stop_unconfirmed : {false, true})
	{
		AntigravityUamGoalFixture fixture;
		fixture.Prepare();
		fixture.Step(1, {{"input_tokens", 12}, {"output_tokens", 3}});
		fixture.CurrentGoal().loop_count = 2;
		fixture.CurrentGoal().current_step = "Retain the owned iteration";
		if (stop_unconfirmed)
		{
			fixture.Session().processing = false;
			fixture.Session().queued_prompt.clear();
			fixture.Session().remote_stop_unconfirmed = true;
		}
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.Chat()));
		const std::string revision = fixture.Chat().goal_command_revision;
		std::string error;
		UAM_ASSERT(!uam::acp_detail::ResumeGoal(fixture.app, fixture.Chat().id, fixture.CurrentGoal().id, &error, true));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{15});
		UAM_ASSERT_EQ(fixture.CurrentGoal().loop_count, 2);
		UAM_ASSERT_EQ(fixture.CurrentGoal().current_step, std::string("Retain the owned iteration"));
		UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Active);
		UAM_ASSERT_EQ(fixture.Chat().goal_command_revision, revision);
		UAM_ASSERT_EQ(fixture.Session().processing, !stop_unconfirmed);
		const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.Chat().id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->goals.front().tokens_used, int64_t{15});
		UAM_ASSERT_EQ(saved->goals.front().loop_count, 2);
	}
}

UAM_TEST(AntigravityUamGoalUsageSaveFailureRefusesBeforeEnvelopePreparation)
{
	AntigravityUamGoalFixture fixture;
	const fs::path blocked = fixture.temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "Private save-failure fixture"));
	fixture.app.data_root = blocked;
	std::string error;
	UAM_ASSERT(!fixture.runtime.OnAcpPreparePrompt(fixture.app, fixture.Session(), fixture.Chat(), error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(fixture.Session().antigravity_goal_turn.goal_id.empty());
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{0});
	UAM_ASSERT(fixture.CurrentGoal().last_diagnostic.empty());
	UAM_ASSERT(!fixture.Session().running);
}

UAM_TEST(AntigravityUamGoalOldOwnerCannotChargeRecreatedGoal)
{
	for (const bool changed_identity : {false, true})
	{
		AntigravityUamGoalFixture fixture;
		fixture.Prepare();
		if (changed_identity)
			fixture.Chat().native_session_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
		else
			fixture.Chat().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		fixture.CurrentGoal().tokens_used = 7;
		fixture.Step(1, {{"input_tokens", 12}, {"output_tokens", 3}});
		UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{7});
		UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Active);
		UAM_ASSERT_EQ(fixture.Session().antigravity_goal_turn.accounted_tokens, int64_t{0});
		UAM_ASSERT(fixture.Session().antigravity_goal_turn.settled);
		UAM_ASSERT(fixture.Session().goal_resume_suppressed);
		UAM_ASSERT(!fixture.Session().processing);
		std::string error;
		UAM_ASSERT(!fixture.runtime.OnAcpPreparePrompt(fixture.app, fixture.Session(), fixture.Chat(), error));
		UAM_ASSERT(!error.empty());
	}
	// Chat copies intentionally retain metadata_incarnation. A different owner ID
	// must still obtain its own durable preflight, even if all copied fields match.
	AntigravityUamGoalFixture fixture;
	fixture.Prepare();
	ChatSession copied = fixture.Chat();
	copied.id = "copied-goal-owner";
	fixture.app.chats.push_back(copied);
	fixture.Chat().goal_owner_chat_id = copied.id;
	std::string error;
	UAM_ASSERT(!fixture.runtime.OnAcpPreparePrompt(fixture.app, fixture.Session(), fixture.Chat(), error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(fixture.Session().antigravity_goal_turn.owner_chat_id, fixture.Chat().id);
	UAM_ASSERT_EQ(fixture.app.chats.back().goals.front().last_diagnostic, std::string("antigravity_goal_usage_pending"));
}

UAM_TEST(AntigravityUamGoalRolledBackControlReconcilesObservedTurnUsage)
{
	AntigravityUamGoalFixture fixture;
	fixture.Prepare();
	const std::string original_revision = fixture.Chat().goal_command_revision;
	fixture.Chat().goal_command_revision = "pending-pause-revision";
	fixture.Step(1, {{"input_tokens", 12}, {"output_tokens", 3}});
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{0});
	// The failed control restores its prior revision before this turn's native result.
	fixture.Chat().goal_command_revision = original_revision;
	fixture.Result();
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{15});
	UAM_ASSERT_EQ(fixture.CurrentGoal().last_diagnostic, std::string("antigravity_goal_usage_observed"));
	UAM_ASSERT(fixture.Session().antigravity_goal_turn.settled);
}

UAM_TEST(AntigravityUamGoalCommittedPauseRetainsKnownUsageFromCompletedTurn)
{
	AntigravityUamGoalFixture fixture;
	fixture.Prepare();
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(fixture.app, fixture.Chat().id, fixture.CurrentGoal().id, GoalStatus::Paused, true, true));
	fixture.Step(1, {{"input_tokens", 12}, {"output_tokens", 3}});
	fixture.Result();
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{0});
	UAM_ASSERT(!fixture.Session().processing);
	std::string error;
	UAM_ASSERT(fixture.runtime.OnAcpGoalControlCommitted(fixture.app, fixture.Session(), fixture.Chat(), fixture.CurrentGoal().id, error));
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{15});
	UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Paused);
	UAM_ASSERT_EQ(fixture.CurrentGoal().last_diagnostic, std::string("antigravity_goal_usage_observed"));
	UAM_ASSERT(fixture.runtime.OnAcpValidateGoalResume(fixture.Chat(), fixture.CurrentGoal(), fixture.app.settings, false, error));
}

UAM_TEST(AntigravityNativeLaunchModesPreserveCliPolicyAndIdentity)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	const ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
	ChatSession chat;
	chat.native_session_id = "11111111-2222-4333-8444-555555555555";
	chat.model_id = "private-model";
	AppSettings settings;
	settings.provider_extra_flags = "--effort high";
	for (const std::string mode : {"default", "unknown", "plan", "acceptEdits"})
	{
		chat.approval_mode = mode;
		std::vector<std::string> terminal = {"agy", "--conversation", chat.native_session_id, "--model", chat.model_id, "--effort", "high"};
		std::vector<std::string> structured = {"agy", "--input-format", "stream-json", "--output-format", "stream-json", "--conversation", chat.native_session_id, "--model", chat.model_id};
		if (mode == "plan" || mode == "acceptEdits")
		{
			const std::string native_mode = mode == "plan" ? "plan" : "accept-edits";
			terminal.insert(terminal.end(), {"--mode", native_mode});
			structured.insert(structured.end(), {"--mode", native_mode});
		}
		UAM_ASSERT_EQ(runtime.BuildInteractiveArgv(profile, chat, settings), terminal);
		UAM_ASSERT_EQ(runtime.BuildStructuredLaunchArgv(profile, chat), structured);
	}
}

UAM_TEST(AntigravityPlanLaunchCannotBeWeakenedByExplicitExpertFlags)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	for (const std::string flags : {"--mode accept-edits --dangerously-skip-permissions --effort high", "--mode=accept-edits --dangerously-skip-permissions=true --effort high", "-mode accept-edits -dangerously-skip-permissions --effort high", "--mode --effort high", "-mode -dangerously-skip-permissions --effort high"})
	{
		ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
		ChatSession chat;
		chat.approval_mode = "plan";
		AppSettings settings;
		settings.provider_extra_flags = flags + " -- literal --mode accept-edits --dangerously-skip-permissions";
		UAM_ASSERT_EQ(runtime.BuildInteractiveArgv(profile, chat, settings),
		    (std::vector<std::string>{"agy", "--effort", "high", "--mode", "plan", "--", "literal", "--mode", "accept-edits", "--dangerously-skip-permissions"}));
		profile.interactive_command = "agy " + flags;
		UAM_ASSERT_EQ(runtime.BuildStructuredLaunchArgv(profile, chat),
		    (std::vector<std::string>{"agy", "--effort", "high", "--input-format", "stream-json", "--output-format", "stream-json", "--mode", "plan"}));
	}
	ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
	ChatSession ordinary;
	ordinary.approval_mode = "default";
	AppSettings expert;
	expert.provider_extra_flags = "--dangerously-skip-permissions --mode accept-edits";
	UAM_ASSERT_EQ(runtime.BuildInteractiveArgv(profile, ordinary, expert),
	    (std::vector<std::string>{"agy", "--dangerously-skip-permissions", "--mode", "accept-edits"}));
}

UAM_TEST(AntigravityCapturedNativeStreamPersistsIdentityAndAvoidsDuplicateAnswer)
{
	TempDir temp("uam-agy-stream");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-stream";
	chat.provider_id = "antigravity-cli";
	app.chats.push_back(chat);
	ChatSession& active = app.chats.front();
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.chat_id = chat.id;
	session.running = true;
	session.processing = true;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	active.messages.push_back({MessageRole::Assistant, "AGY_PROTOCOL_20261007\n", "earlier"});
	active.messages.push_back({MessageRole::User, "Repeat the marker", "now"});
	session.turn_user_message_index = 1;
	uam::acp_detail::RememberAssistantReplayPrefixes(session, active, 1);
	uam::acp_detail::RememberLoadHistoryReplayUpdates(session, active, 1);
	const auto consume = [&](const char* line) { UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, active, nlohmann::json::parse(line), nullptr)); };
	consume(R"({"event":"init","conversation_id":"11111111-2222-4333-8444-555555555555","init":{"permission_mode":"request-review"}})");
	consume(R"({"event":"step_update","step_update":{"conversation_id":"11111111-2222-4333-8444-555555555555","step_index":1,"state":"ACTIVE","step_type":"agent_response","text_delta":"AGY_PROTOCOL_20261007"}})");
	consume(R"({"event":"step_update","step_update":{"conversation_id":"11111111-2222-4333-8444-555555555555","step_index":1,"state":"DONE","step_type":"agent_response","text_delta":"\n","usage":{"input_tokens":11765,"output_tokens":171}}})");
	consume(R"({"event":"result","result":{"conversation_id":"11111111-2222-4333-8444-555555555555","status":"SUCCESS","response":"AGY_PROTOCOL_20261007\n","num_turns":1,"usage":{"input_tokens":11765,"output_tokens":171}}})");
	UAM_ASSERT_EQ(active.native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
	UAM_ASSERT(!session.processing);
	UAM_ASSERT_EQ(active.messages.size(), std::size_t{3});
	UAM_ASSERT_EQ(active.messages.back().content, std::string("AGY_PROTOCOL_20261007\n"));
	UAM_ASSERT_EQ(active.messages.back().tokens_input, 11765);
	UAM_ASSERT_EQ(active.messages.back().tokens_output, 171);
	const auto saved = ChatRepository::LoadLocalChat(temp.root, active.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->native_session_id, active.native_session_id);
}

UAM_TEST(AntigravityCumulativeUsageDuplicateResultsAndNewProcessReset)
{
	TempDir temp("uam-agy-counters");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-counters";
	chat.provider_id = "antigravity-cli";
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.chat_id = chat.id;
	session.running = true;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	runtime.OnAcpBuildInitialize(session, 1);
	const auto result = [&](int turns, int input, int output, const char* text)
	{
		return nlohmann::json{{"event", "result"}, {"result", {{"conversation_id", "11111111-2222-4333-8444-555555555555"}, {"status", "SUCCESS"}, {"response", text}, {"num_turns", turns}, {"usage", {{"input_tokens", input}, {"output_tokens", output}}}}}};
	};
	const auto step = [&](int index, int input, int output)
	{
		runtime.OnAcpHandleMessage(app, session, chat, {{"event", "step_update"}, {"step_update", {{"conversation_id", "11111111-2222-4333-8444-555555555555"}, {"step_type", "agent_response"}, {"step_index", index}, {"state", "DONE"}, {"usage", {{"input_tokens", input}, {"output_tokens", output}}}}}}, nullptr);
	};
	std::string method;
	runtime.OnAcpBuildPrompt(session, 1, "first", chat, method);
	session.processing = true;
	step(1, 100, 10);
	runtime.OnAcpHandleMessage(app, session, chat, result(1, 100, 10, "one"), nullptr);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	uam::acp_detail::ResetAcpTurnStreamState(session);
	session.processing = true;
	session.current_assistant_message_index = -1;
	runtime.OnAcpBuildPrompt(session, 2, "second", chat, method);
	runtime.OnAcpHandleMessage(app, session, chat, result(1, 100, 10, "duplicate"), nullptr);
	UAM_ASSERT(session.processing);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	step(2, 25, 4);
	step(2, 25, 4); // Duplicate completed step must not add usage twice.
	runtime.OnAcpHandleMessage(app, session, chat, result(2, 125, 14, "two"), nullptr);
	UAM_ASSERT_EQ(chat.messages.back().content, std::string("two"));
	UAM_ASSERT_EQ(chat.messages.back().tokens_input, 25);
	UAM_ASSERT_EQ(chat.messages.back().tokens_output, 4);
	runtime.OnAcpBuildInitialize(session, 2);
	UAM_ASSERT_EQ(session.antigravity_completed_turns, int64_t{0});
	uam::acp_detail::ResetAcpTurnStreamState(session);
	session.current_assistant_message_index = -1;
	session.processing = true;
	runtime.OnAcpBuildPrompt(session, 3, "resumed", chat, method);
	step(9, 30, 3);
	uam::acp_detail::RememberAssistantReplayPrefixes(session, chat, static_cast<int>(chat.messages.size()));
	runtime.OnAcpHandleMessage(app, session, chat, result(3, 1000000, 100000, "two"), nullptr);
	UAM_ASSERT_EQ(chat.messages.back().content, std::string("two"));
	UAM_ASSERT_EQ(chat.messages.back().tokens_input, 30);
	UAM_ASSERT_EQ(chat.messages.back().tokens_output, 3);
}

UAM_TEST(AntigravityObservedStepUsageReachesLiveSerializedSnapshot)
{
	TempDir temp("uam-agy-live-usage");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-live-usage";
	chat.provider_id = "antigravity-cli";
	chat.native_session_id = "11111111-2222-4333-8444-555555555555";
	app.chats.push_back(chat);
	auto owned = std::make_unique<uam::AcpSessionState>();
	auto& session = *owned;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.session_id = chat.native_session_id;
	session.running = true;
	session.session_ready = true;
	app.acp_sessions.push_back(std::move(owned));
	const auto& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
	runtime.OnAcpBuildInitialize(session, 1);
	const auto snapshot = [&]()
	{
		return uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["providerUsage"];
	};
	UAM_ASSERT(snapshot().is_null()); // No observed counter is unknown, not a reported zero.
	const auto step = [&](int index, int input, int output)
	{
		const nlohmann::json event{{"event", "step_update"}, {"step_update", {{"conversation_id", chat.native_session_id}, {"step_type", "agent_response"}, {"step_index", index}, {"state", "DONE"}, {"usage", {{"input_tokens", input}, {"output_tokens", output}}}}}};
		runtime.OnAcpHandleMessage(app, session, app.chats.front(), event, nullptr);
	};
	const auto finish = [&](int turn)
	{
		const nlohmann::json event{{"event", "result"}, {"result", {{"conversation_id", chat.native_session_id}, {"status", "SUCCESS"}, {"response", "observed answer"}, {"num_turns", turn}, {"usage", {{"input_tokens", 1000000}, {"output_tokens", 100000}}}}}};
		runtime.OnAcpHandleMessage(app, session, app.chats.front(), event, nullptr);
	};
	std::string method;
	runtime.OnAcpBuildPrompt(session, 1, "first", app.chats.front(), method);
	session.processing = true;
	step(1, 100, 10);
	step(1, 100, 10); // The same native DONE event must not count twice.
	finish(1);
	const auto first = snapshot();
	UAM_ASSERT(first.is_object()); // Intended public projection RED: saved message counts alone are insufficient.
	UAM_ASSERT_EQ(first["tokenUsage"]["total"]["inputTokens"], nlohmann::json(100));
	UAM_ASSERT_EQ(first["tokenUsage"]["total"]["outputTokens"], nlohmann::json(10));
	UAM_ASSERT_EQ(first["tokenUsage"]["total"]["totalTokens"], nlohmann::json(110));
	UAM_ASSERT_EQ(first["tokenUsage"]["last"]["totalTokens"], nlohmann::json(110));
	UAM_ASSERT(first["contextUsage"].is_null()); // Consumption does not establish context-window occupancy.
	uam::acp_detail::ResetAcpTurnStreamState(session);
	session.current_assistant_message_index = -1;
	runtime.OnAcpBuildPrompt(session, 2, "second", app.chats.front(), method);
	session.processing = true;
	finish(1); // A duplicate old result neither ends this turn nor adds its cumulative usage.
	UAM_ASSERT(session.processing);
	step(2, 25, 4);
	step(2, 25, 4);
	finish(2);
	const auto second = snapshot();
	UAM_ASSERT_EQ(second["tokenUsage"]["total"]["totalTokens"], nlohmann::json(139));
	UAM_ASSERT_EQ(second["tokenUsage"]["last"]["inputTokens"], nlohmann::json(25));
	UAM_ASSERT_EQ(second["tokenUsage"]["last"]["outputTokens"], nlohmann::json(4));
	UAM_ASSERT_EQ(second["tokenUsage"]["last"]["totalTokens"], nlohmann::json(29));
	UAM_ASSERT_EQ(app.chats.front().messages.back().tokens_input, 25);
	UAM_ASSERT_EQ(app.chats.front().messages.back().tokens_output, 4);
}

UAM_TEST(AntigravityLiveUsageDistinguishesMissingMalformedAndReportedZero)
{
	for (const nlohmann::json input : std::vector<nlohmann::json>{nullptr, -1, "1", 1.5, false, std::numeric_limits<uint64_t>::max(), 0})
	{
		TempDir temp("uam-agy-usage-certainty");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "agy-usage-certainty";
		chat.provider_id = "antigravity-cli";
		chat.native_session_id = "11111111-2222-4333-8444-555555555555";
		app.chats.push_back(chat);
		auto owned = std::make_unique<uam::AcpSessionState>();
		auto& session = *owned;
		session.chat_id = chat.id; session.provider_id = chat.provider_id;
		session.session_id = chat.native_session_id; session.running = true; session.session_ready = true;
		app.acp_sessions.push_back(std::move(owned));
		const auto& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
		runtime.OnAcpBuildInitialize(session, 1);
		std::string method;
		runtime.OnAcpBuildPrompt(session, 1, "observe exact counters", app.chats.front(), method);
		session.processing = true;
		nlohmann::json usage{{"output_tokens", 0}};
		if (!input.is_null()) usage["input_tokens"] = input;
		runtime.OnAcpHandleMessage(app, session, app.chats.front(), {{"event", "step_update"}, {"step_update", {{"conversation_id", chat.native_session_id}, {"step_type", "agent_response"}, {"step_index", 0}, {"state", "DONE"}, {"usage", usage}}}}, nullptr);
		runtime.OnAcpHandleMessage(app, session, app.chats.front(), {{"event", "result"}, {"result", {{"conversation_id", chat.native_session_id}, {"status", "SUCCESS"}, {"response", "answer"}, {"num_turns", 1}}}}, nullptr);
		const auto snapshot = uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["providerUsage"];
		if (input.is_number_integer() && input == 0)
		{
			UAM_ASSERT(snapshot.is_object());
			UAM_ASSERT_EQ(snapshot["tokenUsage"]["total"]["totalTokens"], nlohmann::json(0));
			UAM_ASSERT_EQ(snapshot["tokenUsage"]["last"]["totalTokens"], nlohmann::json(0));
		}
		else UAM_ASSERT(snapshot.is_null());
	}
}

UAM_TEST(AntigravityLiveUsagePreservesObservedScopeThroughPartialAndResumedTurns)
{
	TempDir temp("uam-agy-usage-observed-scope");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-usage-observed-scope"; chat.provider_id = "antigravity-cli";
	chat.native_session_id = "11111111-2222-4333-8444-555555555555";
	app.chats.push_back(chat);
	auto owned = std::make_unique<uam::AcpSessionState>();
	auto& session = *owned;
	session.chat_id = chat.id; session.provider_id = chat.provider_id;
	session.session_id = chat.native_session_id; session.running = true; session.session_ready = true;
	app.acp_sessions.push_back(std::move(owned));
	const auto& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
	const auto emit = [&](const nlohmann::json& event) { runtime.OnAcpHandleMessage(app, session, app.chats.front(), event, nullptr); };
	const auto step = [&](int index, const nlohmann::json& usage, const std::string& native)
	{
		emit({{"event", "step_update"}, {"step_update", {{"conversation_id", native}, {"step_type", "agent_response"}, {"step_index", index}, {"state", "DONE"}, {"usage", usage}}}});
	};
	const auto finish = [&](int turn)
	{
		emit({{"event", "result"}, {"result", {{"conversation_id", chat.native_session_id}, {"status", "SUCCESS"}, {"response", "answer"}, {"num_turns", turn}, {"usage", {{"input_tokens", 1000000}, {"output_tokens", 100000}}}}}});
	};
	const auto snapshot = [&]() { return uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["providerUsage"]; };
	std::string method;
	runtime.OnAcpBuildInitialize(session, 1);
	runtime.OnAcpBuildPrompt(session, 1, "partially observed", app.chats.front(), method);
	session.processing = true;
	step(1, {{"input_tokens", 25}, {"output_tokens", 4}}, chat.native_session_id);
	step(2, nlohmann::json::object(), chat.native_session_id); // Missing values cannot complete a known last-turn total.
	step(3, {{"input_tokens", 9007199254740991LL}, {"output_tokens", 1}}, chat.native_session_id); // Individually exact buckets cannot overflow the wire's exact total.
	finish(1);
	UAM_ASSERT_EQ(snapshot()["tokenUsage"]["total"]["totalTokens"], nlohmann::json(29));
	UAM_ASSERT(snapshot()["tokenUsage"]["last"].is_null());
	runtime.OnAcpBuildInitialize(session, 2); // Same native conversation, fresh native step numbering.
	uam::acp_detail::ResetAcpTurnStreamState(session); session.current_assistant_message_index = -1;
	runtime.OnAcpBuildPrompt(session, 2, "resumed observed counts", app.chats.front(), method);
	session.processing = true;
	step(1, {{"input_tokens", 30}, {"output_tokens", 3}}, chat.native_session_id);
	step(1, {{"input_tokens", 30}, {"output_tokens", 3}}, chat.native_session_id);
	finish(2);
	UAM_ASSERT_EQ(snapshot()["tokenUsage"]["total"]["totalTokens"], nlohmann::json(62));
	UAM_ASSERT_EQ(snapshot()["tokenUsage"]["last"]["totalTokens"], nlohmann::json(33));
	uam::acp_detail::ResetAcpTurnStreamState(session); session.current_assistant_message_index = -1;
	runtime.OnAcpBuildPrompt(session, 3, "exact aggregate limit", app.chats.front(), method);
	session.processing = true;
	step(2, {{"input_tokens", 9007199254740991LL}, {"output_tokens", 0}}, chat.native_session_id);
	finish(3);
	UAM_ASSERT_EQ(snapshot()["tokenUsage"]["total"]["totalTokens"], nlohmann::json(62));
	UAM_ASSERT(snapshot()["tokenUsage"]["last"].is_null());
	const auto before = snapshot();
	session.processing = true;
	step(3, {{"input_tokens", 900}, {"output_tokens", 90}}, "99999999-2222-4333-8444-555555555555");
	UAM_ASSERT_EQ(snapshot(), before);
	UAM_ASSERT_EQ(session.session_id, chat.native_session_id);
	UAM_ASSERT(!session.processing && session.last_error.find("identity") != std::string::npos);
}

UAM_TEST(AntigravityMalformedStepIdentityCannotHideValidZeroIndexedUsage)
{
	for (const nlohmann::json invalid_index : std::vector<nlohmann::json>{nullptr, -1, "0", false})
	{
		TempDir temp("uam-agy-usage-step-identity");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "agy-usage-step-identity"; chat.provider_id = "antigravity-cli";
		chat.native_session_id = "11111111-2222-4333-8444-555555555555";
		app.chats.push_back(chat);
		auto owned = std::make_unique<uam::AcpSessionState>();
		auto& session = *owned;
		session.chat_id = chat.id; session.provider_id = chat.provider_id;
		session.session_id = chat.native_session_id; session.running = true; session.session_ready = true;
		app.acp_sessions.push_back(std::move(owned));
		const auto& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
		runtime.OnAcpBuildInitialize(session, 1);
		std::string method;
		runtime.OnAcpBuildPrompt(session, 1, "validate observed step identity", app.chats.front(), method);
		session.processing = true;
		nlohmann::json malformed{{"conversation_id", chat.native_session_id}, {"step_type", "agent_response"}, {"state", "DONE"}, {"usage", {{"input_tokens", 99}, {"output_tokens", 9}}}};
		if (!invalid_index.is_null()) malformed["step_index"] = invalid_index;
		runtime.OnAcpHandleMessage(app, session, app.chats.front(), {{"event", "step_update"}, {"step_update", malformed}}, nullptr);
		const nlohmann::json valid{{"event", "step_update"}, {"step_update", {{"conversation_id", chat.native_session_id}, {"step_type", "agent_response"}, {"step_index", 0}, {"state", "DONE"}, {"usage", {{"input_tokens", 10}, {"output_tokens", 1}}}}}};
		runtime.OnAcpHandleMessage(app, session, app.chats.front(), valid, nullptr);
		runtime.OnAcpHandleMessage(app, session, app.chats.front(), valid, nullptr);
		runtime.OnAcpHandleMessage(app, session, app.chats.front(), {{"event", "result"}, {"result", {{"conversation_id", chat.native_session_id}, {"status", "SUCCESS"}, {"response", "known observed answer"}, {"num_turns", 1}}}}, nullptr);
		UAM_ASSERT_EQ(app.chats.front().messages.back().tokens_input, 10); // Existing public saved counter proves malformed-index poisoning independently of the new DTO.
		UAM_ASSERT_EQ(app.chats.front().messages.back().tokens_output, 1);
		const auto usage = uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["providerUsage"]["tokenUsage"];
		UAM_ASSERT_EQ(usage["total"]["totalTokens"], nlohmann::json(11));
		UAM_ASSERT(usage["last"].is_null()); // The unidentified event leaves the complete turn unknown.
	}
}

UAM_TEST(AntigravityRejectsChangedIdentityAndKeepsNativePermissionPolicy)
{
	TempDir temp("uam-agy-identity");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-identity";
	chat.provider_id = "antigravity-cli";
	chat.native_session_id = "11111111-2222-4333-8444-555555555555";
	uam::AcpSessionState session;
	session.session_id = chat.native_session_id;
	session.processing = true;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	runtime.OnAcpHandleMessage(app, session, chat, {{"event", "init"}, {"conversation_id", "99999999-2222-4333-8444-555555555555"}}, nullptr);
	UAM_ASSERT(!session.processing);
	UAM_ASSERT(session.last_error.find("identity") != std::string::npos);
	UAM_ASSERT_EQ(chat.native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
	for (const nlohmann::json& counter : std::vector<nlohmann::json>{nullptr, 0, -1, "1", 1.5, false, std::numeric_limits<uint64_t>::max()})
	{
		uam::AcpSessionState malformed;
		malformed.session_id = chat.native_session_id;
		malformed.processing = true;
		nlohmann::json result{{"conversation_id", chat.native_session_id}, {"status", "SUCCESS"}, {"response", "must not complete"}};
		if (!counter.is_null()) result["num_turns"] = counter;
		runtime.OnAcpHandleMessage(app, malformed, chat, {{"event", "result"}, {"result", result}}, nullptr);
		UAM_ASSERT(!malformed.processing);
		UAM_ASSERT(malformed.last_error.find("turn counter") != std::string::npos);
		UAM_ASSERT_EQ(malformed.antigravity_completed_turns, int64_t{0});
	}
	const nlohmann::json serialized = uam::StateSerializer::SerializeProvider(ProviderProfileStore::DefaultAntigravityProfile());
	UAM_ASSERT_EQ(serialized["structuredPermissionControl"], nlohmann::json("provider"));
}

UAM_TEST(AntigravityDocumentedToolStepRendersOutputAndError)
{
	TempDir temp("uam-agy-tool-step");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-tool-step";
	chat.provider_id = "antigravity-cli";
	uam::AcpSessionState session;
	session.processing = true;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	runtime.OnAcpHandleMessage(app, session, chat, nlohmann::json::parse(R"({"event":"step_update","step_update":{"conversation_id":"11111111-2222-4333-8444-555555555555","step_index":4,"state":"DONE","step_type":"tool","tool_name":"run_command","tool_info":{"parameters":{"CommandLine":"fixture only"},"output":"fixture output","error":{"message":"fixture failure"}}}})"), nullptr);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(chat.messages.front().tool_calls.size(), std::size_t{1});
	UAM_ASSERT_EQ(chat.messages.front().tool_calls.front().status, std::string("failed"));
	UAM_ASSERT(chat.messages.front().tool_calls.front().result_text.find("fixture failure") != std::string::npos);
}

UAM_TEST(AntigravityCapturedPermissionDenialPreservesOtherSuccessAndReportsAmbiguity)
{
	for (const std::pair<int, bool>& scenario : {std::pair{1, false}, std::pair{2, false}, std::pair{2, true}, std::pair{0, false}})
	{
		const int matching_calls = scenario.first;
		const bool existing_mcp_failure = scenario.second;
		TempDir temp("uam-agy-denied-action");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "agy-denied-action";
		chat.provider_id = "antigravity-cli";
		uam::AcpSessionState session;
		session.processing = true;
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
		const std::string identity = "11111111-2222-4333-8444-555555555555";
		for (int index = 0; index < matching_calls; ++index)
		{
			// Captured native MCP denial emits ACTIVE/DONE without tool_info.error.
			for (const char* state : {"ACTIVE", "DONE"})
			{
				nlohmann::json update = {{"event", "step_update"}, {"step_update", {
				    {"conversation_id", identity}, {"step_index", index + 2}, {"state", state}, {"step_type", "tool"},
				    {"tool_name", "call_mcp_tool"}, {"tool_info", {{"parameters", {{"ServerName", "fixture"}, {"ToolName", "marker"}}}}}}}};
				if (existing_mcp_failure && index == 0)
					update["step_update"]["tool_info"]["error"] = {{"message", "independent MCP failure"}};
				runtime.OnAcpHandleMessage(app, session, chat, update, nullptr);
			}
		}
		runtime.OnAcpHandleMessage(app, session, chat, {{"event", "step_update"}, {"step_update", {
		    {"conversation_id", identity}, {"step_index", 10}, {"state", "DONE"}, {"step_type", "tool"},
		    {"tool_name", "view_file"}, {"tool_info", {{"output", "successful output"}}}}}}, nullptr);
		runtime.OnAcpHandleMessage(app, session, chat, {{"event", "step_update"}, {"step_update", {
		    {"conversation_id", identity}, {"step_index", 12}, {"state", "DONE"}, {"step_type", "tool"},
		    {"tool_name", "run_command"}, {"tool_info", {{"error", {{"message", "independent failure"}}}}}}}}, nullptr);
		runtime.OnAcpHandleMessage(app, session, chat, {{"event", "step_update"}, {"step_update", {
		    {"conversation_id", identity}, {"step_index", 11}, {"state", "DONE"}, {"step_type", "agent_response"},
		    {"text_delta", "Successful answer."}}}}, nullptr);
		nlohmann::json result = {{"event", "result"}, {"result", {{"conversation_id", identity},
		    {"status", "SUCCESS"}, {"response", "Successful answer."}, {"num_turns", 1},
		    {"denied_actions", {{{"action", "mcp"}, {"display_name", "CallMcpTool"}}}}}}};
		if (matching_calls == 0)
		{
			std::string unicode_label;
			for (int index = 0; index < 100; ++index) unicode_label += "\xE7\x95\x8C";
			result["result"]["denied_actions"].push_back({{"action", "other"}, {"display_name", unicode_label}});
			for (int index = 0; index < 20; ++index)
				result["result"]["denied_actions"].push_back({{"action", "other"}, {"display_name", std::to_string(index) + std::string(1000, 'x')}});
		}
		runtime.OnAcpHandleMessage(app, session, chat, result, nullptr);
		UAM_ASSERT(!session.processing);
		UAM_ASSERT_EQ(chat.messages.back().content, std::string("Successful answer."));
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(saved.has_value());
		app.chats.push_back(*saved);
		UAM_ASSERT(!uam::StateSerializer::Serialize(app).dump().empty());
		int denied_count = 0;
		int summaries = 0;
		for (const Message& message : saved->messages)
			for (const ToolCall& tool : message.tool_calls)
			{
				if (tool.name == "call_mcp_tool")
				{
					++denied_count;
					if (existing_mcp_failure && denied_count == 1)
					{
						UAM_ASSERT_EQ(tool.status, std::string("failed"));
						UAM_ASSERT_EQ(tool.result_text, std::string("independent MCP failure"));
					}
					else
					{
						UAM_ASSERT_EQ(tool.status, std::string(matching_calls == 1 ? "failed" : "unknown"));
						UAM_ASSERT(tool.result_text.find("denied") != std::string::npos);
					}
				}
				else if (tool.name == "view_file")
				{
					UAM_ASSERT_EQ(tool.status, std::string("completed"));
					UAM_ASSERT_EQ(tool.result_text, std::string("successful output"));
				}
				else if (tool.name == "run_command")
				{
					UAM_ASSERT_EQ(tool.status, std::string("failed"));
					UAM_ASSERT_EQ(tool.result_text, std::string("independent failure"));
				}
				else
				{
					++summaries;
					UAM_ASSERT_EQ(tool.status, std::string("failed"));
					UAM_ASSERT(tool.result_text.find("cannot identify") != std::string::npos);
					UAM_ASSERT(tool.result_text.size() < 256);
				}
			}
		UAM_ASSERT_EQ(denied_count, matching_calls);
		UAM_ASSERT_EQ(summaries, matching_calls == 0 ? 8 : matching_calls == 2 ? 1 : 0);
		const std::size_t tool_count = session.tool_calls.size();
		runtime.OnAcpHandleMessage(app, session, chat, result, nullptr);
		UAM_ASSERT_EQ(session.tool_calls.size(), tool_count);
	}
}

UAM_TEST(AntigravityNativeSubagentsPreserveValidatedChildIdentityWithoutFetchingUris)
{
	TempDir temp("uam-agy-native-children");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-children"; chat.provider_id = "antigravity-cli";
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id; session.chat_id = chat.id; session.running = true; session.processing = true;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
	const std::string parent = "11111111-2222-4333-8444-555555555555";
	const std::string child = "22222222-2222-4222-8222-222222222222";
	nlohmann::json update{{"event", "step_update"}, {"step_update", {{"conversation_id", parent}, {"step_index", 4}, {"state", "ACTIVE"}, {"step_type", "subagent"},
	    {"subagent_info", {{"subagents", nlohmann::json::array({{{"type_name", "reviewer"}, {"role", "Inspect source"}, {"conversation_id", child},
	        {"log_uri", "https://logs.invalid/private/path?access_token=secret#fragment"}, {"workspace_uris", nlohmann::json::array({"file:///remote/workspace"})}},
	        {{"type_name", "invalid identity"}, {"conversation_id", "../escape"}}, {{"type_name", "parent alias"}, {"conversation_id", parent}}})}}}}}};
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, update, nullptr));
	UAM_ASSERT_EQ(session.tool_calls.size(), std::size_t{3});
	const auto& tool = session.tool_calls.front();
	UAM_ASSERT(tool.is_sub_agent);
	UAM_ASSERT_EQ(tool.sub_agent_id, child);
	UAM_ASSERT_EQ(tool.sub_agent_title, std::string("reviewer"));
	const auto metadata = nlohmann::json::parse(tool.args_json);
	UAM_ASSERT_EQ(metadata["role"], nlohmann::json("Inspect source"));
	UAM_ASSERT_EQ(metadata["log_uri"], nlohmann::json("https://logs.invalid/private/path"));
	UAM_ASSERT_EQ(metadata["workspace_uris"][0], nlohmann::json("file:///remote/workspace"));
	UAM_ASSERT(session.tool_calls[1].sub_agent_id.empty());
	UAM_ASSERT(session.tool_calls[2].sub_agent_id.empty());
	UAM_ASSERT_EQ(chat.messages.back().tool_calls.front().sub_agent_id, child);
	update["step_update"]["state"] = "DONE";
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, update, nullptr));
	UAM_ASSERT_EQ(session.tool_calls.size(), std::size_t{3});
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("completed"));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, update, nullptr));
	UAM_ASSERT_EQ(session.tool_calls.size(), std::size_t{3});
	ChatSession resume;
	resume.native_session_id = child;
	UAM_ASSERT_EQ(runtime.OnAcpValidateResumeId(resume), child);
	resume.native_session_id = "../escape";
	UAM_ASSERT(runtime.OnAcpValidateResumeId(resume).empty());
}

UAM_TEST(AntigravityReadOnlyEffortMetadataRejectsModelTurnsAndInventedChoices)
{
	const nlohmann::json valid{{"conversation_id", ""}, {"status", "SUCCESS"}, {"num_turns", 0},
	    {"command", {{"name", "effort"}, {"data", {{"adjustable", true}, {"current", "high"}, {"available", nlohmann::json::array({"low", "high"})}}}}}};
	const auto parsed = uam::ProviderModelCatalogService::ParseAntigravityEffort(valid.dump(), "actual-native-model");
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ((*parsed)["supportedReasoningEfforts"], nlohmann::json::array({"low", "high"}));
	UAM_ASSERT_EQ((*parsed)["defaultReasoningEffort"], nlohmann::json("high"));
	UAM_ASSERT(!uam::ProviderModelCatalogService::ParseAntigravityEffort(valid.dump(), ""));
	for (const std::string field : {"conversation_id", "num_turns", "status", "command"})
	{
		auto invalid = valid;
		invalid.erase(field);
		UAM_ASSERT(!uam::ProviderModelCatalogService::ParseAntigravityEffort(invalid.dump(), "actual-native-model"));
	}
	for (const auto invalid_levels : {nlohmann::json::array({"low", "ultra"}), nlohmann::json::array({"high", "high"}), nlohmann::json::array(), nlohmann::json::array({42})})
	{
		auto invalid = valid; invalid["command"]["data"]["available"] = invalid_levels;
		UAM_ASSERT(!uam::ProviderModelCatalogService::ParseAntigravityEffort(invalid.dump(), "actual-native-model"));
	}
	auto invalid = valid; invalid["conversation_id"] = "11111111-2222-4333-8444-555555555555";
	UAM_ASSERT(!uam::ProviderModelCatalogService::ParseAntigravityEffort(invalid.dump(), "actual-native-model"));
	invalid = valid; invalid["num_turns"] = 1;
	UAM_ASSERT(!uam::ProviderModelCatalogService::ParseAntigravityEffort(invalid.dump(), "actual-native-model"));
	auto fixed = valid; fixed["command"]["data"]["adjustable"] = false;
	const auto fixed_parsed = uam::ProviderModelCatalogService::ParseAntigravityEffort(fixed.dump(), "fixed-native-model");
	UAM_ASSERT(fixed_parsed.has_value());
	UAM_ASSERT((*fixed_parsed)["supportedReasoningEfforts"].empty());
	UAM_ASSERT_EQ((*fixed_parsed)["defaultReasoningEffort"], nlohmann::json(""));
}

#if !defined(_WIN32)
UAM_TEST(AntigravityEffortDiscoveryQueriesOnlyActualDefaultAndSelectedModels)
{
	TempDir temp("uam-agy-effort-discovery");
	ScopedEnvVar log("UAM_ANTIGRAVITY_EFFORT_LOG", (temp.root / "calls").string());
	const fs::path script = temp.root / "native.sh";
	const std::string source = R"SCRIPT(printf '%s\n' "$*" >> "$UAM_ANTIGRAVITY_EFFORT_LOG"
if test "$1" = --version; then printf '1.4.0'; exit 0; fi
if test "$1" = models; then printf 'adjustable-model\tAdjustable Model\nfixed-model\tFixed Model\n'; exit 0; fi
if test "$2" = /model; then printf '%s' '{"conversation_id":"","status":"SUCCESS","num_turns":0,"command":{"name":"model","data":{"id":"fixed-model"}}}'; exit 0; fi
if test "$2" = /effort; then
if test "$6" = fixed-model; then printf '%s' '{"conversation_id":"","status":"SUCCESS","num_turns":0,"command":{"name":"effort","data":{"adjustable":false,"current":"high","available":[]}}}'; else
printf '%s' '{"conversation_id":"","status":"SUCCESS","num_turns":0,"command":{"name":"effort","data":{"adjustable":true,"current":"high","available":["low","high"]}}}'; fi
exit 0; fi
exit 9
)SCRIPT";
	UAM_ASSERT(uam::io::WriteTextFile(script, source));
	ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
	profile.interactive_command = "/bin/sh " + uam::shell::EscapeArg(script.string());
	uam::ProviderModelCatalogService catalog;
	catalog.Initialize(temp.root / "data", {profile});
	const ExecutionHost host = uam::execution_hosts::LocalHost();
	catalog.SetExecutionHostContext({host});
	const auto settle = [&]()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (catalog.ActiveNativeModelDiscoveries() > 0 && std::chrono::steady_clock::now() < deadline)
		{
			(void)catalog.Poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT_EQ(catalog.ActiveNativeModelDiscoveries(), std::size_t{0});
	};
	std::string error;
	UAM_ASSERT(catalog.StartAntigravityModelDiscovery(profile, host, temp.root.string(), &error));
	settle();
	auto models = catalog.GetCachedProviderModels(profile.id, temp.root.string());
	UAM_ASSERT_EQ(models[0]["id"], nlohmann::json("fixed-model"));
	UAM_ASSERT(models[0].value("isDefault", false));
	UAM_ASSERT(models[0]["supportedReasoningEfforts"].empty());
	UAM_ASSERT(!models[1].contains("supportedReasoningEfforts"));
	UAM_ASSERT(catalog.StartAntigravityEffortDiscovery(profile, host, temp.root.string(), "adjustable-model", &error));
	UAM_ASSERT(catalog.StartAntigravityEffortDiscovery(profile, host, temp.root.string(), "adjustable-model", &error));
	settle();
	models = catalog.GetCachedProviderModels(profile.id, temp.root.string());
	UAM_ASSERT_EQ(models[1]["supportedReasoningEfforts"], nlohmann::json::array({"low", "high"}));
	const std::string calls = uam::io::ReadTextFile(temp.root / "calls");
	UAM_ASSERT_EQ(std::ranges::count(calls, '\n'), 6);
	UAM_ASSERT(catalog.StartAntigravityEffortDiscovery(profile, host, temp.root.string(), "adjustable-model", &error));
	UAM_ASSERT_EQ(catalog.ActiveNativeModelDiscoveries(), std::size_t{0});
	UAM_ASSERT_EQ(uam::io::ReadTextFile(temp.root / "calls"), calls);
	UAM_ASSERT(!catalog.StartAntigravityEffortDiscovery(profile, host, temp.root.string(), "unadvertised", &error));
	models[1]["effortMetadataAtSec"] = 0;
	UAM_ASSERT(catalog.RememberSuccessfulModels(profile.id, models, temp.root.string()));
	UAM_ASSERT(uam::io::WriteTextFile(script, "printf '%s' 'unsupported old CLI'\n"));
	UAM_ASSERT(catalog.StartAntigravityEffortDiscovery(profile, host, temp.root.string(), "adjustable-model", &error));
	settle();
	UAM_ASSERT_EQ(catalog.GetCachedProviderModels(profile.id, temp.root.string())[1]["supportedReasoningEfforts"], nlohmann::json::array({"low", "high"}));
	UAM_ASSERT(!catalog.GetProviderRefreshError(profile.id, temp.root.string()).empty());
	UAM_ASSERT(catalog.StartAntigravityEffortDiscovery(profile, host, temp.root.string(), "adjustable-model", &error));
	UAM_ASSERT_EQ(catalog.ActiveNativeModelDiscoveries(), std::size_t{0});
}
UAM_TEST(AntigravityLegacyDiscoveryNeverSubmitsUnknownSlashCommands)
{
	TempDir temp("uam-agy-legacy-no-inference");
	ScopedEnvVar marker("UAM_AGY_UNEXPECTED_PROMPT", (temp.root / "unexpected-prompt").string());
	const fs::path script = temp.root / "legacy.sh";
	UAM_ASSERT(uam::io::WriteTextFile(script, R"SCRIPT(if test "$1" = models; then printf 'legacy-model\tLegacy Model\n'; exit 0; fi
if test "$1" = --version; then printf '1.0.0'; exit 0; fi
printf 'unexpected model prompt' > "$UAM_AGY_UNEXPECTED_PROMPT"
exit 9
)SCRIPT"));
	ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
	profile.interactive_command = "/bin/sh " + uam::shell::EscapeArg(script.string());
	uam::ProviderModelCatalogService catalog;
	catalog.Initialize(temp.root / "data", {profile});
	const auto host = uam::execution_hosts::LocalHost();
	catalog.SetExecutionHostContext({host});
	const auto settle = [&]()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (catalog.ActiveNativeModelDiscoveries() && std::chrono::steady_clock::now() < deadline)
		{
			(void)catalog.Poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT_EQ(catalog.ActiveNativeModelDiscoveries(), std::size_t{0});
	};
	std::string error;
	UAM_ASSERT(catalog.StartAntigravityModelDiscovery(profile, host, temp.root.string(), &error));
	settle();
	UAM_ASSERT_EQ(catalog.GetCachedProviderModels(profile.id, temp.root.string())[0]["id"], nlohmann::json("legacy-model"));
	UAM_ASSERT(!catalog.GetProviderRefreshError(profile.id, temp.root.string()).empty());
	UAM_ASSERT(catalog.StartAntigravityEffortDiscovery(profile, host, temp.root.string(), "legacy-model", &error));
	settle();
	UAM_ASSERT(!fs::exists(temp.root / "unexpected-prompt"));
}
#endif

#endif


#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI && !defined(_WIN32)
UAM_TEST(AntigravityNativeLaunchIncludesComputerServerWithCentralSetupOnAndOff)
{
	for (const bool central : {false, true})
		for (const bool control : {false, true})
			for (const bool global_computer : {false, true})
			{
				TempDir temp("uam-antigravity-computer-launch");
				ScopedEnvVar home("HOME", temp.root.string());
				ScopedEnvVar config("XDG_CONFIG_HOME", (temp.root / "config").string());
				const fs::path script = temp.root / "fake-agy.sh";
				UAM_ASSERT(uam::io::WriteTextFile(script, "cat >/dev/null\n"));
				uam::AppState app;
				app.data_root = temp.root / "data";
				ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
				profile.interactive_command = "/bin/sh " + uam::shell::EscapeArg(script.string());
				app.provider_profiles = {profile};
				app.settings.git_auto_worktree = false;
				app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
				app.settings.central_provider_configuration.enabled = central;
				app.settings.central_provider_configuration.uam_control_enabled = control;
				if (!central) app.settings.central_provider_configuration.mcp_overrides.push_back({profile.id, "local", "shared-setup", false});
				app.settings.computer_use_globally_enabled = global_computer;
				ChatSession chat;
				chat.id = "antigravity-computer-launch";
				chat.provider_id = profile.id;
				chat.workspace_directory = temp.root.string();
				chat.uam_control_enabled = control;
				chat.computer_use_enabled = false; // The server must also permit requesting approval from ordinary chat.
				app.chats.push_back(chat);
				app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
				auto& session = *app.acp_sessions.front();
				session.chat_id = chat.id;
				session.provider_id = profile.id;
				std::string error;
				UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (!session.running && std::chrono::steady_clock::now() < deadline)
				{
					UAM_ASSERT(uam::acp_detail::StartAcpProcessForChat(app, session, app.chats.front(), &error));
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				UAM_ASSERT(session.running);
				std::size_t configs = 0;
				if (!session.provider_configuration_directory.empty())
					for (const auto& entry : fs::recursive_directory_iterator(session.provider_configuration_directory))
						if (entry.path().filename() == "mcp_config.json")
						{
							const auto native = nlohmann::json::parse(ReadFile(entry.path()));
							const auto& servers = native.at("mcpServers");
							UAM_ASSERT_EQ(servers.contains("uam-computer"), global_computer);
							UAM_ASSERT_EQ(servers.contains("uam-control"), control);
							if (global_computer)
							{
								UAM_ASSERT_EQ(servers["uam-computer"].value("command", ""), uam::computer_use::McpExecutablePath());
								UAM_ASSERT_EQ(servers["uam-computer"]["args"], nlohmann::json(uam::computer_use::McpServerArguments(chat)));
							}
							++configs;
						}
				UAM_ASSERT_EQ(configs, central || control || global_computer ? std::size_t{1} : std::size_t{0});
				if (!session.provider_configuration_directory.empty())
				{
					const auto manifest = nlohmann::json::parse(ReadFile(session.provider_configuration_directory / "owner.json"));
					UAM_ASSERT_EQ(manifest.value("mcpOnly", false), !central);
				}
				PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
				PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
				uam::UamControlService::Shutdown(app);
			}
}
#endif

UAM_TEST(AntigravityComputerLaunchPreparationPreservesGlobalAndSshGates)
{
#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
	TempDir temp("uam-antigravity-computer-gates");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
	app.provider_profiles = {ProviderProfileStore::DefaultAntigravityProfile()};
	app.settings.central_provider_configuration.enabled = false;
	app.settings.central_provider_configuration.uam_control_enabled = false;
	ChatSession chat;
	chat.id = "antigravity-local-gates";
	chat.provider_id = "antigravity-cli";
	uam::AcpSessionState session;
	app.settings.computer_use_globally_enabled = true;
	UAM_ASSERT(uam::acp_detail::ShouldDeferLocalAcpConfiguration(app, session, chat));
	app.settings.computer_use_globally_enabled = false;
	UAM_ASSERT(!uam::acp_detail::ShouldDeferLocalAcpConfiguration(app, session, chat));
	app.settings.computer_use_globally_enabled = true;
	chat.execution_host_id = "remote";
	ExecutionHost host;
	host.id = "remote";
	host.transport = "ssh";
	app.settings.execution_hosts.push_back(host);
	UAM_ASSERT(!uam::acp_detail::ShouldDeferLocalAcpConfiguration(app, session, chat));
	UAM_ASSERT(uam::computer_use::AcpMcpServers(chat).empty());
#endif
}

UAM_TEST(AntigravityManagedLaunchRejectsForeignNativePermissionPolicy)
{
#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
	const std::string snapshot = "---\nversion: 1\nname: reviewer\ndescription: Inspect code\nmode: primary\nworkspaceAccess: read\nskills: []\ndelegates: []\nopenCodePermission: {\"edit\":\"deny\",\"bash\":\"ask\"}\n---\nInspect code.\n";
	for (const bool central : {false, true})
	{
		uam::AppState app;
		app.settings.central_provider_configuration.enabled = central;
		ChatSession chat;
		chat.provider_id = "antigravity-cli";
		std::string error;
		UAM_ASSERT(!uam::acp_detail::ValidateCapturedAcpAgentPolicy(app, chat, "reviewer", snapshot, "uam-prompt-injected", &error));
		UAM_ASSERT(error.find("imported permission policy") != std::string::npos);
	}
#endif
}


#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
UAM_TEST(AntigravityMalformedStepIndexCannotReplaceZeroIndexedToolEvidence)
{
	const std::vector<nlohmann::json> invalid_indices = {nullptr, -1, "0", false, 0.0, std::numeric_limits<uint64_t>::max()};
	for (const bool native_child : {false, true})
		for (const auto& invalid_index : invalid_indices)
		{
			TempDir temp("uam-agy-step-evidence");
			uam::AppState app; app.data_root = temp.root;
			ChatSession chat; chat.id = "agy-step-evidence"; chat.provider_id = "antigravity-cli";
			chat.native_session_id = "11111111-2222-4333-8444-555555555555";
			uam::AcpSessionState session;
			session.provider_id = chat.provider_id; session.chat_id = chat.id;
			session.session_id = chat.native_session_id; session.running = true; session.processing = true;
			const auto& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
			const auto update = [&](const nlohmann::json& index, const std::string& evidence, bool later_child = false)
			{
				nlohmann::json payload{{"conversation_id", chat.native_session_id}, {"state", "DONE"},
				    {"step_type", native_child ? "subagent" : "tool"}, {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}};
				if (!index.is_null()) payload["step_index"] = index; // Null represents the actual missing field.
				if (native_child)
				{
					const std::string child = later_child ? "33333333-3333-4333-8333-333333333333" : "22222222-2222-4222-8222-222222222222";
					payload["subagent_info"] = {{"subagents", nlohmann::json::array({{{"conversation_id", child}, {"type_name", evidence}, {"role", evidence}}})}};
				}
				else
				{
					payload["tool_name"] = "run_command";
					payload["tool_info"] = {{"parameters", {{"CommandLine", evidence}}}, {"output", evidence}};
				}
				return nlohmann::json{{"event", "step_update"}, {"step_update", std::move(payload)}};
			};
			const auto valid_zero = update(0, "valid-zero-evidence");
			UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, valid_zero, nullptr));
			UAM_ASSERT_EQ(session.tool_calls.size(), std::size_t{1});
			UAM_ASSERT_EQ(chat.messages.back().tool_calls.size(), std::size_t{1});
			const ToolCall expected = chat.messages.back().tool_calls.front();
			UAM_ASSERT_EQ(expected.status, std::string("completed"));
			UAM_ASSERT_EQ(expected.is_sub_agent, native_child);
			UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, update(invalid_index, "malformed-replacement"), nullptr));
			UAM_ASSERT_EQ(chat.messages.back().tool_calls, std::vector<ToolCall>{expected});
			UAM_ASSERT_EQ(session.tool_calls.size(), std::size_t{1});
			// A genuine zero replay remains idempotent; malformed identity cannot poison its saved row.
			UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, valid_zero, nullptr));
			UAM_ASSERT_EQ(chat.messages.back().tool_calls, std::vector<ToolCall>{expected});
			UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, update(1, "valid-later-evidence", true), nullptr));
			UAM_ASSERT_EQ(session.tool_calls.size(), std::size_t{2});
			UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"event", "result"}, {"result", {
			    {"conversation_id", chat.native_session_id}, {"status", "SUCCESS"}, {"num_turns", 1}, {"response", "fixture answer"}}}}, nullptr));
			UAM_ASSERT(!session.processing);
			const auto saved = ChatRepository::LoadLocalChat(temp.root, chat.id);
			UAM_ASSERT(saved);
			std::vector<ToolCall> saved_calls;
			for (const auto& message : saved->messages)
				saved_calls.insert(saved_calls.end(), message.tool_calls.begin(), message.tool_calls.end());
			UAM_ASSERT_EQ(saved_calls.size(), std::size_t{2});
			UAM_ASSERT_EQ(saved_calls.front(), expected);
			UAM_ASSERT_EQ(saved_calls.back().status, std::string("completed"));
			if (native_child) UAM_ASSERT_EQ(saved_calls.back().sub_agent_id, std::string("33333333-3333-4333-8333-333333333333"));
			else UAM_ASSERT_EQ(saved_calls.back().result_text, std::string("valid-later-evidence"));
		}
}
#endif

#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
UAM_TEST(AntigravityCanonicalAcceptEditsTierReachesBothLaunchAdapters)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	const ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
	ChatSession chat;
	chat.provider_id = "antigravity-cli";
	chat.approval_mode = "default";
	chat.command_safety_tier = "acceptEdits";
	chat.extra_flags = "--unknown-uam-owner-flag=opaque";
	const ChatSession before = chat;
	const auto terminal = runtime.BuildInteractiveArgv(profile, chat, {});
	const auto structured = runtime.BuildStructuredLaunchArgv(profile, chat);
	UAM_ASSERT_EQ(terminal, (std::vector<std::string>{"agy", "--mode", "accept-edits"}));
	UAM_ASSERT_EQ(structured,
	    (std::vector<std::string>{"agy", "--input-format", "stream-json", "--output-format", "stream-json", "--mode", "accept-edits"}));
	UAM_ASSERT_EQ(chat.provider_id, before.provider_id);
	UAM_ASSERT_EQ(chat.approval_mode, before.approval_mode);
	UAM_ASSERT_EQ(chat.command_safety_tier, before.command_safety_tier);
	UAM_ASSERT_EQ(chat.extra_flags, before.extra_flags);
}

UAM_TEST(AntigravityPlanLaunchRetainsCeilingAcrossCanonicalPermissionTiers)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	for (const std::string tier : {"off", "acceptEdits", "aiReview", "yolo"})
	{
		for (const std::string flags : {
		         "--mode accept-edits --dangerously-skip-permissions --effort high",
		         "--mode=accept-edits --dangerously-skip-permissions=true --effort high",
		         "-mode accept-edits -dangerously-skip-permissions --effort high",
		         "--mode --effort high",
		         "-mode -dangerously-skip-permissions --effort high"})
		{
			ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
			ChatSession chat;
			chat.provider_id = "antigravity-cli";
			chat.approval_mode = "plan";
			chat.command_safety_tier = tier;
			AppSettings settings;
			settings.provider_extra_flags = flags + " -- literal --mode accept-edits --dangerously-skip-permissions";
			UAM_ASSERT_EQ(runtime.BuildInteractiveArgv(profile, chat, settings),
			    (std::vector<std::string>{"agy", "--effort", "high", "--mode", "plan", "--", "literal", "--mode", "accept-edits", "--dangerously-skip-permissions"}));
			profile.interactive_command = "agy " + flags;
			UAM_ASSERT_EQ(runtime.BuildStructuredLaunchArgv(profile, chat),
			    (std::vector<std::string>{"agy", "--effort", "high", "--input-format", "stream-json", "--output-format", "stream-json", "--mode", "plan"}));
			UAM_ASSERT_EQ(chat.approval_mode, std::string("plan"));
			UAM_ASSERT_EQ(chat.command_safety_tier, tier);
			UAM_ASSERT_EQ(settings.provider_extra_flags,
			    flags + " -- literal --mode accept-edits --dangerously-skip-permissions");
			UAM_ASSERT_EQ(profile.interactive_command, "agy " + flags);
		}
	}
}
#endif

#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI && defined(__APPLE__)
namespace
{
	// Exercise actual launch projection, then require the macOS missing-file refusal
	// that precedes process creation. No executable, shell, runner or provider is run.
	void AssertAntigravityLaunchProjectionBeforeProcessCreation(
	    bool review, const std::string& tier, const std::string& expected_mode)
	{
		TempDir temp("uam-agy-review-projection");
		ScopedEnvVar home("HOME", temp.root.string());
		ScopedEnvVar config("XDG_CONFIG_HOME", (temp.root / "config").string());
		const fs::path missing_executable = temp.root / "never-created" / "agy";
		UAM_ASSERT(missing_executable.is_absolute());
		UAM_ASSERT(!fs::exists(missing_executable.parent_path()));
		ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
		profile.interactive_command = uam::shell::EscapeArg(missing_executable.string());
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.provider_profiles = {profile};
		app.settings.git_enabled = false;
		app.settings.git_auto_worktree = false;
		app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
		app.settings.central_provider_configuration.enabled = false;
		app.settings.central_provider_configuration.uam_control_enabled = false;
		app.settings.central_provider_configuration.mcp_overrides.push_back({profile.id, "local", "shared-setup", false});
		app.settings.computer_use_globally_enabled = false;
		ChatSession chat;
		chat.id = "agv-goal-review-projection";
		chat.provider_id = profile.id;
		chat.execution_host_id = "local";
		chat.workspace_directory = temp.root.string();
		chat.native_session_id = "11111111-2222-4333-8444-555555555555";
		chat.approval_mode = "default";
		chat.command_safety_tier = tier;
		chat.uam_control_enabled = false;
		chat.computer_use_enabled = false;
		app.chats.push_back(chat);
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		auto& session = *app.acp_sessions.front();
		session.chat_id = chat.id;
		session.provider_id = profile.id;
		// QueueGoalInternalPrompt preserves these fields across its launch reset.
		session.processing = true;
		session.queued_prompt = "Private projection diagnostic; this cannot be delivered.";
		session.prompt_request_id = 0;
		session.goal_internal_session = review;
		session.goal_review_turn = review;
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(profile.id);
		const auto original_argv = runtime.BuildStructuredLaunchArgv(profile, app.chats.front());
		UAM_ASSERT(!original_argv.empty());
		UAM_ASSERT_EQ(original_argv.front(), missing_executable.string());
		ChatSession expected_chat = app.chats.front();
		expected_chat.approval_mode = expected_mode;
		if (review)
			expected_chat.native_session_id.clear();
		const std::string expected_argv = uam::acp_detail::JoinAcpArgvForDiagnostics(
		    runtime.BuildStructuredLaunchArgv(profile, expected_chat));
		std::string error;
		UAM_ASSERT(!uam::acp_detail::StartAcpProcessForChat(app, session, app.chats.front(), &error));
		UAM_ASSERT_EQ(error, missing_executable.string() + " not found on PATH in app environment");
		UAM_ASSERT(!session.running);
		UAM_ASSERT_EQ(session.goal_review_turn, review);
		UAM_ASSERT_EQ(session.goal_internal_session, review);
		UAM_ASSERT(!session.pending_local_launch);
		UAM_ASSERT(!fs::exists(missing_executable.parent_path()));
		UAM_ASSERT_EQ(app.chats.front().approval_mode, std::string("default"));
		UAM_ASSERT_EQ(app.chats.front().command_safety_tier, tier);
		UAM_ASSERT_EQ(app.chats.front().native_session_id, chat.native_session_id);
		const auto starts = std::ranges::count_if(session.diagnostics, [](const auto& entry)
		{
			return entry.event == "process_launch" && entry.reason == "starting";
		});
		const auto failures = std::ranges::count_if(session.diagnostics, [](const auto& entry)
		{
			return entry.event == "process_launch" && entry.reason == "start_failed";
		});
		UAM_ASSERT_EQ(starts, 1);
		UAM_ASSERT_EQ(failures, 1);
		for (const auto& entry : session.diagnostics)
		{
			UAM_ASSERT(entry.event != "process_launch" || entry.reason != "started");
			if (entry.event == "process_launch" && (entry.reason == "starting" || entry.reason == "start_failed"))
				UAM_ASSERT(entry.detail.find(", argv=" + expected_argv + ", nativeSessionId=") != std::string::npos);
		}
	}
}

UAM_TEST(AntigravityGoalReviewDefaultLaunchRequiresPlanBeforeProcessCreation)
{
	AssertAntigravityLaunchProjectionBeforeProcessCreation(true, "off", "plan");
}

UAM_TEST(AntigravityGoalReviewEditLaunchRequiresPlanBeforeProcessCreation)
{
	AssertAntigravityLaunchProjectionBeforeProcessCreation(true, "acceptEdits", "plan");
}

UAM_TEST(AntigravityOrdinaryLaunchKeepsPermissionBeforeProcessCreation)
{
	AssertAntigravityLaunchProjectionBeforeProcessCreation(false, "off", "default");
	AssertAntigravityLaunchProjectionBeforeProcessCreation(false, "acceptEdits", "default");
}

UAM_TEST(AntigravityQueuedGoalWorkersResumeSavedConversationAndReviewersIsolate)
{
	for (const bool review : {false, true})
	{
		AntigravityUamGoalFixture fixture;
		ScopedEnvVar home("HOME", fixture.temp.root.string());
		ScopedEnvVar config("XDG_CONFIG_HOME", (fixture.temp.root / "config").string());
		const fs::path missing = fixture.temp.root / "never-created" / "agy";
		ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
		profile.interactive_command = uam::shell::EscapeArg(missing.string());
		fixture.app.provider_profiles = {profile};
		fixture.app.settings.central_provider_configuration.enabled = false;
		fixture.app.settings.central_provider_configuration.uam_control_enabled = false;
		fixture.app.settings.computer_use_globally_enabled = false;
		fixture.Chat().uam_control_enabled = false;
		fixture.Chat().computer_use_enabled = false;
		fixture.Session().processing = false;
		fixture.Session().queued_prompt.clear();
		UAM_ASSERT(!uam::acp_detail::QueueGoalInternalPrompt(fixture.app, fixture.Session(), fixture.Chat(), "Private bounded goal turn", review, "", true));
		UAM_ASSERT_EQ(fixture.Session().goal_internal_session, review);
		UAM_ASSERT_EQ(fixture.Session().last_error, missing.string() + " not found on PATH in app environment");
		UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
		UAM_ASSERT(!fixture.Session().running);
		UAM_ASSERT(!fs::exists(missing.parent_path()));
	}
}

UAM_TEST(AntigravityConnectedGoalReviewDoesNotAddSharedTextTokenEstimates)
{
	AntigravityUamGoalFixture fixture;
	ScopedEnvVar home("HOME", fixture.temp.root.string());
	ScopedEnvVar config("XDG_CONFIG_HOME", (fixture.temp.root / "config").string());
	const fs::path missing = fixture.temp.root / "never-created" / "agy";
	ProviderProfile profile = ProviderProfileStore::DefaultAntigravityProfile();
	profile.interactive_command = uam::shell::EscapeArg(missing.string());
	fixture.app.provider_profiles = {profile};
	fixture.app.settings.central_provider_configuration.enabled = false;
	fixture.app.settings.central_provider_configuration.uam_control_enabled = false;
	fixture.app.settings.computer_use_globally_enabled = false;
	fixture.Chat().uam_control_enabled = false;
	fixture.Chat().computer_use_enabled = false;
	fixture.Chat().messages.push_back({MessageRole::User, "Please verify the bounded objective", "private-fixture"});
	fixture.Session().turn_first_user_message_index = 0;
	fixture.Session().turn_user_message_index = 0;
	fixture.Prepare();
	fixture.Step(1, {{"input_tokens", 12}, {"output_tokens", 3}});
	fixture.Result();
	// This went through shared review scheduling; its missing executable refuses before process creation.
	UAM_ASSERT_EQ(fixture.CurrentGoal().tokens_used, int64_t{15});
	UAM_ASSERT_EQ(fixture.CurrentGoal().status, GoalStatus::Blocked);
	UAM_ASSERT_EQ(fixture.Session().last_error, missing.string() + " not found on PATH in app environment");
	UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
	UAM_ASSERT(!fixture.Session().running);
	UAM_ASSERT(!fs::exists(missing.parent_path()));
}
#endif
