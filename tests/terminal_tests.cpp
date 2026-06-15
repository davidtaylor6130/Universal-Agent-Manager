#include "test_harness.h"

#include "common/runtime/acp/acp_session_state_helpers.h"

using namespace uam_test;

UAM_TEST(GeminiCliCompatibilityAcceptsCurrentStableVersions)
{
	UAM_ASSERT_EQ(std::string(uam::PreferredGeminiCliVersion()), std::string("0.38.1"));
	UAM_ASSERT_EQ(uam::SupportedGeminiCliVersionsLabel(), std::string("0.36.0 or newer (preferred 0.38.1)"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.38.1"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.36.0"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.39.0"));
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

UAM_TEST(CliLifecycleTransitionsDriveBackgroundShutdownEligibility)
{
	uam::AppState app;
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.frontend_chat_id = "chat-1";
	terminal.attached_chat_id = "chat-1";
	terminal.attached_session_id = "native-1";

	uam::MarkCliTerminalTurnBusy(terminal);
	UAM_ASSERT_EQ(uam::kCliTerminalProcessingLifecycleStates.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(uam::kCliTerminalDefaultIdleShutdownTimeoutSeconds, 60.0);
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

