#include "test_harness.h"

#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_launch.h"

using namespace uam_test;

UAM_TEST(CliTerminalRejectsImportedReadOnlyTranscriptBeforeProviderLaunch)
{
	uam::AppState app;
	uam::CliTerminalState terminal;
	ChatSession chat;
	chat.id = "chat-imported-read-only";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.imported_read_only = true;

	UAM_ASSERT(!uam::StartCliTerminalForChat(app, terminal, chat, 24, 80));
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Disabled);
	UAM_ASSERT(uam::strings::Contains(terminal.last_error, "Imported transcripts are read-only"));
}

#if defined(__APPLE__)
UAM_TEST(CliTerminalRoutesRemoteChatsThroughSshWithoutLaunchingTheProviderLocally)
{
	TempDir temp("uam-remote-terminal-route");
	const fs::path captured = temp.root / "ssh-argv.txt";
	const fs::path ssh = temp.root / "ssh";
	UAM_ASSERT(uam::io::WriteTextFile(
	    ssh, "#!/bin/sh\nprintf '%s\\n' \"$@\" > " + ShellQuoteForTest(captured.string()) +
	             "\nprintf 'remote-terminal-ready\\n'\n"));
	fs::permissions(ssh, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	ScopedEnvVar path("PATH", temp.root.string() + ":" + inherited_path);

	uam::AppState app;
	app.data_root = temp.root;
	ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	provider.output_mode = uam::provider_profile_constants::kOutputModeCli;
	provider.interactive_command = "/usr/bin/printf provider-must-stay-encoded";
	app.provider_profiles = {provider};
	app.settings.active_provider_id = provider.id;
	ExecutionHost host;
	host.id = "lab";
	host.label = "Lab";
	host.transport = "ssh";
	host.ssh_alias = "home-lab";
	host.runner_status = "ready";
	app.settings.execution_hosts = {host};
	uam::execution_hosts::Normalize(app.settings.execution_hosts);

	ChatSession chat;
	chat.id = "remote-terminal-chat";
	chat.provider_id = provider.id;
	chat.execution_host_id = host.id;
	chat.workspace_directory = temp.root.string();
	uam::CliTerminalState terminal;
	UAM_ASSERT(uam::StartCliTerminalForChat(app, terminal, chat, 24, 80));
	for (int attempt = 0; attempt < 100 && !fs::exists(captured); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(fs::exists(captured));
	const std::string args = uam::io::ReadTextFile(captured);
	UAM_ASSERT(uam::strings::Contains(args, "home-lab"));
	UAM_ASSERT(uam::strings::Contains(args, "uam-runner"));
	UAM_ASSERT(uam::strings::Contains(args, "terminal"));
	UAM_ASSERT(!uam::strings::Contains(args, "provider-must-stay-encoded"));
	uam::StopCliTerminal(terminal, true, uam::CliTerminalStopMode::FastExit);
}
#endif

UAM_TEST(CliTurnInactivityRecoveryIgnoresProviderOutputNoiseAndUsesInterruptGrace)
{
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	terminal.last_busy_time_s = 30.0;
	terminal.last_user_input_time_s = 89.0;
	terminal.last_ai_output_time_s = 89.0;
	UAM_ASSERT_EQ(uam::CliTerminalInactivityRecovery(terminal, 89.0, 60.0), uam::CliTerminalInactivityRecoveryAction::None);
	UAM_ASSERT_EQ(uam::CliTerminalInactivityRecovery(terminal, 90.0, 60.0), uam::CliTerminalInactivityRecoveryAction::Interrupt);
	terminal.inactivity_interrupt_requested_time_s = 200.0;
	UAM_ASSERT_EQ(uam::CliTerminalInactivityRecovery(terminal, 204.9, 60.0), uam::CliTerminalInactivityRecoveryAction::None);
	UAM_ASSERT_EQ(uam::CliTerminalInactivityRecovery(terminal, 205.0, 60.0), uam::CliTerminalInactivityRecoveryAction::Stop);
}

UAM_TEST(ProviderChildEnvironmentIsolationKeepsOnlySelectedProviderApiKeys)
{
	const auto value_for = [](const std::vector<std::pair<std::string, std::string>>& values, std::string_view name) -> const std::string*
	{
		const auto found = std::ranges::find_if(values, [name](const auto& value) { return value.first == name; });
		return found == values.end() ? nullptr : &found->second;
	};

	ProviderProfile codex = ProviderProfileStore::DefaultCodexProfile();
	const auto codex_environment = uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(codex);
	UAM_ASSERT(value_for(codex_environment, "OPENAI_API_KEY") == nullptr);
	UAM_ASSERT(value_for(codex_environment, "ANTHROPIC_API_KEY") != nullptr);
	UAM_ASSERT(value_for(codex_environment, "ANTHROPIC_API_KEY")->empty());
	UAM_ASSERT(value_for(codex_environment, "GEMINI_API_KEY") != nullptr);
	UAM_ASSERT(value_for(codex_environment, "GOOGLE_API_KEY") != nullptr);

	ProviderProfile claude = ProviderProfileStore::DefaultClaudeProfile();
	const auto claude_environment = uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(claude);
	UAM_ASSERT(value_for(claude_environment, "ANTHROPIC_API_KEY") == nullptr);
	UAM_ASSERT(value_for(claude_environment, "OPENAI_API_KEY") != nullptr);

	ProviderProfile opencode = ProviderProfileStore::DefaultOpenCodeProfile();
	UAM_ASSERT(uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(opencode).empty());

	uam::AppState worker_app;
	const uam::ProviderWorkerInvocation worker = uam::BuildProviderWorkerInvocation(
	    worker_app, codex, AppSettings{}, "Review without unrelated provider credentials.", "",
	    uam::ProviderWorkerPathMode::BasePath);
	UAM_ASSERT(!worker.Empty());
	UAM_ASSERT(value_for(worker.environment_overrides, "OPENAI_API_KEY") == nullptr);
	UAM_ASSERT(value_for(worker.environment_overrides, "ANTHROPIC_API_KEY") != nullptr);
	UAM_ASSERT(value_for(worker.environment_overrides, "ANTHROPIC_API_KEY")->empty());

	ScopedEnvVar preserve("UAM_PRESERVE_PROVIDER_CHILD_SECRETS", "1");
	UAM_ASSERT(uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(codex).empty());
}

UAM_TEST(CliSilentTurnInterruptsThenStopsWithoutReplayingPrompt)
{
	TempDir temp("uam-cli-inactivity-timeout");
	uam::AppState app;
	app.settings.active_turn_inactivity_timeout_seconds = 60;
	uam::CliTerminalState terminal;
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd.exe", "/C", "ping -n 31 127.0.0.1 >NUL"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "trap '' HUP INT TERM; while :; do sleep 1; done"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, temp.root, argv, &error));
	terminal.running = true;
	terminal.should_launch = true;
	terminal.lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	terminal.turn_state = uam::CliTerminalTurnState::Busy;
	terminal.generation_in_progress = true;
	terminal.last_busy_time_s = 1.0;
	terminal.last_user_input_time_s = 1.0;
	terminal.last_ai_output_time_s = 1.0;

	UAM_ASSERT(uam::HandleCliTerminalInactivityTimeout(app, terminal, 61.0));
	UAM_ASSERT(terminal.running);
	UAM_ASSERT_EQ(terminal.inactivity_interrupt_requested_time_s, 61.0);
	UAM_ASSERT(uam::strings::Contains(terminal.last_error, "will not be replayed"));
	UAM_ASSERT(uam::HandleCliTerminalInactivityTimeout(app, terminal, 66.0));
	UAM_ASSERT(!terminal.running);
	UAM_ASSERT(!terminal.should_launch);
	UAM_ASSERT(uam::strings::Contains(terminal.last_error, "not replayed"));
}

UAM_TEST(GeminiCliCompatibilityAcceptsCurrentStableVersions)
{
	UAM_ASSERT_EQ(std::string(uam::PreferredGeminiCliVersion()), std::string("latest"));
	UAM_ASSERT_EQ(uam::SupportedGeminiCliVersionsLabel(), std::string("0.55.1 or newer (verified 2026-08-27)"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.38.1"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.55.0"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.55.1"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.56.0"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("1.0.0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.30.0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0..0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.36."));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("-1.36.0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.39.0-beta"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("not-a-version"));
}

UAM_TEST(GeminiPromptClassifierStripsAnsiAndDetectsPrompt)
{
	const std::string output = "\x1b[33mThinking...\x1b[0m\r\n\xe2\x94\x82 > Type your message or @path\r\n";
	UAM_ASSERT(uam::GeminiCliRecentOutputIndicatesInputPrompt(output));

	const std::string stripped = uam::StripTerminalControlSequencesForLifecycle("\x1b[31mhello\x1b[0m\b!");
	UAM_ASSERT_EQ(stripped, std::string("hell!"));
	UAM_ASSERT_EQ(uam::NormalizeGeminiPromptLine(std::string_view("xx\xe2\x94\x82 > Type your message yy").substr(2, 23)), std::string("> Type your message"));
	UAM_ASSERT(!uam::GeminiCliRecentOutputIndicatesInputPrompt("tool output is still streaming\nno prompt yet"));
}

UAM_TEST(CliInitialPromptCanSettleImmediately)
{
	uam::CliTerminalState terminal;
	terminal.running = true;
	uam::MarkCliTerminalTurnBusy(terminal, false);
	terminal.last_busy_time_s = 100.0;
	terminal.current_turn_output_bytes = "\xE2\x80\xBA Send message";

	UAM_ASSERT(uam::CliTerminalPromptConfirmsTurnIdle(
	    terminal,
	    uam::CodexCliRecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes),
	    true,
	    100.05));
}

UAM_TEST(CliStalePromptCannotPoisonLaterTurnOutput)
{
	const std::string old_prompt = "\xE2\x80\xBA Send message";
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.recent_output_bytes = old_prompt;
	terminal.current_turn_output_bytes = old_prompt;

	uam::MarkCliTerminalTurnBusy(terminal);
	terminal.last_busy_time_s = 100.0;

	terminal.current_turn_output_bytes.append(old_prompt);
	UAM_ASSERT(!uam::CliTerminalPromptConfirmsTurnIdle(
	    terminal,
	    uam::CodexCliRecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes),
	    true,
	    100.05));
	UAM_ASSERT(terminal.current_turn_output_bytes.empty());

	terminal.current_turn_output_bytes.append("\nWorking on the next turn");
	UAM_ASSERT(!uam::CliTerminalPromptConfirmsTurnIdle(
	    terminal,
	    uam::CodexCliRecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes),
	    true,
	    100.5));

	terminal.current_turn_output_bytes.append("\n\xE2\x80\xBA Send message");
	UAM_ASSERT(uam::CliTerminalPromptConfirmsTurnIdle(
	    terminal,
	    uam::CodexCliRecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes),
	    true,
	    100.6));
	UAM_ASSERT_EQ(terminal.recent_output_bytes, old_prompt);
}

UAM_TEST(CliFastPromptSettlesWithoutAdditionalOutput)
{
	uam::CliTerminalState terminal;
	terminal.running = true;
	uam::MarkCliTerminalTurnBusy(terminal);
	terminal.last_busy_time_s = 100.0;
	terminal.current_turn_output_bytes = "\xE2\x80\xBA Send message";

	UAM_ASSERT(!uam::CliTerminalPromptConfirmsTurnIdle(
	    terminal,
	    uam::CodexCliRecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes),
	    true,
	    100.05));
	UAM_ASSERT(terminal.current_turn_output_bytes.empty());
	UAM_ASSERT(uam::CliTerminalPromptConfirmsTurnIdle(terminal, false, false, 100.3));
}

UAM_TEST(CliLifecycleTransitionsDriveBackgroundShutdownEligibility)
{
	uam::AppState app;
	app.settings.cli_idle_timeout_seconds = 60;
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.frontend_chat_id = "chat-1";
	terminal.attached_chat_id = "chat-1";
	terminal.attached_session_id = "native-1";

	uam::MarkCliTerminalTurnBusy(terminal);
	UAM_ASSERT_EQ(uam::kCliTerminalProcessingLifecycleStates.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(uam::kCliTerminalQuitCommand, std::string_view("/quit\r\n"));
	UAM_ASSERT_EQ(uam::kCliTerminalDefaultRows, 24);
	UAM_ASSERT_EQ(uam::kCliTerminalDefaultCols, 80);
	UAM_ASSERT_EQ(uam::ClampCliTerminalResizeRows(0), 1);
	UAM_ASSERT_EQ(uam::ClampCliTerminalResizeCols(0), 1);
	UAM_ASSERT_EQ(uam::ClampCliTerminalLaunchRows(0), 8);
	UAM_ASSERT_EQ(uam::ClampCliTerminalLaunchCols(0), 20);
	UAM_ASSERT_EQ(uam::CliDiagnosticQuotedField("quote\" newline\nslash\\"), std::string("\"quote\\\" newline slash\\\\\""));
	const std::string nearly_full_diagnostic_field(uam::kCliDiagnosticFieldMaxBytes - 1, 'x');
	UAM_ASSERT_EQ(uam::CliDiagnosticQuotedField(nearly_full_diagnostic_field + "\""), "\"" + nearly_full_diagnostic_field + "...\"");
	UAM_ASSERT_EQ(uam::CliDiagnosticQuotedField(nearly_full_diagnostic_field + "\\"), "\"" + nearly_full_diagnostic_field + "...\"");
	UAM_ASSERT(uam::CliTerminalLifecycleStateIsProcessing(uam::CliTerminalLifecycleState::Busy));
	UAM_ASSERT(uam::CliTerminalLifecycleStateIsProcessing(uam::CliTerminalLifecycleState::ShuttingDown));
	UAM_ASSERT(!uam::CliTerminalLifecycleStateIsProcessing(uam::CliTerminalLifecycleState::Idle));
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Busy);
	UAM_ASSERT_EQ(terminal.turn_state, uam::CliTerminalTurnState::Busy);
	UAM_ASSERT_EQ(uam::CliLifecycleStateLabel(terminal), std::string(uam::CliTerminalLifecycleStateLabel(terminal)));
	UAM_ASSERT_EQ(std::string(uam::CliTurnStateLabel(terminal)), std::string("busy"));
	UAM_ASSERT(terminal.generation_in_progress);
	terminal.last_idle_confirmed_time_s = 1.0;
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 121.0));

	uam::MarkCliTerminalTurnIdle(terminal);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Idle);
	UAM_ASSERT_EQ(terminal.turn_state, uam::CliTerminalTurnState::Idle);
	UAM_ASSERT_EQ(std::string(uam::CliTurnStateLabel(terminal)), std::string("idle"));
	UAM_ASSERT(!terminal.generation_in_progress);
	terminal.last_idle_confirmed_time_s = 59.0;
	UAM_ASSERT(uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	app.settings.cli_idle_timeout_seconds = 120;
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	app.settings.cli_idle_timeout_seconds = 60;
	UAM_ASSERT(uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, std::string_view("xxchat-2yy").substr(2, 6), 120.0));
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-1", 120.0));
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "native-1", 120.0));

	terminal.ui_attached = true;
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	terminal.ui_attached = false;

	PendingRuntimeCall pending;
	pending.chat_id = "chat-1";
	app.pending_calls.push_back(std::move(pending));
	UAM_ASSERT(uam::PendingCallMatchesCliTerminalIdentity(app, " chat-1 "));
	UAM_ASSERT(!uam::PendingCallMatchesCliTerminalIdentity(app, "   "));
	UAM_ASSERT(uam::CliTerminalHasPendingCall(app, terminal));
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));

	app.pending_calls.clear();
	terminal.attached_chat_id = " chat-detached ";
	terminal.attached_session_id = " native-detached ";
	PendingRuntimeCall native_pending;
	native_pending.chat_id = "native-detached";
	app.pending_calls.push_back(std::move(native_pending));
	UAM_ASSERT(uam::CliTerminalHasPendingCall(app, terminal));
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));

	app.chats_with_unseen_updates.insert("chat-1");
	uam::ClearCliReadyForChat(app, std::string_view("xx chat-1 yy").substr(2, 8));
	UAM_ASSERT(app.chats_with_unseen_updates.empty());
}

UAM_TEST(CliWriteTransportFailureCannotRemainRunning)
{
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.should_launch = true;
	uam::MarkCliTerminalTurnBusy(terminal);

	constexpr char input = 'x';
	UAM_ASSERT(!uam::WriteToCliTerminal(terminal, &input, 1));
	UAM_ASSERT(!terminal.running);
	UAM_ASSERT(!terminal.should_launch);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Stopped);
	UAM_ASSERT(!terminal.last_error.empty());
}

UAM_TEST(CliMissingOutputTransportCannotRemainRunning)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.should_launch = true;
	uam::MarkCliTerminalTurnBusy(terminal);

	UAM_ASSERT(uam::PollCliTerminal(nullptr, app, terminal, false));
	UAM_ASSERT(!terminal.running);
	UAM_ASSERT(!terminal.should_launch);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Stopped);
	UAM_ASSERT(!terminal.last_error.empty());
}

UAM_TEST(CliTerminalSteeringInputIsBracketedAndDropsUnsafeControls)
{
	const std::string input = uam::BuildCliTerminalPromptInput(std::string_view("xx  Change\x1b[31m direction\nnow  yy").substr(2, 30));
	UAM_ASSERT(input.starts_with("\x1b[200~"));
	UAM_ASSERT(input.ends_with("\x1b[201~\r"));
	UAM_ASSERT(input.find('\x1b', 1) == std::string::npos || input.find('\x1b', 1) == input.size() - 7);
	UAM_ASSERT(input.find("Change[31m direction\nnow") != std::string::npos);
	UAM_ASSERT(uam::BuildCliTerminalPromptInput(" \x01\x02 ").empty());

	uam::CliTerminalState terminal;
	terminal.running = true;
	uam::MarkCliTerminalTurnBusy(terminal);
	std::string error;
	UAM_ASSERT(!uam::RequestCliTerminalSteer(terminal, "Preserve this", false, &error));
	UAM_ASSERT_EQ(terminal.pending_steer_prompt, std::string("Preserve this"));
	UAM_ASSERT(error.find("retained for retry") != std::string::npos);
	UAM_ASSERT(!uam::RequestCliTerminalSteer(terminal, "Duplicate", false, &error));
	UAM_ASSERT_EQ(terminal.pending_steer_prompt, std::string("Preserve this"));
	UAM_ASSERT(!uam::RequestCliTerminalSteer(terminal, "Changed retry", true, &error));
	UAM_ASSERT_EQ(terminal.pending_steer_prompt, std::string("Preserve this"));

	terminal.pending_steer_started_time_s = 100.0;
	terminal.pending_steer_restart_attempted = false;
	UAM_ASSERT_EQ(uam::CliTerminalSteerRecovery(terminal, 102.9), uam::CliTerminalSteerRecoveryAction::None);
	UAM_ASSERT_EQ(uam::CliTerminalSteerRecovery(terminal, 103.0), uam::CliTerminalSteerRecoveryAction::Restart);
	terminal.pending_steer_restart_attempted = true;
	terminal.last_error.clear();
	UAM_ASSERT_EQ(uam::CliTerminalSteerRecovery(terminal, 109.9), uam::CliTerminalSteerRecoveryAction::None);
	UAM_ASSERT_EQ(uam::CliTerminalSteerRecovery(terminal, 110.0), uam::CliTerminalSteerRecoveryAction::ReportTimeout);
}

UAM_TEST(CliTerminalActiveHelpersUseTrimmedIdentity)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-1";
	chat.native_session_id = " native-1 ";
	app.chats.push_back(chat);

	UAM_ASSERT(!uam::HasAnyActiveCliTerminal(app));
	UAM_ASSERT(!uam::ChatHasActiveCliTerminal(app, "chat-1"));
	UAM_ASSERT(uam::ChatExists(app, " chat-1 "));
	UAM_ASSERT(uam::NativeChatMatchesPreferredSyncId(chat, " chat-1 "));
	UAM_ASSERT(uam::NativeChatMatchesPreferredSyncId(chat, " native-1 "));
	UAM_ASSERT(!uam::NativeChatMatchesPreferredSyncId(chat, "   "));
	UAM_ASSERT(!uam::NativeChatMatchesPreferredSyncId(chat, "other-chat"));
	uam::MarkChatUnseen(app, " chat-1 ");
	UAM_ASSERT(app.chats_with_unseen_updates.contains("chat-1"));
	UAM_ASSERT(!app.chats_with_unseen_updates.contains(" chat-1 "));

	app.cli_terminals.push_back(nullptr);
	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->running = true;
	terminal->attached_chat_id = " chat-1 ";
	app.cli_terminals.push_back(std::move(terminal));

	UAM_ASSERT(uam::HasAnyActiveCliTerminal(app));
	UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, "chat-1"));
	UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, " chat-1 "));
	UAM_ASSERT(uam::FindCliTerminalForChat(app, std::string_view("xxchat-1yy").substr(2, 6)) != nullptr);
	UAM_ASSERT_EQ(uam::NormalizeChatSyncTargetId(std::string_view("xx chat-1 yy").substr(2, 8)), std::string("chat-1"));
	UAM_ASSERT(!uam::ChatHasActiveCliTerminal(app, "  "));
	UAM_ASSERT(!uam::ChatHasActiveCliTerminal(app, "other-chat"));
}

UAM_TEST(ChatRuntimeHelpersRecognizeAcpAndCliActiveTurns)
{
	uam::AppState app;
	UAM_ASSERT(!uam::ChatHasActiveAcpSession(app, "chat-1"));
	UAM_ASSERT(!uam::ChatHasBusyCliTerminal(app, "chat-1"));
	UAM_ASSERT(!uam::ChatHasRunningRuntime(app, "chat-1"));

	auto acp = std::make_unique<uam::AcpSessionState>();
	acp->chat_id = " chat-1 ";
	acp->running = true;
	acp->waiting_for_user_input = true;
	UAM_ASSERT(uam::AcpSessionIsWaitingForInput(*acp));
	UAM_ASSERT(uam::AcpSessionHasActiveTurn(*acp));
	app.acp_sessions.push_back(std::move(acp));

	UAM_ASSERT(uam::ChatHasActiveAcpSession(app, "chat-1"));
	UAM_ASSERT(uam::ChatHasActiveAcpSession(app, " chat-1 "));
	UAM_ASSERT(uam::ChatHasActiveAcpSession(app, std::string_view("xx chat-1 yy").substr(2, 8)));
	UAM_ASSERT(uam::ChatHasRunningRuntime(app, "chat-1"));
	UAM_ASSERT(!uam::ChatHasActiveAcpSession(app, "  "));
	UAM_ASSERT(!uam::ChatHasRunningRuntime(app, "  "));

	PendingRuntimeCall pending;
	pending.chat_id = " pending-1 ";
	app.pending_calls.push_back(std::move(pending));
	UAM_ASSERT(uam::HasPendingCallForChat(app, "pending-1"));
	UAM_ASSERT(uam::HasPendingCallForChat(app, std::string_view("xx pending-1 yy").substr(2, 11)));
	UAM_ASSERT(uam::FirstPendingCallForChat(app, " pending-1 ") != nullptr);
	UAM_ASSERT(uam::FirstPendingCallForChat(app, std::string_view("xxpending-1yy").substr(2, 9)) != nullptr);
	UAM_ASSERT(!uam::HasPendingCallForChat(app, "  "));
	UAM_ASSERT(uam::FirstPendingCallForChat(app, "  ") == nullptr);
	UAM_ASSERT(uam::ChatHasRunningRuntime(app, "pending-1"));

	uam::AcpSessionState canceling_session;
	canceling_session.cancel_requested = true;
	canceling_session.cancel_request_id = 7;
	UAM_ASSERT(!uam::AcpSessionHasActiveTurn(canceling_session));
	UAM_ASSERT(uam::AcpSessionHasCancelableWork(canceling_session));
	UAM_ASSERT(uam::AcpSessionHasPendingCancel(canceling_session));
	UAM_ASSERT(uam::AcpSessionHasPendingRuntimeRequest(canceling_session));
	UAM_ASSERT(uam::AcpSessionHasBlockingRuntimeWork(canceling_session));

	uam::AcpSessionState startup_model_session;
	startup_model_session.startup_model_request_id = 8;
	UAM_ASSERT(!uam::AcpSessionHasActiveTurn(startup_model_session));
	UAM_ASSERT(uam::AcpSessionHasPendingRuntimeRequest(startup_model_session));
	UAM_ASSERT(uam::AcpSessionHasBlockingRuntimeWork(startup_model_session));

	app.acp_sessions.clear();
	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->running = true;
	terminal->attached_session_id = " native-1 ";
	terminal->lifecycle_state = uam::CliTerminalLifecycleState::ShuttingDown;
	app.cli_terminals.push_back(std::move(terminal));

	UAM_ASSERT(uam::ChatHasBusyCliTerminal(app, "native-1"));
	UAM_ASSERT(uam::ChatHasBusyCliTerminal(app, std::string_view("xx native-1 yy").substr(2, 10)));
	UAM_ASSERT(uam::ChatHasRunningRuntime(app, " native-1 "));
}

UAM_TEST(NativeHistorySnapshotDigestTracksSameLengthMessageContentChanges)
{
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "native-1";
	chat.updated_at = "2026-05-14 10:00:00";

	Message message;
	message.role = MessageRole::Assistant;
	message.created_at = "2026-05-14 10:00:01";
	message.provider = "gemini-cli";
	message.content = "alpha";
	chat.messages.push_back(message);

	ChatSession changed = chat;
	changed.messages.back().content = "bravo";

	UAM_ASSERT_EQ(chat.messages.back().content.size(), changed.messages.back().content.size());
	UAM_ASSERT(uam::NativeHistorySnapshotDigest({chat}) != uam::NativeHistorySnapshotDigest({changed}));
}

UAM_TEST(CliTerminalIdentitySeparatesFrontendChatAndNativeSession)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-local";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = " native-session ";
	app.chats.push_back(chat);

	uam::CliTerminalState terminal;
	terminal.terminal_id = " term-chat-local ";
	terminal.frontend_chat_id = "chat-local";
	terminal.attached_chat_id = "chat-local";
	terminal.attached_session_id = "native-session";

	UAM_ASSERT(uam::CliTerminalMatchesTerminalId(terminal, "term-chat-local"));
	UAM_ASSERT(uam::CliTerminalMatchesTerminalId(terminal, " term-chat-local "));
	UAM_ASSERT(!uam::CliTerminalMatchesTerminalId(terminal, "other-terminal"));
	UAM_ASSERT(uam::CliTerminalMatchesChatId(terminal, "chat-local"));
	UAM_ASSERT(uam::CliTerminalMatchesChatId(terminal, "native-session"));
	UAM_ASSERT(!uam::CliTerminalMatchesChatId(terminal, "other-chat"));
	UAM_ASSERT(uam::CliTerminalMatchesChat(terminal, chat));
	UAM_ASSERT_EQ(uam::CliTerminalPrimaryChatId(terminal), std::string("chat-local"));
	UAM_ASSERT_EQ(uam::CliTerminalSyncTargetId(terminal), std::string("native-session"));
	UAM_ASSERT_EQ(uam::FindChatIndexForCliTerminal(app, terminal), 0);

	ChatSession invalid_codex_native_id = chat;
	invalid_codex_native_id.provider_id = uam::provider_ids::kCodexCli;
	invalid_codex_native_id.native_session_id = "native-session";
	UAM_ASSERT(uam::CliTerminalMatchesChat(terminal, invalid_codex_native_id));

	terminal.frontend_chat_id = " chat-local ";
	terminal.attached_chat_id = " chat-local ";
	terminal.attached_session_id = " native-session ";
	UAM_ASSERT_EQ(uam::TrimCliTerminalIdentityView(" native-session "), std::string_view("native-session"));
	UAM_ASSERT(uam::TrimmedCliTerminalIdMatches(" native-session ", " native-session "));
	UAM_ASSERT(uam::TrimmedCliTerminalIdMatches("   ", "   "));
	UAM_ASSERT(uam::CliTerminalMatchesNonEmptyIdentity(" native-session ", " native-session "));
	UAM_ASSERT(!uam::CliTerminalMatchesNonEmptyIdentity("   ", "   "));
	UAM_ASSERT(!uam::CliTerminalMatchesTerminalId(terminal, "   "));
	UAM_ASSERT(!uam::CliTerminalMatchesChatId(terminal, "   "));
	UAM_ASSERT(uam::CliTerminalMatchesChatId(terminal, " native-session "));
	UAM_ASSERT(uam::CliTerminalMatchesChat(terminal, chat));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(terminal), std::string("chat-local"));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(terminal), std::string("native-session"));
	UAM_ASSERT_EQ(uam::CliTerminalPrimaryChatId(terminal), std::string("chat-local"));
	UAM_ASSERT_EQ(uam::CliTerminalSyncTargetId(terminal), std::string("native-session"));
}

UAM_TEST(CliTerminalMatchesResolvedNativeSessionWhenRawSessionIsMissing)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-opencode";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-1";

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = "chat-opencode";
	terminal.attached_chat_id = "chat-opencode";
	terminal.attached_session_id = "opencode-session-1";

	UAM_ASSERT(uam::CliTerminalMatchesChat(app, terminal, app.chats.front()));
	UAM_ASSERT_EQ(uam::FindChatIndexForCliTerminal(app, terminal), 0);
	UAM_ASSERT_EQ(uam::FindChatForCliTerminal(app, terminal), &app.chats.front());
}

UAM_TEST(CliTerminalMatchesResolvedNativeSessionPrefersLiveRawChatForSessionOnlyTerminal)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "stale-opencode-session";
	stale_chat.updated_at = "2026-05-31T12:00:00.000Z";
	stale_chat.last_opened_at = "2026-05-31T12:00:00.000Z";
	app.chats.push_back(stale_chat);
	app.resolved_native_sessions_by_chat_id[stale_chat.id] = "opencode-session-1";

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "other-session";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);
	app.resolved_native_sessions_by_chat_id[live_chat.id] = "opencode-session-1";

	uam::CliTerminalState terminal;
	terminal.attached_session_id = "opencode-session-1";

	UAM_ASSERT(uam::CliTerminalMatchesChat(app, terminal, app.chats.front()));
	UAM_ASSERT(uam::CliTerminalMatchesChat(app, terminal, app.chats.back()));
	UAM_ASSERT_EQ(uam::FindChatForCliTerminal(app, terminal), &app.chats.back());
}

UAM_TEST(CliTerminalMatchesResolvedNativeSessionPrefersLiveChatOverStaleRawCollision)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	app.chats.push_back(stale_chat);

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "stale-opencode-session";
	app.chats.push_back(live_chat);
	app.resolved_native_sessions_by_chat_id[live_chat.id] = "opencode-session-1";

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = "chat-live";
	terminal.attached_chat_id = "chat-live";
	terminal.attached_session_id = "opencode-session-1";

	UAM_ASSERT(uam::CliTerminalMatchesChat(app, terminal, app.chats.front()));
	UAM_ASSERT(uam::CliTerminalMatchesChat(app, terminal, app.chats.back()));
	UAM_ASSERT_EQ(uam::FindChatForCliTerminal(app, terminal), &app.chats.back());
}

UAM_TEST(FindCliTerminalForChatPrefersLiveTerminalOverStaleCollision)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	stale_chat.updated_at = "2026-05-31T12:00:00.000Z";
	stale_chat.last_opened_at = "2026-05-31T12:00:00.000Z";
	app.chats.push_back(stale_chat);
	app.resolved_native_sessions_by_chat_id[stale_chat.id] = "opencode-session-1";

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "opencode-session-1";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = stale_chat.id;
	stale_terminal->attached_chat_id = stale_chat.id;
	stale_terminal->attached_session_id = "opencode-session-1";
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = live_chat.id;
	live_terminal->attached_chat_id = live_chat.id;
	live_terminal->attached_session_id = "opencode-session-1";
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalIndexForChat(app, live_chat), 1);
	UAM_ASSERT_EQ(uam::FindCliTerminalForChat(app, live_chat), app.cli_terminals[1].get());
	UAM_ASSERT_EQ(uam::FindCliTerminalForChat(app, stale_chat), app.cli_terminals[0].get());
}

UAM_TEST(FindCliTerminalForChatPrefersLiveTerminalOverLaterStaleCollision)
{
	uam::AppState app;

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "opencode-session-1";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	app.chats.push_back(stale_chat);
	app.resolved_native_sessions_by_chat_id[live_chat.id] = "opencode-session-1";

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = live_chat.id;
	live_terminal->attached_chat_id = live_chat.id;
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->running = true;
	live_terminal->ui_attached = true;
	live_terminal->last_activity_time_s = 2.0;
	app.cli_terminals.push_back(std::move(live_terminal));

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = stale_chat.id;
	stale_terminal->attached_chat_id = stale_chat.id;
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->running = false;
	stale_terminal->ui_attached = false;
	stale_terminal->last_activity_time_s = 1.0;
	app.cli_terminals.push_back(std::move(stale_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalForChat(app, live_chat), app.cli_terminals[0].get());
	UAM_ASSERT_EQ(uam::FindCliTerminalForChat(app, stale_chat), app.cli_terminals[1].get());
}

UAM_TEST(FindChatForCliTerminalPrefersLiveChatOverLaterStaleCollision)
{
	uam::AppState app;

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "opencode-session-1";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	stale_chat.last_opened_at = "2026-05-31T12:00:00.000Z";
	stale_chat.updated_at = "2026-05-31T12:00:00.000Z";
	app.chats.push_back(stale_chat);

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = "chat-live";
	terminal.attached_chat_id = "chat-live";
	terminal.attached_session_id = "opencode-session-1";

	UAM_ASSERT_EQ(uam::FindChatForCliTerminal(app, terminal), &app.chats.front());
}

UAM_TEST(FindCliTerminalForRoutingKeyPrefersLiveTerminalOverStaleCollision)
{
	uam::AppState app;

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = "chat-stale";
	stale_terminal->attached_chat_id = "chat-stale";
	stale_terminal->attached_session_id = "opencode-session-1";
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = "chat-live";
	live_terminal->attached_chat_id = "chat-live";
	live_terminal->attached_session_id = "opencode-session-1";
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalForRoutingKey(app, "opencode-session-1", ""), app.cli_terminals[1].get());
}

UAM_TEST(FindCliTerminalForRoutingKeyPrefersLiveTerminalOverStaleTerminalIdCollision)
{
	uam::AppState app;

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->terminal_id = "term-chat-live";
	stale_terminal->frontend_chat_id = "chat-stale";
	stale_terminal->attached_chat_id = "chat-stale";
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->last_activity_time_s = 1.0;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->terminal_id = "term-chat-live";
	live_terminal->frontend_chat_id = "chat-live";
	live_terminal->attached_chat_id = "chat-live";
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->running = true;
	live_terminal->ui_attached = true;
	live_terminal->last_activity_time_s = 2.0;
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalForRoutingKey(app, "", "term-chat-live"), app.cli_terminals[1].get());
}

UAM_TEST(FindCliTerminalForRoutingKeyPrefersChatMatchingTerminalOverStaleTerminalIdCollision)
{
	uam::AppState app;

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->terminal_id = "term-chat-live";
	stale_terminal->frontend_chat_id = "chat-stale";
	stale_terminal->attached_chat_id = "chat-stale";
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->last_activity_time_s = 5.0;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->terminal_id = "term-chat-live";
	live_terminal->frontend_chat_id = "chat-live";
	live_terminal->attached_chat_id = "chat-live";
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->last_activity_time_s = 1.0;
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalForRoutingKey(app, "chat-live", "term-chat-live"), app.cli_terminals[1].get());
}

UAM_TEST(FindCliTerminalForChatStringPrefersLiveTerminalOverStaleCollision)
{
	uam::AppState app;

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = "chat-stale";
	stale_terminal->attached_chat_id = "chat-stale";
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->running = false;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = "chat-live";
	live_terminal->attached_chat_id = "chat-live";
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->running = true;
	live_terminal->ui_attached = true;
	live_terminal->lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalForChat(app, std::string_view("chat-live")), app.cli_terminals[1].get());
}

UAM_TEST(ChatRuntimeHelpersPreferBestMatchCliTerminalOverStaleCollision)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	stale_chat.updated_at = "2026-05-31T12:00:00.000Z";
	stale_chat.last_opened_at = "2026-05-31T12:00:00.000Z";
	app.chats.push_back(stale_chat);
	app.resolved_native_sessions_by_chat_id[stale_chat.id] = "opencode-session-1";

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "opencode-session-1";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = stale_chat.id;
	stale_terminal->attached_chat_id = stale_chat.id;
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->running = true;
	stale_terminal->lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	stale_terminal->turn_state = uam::CliTerminalTurnState::Busy;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = live_chat.id;
	live_terminal->attached_chat_id = live_chat.id;
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->running = true;
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, live_chat.id));
	UAM_ASSERT(uam::ChatHasBusyCliTerminal(app, stale_chat.id));
	UAM_ASSERT(!uam::ChatHasBusyCliTerminal(app, live_chat.id));
	UAM_ASSERT(uam::ChatHasRunningRuntime(app, stale_chat.id));
}

UAM_TEST(ChatRuntimeHelpersPreferBestMatchCliTerminalForNativeSessionLookup)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	stale_chat.updated_at = "2026-05-31T12:00:00.000Z";
	stale_chat.last_opened_at = "2026-05-31T12:00:00.000Z";
	app.chats.push_back(stale_chat);
	app.resolved_native_sessions_by_chat_id[stale_chat.id] = "opencode-session-1";

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "opencode-session-1";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = stale_chat.id;
	stale_terminal->attached_chat_id = stale_chat.id;
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->running = true;
	stale_terminal->lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	stale_terminal->turn_state = uam::CliTerminalTurnState::Busy;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = live_chat.id;
	live_terminal->attached_chat_id = live_chat.id;
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->running = true;
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, "opencode-session-1"));
	UAM_ASSERT(uam::ChatHasBusyCliTerminal(app, stale_chat.id));
	UAM_ASSERT(!uam::ChatHasBusyCliTerminal(app, "opencode-session-1"));
}

UAM_TEST(LoadSidebarChatsPreservesResolvedOpenCodeSessionMappings)
{
	TempDir temp("uam-opencode-sidebar-resolved-mapping");

	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode";
	chat.provider_id.clear();
	chat.native_session_id = "opencode-session-1";
	chat.title = "OpenCode";
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));

	ChatHistorySyncService().LoadSidebarChats(app);

	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().id, std::string("chat-opencode"));
	UAM_ASSERT_EQ(app.chats.front().provider_id, std::string(uam::provider_ids::kOpenCodeCli));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[app.chats.front().id], std::string("opencode-session-1"));
}

UAM_TEST(EnsureCliTerminalRepairsWhitespaceOnlyAttachedSessionId)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-terminal-repair";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	app.chats.push_back(chat);
	UAM_ASSERT_EQ(uam::CliTerminalIdForChat(chat.id), std::string("term-chat-terminal-repair"));

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "   ";
	app.cli_terminals.push_back(std::move(terminal));

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, chat);
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), chat.native_session_id);
#endif
}

UAM_TEST(EnsureCliTerminalPrefersLiveAcpSessionIdForOpenCode)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id.clear();
	app.chats.push_back(chat);

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = uam::provider_ids::kOpenCodeCli;
	session->protocol_kind = uam::provider_profile_constants::kProtocolOpenCodeAcp;
	session->running = true;
	session->session_id = "opencode-session-live";
	app.acp_sessions.push_back(std::move(session));

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, app.chats.front());
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("opencode-session-live"));
#endif
}

UAM_TEST(EnsureCliTerminalUsesResolvedNativeSessionIdForOpenCode)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-resolved";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id.clear();
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-resolved";

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, app.chats.front());
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("opencode-session-resolved"));
#endif
}

UAM_TEST(EnsureCliTerminalRepairsStaleAttachedSessionIdForOpenCode)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-stale-attached";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id.clear();
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-resolved";

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "opencode-session-stale";
	app.cli_terminals.push_back(std::move(terminal));

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, app.chats.front());
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("opencode-session-resolved"));
#endif
}

UAM_TEST(EnsureCliTerminalRepairsAttachedChatIdForOpenCode)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-attached-chat-repair";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "opencode-session-resolved";
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-resolved";

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = "stale-frontend-chat";
	terminal->terminal_id = "term-stale-frontend-chat";
	terminal->attached_chat_id = "stale-chat";
	terminal->attached_session_id = "opencode-session-stale";
	app.cli_terminals.push_back(std::move(terminal));

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, app.chats.front());
	UAM_ASSERT_EQ(ensured.frontend_chat_id, chat.id);
	UAM_ASSERT_EQ(ensured.terminal_id, uam::CliTerminalIdForChat(chat.id));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(ensured), chat.id);
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("opencode-session-resolved"));
#endif
}

UAM_TEST(EnsureCliTerminalRepairsRunningOpenCodeTerminalIdentity)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-running-repair";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "opencode-session-resolved";
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-resolved";

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = "stale-frontend-chat";
	terminal->terminal_id = "term-stale-frontend-chat";
	terminal->attached_chat_id = "stale-chat";
	terminal->attached_session_id = "opencode-session-stale";
	terminal->running = true;
	terminal->ui_attached = true;
	app.cli_terminals.push_back(std::move(terminal));

	uam::CliTerminalState& running_terminal = *app.cli_terminals.front();
	uam::RepairCliTerminalIdentityForChat(app, running_terminal, app.chats.front(), ProviderProfileStore::DefaultOpenCodeProfile());
	UAM_ASSERT(running_terminal.running);
	UAM_ASSERT_EQ(running_terminal.frontend_chat_id, chat.id);
	UAM_ASSERT_EQ(running_terminal.terminal_id, uam::CliTerminalIdForChat(chat.id));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(running_terminal), chat.id);
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(running_terminal), std::string("opencode-session-resolved"));
#endif
}

UAM_TEST(EnsureCliTerminalClearsStaleOpenCodeSessionWhenNoResumeIdExists)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-no-resume";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id.clear();
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = "stale-frontend-chat";
	terminal->terminal_id = "term-stale-frontend-chat";
	terminal->attached_chat_id = "stale-chat";
	terminal->attached_session_id = "opencode-session-stale";
	terminal->running = true;
	app.cli_terminals.push_back(std::move(terminal));

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, app.chats.front());
	UAM_ASSERT_EQ(ensured.frontend_chat_id, chat.id);
	UAM_ASSERT_EQ(ensured.terminal_id, uam::CliTerminalIdForChat(chat.id));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(ensured), chat.id);
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string(""));
#endif
}

UAM_TEST(ClearStoppedCliTerminalAttachmentForChatClearsStoppedOpenCodeDuplicate)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-clear";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "opencode-session-live";
	app.chats.push_back(chat);

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = chat.id;
	live_terminal->attached_chat_id = chat.id;
	live_terminal->attached_session_id = "opencode-session-live";
	live_terminal->running = true;
	app.cli_terminals.push_back(std::move(live_terminal));

	auto stopped_terminal = std::make_unique<uam::CliTerminalState>();
	stopped_terminal->frontend_chat_id = chat.id;
	stopped_terminal->attached_chat_id = chat.id;
	stopped_terminal->attached_session_id = "opencode-session-stale";
	stopped_terminal->running = false;
	app.cli_terminals.push_back(std::move(stopped_terminal));

	uam::ClearStoppedCliTerminalAttachmentForChat(app, chat.id);

	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(*app.cli_terminals[0]), std::string("opencode-session-live"));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(*app.cli_terminals[1]), std::string(""));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(*app.cli_terminals[1]), std::string(""));
	UAM_ASSERT_EQ(app.cli_terminals[1]->terminal_id, std::string(""));
#endif
}

UAM_TEST(TryAttachLocalHistorySessionFromChatsRebindsOpenCodeTerminal)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-local-rebind");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-local-rebind";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id.clear();
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id.clear();
	terminal->session_ids_before = {"existing-session"};
	app.cli_terminals.push_back(std::move(terminal));
	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();

	std::vector<ChatSession> local_chats;
	ChatSession existing;
	existing.id = "existing-session";
	existing.provider_id = uam::provider_ids::kOpenCodeCli;
	existing.native_session_id = "existing-session";
	local_chats.push_back(existing);

	ChatSession discovered;
	discovered.id = "discovered-session";
	discovered.provider_id = uam::provider_ids::kOpenCodeCli;
	discovered.native_session_id = "discovered-session";
	local_chats.push_back(discovered);

	uam::CliTerminalState& ensured = *app.cli_terminals.front();
	UAM_ASSERT(uam::TryAttachLocalHistorySessionFromChats(app, opencode_provider, ensured, local_chats));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("discovered-session"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("discovered-session"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[chat.id], std::string("discovered-session"));
#endif
}

UAM_TEST(TryAttachLocalHistorySessionFromChatsRepairsStaleAttachedOpenCodeTerminal)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-local-repair-stale");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-local-stale";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "stale-session";
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "stale-session";
	terminal->session_ids_before = {"existing-session"};
	app.cli_terminals.push_back(std::move(terminal));
	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();

	std::vector<ChatSession> local_chats;
	ChatSession discovered;
	discovered.id = "discovered-session";
	discovered.provider_id = uam::provider_ids::kOpenCodeCli;
	discovered.native_session_id = "discovered-session";
	local_chats.push_back(discovered);

	uam::CliTerminalState& ensured = *app.cli_terminals.front();
	UAM_ASSERT(uam::TryAttachLocalHistorySessionFromChats(app, opencode_provider, ensured, local_chats));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("discovered-session"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("discovered-session"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[chat.id], std::string("discovered-session"));
#endif
}

UAM_TEST(TryAttachLocalHistorySessionFromChatsRepairsOpenCodeWithoutSessionSnapshot)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-local-repair-nosnapshot");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-local-nosnapshot";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "stale-session";
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id.clear();
	terminal->session_ids_before.clear();
	app.cli_terminals.push_back(std::move(terminal));
	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();

	std::vector<ChatSession> local_chats;
	ChatSession discovered;
	discovered.id = chat.id;
	discovered.provider_id = uam::provider_ids::kOpenCodeCli;
	discovered.native_session_id = "discovered-session";
	local_chats.push_back(discovered);

	uam::CliTerminalState& ensured = *app.cli_terminals.front();
	UAM_ASSERT(uam::TryAttachLocalHistorySessionFromChats(app, opencode_provider, ensured, local_chats));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("discovered-session"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("discovered-session"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[chat.id], std::string("discovered-session"));
#endif
}

UAM_TEST(TryAttachLocalHistorySessionFromChatsIgnoresCrossProviderAttachedChat)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-opencode-local-cross-provider");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "cross-provider-session";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.native_session_id = "stale-session";
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "stale-session";
	terminal->session_ids_before = {"existing-session"};
	app.cli_terminals.push_back(std::move(terminal));
	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();

	std::vector<ChatSession> local_chats;
	ChatSession discovered;
	discovered.id = "discovered-session";
	discovered.provider_id = uam::provider_ids::kOpenCodeCli;
	discovered.native_session_id = "discovered-session";
	local_chats.push_back(discovered);

	uam::CliTerminalState& ensured = *app.cli_terminals.front();
	UAM_ASSERT(!uam::TryAttachLocalHistorySessionFromChats(app, opencode_provider, ensured, local_chats));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("stale-session"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("stale-session"));
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id.find(chat.id) == app.resolved_native_sessions_by_chat_id.end());
#endif
}

UAM_TEST(TryAttachLocalHistorySessionFromChatsSkipsCodexTerminal)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-local-rebind-skip-codex");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-codex-local-rebind";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.native_session_id.clear();
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id.clear();
	terminal->session_ids_before = {"existing-session"};
	app.cli_terminals.push_back(std::move(terminal));

	std::vector<ChatSession> local_chats;
	ChatSession discovered;
	discovered.id = "discovered-session";
	discovered.provider_id = uam::provider_ids::kCodexCli;
	discovered.native_session_id = "discovered-session";
	local_chats.push_back(discovered);

	uam::CliTerminalState& ensured = *app.cli_terminals.front();
	UAM_ASSERT(!uam::TryAttachLocalHistorySessionFromChats(app, ProviderProfileStore::DefaultCodexProfile(), ensured, local_chats));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string(""));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string(""));
#endif
}

UAM_TEST(TryAttachLocalHistorySessionFromChatsIgnoresDifferentWorkspaceOpenCode)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-local-rebind-workspace-filter");
	uam::AppState app;
	app.data_root = temp.root / "data";
	fs::create_directories(temp.root / "workspace-a");
	fs::create_directories(temp.root / "workspace-b");

	ChatSession chat;
	chat.id = "chat-opencode-workspace-filter";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.workspace_directory = (temp.root / "workspace-a").string();
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->session_ids_before = {"existing-session"};
	app.cli_terminals.push_back(std::move(terminal));
	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();

	std::vector<ChatSession> local_chats;
	ChatSession different_workspace;
	different_workspace.id = "different-workspace-session";
	different_workspace.provider_id = uam::provider_ids::kOpenCodeCli;
	different_workspace.workspace_directory = (temp.root / "workspace-b").string();
	different_workspace.native_session_id = "different-workspace-session";
	local_chats.push_back(different_workspace);

	uam::CliTerminalState& ensured = *app.cli_terminals.front();
	UAM_ASSERT(!uam::TryAttachLocalHistorySessionFromChats(app, opencode_provider, ensured, local_chats));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string(""));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string(""));
#endif
}

UAM_TEST(TryAttachLocalHistorySessionFromChatsAcceptsEquivalentWorkspacePathsOpenCode)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-local-rebind-workspace-normalized");
	uam::AppState app;
	app.data_root = temp.root / "data";
	fs::create_directories(temp.root / "workspace-a");

	ChatSession chat;
	chat.id = "chat-opencode-workspace-normalized";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.workspace_directory = (temp.root / "workspace-a").string();
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->session_ids_before = {"existing-session"};
	app.cli_terminals.push_back(std::move(terminal));
	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();

	std::vector<ChatSession> local_chats;
	ChatSession discovered;
	discovered.id = "discovered-session";
	discovered.provider_id = uam::provider_ids::kOpenCodeCli;
	discovered.workspace_directory = (temp.root / "workspace-a" / ".").string();
	discovered.native_session_id = "discovered-session";
	local_chats.push_back(discovered);

	uam::CliTerminalState& ensured = *app.cli_terminals.front();
	UAM_ASSERT(uam::TryAttachLocalHistorySessionFromChats(app, opencode_provider, ensured, local_chats));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("discovered-session"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("discovered-session"));
#endif
}

UAM_TEST(ShouldAttemptOpenCodeLocalHistoryRebindAllowsStaleAttachedSessionId)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-rebind-gate";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "stale-session";
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "stale-session";
	terminal->session_ids_before = {"existing-session"};
	app.cli_terminals.push_back(std::move(terminal));

	std::vector<ChatSession> local_chats;
	ChatSession discovered;
	discovered.id = "discovered-session";
	discovered.provider_id = uam::provider_ids::kOpenCodeCli;
	discovered.native_session_id = "discovered-session";
	local_chats.push_back(discovered);

	UAM_ASSERT(uam::ShouldAttemptOpenCodeLocalHistoryRebind(ProviderProfileStore::DefaultOpenCodeProfile(), *app.cli_terminals.front(), local_chats));
#endif
}

UAM_TEST(ShouldAttemptOpenCodeLocalHistoryRebindSkipsLiveAttachedSessionId)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-rebind-gate-live";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "live-session";
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "live-session";
	terminal->session_ids_before = {"existing-session"};
	app.cli_terminals.push_back(std::move(terminal));

	std::vector<ChatSession> local_chats;
	ChatSession live;
	live.id = "chat-opencode-rebind-gate-live";
	live.provider_id = uam::provider_ids::kOpenCodeCli;
	live.native_session_id = "live-session";
	local_chats.push_back(live);

	UAM_ASSERT(!uam::ShouldAttemptOpenCodeLocalHistoryRebind(ProviderProfileStore::DefaultOpenCodeProfile(), *app.cli_terminals.front(), local_chats));
#endif
}

UAM_TEST(ShouldAttemptOpenCodeLocalHistoryRebindIgnoresUnrelatedLoadedCollision)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-rebind-gate-workspace";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.workspace_directory = "/workspace-a";
	chat.native_session_id = "stale-session";
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "stale-session";
	terminal->session_ids_before = {"existing-session"};
	app.cli_terminals.push_back(std::move(terminal));

	std::vector<ChatSession> matching_chats;
	ChatSession discovered;
	discovered.id = "discovered-session";
	discovered.provider_id = uam::provider_ids::kOpenCodeCli;
	discovered.workspace_directory = "/workspace-a";
	discovered.native_session_id = "discovered-session";
	matching_chats.push_back(discovered);

	std::vector<ChatSession> unrelated_loaded_chats = matching_chats;
	ChatSession unrelated_collision = discovered;
	unrelated_collision.workspace_directory = "/workspace-b";
	unrelated_loaded_chats.push_back(unrelated_collision);

	UAM_ASSERT(uam::ShouldAttemptOpenCodeLocalHistoryRebind(ProviderProfileStore::DefaultOpenCodeProfile(), *app.cli_terminals.front(), matching_chats));
#endif
}

UAM_TEST(ShouldPollOpenCodeLocalHistoryRebindAllowsEmptySnapshotRecovery)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-rebind-poll";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "stale-session";
	app.chats.push_back(chat);

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = chat.id;
	terminal.attached_chat_id = chat.id;
	terminal.attached_session_id.clear();
	terminal.session_ids_before.clear();

	std::vector<ChatSession> matching_chats;
	ChatSession discovered;
	discovered.id = chat.id;
	discovered.provider_id = uam::provider_ids::kOpenCodeCli;
	discovered.native_session_id = "discovered-session";
	matching_chats.push_back(discovered);

	UAM_ASSERT(uam::ShouldPollOpenCodeLocalHistoryRebind(ProviderProfileStore::DefaultOpenCodeProfile(), terminal, matching_chats));
#endif
}

UAM_TEST(OpenCodeLocalHistoryPollingAcceptsBlankProviderLegacyChat)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-local-history-blank-provider-poll");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession legacy_chat;
	legacy_chat.id = "chat-legacy";
	legacy_chat.provider_id.clear();
	legacy_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(legacy_chat);

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = legacy_chat.id;
	terminal.attached_chat_id = legacy_chat.id;
	terminal.attached_session_id.clear();
	terminal.session_ids_before.clear();

	std::vector<ChatSession> local_chats;
	ChatSession discovered;
	discovered.id = legacy_chat.id;
	discovered.provider_id.clear();
	discovered.workspace_directory = source_workspace.string();
	discovered.native_session_id = "open-code-session-1";
	local_chats.push_back(discovered);

	UAM_ASSERT(uam::TryAttachLocalHistorySessionFromChats(app, ProviderProfileStore::DefaultOpenCodeProfile(), terminal, local_chats));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(terminal), std::string("open-code-session-1"));
	UAM_ASSERT_EQ(app.chats.front().provider_id, std::string(uam::provider_ids::kOpenCodeCli));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("open-code-session-1"));
#endif
}

UAM_TEST(OpenCodeLocalHistoryPollingChatFileFallbackNormalizesBlankProviderLegacyChat)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-local-history-blank-provider-chat-file");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession legacy_chat;
	legacy_chat.id = "chat-legacy";
	legacy_chat.provider_id.clear();
	legacy_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(legacy_chat);

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = legacy_chat.id;
	terminal.attached_chat_id = legacy_chat.id;
	terminal.attached_session_id.clear();
	terminal.session_ids_before = {"existing-session"};

	std::vector<ChatSession> matching_chats;
	ChatSession discovered;
	discovered.id = legacy_chat.id;
	discovered.provider_id.clear();
	discovered.workspace_directory = source_workspace.string();
	discovered.native_session_id = "open-code-session-2";
	matching_chats.push_back(discovered);

	UAM_ASSERT(uam::TryAttachOpenCodeLocalHistorySessionFromChatFile(app, ProviderProfileStore::DefaultOpenCodeProfile(), terminal, matching_chats));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(terminal), std::string("open-code-session-2"));
	UAM_ASSERT_EQ(app.chats.front().provider_id, std::string(uam::provider_ids::kOpenCodeCli));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("open-code-session-2"));
#endif
}

UAM_TEST(OpenCodeLocalHistoryPollingChatFileFallbackFailsClosedWhenPersistenceFails)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-local-history-save-fail");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	const fs::path blocked_root = temp.root / "blocked-root";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_root, "not a directory"));
	app.data_root = blocked_root;

	ChatSession legacy_chat;
	legacy_chat.id = "chat-legacy";
	legacy_chat.provider_id.clear();
	legacy_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(legacy_chat);

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = legacy_chat.id;
	terminal.attached_chat_id = legacy_chat.id;
	terminal.attached_session_id.clear();
	terminal.session_ids_before = {"existing-session"};

	std::vector<ChatSession> matching_chats;
	ChatSession discovered;
	discovered.id = legacy_chat.id;
	discovered.provider_id.clear();
	discovered.workspace_directory = source_workspace.string();
	discovered.native_session_id = "open-code-session-fail";
	matching_chats.push_back(discovered);

	UAM_ASSERT(!uam::TryAttachOpenCodeLocalHistorySessionFromChatFile(app, ProviderProfileStore::DefaultOpenCodeProfile(), terminal, matching_chats));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(terminal), std::string(""));
	UAM_ASSERT_EQ(app.chats.front().provider_id, std::string(""));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string(""));
#endif
}

UAM_TEST(ProviderInteractiveTerminalReasonMatchesSupportPredicate)
{
	ProviderProfile unknown_provider;
	unknown_provider.id = "unknown-provider";
	UAM_ASSERT(!uam::ProviderSupportsInteractiveTerminal(unknown_provider));
	UAM_ASSERT(!uam::ProviderInteractiveTerminalUnavailableReason(unknown_provider).empty());

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	UAM_ASSERT(uam::ProviderSupportsInteractiveTerminal(opencode_provider));
	UAM_ASSERT(uam::ProviderInteractiveTerminalUnavailableReason(opencode_provider).empty());

	opencode_provider.supports_interactive = false;
	UAM_ASSERT(!uam::ProviderSupportsInteractiveTerminal(opencode_provider));
	UAM_ASSERT_EQ(uam::ProviderInteractiveTerminalUnavailableReason(opencode_provider), std::string("Provider does not expose an interactive CLI runtime."));
#endif
}

UAM_TEST(ProviderInteractiveTerminalRejectsPermissionBypassSettings)
{
	uam::AppState app;
	ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	app.settings.provider_extra_flags = "--debug";
	UAM_ASSERT(uam::ProviderInteractivePermissionFlagError(app, provider).empty());

	app.settings.provider_extra_flags = "--debug --dangerously-skip-permissions";
	UAM_ASSERT(!uam::ProviderInteractivePermissionFlagError(app, provider).empty());

	app.settings.provider_extra_flags.clear();
	provider.runtime_flags = {"--ask-for-approval", "never"};
	UAM_ASSERT(!uam::ProviderInteractivePermissionFlagError(app, provider).empty());

	provider.runtime_flags = {"--auto"};
	UAM_ASSERT(!uam::ProviderInteractivePermissionFlagError(app, provider).empty());
	app.settings.provider_extra_flags = "--auto";
	provider.runtime_flags.clear();
	UAM_ASSERT(!uam::ProviderInteractivePermissionFlagError(app, provider).empty());
}
