#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/config/provider_chat_defaults.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
namespace
{
	struct ClaudeMaintenanceFixture
	{
		TempDir temp{"uam-claude-maintenance"};
		uam::AppState app;
		uam::AcpSessionState session;
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli");
		ClaudeMaintenanceFixture()
		{
			app.data_root = temp.root;
			ChatSession chat;
			chat.id = "maintenance-chat";
			chat.provider_id = "claude-cli";
			chat.native_session_id = "11111111-2222-4333-8444-555555555555";
			chat.active_goal_id = "existing-goal";
			chat.messages = {{MessageRole::User, "Existing request", "before"}, {MessageRole::Assistant, "Existing answer", "before"}};
			app.chats.push_back(chat);
			session.chat_id = chat.id;
			session.provider_id = chat.provider_id;
			session.session_id = chat.native_session_id;
			session.protocol_kind = "claude-code-stream-json";
			session.running = true;
			session.session_ready = true;
			session.processing = true;
			session.manual_compaction_pending = true;
			session.pending_request_methods[42] = "claude/compact";
			session.turn_serial = 7;
		}
		void Send(nlohmann::json message)
		{
			if (!message.contains("session_id")) message["session_id"] = session.session_id;
			UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, app.chats.front(), message, nullptr));
		}
		nlohmann::json Boundary() const
		{
			return {{"type", "system"}, {"subtype", "compact_boundary"}, {"uuid", "22222222-3333-4444-8555-666666666666"},
			    {"compact_metadata", {{"trigger", "manual"}, {"pre_tokens", 80000}, {"post_tokens", 10000}}}};
		}
	};
}

UAM_TEST(ClaudeManualCompactionRequiresAdvertisedNativeCommand)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli");
	uam::AcpSessionState session;
	session.session_id = "11111111-2222-4333-8444-555555555555";
	std::string method;
	UAM_ASSERT(runtime.SupportsAcpCompaction());
	UAM_ASSERT(runtime.OnAcpBuildCompact(session, 42, method).is_null());
	runtime.OnAcpInitializeResult(session, {{"commands", {{{"name", "compact"}, {"builtin", false}}}}});
	UAM_ASSERT(runtime.OnAcpBuildCompact(session, 42, method).is_null());
	runtime.OnAcpInitializeResult(session, {{"commands", {{{"name", "compact"}, {"builtin", true}}}}});
	const auto request = runtime.OnAcpBuildCompact(session, 42, method);
	UAM_ASSERT_EQ(method, std::string("claude/compact"));
	UAM_ASSERT_EQ(request["type"], nlohmann::json("user"));
	UAM_ASSERT_EQ(request["message"]["role"], nlohmann::json("user"));
	UAM_ASSERT_EQ(request["message"]["content"], nlohmann::json("/compact"));
	UAM_ASSERT_EQ(request["session_id"], nlohmann::json(session.session_id));
}

UAM_TEST(ClaudeManualCompactionRequiresOwnedBoundaryAndAvoidsNormalTurnCompletion)
{
	ClaudeMaintenanceFixture fixture;
	fixture.Send({{"type", "user"}, {"message", {{"role", "user"}, {"content", "/compact"}}}});
	fixture.Send({{"type", "assistant"}, {"message", {{"content", {{{"type", "text"}, {"text", "maintenance chatter"}}}}}}});
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{2});
	fixture.Send(fixture.Boundary());
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{2});
	fixture.Send({{"type", "result"}, {"subtype", "success"}, {"result", "Compacted"}});
	UAM_ASSERT(!fixture.session.manual_compaction_pending);
	UAM_ASSERT(!fixture.session.processing);
	UAM_ASSERT(!fixture.session.context_compaction_in_progress);
	UAM_ASSERT(fixture.session.pending_request_methods.empty());
	UAM_ASSERT_EQ(fixture.session.turn_serial, 7);
	UAM_ASSERT_EQ(fixture.app.chats.front().active_goal_id, std::string("existing-goal"));
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{3});
	const auto& blocks = fixture.app.chats.front().messages.back().blocks;
	UAM_ASSERT_EQ(blocks.size(), std::size_t{1});
	UAM_ASSERT_EQ(blocks.front().type, std::string("context_compaction"));
	UAM_ASSERT_EQ(blocks.front().compaction_tokens_before, std::int64_t{80000});
	UAM_ASSERT(fixture.app.chats.front().messages.back().content.find("Compacted") == std::string::npos);
}

UAM_TEST(ClaudeManualCompactionNoOpFailureCancellationAndForeignEventsDoNotComplete)
{
	for (const std::string outcome : {"noop", "error", "cancelled"})
	{
		ClaudeMaintenanceFixture fixture;
		auto foreign = fixture.Boundary(); foreign["session_id"] = "another-session";
		fixture.Send(foreign);
		fixture.Send({{"type", "result"}, {"subtype", "success"}, {"session_id", "another-session"}});
		UAM_ASSERT(fixture.session.manual_compaction_pending);
		UAM_ASSERT(!fixture.session.claude_manual_compaction_boundary_seen);
		if (outcome != "noop") fixture.Send(fixture.Boundary());
		if (outcome == "cancelled") fixture.session.cancel_requested = true;
		fixture.Send({{"type", "result"}, {"subtype", outcome == "error" ? "error_during_execution" : "success"}, {"is_error", outcome == "error"}});
		UAM_ASSERT(!fixture.session.manual_compaction_pending);
		UAM_ASSERT(!fixture.session.last_error.empty());
		UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{2});
		UAM_ASSERT_EQ(fixture.app.chats.front().active_goal_id, std::string("existing-goal"));
	}
}

UAM_TEST(ClaudeRemoteCompactionBoundaryIsDurableBeforeFinalResult)
{
	ClaudeMaintenanceFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.execution_host_id = "remote-fixture";
	chat.remote_process_exists = true;
	chat.remote_manual_compaction_pending = true;
	fixture.Send(fixture.Boundary());
	const auto before = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id);
	UAM_ASSERT(before.has_value());
	UAM_ASSERT(before->remote_manual_compaction_pending);
	const auto evidence = nlohmann::json::parse(before->remote_manual_compaction_boundary_json);
	UAM_ASSERT_EQ(evidence["sessionId"], nlohmann::json(chat.native_session_id));
	UAM_ASSERT_EQ(evidence["tokensBefore"], nlohmann::json(80000));
	fixture.Send({{"type", "result"}, {"subtype", "success"}});
	const auto after = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id);
	UAM_ASSERT(after.has_value());
	UAM_ASSERT(!after->remote_manual_compaction_pending);
	UAM_ASSERT(after->remote_manual_compaction_boundary_json.empty());
	UAM_ASSERT_EQ(after->messages.back().blocks.front().type, std::string("context_compaction"));
}

UAM_TEST(ClaudeDiscoveryPreservesOnlyAdvertisedReasoningEffortChoices)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli");
	uam::AcpSessionState session;
	runtime.OnAcpInitializeResult(session, {{"models", {
	    {{"value", "supported"}, {"supportsEffort", true}, {"supportedEffortLevels", {"low", "high", "high", "future"}}},
	    {{"value", "unsupported"}, {"supportsEffort", false}, {"supportedEffortLevels", {"high"}}},
	    {{"value", "unknown"}}}}});
	UAM_ASSERT_EQ(session.available_models[0].supported_reasoning_efforts, (std::vector<std::string>{"low", "high"}));
	UAM_ASSERT(session.available_models[0].default_reasoning_effort.empty());
	UAM_ASSERT(session.available_models[1].supported_reasoning_efforts.empty());
	UAM_ASSERT(session.available_models[2].supported_reasoning_efforts.empty());
}

UAM_TEST(ClaudeSavedEffortReachesBothNativeLaunchPaths)
{
	TempDir temp("uam-claude-effort-defaults");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli");
	const auto profile = ProviderProfileStore::DefaultClaudeProfile();
	for (const std::string effort : {"low", "medium", "high", "xhigh", "max"})
	{
		UAM_ASSERT_EQ(uam::provider_chat_defaults::NormalizeReasoningEffort("claude-cli", " " + effort + " "), effort);
		AppSettings settings;
		settings.provider_chat_defaults["claude-cli"].reasoning_effort = effort;
		UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
		AppSettings restored;
		UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", restored).loaded);
		ChatSession chat; chat.provider_id = "claude-cli";
		uam::provider_chat_defaults::ApplyToChat(restored, chat);
		UAM_ASSERT_EQ(chat.reasoning_effort, effort);
		for (const auto& argv : {runtime.BuildInteractiveArgv(profile, chat, {}), runtime.BuildStructuredLaunchArgv(profile, chat)})
		{
			const auto flag = std::ranges::find(argv, "--effort");
			UAM_ASSERT(flag != argv.end() && std::next(flag) != argv.end());
			UAM_ASSERT_EQ(*std::next(flag), effort);
		}
	}
}

UAM_TEST(ClaudeFastModeWaitsForNativeSettingsAndAdvertisedModel)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli");
	for (const bool supported : {false, true})
	{
		uam::AppState app;
		ChatSession chat;
		chat.provider_id = "claude-cli";
		chat.model_id = "fixture-model";
		chat.service_tier = "fast";
		chat.service_tier_explicit = true;
		uam::AcpSessionState session;
		session.initialized = true;
		runtime.OnAcpInitializeResult(session, {{"models", {{{"value", chat.model_id}, {"supportsFastMode", supported}}}}});
		UAM_ASSERT_EQ(session.available_models[0].additional_speed_tiers.empty(), !supported);
		std::string method;
		const auto setup = runtime.OnAcpBuildSetupRequest(8, chat, "/tmp", false, method);
		UAM_ASSERT_EQ(method, std::string("claude/apply_flag_settings"));
		UAM_ASSERT_EQ(setup["request"]["settings"], (nlohmann::json{{"fastMode", true}}));
		UAM_ASSERT(!session.session_ready);
		session.session_setup_request_id = 8;
		UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, method, "8", nlohmann::json::object()));
		UAM_ASSERT_EQ(session.session_ready, supported);
		UAM_ASSERT_EQ(session.last_error.empty(), supported);
	}
	ChatSession disabled;
	disabled.service_tier_explicit = true;
	std::string method;
	const auto setup = runtime.OnAcpBuildSetupRequest(9, disabled, "/tmp", false, method);
	UAM_ASSERT_EQ(setup["request"]["settings"], (nlohmann::json{{"fastMode", false}}));
}

UAM_TEST(ClaudeFastTerminalSettingsPreservePermissionsAndFileOverrides)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli");
	const auto profile = ProviderProfileStore::DefaultClaudeProfile();
	for (const bool fast : {false, true})
	{
		ChatSession chat;
		chat.service_tier = fast ? "fast" : "";
		chat.service_tier_explicit = true;
		AppSettings settings;
		settings.provider_extra_flags = "--settings '{\"permissions\":{\"deny\":[\"Bash\"]},\"disableAllHooks\":true}'";
		const auto argv = runtime.BuildInteractiveArgv(profile, chat, settings);
		const auto flag = std::ranges::find(argv, "--settings");
		UAM_ASSERT(flag != argv.end() && std::next(flag) != argv.end());
		const auto merged = nlohmann::json::parse(*std::next(flag));
		UAM_ASSERT_EQ(merged["permissions"]["deny"][0], nlohmann::json("Bash"));
		UAM_ASSERT_EQ(merged["disableAllHooks"], nlohmann::json(true));
		UAM_ASSERT_EQ(merged["fastMode"], nlohmann::json(fast));
		settings.provider_extra_flags = "--settings /remote/settings.json";
		const auto with_file = runtime.BuildInteractiveArgv(profile, chat, settings);
		const auto file_flag = std::ranges::find(with_file, "--settings");
		UAM_ASSERT_EQ(std::ranges::count(with_file, "--settings"), 1);
		UAM_ASSERT_EQ(*std::next(file_flag), std::string("/remote/settings.json"));
	}
}

UAM_TEST(ClaudeProviderDefaultsRetainOnlyNativeFastTier)
{
	for (const std::string provider : {"claude-cli", "codex-cli", "gemini-cli"})
	{
		ProviderChatDefaults defaults;
		defaults.service_tier = "fast";
		ChatSession chat; chat.provider_id = provider;
		uam::provider_chat_defaults::ApplyToChat(chat, defaults);
		UAM_ASSERT_EQ(chat.service_tier, provider == "gemini-cli" ? std::string{} : std::string("fast"));
		defaults.service_tier = "flex";
		uam::provider_chat_defaults::ApplyToChat(chat, defaults);
		UAM_ASSERT_EQ(chat.service_tier, provider == "codex-cli" ? std::string("flex") : std::string{});
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
UAM_TEST(ClaudeRecoveredManualCompactionRetiresDurableMaintenanceAndCancel)
{
	for (const bool cancelled : {false, true})
	{
		ClaudeMaintenanceFixture fixture;
		auto& chat = fixture.app.chats.front();
		chat.execution_host_id = fixture.session.process_execution_host_id = "remote-host";
		chat.remote_process_exists = chat.remote_manual_compaction_pending = chat.remote_turn_reconnect_pending = true;
		chat.remote_process_control_token = fixture.session.remote_output_delivery_token = "private-claude-maintenance-token";
		fixture.session.recovering_remote_turn = true;
		chat.remote_pending_requests.push_back({.request_id = 42, .method = "claude/compact", .delivery_id = "compact", .payload = "native maintenance"});
		if (cancelled)
		{
			fixture.session.cancel_requested = true;
			fixture.session.cancel_request_id = 43;
			fixture.session.pending_request_methods[43] = "claude/interrupt";
			chat.remote_pending_requests.push_back({.request_id = 43, .method = "claude/interrupt", .delivery_id = "cancel", .payload = "native interrupt"});
		}
		fixture.Send(fixture.Boundary());
		fixture.Send({{"type", "result"}, {"subtype", "success"}, {"result", "Compacted"}});
		UAM_ASSERT(!fixture.session.recovering_remote_turn && !fixture.session.manual_compaction_pending && !fixture.session.processing);
		UAM_ASSERT(!chat.remote_manual_compaction_pending && !chat.remote_turn_reconnect_pending);
		UAM_ASSERT(chat.remote_manual_compaction_boundary_json.empty());
		UAM_ASSERT_EQ(chat.remote_pending_requests.size(), cancelled ? std::size_t{2} : std::size_t{1});
		for (const auto& request : chat.remote_pending_requests) UAM_ASSERT(request.response_consumed && request.payload.empty());
		UAM_ASSERT(chat.remote_pending_requests.front().response_consumed && chat.remote_pending_requests.front().payload.empty());
		const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT(!saved->remote_manual_compaction_pending && !saved->remote_turn_reconnect_pending);
		uam::AcpSessionState recovered;
		recovered.provider_id = "claude-cli";
		recovered.protocol_kind = "claude-code-stream-json";
		recovered.process_execution_host_id = chat.execution_host_id;
		recovered.remote_output_delivery_token = chat.remote_process_control_token;
		uam::acp_detail::RestoreRemoteAcpRequests(recovered, *saved);
		UAM_ASSERT(!recovered.manual_compaction_pending && !recovered.processing && recovered.pending_request_methods.empty());
	}
}
#endif
