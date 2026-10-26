#include "test_harness.h"
#include "app/agent_run_ledger.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/provider/provider_runtime.h"

using namespace uam_test;

UAM_TEST(CodexNativeGoalReceiptsRejectChangedIntentAndForeignThreads)
{
	uam::AcpSessionState session;
	session.provider_id = "codex-cli";
	session.running = true;
	session.session_ready = true;
	session.chat_id = "owner";
	session.session_id = "thread-1";
	ChatSession chat;
	chat.id = session.chat_id;
	chat.provider_id = session.provider_id;
	chat.active_goal_id = "goal-1";
	chat.goal_command_revision = "revision-1";
	Goal goal;
	goal.id = chat.active_goal_id;
	goal.objective = "Preserve history";
	goal.execution_owner = "provider";
	goal.provider_command = "/goal";
	goal.token_budget = 8192;
	chat.goals.push_back(goal);
	const uam::AcpSessionState::CodexGoalOwner owner = uam::acp_detail::CaptureCodexNativeGoalOwner(session, chat, goal);
	UAM_ASSERT(uam::acp_detail::CodexGoalReceiptMatchesOwner(session, chat, owner, "thread-1"));
	UAM_ASSERT(!uam::acp_detail::CodexGoalReceiptMatchesOwner(session, chat, owner, "child-thread"));
	chat.goal_command_revision = "revision-2";
	UAM_ASSERT(!uam::acp_detail::CodexGoalReceiptMatchesOwner(session, chat, owner, "thread-1"));
	chat.goal_command_revision = owner.revision;
	chat.goals.front().token_budget = 4096;
	UAM_ASSERT(!uam::acp_detail::CodexGoalReceiptMatchesOwner(session, chat, owner, "thread-1"));
	chat.goals.front().token_budget = owner.token_budget;
	chat.imported_read_only = true;
	UAM_ASSERT(!uam::acp_detail::CodexGoalReceiptMatchesOwner(session, chat, owner, "thread-1"));
}

UAM_TEST(CodexNativeGoalRequestsPreserveObjectiveBudgetAndProvenance)
{
	const nlohmann::json request = uam::acp_detail::BuildCodexGoalSetRequest(17, "thread-1", "Keep all history", "active", 8192, true);
	UAM_ASSERT(request["method"] == "thread/goal/set");
	UAM_ASSERT(request["params"]["threadId"] == "thread-1");
	UAM_ASSERT(request["params"]["objective"] == "Keep all history");
	UAM_ASSERT(request["params"]["tokenBudget"] == 8192);
	UAM_ASSERT(request["params"]["origin"] == "user");
	const nlohmann::json automatic = uam::acp_detail::BuildCodexGoalSetRequest(18, "thread-1", "Keep all history", "paused", 0, false);
	UAM_ASSERT(automatic["params"]["origin"] == "automatic");
	UAM_ASSERT(automatic["params"]["tokenBudget"].is_null());
}

UAM_TEST(CodexNativeGoalQueriesDoNotStartTurns)
{
	const nlohmann::json query = uam::acp_detail::BuildCodexGoalGetRequest(19, "thread-1");
	UAM_ASSERT(query["method"] == "thread/goal/get");
	UAM_ASSERT(query["params"].size() == 1);
	const nlohmann::json clear = uam::acp_detail::BuildCodexGoalClearRequest(20, "thread-1", true);
	UAM_ASSERT(clear["method"] == "thread/goal/clear");
	UAM_ASSERT(clear["params"]["origin"] == "user");
	UAM_ASSERT(!clear["params"].contains("input"));
}

namespace
{
	void ConfigureCodexNativeGoalFixture(uam::AcpSessionState& session, ChatSession& chat, std::int64_t budget = 100)
	{
		session.provider_id = "codex-cli";
		session.chat_id = "native-goal-owner";
		session.session_id = "thread-1";
		session.running = true;
		session.session_ready = true;
		chat.id = session.chat_id;
		chat.provider_id = session.provider_id;
		chat.active_goal_id = "goal-1";
		chat.goal_command_revision = "revision-1";
		Goal goal;
		goal.id = chat.active_goal_id;
		goal.objective = "Preserve history";
		goal.execution_owner = "provider";
		goal.provider_command = "/goal";
		goal.token_budget = budget;
		goal.tokens_used = 20;
		chat.goals.push_back(goal);
		session.codex_goal_owner = uam::acp_detail::CaptureCodexNativeGoalOwner(session, chat, goal);
	}

	nlohmann::json NativeGoalFixtureSnapshot(const ChatSession& chat, const std::string& status = "active", std::int64_t used = 10)
	{
		return {{"threadId", "thread-1"}, {"objective", chat.goals.front().objective}, {"status", status},
			{"createdAt", 100}, {"updatedAt", 101}, {"tokensUsed", used}, {"timeUsedSeconds", 3},
			{"tokenBudget", chat.goals.front().token_budget > 0 ? nlohmann::json(chat.goals.front().token_budget - 20) : nlohmann::json(nullptr)}};
	}

	int WaitForGoalTimingActivation(uam::AppState& app, uam::AcpSessionState& session)
	{
		for (int attempt = 0; attempt < 200; ++attempt)
		{
			for (const std::pair<const std::string, uam::AcpSessionState::CodexGoalOwner>& pending : session.codex_goal_pending_requests)
				if (pending.second.action == uam::AcpSessionState::CodexGoalOwner::Action::Activate) return std::stoi(pending.first);
			uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(false);
		return 0;
	}


	std::string InstallSilentGoalProviderShims(const TempDir& temp)
	{
#if defined(_WIN32)
		for (const std::string name : {"gemini.cmd", "opencode.cmd", "codex.cmd"})
		{
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / name, "@echo off\r\nmore > NUL\r\n"));
		}
		constexpr char separator = ';';
#else
		for (const std::string name : {"gemini", "opencode", "codex"})
		{
			const fs::path shim = temp.root / name;
			UAM_ASSERT(uam::io::WriteTextFile(shim, "#!/bin/sh\ncat >/dev/null\n"));
			std::error_code error;
			fs::permissions(shim, fs::perms::owner_all, fs::perm_options::replace, error);
			UAM_ASSERT(!error);
		}
		constexpr char separator = ':';
#endif
		const char* existing = std::getenv("PATH");
		return temp.root.string() + (existing == nullptr ? "" : std::string(1, separator) + existing);
	}

	uam::AcpSessionState* FindTestAcpSession(uam::AppState& app, std::string_view chat_id)
	{
		const auto found = std::ranges::find_if(app.acp_sessions, [&](const auto& session)
		{
			return session != nullptr && session->chat_id == chat_id;
		});
		return found == app.acp_sessions.end() ? nullptr : found->get();
	}
}

namespace
{
	struct CodexGoalIncarnationFixtureTransport
	{
		IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
		uam::AcpSessionState& session;
		CodexGoalIncarnationFixtureTransport(uam::AcpSessionState& owned_session, const TempDir& temp) : session(owned_session)
		{
			std::string error;
#if defined(_WIN32)
			const std::vector<std::string> sink = {"cmd.exe", "/d", "/s", "/c", "more > wire.jsonl"};
#else
			const std::vector<std::string> sink = {"/bin/sh", "-c", "cat > wire.jsonl"};
#endif
			UAM_ASSERT(process.StartStdioProcess(session, temp.root, sink, &error));
		}
		~CodexGoalIncarnationFixtureTransport()
		{
			process.StopStdioProcess(session, true);
			process.CloseStdioProcessHandles(session);
		}
	};
}

UAM_TEST(CodexNativeGoalReceiptsRejectRecreatedChatIncarnation)
{
	TempDir temp("native-goal-chat-incarnation");
	uam::AppState app;
	app.data_root = temp.root;
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	session.codex_goal_owner = {};
	chat.created_at = "2026-10-13T00:00:00.000Z";
	chat.updated_at = chat.created_at;
	app.chats.push_back(chat);
	CodexGoalIncarnationFixtureTransport transport(session, temp);
	std::string error;
	const int request_id = session.next_request_id;
	UAM_ASSERT(uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), true, &error, true));
	const uam::AcpSessionState::CodexGoalOwner owner = session.codex_goal_pending_requests.at(std::to_string(request_id));
	UAM_ASSERT(uam::acp_detail::CodexGoalReceiptMatchesOwner(session, app.chats.front(), owner, session.session_id));
	ChatSession recreated = app.chats.front();
	recreated.metadata_incarnation = std::make_shared<const std::uint8_t>(0);
	UAM_ASSERT(recreated.id == app.chats.front().id);
	UAM_ASSERT(recreated.created_at == app.chats.front().created_at);
	UAM_ASSERT(recreated.goal_command_revision == app.chats.front().goal_command_revision);
	UAM_ASSERT(!uam::acp_detail::CodexGoalReceiptMatchesOwner(session, recreated, owner, session.session_id));
}

UAM_TEST(CodexNativeGoalLateClearReceiptPreservesCurrentOwner)
{
	TempDir temp("native-goal-late-clear-incarnation");
	uam::AppState app;
	app.data_root = temp.root;
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	session.codex_goal_owner = {};
	app.chats.push_back(chat);
	CodexGoalIncarnationFixtureTransport transport(session, temp);
	std::string error;
	const int probe_id = session.next_request_id;
	UAM_ASSERT(uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), true, &error, true));
	session.codex_goal_owner = session.codex_goal_pending_requests.at(std::to_string(probe_id));
	session.codex_goal_pending_requests.clear();
	session.pending_request_methods.clear();
	session.codex_goal_created_at = 100;
	session.codex_goal_owner.binding_created_at = 100;
	session.codex_goal_native_active = true;
	const int clear_id = session.next_request_id;
	UAM_ASSERT(uam::acp_detail::StopCodexNativeGoal(session, app.chats.front(), true, chat.goals.front().id, true));
	UAM_ASSERT(session.codex_goal_pending_requests.at(std::to_string(clear_id)).action == uam::AcpSessionState::CodexGoalOwner::Action::StopClear);
	ChatSession recreated = app.chats.front();
	recreated.metadata_incarnation = std::make_shared<const std::uint8_t>(0);
	recreated.goals.front().native_binding = NativeGoalBinding{1, "codex-cli", "local", session.session_id, recreated.id,
		recreated.goals.front().id, recreated.goals.front().objective, 200, 201, 20, 100, 80, 0};
	app.chats.front() = recreated;
	UAM_ASSERT(uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), true, &error, true));
	UAM_ASSERT(session.codex_goal_queued_user_action.has_value());
	// A current owner captured by the real entry point must survive the older receipt.
	session.codex_goal_owner = *session.codex_goal_queued_user_action;
	session.codex_goal_native_active = true;
	session.processing = true;
	const uam::AcpSessionState::CodexGoalOwner current = session.codex_goal_owner;
	const int next_request_id = session.next_request_id;
	UAM_ASSERT(uam::acp_detail::HandleCodexGoalResult(app, session, app.chats.front(), "thread/goal/clear", std::to_string(clear_id), {{"cleared", true}}));
	UAM_ASSERT(session.codex_goal_owner.goal_id == current.goal_id);
	UAM_ASSERT(session.codex_goal_native_active);
	UAM_ASSERT(session.processing);
	UAM_ASSERT(session.codex_goal_queued_user_action.has_value());
	UAM_ASSERT(app.chats.front().goals.front().native_binding.has_value());
	UAM_ASSERT(app.chats.front().goals.front().native_binding->created_at == 200);
	UAM_ASSERT(session.next_request_id == next_request_id);
}

UAM_TEST(CodexNativeReplacementNotificationRetiresOwnershipWithoutControls)
{
	TempDir temp("native-goal-replacement");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	uam::AcpSessionState session;
	ConfigureCodexNativeGoalFixture(session, chat);
	session.codex_goal_created_at = 100;
	session.codex_goal_updated_at = 101;
	session.codex_goal_native_active = true;
	app.chats.push_back(chat);
	nlohmann::json replacement = NativeGoalFixtureSnapshot(chat);
	replacement["createdAt"] = 102;
	replacement["updatedAt"] = 103;
	replacement["objective"] = "Other native intent";
	ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpHandleMessage(app, session, app.chats.front(),
		{{"method", "thread/goal/updated"}, {"params", {{"threadId", session.session_id}, {"goal", replacement}}}}, nullptr);
	UAM_ASSERT(session.codex_goal_owner.goal_id.empty());
	UAM_ASSERT(!session.codex_goal_native_active);
	UAM_ASSERT(session.codex_goal_pending_requests.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_diagnostic, "native_goal_binding_replaced");
}

UAM_TEST(CodexNativeBindingRoundTripsAndCorruptVersionsHold)
{
	TempDir temp("native-binding-roundtrip");
	ChatSession chat;
	uam::AcpSessionState session;
	ConfigureCodexNativeGoalFixture(session, chat);
	chat.goals.front().native_binding = NativeGoalBinding{1, "codex-cli", "local", "thread-1", chat.id, chat.goals.front().id, "Preserve history", 100, 101, 20, 100, 80, 0};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->goals.front().native_binding == chat.goals.front().native_binding);
	std::string json;
	const fs::path file = temp.root / "chats" / (chat.id + ".json");
	UAM_ASSERT(uam::io::TryReadTextFile(file, json));
	nlohmann::json corrupt = nlohmann::json::parse(json);
	corrupt["goals"][0]["nativeBinding"]["version"] = 2;
	UAM_ASSERT(uam::io::WriteTextFile(file, corrupt.dump()));
	const std::optional<ChatSession> invalid = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(invalid.has_value());
	UAM_ASSERT(invalid->goals.front().native_binding.has_value());
	UAM_ASSERT_EQ(invalid->goals.front().native_binding->version, 0);
}

UAM_TEST(CodexNativeDurableBindingMismatchCannotAuthorizeResume)
{
	TempDir temp("native-binding-mismatch");
	for (int mismatch = 0; mismatch < 11; ++mismatch)
	{
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		uam::AcpSessionState& session = *app.acp_sessions.back();
		ConfigureCodexNativeGoalFixture(session, chat);
		const uam::AcpSessionState::CodexGoalOwner previous_owner = session.codex_goal_owner;
		session.codex_goal_owner = {};
		chat.goals.front().native_binding = NativeGoalBinding{1, "codex-cli", "local", "thread-1", chat.id, chat.goals.front().id, "Preserve history", 100, 101, 20, 100, 80, 0};
		NativeGoalBinding& binding = *chat.goals.front().native_binding;
		if (mismatch == 0) binding.version = 0;
		if (mismatch == 1) binding.thread_id = "foreign-thread";
		if (mismatch == 2) binding.execution_host_id = "foreign-host";
		if (mismatch == 3) binding.objective = "Other intent";
		if (mismatch == 4) binding.uam_budget_at_creation = 200;
		if (mismatch == 5) binding.last_native_tokens_used = 50;
		if (mismatch == 6) binding.native_token_budget = 81;
		if (mismatch == 7) binding.uam_chat_id = "copied-chat";
		if (mismatch == 8) binding.uam_goal_id = "copied-goal";
		if (mismatch >= 9)
		{
			binding.version = 0;
			session.codex_goal_owner = previous_owner;
			if (mismatch == 9) session.codex_goal_owner.objective = "Prior native intent";
			session.codex_goal_created_at = 100;
			session.codex_goal_native_active = true;
		}
		const int next_request_id = session.next_request_id;
		app.chats.push_back(chat);
		std::string error;
		UAM_ASSERT(!uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), true, &error));
		UAM_ASSERT_EQ(session.next_request_id, next_request_id);
		UAM_ASSERT(session.codex_goal_owner.goal_id.empty());
		UAM_ASSERT(!session.codex_goal_native_active);
		UAM_ASSERT(session.pending_request_methods.empty());
		UAM_ASSERT(session.codex_goal_pending_requests.empty());
		UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	}
}

UAM_TEST(CodexNativeDurableReceiptRejectsExpandedUnlimitedCeiling)
{
	ChatSession chat;
	uam::AcpSessionState session;
	ConfigureCodexNativeGoalFixture(session, chat, 0);
	chat.goals.front().native_binding = NativeGoalBinding{1, "codex-cli", "local", "thread-1", chat.id, chat.goals.front().id, "Preserve history", 100, 101, 20, 0, 500, 0};
	nlohmann::json receipt = NativeGoalFixtureSnapshot(chat, "active", 10);
	receipt["tokenBudget"] = 501;
	UAM_ASSERT(!uam::acp_detail::ApplyCodexNativeGoalReceipt(session, chat, receipt));
	UAM_ASSERT_EQ(chat.goals.front().tokens_used, static_cast<std::int64_t>(20));
	receipt["tokenBudget"] = 500;
	UAM_ASSERT(uam::acp_detail::ApplyCodexNativeGoalReceipt(session, chat, receipt));
	UAM_ASSERT_EQ(chat.goals.front().native_binding->last_native_tokens_used, static_cast<std::int64_t>(10));
}

UAM_TEST(CodexNativeRestartProbeMismatchPreservesNativeState)
{
	TempDir temp("native-binding-probe");
	for (int mismatch = 0; mismatch < 4; ++mismatch)
	{
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		uam::AcpSessionState session;
		ConfigureCodexNativeGoalFixture(session, chat);
		chat.goals.front().native_binding = NativeGoalBinding{1, "codex-cli", "local", "thread-1", chat.id, chat.goals.front().id, "Preserve history", 100, 101, 20, 100, 80, 0};
		uam::AcpSessionState::CodexGoalOwner owner = session.codex_goal_owner;
		owner.action = uam::AcpSessionState::CodexGoalOwner::Action::Probe;
		owner.resume_existing = true;
		owner.binding_created_at = 100;
		session.codex_goal_owner = {};
		session.codex_goal_created_at = 100;
		session.codex_goal_updated_at = 101;
		session.codex_goal_pending_requests["42"] = owner;
		app.chats.push_back(chat);
		nlohmann::json receipt = NativeGoalFixtureSnapshot(chat, "paused", 0);
		if (mismatch == 0) receipt["createdAt"] = 99;
		if (mismatch == 1) receipt["objective"] = "Other intent";
		if (mismatch == 2) receipt["tokenBudget"] = 81;
		if (mismatch == 3) receipt["updatedAt"] = 100;
		UAM_ASSERT(uam::acp_detail::HandleCodexGoalResult(app, session, app.chats.front(), "thread/goal/get", "42", {{"goal", receipt}}));
		UAM_ASSERT(session.codex_goal_pending_requests.empty());
		UAM_ASSERT(session.pending_request_methods.empty());
		UAM_ASSERT(session.codex_goal_owner.goal_id.empty());
		UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	}
}

UAM_TEST(CodexNativeAutomaticRestartCannotUseDurableBindingAsAuthorization)
{
	TempDir temp("native-binding-automatic");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	uam::AcpSessionState session;
	ConfigureCodexNativeGoalFixture(session, chat);
	session.codex_goal_owner = {};
	chat.goals.front().native_binding = NativeGoalBinding{1, "codex-cli", "local", "thread-1", chat.id, chat.goals.front().id, "Preserve history", 100, 101, 20, 100, 80, 0};
	app.chats.push_back(chat);
	std::string error;
	UAM_ASSERT(!uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), false, &error));
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT(session.codex_goal_pending_requests.empty());
	UAM_ASSERT(app.chats.front().goals.front().native_binding.has_value());
}

UAM_TEST(CodexNativeBindingSaveFailurePreventsInitialWork)
{
	TempDir temp("native-binding-save-failure");
	uam::AppState app;
	app.data_root = temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "private fixture"));
	ChatSession chat;
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	ConfigureCodexNativeGoalFixture(session, chat);
	uam::AcpSessionState::CodexGoalOwner owner = uam::acp_detail::CaptureCodexNativeGoalOwner(app, session, chat, chat.goals.front());
	owner.action = uam::AcpSessionState::CodexGoalOwner::Action::PreparePaused;
	session.codex_goal_owner = {};
	session.codex_goal_pending_requests["42"] = owner;
	app.chats.push_back(chat);
	UAM_ASSERT(uam::acp_detail::HandleCodexGoalResult(app, session, app.chats.front(), "thread/goal/set", "42", {{"goal", NativeGoalFixtureSnapshot(chat, "paused", 0)}}));
	for (int attempt = 0; attempt < 200 && session.codex_goal_initial_turn_pending.has_value(); ++attempt)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	UAM_ASSERT(!session.codex_goal_initial_turn_pending.has_value());
	UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages.front().role, MessageRole::User);
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, app.chats.front().goals.front().objective);
	UAM_ASSERT(app.chats.front().messages.front().acp_prompt_not_sent);
	UAM_ASSERT(app.chats.front().goals.front().native_binding.has_value());
	UAM_ASSERT_EQ(app.chats.front().goals.front().native_binding->created_at, static_cast<std::int64_t>(100));
	UAM_ASSERT(session.goal_resume_suppressed);
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_diagnostic, std::string("native_goal_initial_history_save_failed"));
	std::string blocked_bytes;
	UAM_ASSERT(uam::io::TryReadTextFile(app.data_root, blocked_bytes));
	UAM_ASSERT_EQ(blocked_bytes, std::string("private fixture"));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
}

UAM_TEST(CodexNativeLegacyResumeCannotRecreateMissingNativeGoal)
{
	TempDir temp("native-binding-legacy");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	uam::AcpSessionState session;
	ConfigureCodexNativeGoalFixture(session, chat);
	uam::AcpSessionState::CodexGoalOwner owner = session.codex_goal_owner;
	owner.action = uam::AcpSessionState::CodexGoalOwner::Action::Probe;
	session.codex_goal_owner = {};
	session.codex_goal_pending_requests["42"] = owner;
	app.chats.push_back(chat);
	UAM_ASSERT(uam::acp_detail::HandleCodexGoalResult(app, session, app.chats.front(), "thread/goal/get", "42", {{"goal", nullptr}}));
	UAM_ASSERT(session.codex_goal_pending_requests.empty());
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
}

UAM_TEST(CodexNativeCompletionRemainsUnverifiedAndRetainsActualUsage)
{
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	chat.goals.front().last_verification = "Previous verified evidence";
	UAM_ASSERT(uam::acp_detail::ApplyCodexNativeGoalReceipt(session, chat, NativeGoalFixtureSnapshot(chat, "complete", 30)));
	UAM_ASSERT_EQ(chat.goals.front().status, GoalStatus::Paused);
	UAM_ASSERT_EQ(chat.goals.front().tokens_used, static_cast<std::int64_t>(50));
	UAM_ASSERT_EQ(chat.goals.front().last_diagnostic, std::string("native_goal_complete_unverified"));
	UAM_ASSERT_EQ(chat.goals.front().last_verification, std::string("Previous verified evidence"));
	UAM_ASSERT(chat.active_goal_id.empty());
	UAM_ASSERT(session.goal_resume_suppressed);
}

UAM_TEST(CodexNativeReceiptsRejectStaleCountersIncarnationsAndExpandedBudgets)
{
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	const nlohmann::json receipt = NativeGoalFixtureSnapshot(chat);
	UAM_ASSERT(uam::acp_detail::ApplyCodexNativeGoalReceipt(session, chat, receipt));
	for (const std::pair<std::string, nlohmann::json>& mutation : std::vector<std::pair<std::string, nlohmann::json>>{
		{"threadId", "foreign-thread"}, {"objective", "Foreign objective"}, {"createdAt", 99},
		{"updatedAt", 100}, {"tokensUsed", 9}, {"tokensUsed", -1}, {"tokenBudget", 81}, {"status", "invented"}})
	{
		nlohmann::json stale = receipt;
		stale[mutation.first] = mutation.second;
		UAM_ASSERT(!uam::acp_detail::ApplyCodexNativeGoalReceipt(session, chat, stale));
		UAM_ASSERT_EQ(chat.goals.front().tokens_used, static_cast<std::int64_t>(30));
		UAM_ASSERT_EQ(chat.goals.front().status, GoalStatus::Active);
	}
}

UAM_TEST(CodexNativeBudgetExhaustionHoldsButZeroRemainsUnlimited)
{
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	UAM_ASSERT(uam::acp_detail::ApplyCodexNativeGoalReceipt(session, chat, NativeGoalFixtureSnapshot(chat, "active", 80)));
	UAM_ASSERT_EQ(chat.goals.front().status, GoalStatus::Paused);
	UAM_ASSERT_EQ(chat.goals.front().tokens_used, static_cast<std::int64_t>(100));
	uam::AcpSessionState unlimited_session;
	ChatSession unlimited;
	ConfigureCodexNativeGoalFixture(unlimited_session, unlimited, 0);
	UAM_ASSERT(uam::acp_detail::ApplyCodexNativeGoalReceipt(unlimited_session, unlimited, NativeGoalFixtureSnapshot(unlimited, "active", 500)));
	UAM_ASSERT_EQ(unlimited.goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(unlimited.goals.front().tokens_used, static_cast<std::int64_t>(520));
}

UAM_TEST(CodexNativeProbePreservesUnboundGoalEvenWithMatchingText)
{
	TempDir temp("uam-native-goal-foreign-probe");
	uam::AppState app;
	app.data_root = temp.root;
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	session.codex_goal_pending_requests["42"] = session.codex_goal_owner;
	session.pending_request_methods[42] = "thread/goal/get";
	session.codex_goal_owner = {};
	app.chats.push_back(chat);
	UAM_ASSERT(uam::acp_detail::HandleCodexGoalResult(app, session, app.chats.front(), "thread/goal/get", "42", {{"goal", NativeGoalFixtureSnapshot(chat)}}));
	UAM_ASSERT(session.codex_goal_pending_requests.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_diagnostic, std::string("native_goal_binding_unavailable_requires_explicit_replace"));
	UAM_ASSERT(session.queued_prompt.empty());
	UAM_ASSERT_EQ(session.pending_request_methods.size(), static_cast<std::size_t>(1));
}

UAM_TEST(CodexNativeClearReceiptCannotStartChangedGoalRevision)
{
	TempDir temp("uam-native-goal-stale-clear");
	uam::AppState app;
	app.data_root = temp.root;
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	session.codex_goal_pending_requests["42"] = session.codex_goal_owner;
	session.codex_goal_pending_requests["42"].action = uam::AcpSessionState::CodexGoalOwner::Action::ReplaceClear;
	session.codex_goal_owner = {};
	chat.goal_command_revision = "replacement-revision";
	app.chats.push_back(chat);
	UAM_ASSERT(uam::acp_detail::HandleCodexGoalResult(app, session, app.chats.front(), "thread/goal/clear", "42", {{"cleared", true}}));
	UAM_ASSERT(session.codex_goal_pending_requests.empty());
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT(session.codex_goal_owner.goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<std::int64_t>(20));
}

UAM_TEST(CodexNativeProtocolControlsUseRemainingBudgetWithoutDuplicateLoops)
{
	TempDir temp("uam-native-goal-protocol-controls");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	session.codex_goal_owner = {};
	chat.model_id = "goal-fixture-model";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);
	IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> sink = {"cmd.exe", "/d", "/s", "/c", "more > wire.jsonl"};
#else
	const std::vector<std::string> sink = {"/bin/sh", "-c", "cat > wire.jsonl"};
#endif
	UAM_ASSERT(process.StartStdioProcess(session, temp.root, sink, &error));
	struct ProcessGuard
	{
		IPlatformProcessService& process;
		uam::AcpSessionState& session;
		~ProcessGuard() { process.StopStdioProcess(session, true); process.CloseStdioProcessHandles(session); }
	} guard{process, session};
	const int query_id = session.next_request_id;
	UAM_ASSERT(uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), true, &error, true));
	UAM_ASSERT_EQ(session.pending_request_methods.at(query_id), std::string("thread/goal/get"));
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", query_id}, {"result", {{"goal", nullptr}}}});
	UAM_ASSERT_EQ(session.pending_request_methods.at(query_id + 1), std::string("thread/goal/clear"));
	// A repeated get reply must not produce a second clear or set.
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", query_id}, {"result", {{"goal", nullptr}}}});
	UAM_ASSERT_EQ(session.codex_goal_pending_requests.size(), static_cast<std::size_t>(1));
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", query_id + 1}, {"result", {{"cleared", false}}}});
	UAM_ASSERT_EQ(session.pending_request_methods.at(query_id + 2), std::string("thread/goal/set"));
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", query_id + 2}, {"result", {{"goal", NativeGoalFixtureSnapshot(chat, "paused", 0)}}}});
	for (int attempt = 0; attempt < 200 && session.codex_goal_initial_turn_request_id == 0; ++attempt)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	const int first_turn_request = session.codex_goal_initial_turn_request_id;
	UAM_ASSERT_EQ(session.pending_request_methods.at(first_turn_request), std::string("turn/start"));
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", first_turn_request}, {"result", {{"turn", {{"id", "native-turn-1"}}}}}});
	const int activate_request = WaitForGoalTimingActivation(app, session);
	UAM_ASSERT_EQ(session.pending_request_methods.at(activate_request), std::string("thread/goal/set"));
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", activate_request}, {"result", {{"goal", NativeGoalFixtureSnapshot(chat, "active", 0)}}}});
	UAM_ASSERT_EQ(session.codex_goal_capability, uam::AcpSessionState::CodexGoalCapability::StateApi);
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<std::int64_t>(20));
	UAM_ASSERT(session.codex_goal_pending_requests.empty());
	UAM_ASSERT(session.processing);
	ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpHandleMessage(app, session, app.chats.front(), {{"method", "turn/started"}, {"params", {{"threadId", session.session_id}, {"turn", {{"id", "native-turn-1"}}}}}}, nullptr);
	ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpHandleMessage(app, session, app.chats.front(), {{"method", "turn/completed"}, {"params", {{"threadId", session.session_id}, {"turn", {{"id", "native-turn-1"}, {"status", "completed"}}}}}}, nullptr);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT(session.processing);
	UAM_ASSERT(session.queued_prompt.empty());
	UAM_ASSERT(!session.goal_review_scheduled);
	const int next_request = session.next_request_id;
	UAM_ASSERT(uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), false, &error));
	UAM_ASSERT_EQ(session.next_request_id, next_request);
	// Pausing and resuming the same bound native goal preserves its original accounting.
	UAM_ASSERT(uam::acp_detail::ApplyCodexNativeGoalReceipt(session, app.chats.front(), NativeGoalFixtureSnapshot(chat, "active", 10)));
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, chat.id, chat.active_goal_id, GoalStatus::Paused, true));
	int pause_id = -1;
	for (const std::pair<const std::string, uam::AcpSessionState::CodexGoalOwner>& pending : session.codex_goal_pending_requests)
	{
		if (pending.second.action != uam::AcpSessionState::CodexGoalOwner::Action::StopPause) continue;
		UAM_ASSERT(pause_id < 0);
		pause_id = std::stoi(pending.first);
	}
	UAM_ASSERT(pause_id >= 0);
	UAM_ASSERT_EQ(session.pending_request_methods.at(pause_id), std::string("thread/goal/set"));
	const int requests_after_pause = session.next_request_id;
	UAM_ASSERT(uam::acp_detail::StopCodexNativeGoal(session, app.chats.front(), true, chat.active_goal_id, false));
	UAM_ASSERT_EQ(session.next_request_id, requests_after_pause);
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", pause_id}, {"result", {{"goal", NativeGoalFixtureSnapshot(chat, "paused", 30)}}}});
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<std::int64_t>(50));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, chat.active_goal_id));
	// Simulate a new provider process losing volatile receipt ownership after durable pause.
	const std::optional<ChatSession> paused_saved = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(paused_saved.has_value() && paused_saved->goals.front().native_binding.has_value());
	session.codex_goal_owner = {};
	session.codex_goal_created_at = 0;
	session.codex_goal_updated_at = 0;
	const int resume_id = session.next_request_id;
	UAM_ASSERT(uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), true, &error));
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", resume_id}, {"result", {{"goal", NativeGoalFixtureSnapshot(chat, "paused", 30)}}}});
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", resume_id + 1}, {"result", {{"goal", NativeGoalFixtureSnapshot(chat, "paused", 30)}}}});
	for (int attempt = 0; attempt < 200 && session.codex_goal_initial_turn_request_id == 0; ++attempt)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	const int resume_turn_request = session.codex_goal_initial_turn_request_id;
	UAM_ASSERT_EQ(session.pending_request_methods.at(resume_turn_request), std::string("turn/start"));
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", resume_turn_request}, {"result", {{"turn", {{"id", "native-resume-2"}}}}}});
	const int resume_activation = WaitForGoalTimingActivation(app, session);
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", resume_activation}, {"result", {{"goal", NativeGoalFixtureSnapshot(chat, "active", 30)}}}});
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<std::int64_t>(50));
	UAM_ASSERT_EQ(session.codex_goal_created_at, static_cast<std::int64_t>(100));
	std::string wire;
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		if (uam::io::TryReadTextFile(temp.root / "wire.jsonl", wire) && std::count(wire.begin(), wire.end(), '\n') >= 10) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	std::istringstream lines(wire);
	std::vector<nlohmann::json> requests;
	for (std::string line; std::getline(lines, line);) requests.push_back(nlohmann::json::parse(line));
	UAM_ASSERT_EQ(requests.size(), static_cast<std::size_t>(10));
	UAM_ASSERT_EQ(requests[2]["params"]["tokenBudget"].get<std::int64_t>(), static_cast<std::int64_t>(80));
	UAM_ASSERT(requests[2]["params"]["origin"] == "user");
	UAM_ASSERT(requests[2]["params"]["status"] == "paused");
	UAM_ASSERT(requests[3]["method"] == "turn/start");
	UAM_ASSERT(requests[3]["params"]["input"][0]["text"] == chat.goals.front().objective);
	UAM_ASSERT(requests[3]["params"]["approvalPolicy"] == "on-request");
	UAM_ASSERT(requests[3]["params"]["sandboxPolicy"]["networkAccess"] == false);
	UAM_ASSERT(requests[3]["params"]["model"] == "goal-fixture-model");
	UAM_ASSERT(requests[4]["params"]["status"] == "active");
	UAM_ASSERT(!requests[4]["params"].contains("objective"));
	UAM_ASSERT(!requests[4]["params"].contains("tokenBudget"));
	UAM_ASSERT(requests[5]["params"]["status"] == "paused");
	UAM_ASSERT(!requests[5]["params"].contains("tokenBudget"));
	UAM_ASSERT(requests[6]["method"] == "thread/goal/get");
	UAM_ASSERT(!requests[7]["params"].contains("objective"));
	UAM_ASSERT(requests[8]["params"]["input"].empty());
	UAM_ASSERT(!requests[9]["params"].contains("tokenBudget"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	for (const nlohmann::json& request : requests)
	{
		if (request["method"] != "turn/start") continue;
		for (const nlohmann::json& input : request["params"]["input"])
		{
			if (!input.contains("text") || !input["text"].is_string()) continue;
			const std::string text = input["text"].get<std::string>();
			UAM_ASSERT(text != "/goal" && !text.starts_with("/goal ") && !text.starts_with("/goal\n"));
		}
	}
}

UAM_TEST(CodexNativeMalformedClearNeverSetsOrExpandsBudget)
{
	TempDir temp("uam-native-goal-bad-clear");
	uam::AppState app;
	app.data_root = temp.root;
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	session.codex_goal_pending_requests["42"] = session.codex_goal_owner;
	session.codex_goal_pending_requests["42"].action = uam::AcpSessionState::CodexGoalOwner::Action::ReplaceClear;
	session.codex_goal_owner = {};
	app.chats.push_back(chat);
	UAM_ASSERT(uam::acp_detail::HandleCodexGoalResult(app, session, app.chats.front(), "thread/goal/clear", "42", {{"cleared", "yes"}}));
	UAM_ASSERT(session.codex_goal_pending_requests.empty());
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<std::int64_t>(20));
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_diagnostic, std::string("native_goal_clear_unconfirmed"));
}

UAM_TEST(CodexNativeUnsupportedProbeUsesDurableUamOwnership)
{
	TempDir temp("uam-native-goal-unsupported");
	uam::AppState app;
	app.data_root = temp.root;
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	session.processing = true;
	session.codex_goal_pending_requests["42"] = session.codex_goal_owner;
	session.codex_goal_owner = {};
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	app.acp_sessions.front()->chat_id = chat.id;
	app.acp_sessions.front()->processing = true;
	uam::acp_detail::AcpFailureDetails failure;
	failure.method = "thread/goal/get";
	failure.request_id = "42";
	failure.has_code = true;
	failure.code = -32601;
	UAM_ASSERT(uam::acp_detail::HandleCodexGoalError(app, session, app.chats.front(), failure));
	UAM_ASSERT_EQ(session.codex_goal_capability, uam::AcpSessionState::CodexGoalCapability::Unsupported);
	UAM_ASSERT_EQ(app.chats.front().goals.front().execution_owner, std::string("uam"));
	UAM_ASSERT(app.chats.front().goals.front().provider_command.empty());
	UAM_ASSERT(session.codex_goal_pending_requests.empty());
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->goals.front().execution_owner, std::string("uam"));
}

UAM_TEST(CodexNativeQueuedUserControlRejectsChangedRevision)
{
	TempDir temp("uam-native-goal-queued-revision");
	uam::AppState app;
	app.data_root = temp.root;
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	session.codex_goal_queued_user_action = session.codex_goal_owner;
	session.codex_goal_owner = {};
	chat.goal_command_revision = "new-user-intent";
	app.chats.push_back(chat);
	uam::acp_detail::DrainQueuedCodexNativeGoal(app, session, app.chats.front());
	UAM_ASSERT(!session.codex_goal_queued_user_action.has_value());
	UAM_ASSERT(session.codex_goal_pending_requests.empty());
	UAM_ASSERT(session.pending_request_methods.empty());
}

UAM_TEST(GoalServiceBuildContinuationPromptIncludesObjectiveAndBudget)
{
	Goal goal;
	goal.id = "goal_test_1";
	goal.objective = "Refactor the caching layer to reduce cold-start latency by 50%.";
	goal.status = GoalStatus::Active;
	goal.token_budget = 8000;
	goal.tokens_used = 1500;

	const std::string prompt = uam::GoalService::BuildContinuationPrompt(goal, goal.tokens_used, goal.token_budget);

	UAM_ASSERT(!prompt.empty());
	UAM_ASSERT(prompt.find(goal.objective) != std::string::npos);
	UAM_ASSERT(prompt.find("Continue working toward the active thread goal.") != std::string::npos);
	UAM_ASSERT(prompt.find("used=1500") != std::string::npos);
	UAM_ASSERT(prompt.find("budget=8000") != std::string::npos);
	UAM_ASSERT(prompt.find("remaining=6500") != std::string::npos);
}

UAM_TEST(GoalServiceBuildContinuationPromptReturnsEmptyForBlankObjective)
{
	Goal goal;
	goal.id = "goal_test_2";
	goal.objective = " \t ";
	goal.status = GoalStatus::Active;

	const std::string prompt = uam::GoalService::BuildContinuationPrompt(goal, 0, 0);
	UAM_ASSERT(prompt.empty());
}

UAM_TEST(GoalServiceRejectsBlankObjectivesAndUpdatesOnlyEditableUamGoals)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-edit-goal";
	app.chats.push_back(chat);

	std::string goal_id;
	UAM_ASSERT(!uam::GoalService::CreateGoal(app, chat.id, " \t ", 0, &goal_id));
	UAM_ASSERT(app.chats.front().goals.empty());
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Original objective", 100, &goal_id));
	Goal& goal = app.chats.front().goals.front();
	goal.completed_items = {"kept"};
	goal.remaining_items = {"next"};
	goal.current_step = "next";
	goal.loop_count = 3;
	const std::string created_at = goal.created_at;

	std::string error;
	UAM_ASSERT(uam::GoalService::UpdateGoalObjective(app, chat.id, goal_id, "  Revised objective  ", &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(goal.objective, std::string("Revised objective"));
	UAM_ASSERT_EQ(goal.completed_items, (std::vector<std::string>{"kept"}));
	UAM_ASSERT_EQ(goal.remaining_items, (std::vector<std::string>{"next"}));
	UAM_ASSERT_EQ(goal.current_step, std::string("next"));
	UAM_ASSERT_EQ(goal.loop_count, 3);
	UAM_ASSERT_EQ(goal.created_at, created_at);
	UAM_ASSERT(uam::GoalService::BuildContinuationPrompt(goal, 0, 100).find("Revised objective") != std::string::npos);

	UAM_ASSERT(!uam::GoalService::UpdateGoalObjective(app, chat.id, goal_id, "   ", &error));
	UAM_ASSERT_EQ(error, std::string("Goal objective is required."));
	goal.execution_owner = "provider";
	goal.provider_command = "/goal";
	UAM_ASSERT(!uam::GoalService::UpdateGoalObjective(app, chat.id, goal_id, "Provider edit", &error));
	UAM_ASSERT_EQ(error, std::string("Only non-complete UAM-managed goals can be edited."));
	goal.execution_owner = "uam";
	goal.status = GoalStatus::Complete;
	UAM_ASSERT(!uam::GoalService::UpdateGoalObjective(app, chat.id, goal_id, "Completed edit", &error));
	UAM_ASSERT_EQ(error, std::string("Only non-complete UAM-managed goals can be edited."));
}

UAM_TEST(GoalServiceUsesTheNormalChatModelWhenArchitectModeIsOff)
{
	ChatSession chat;
	chat.model_id = "normal-model";
	Goal goal;
	goal.worker_model_id = "fast-worker";
	goal.reviewer_model_id = "smart-reviewer";

	UAM_ASSERT_EQ(uam::GoalService::WorkerModelId(chat, goal), std::string("normal-model"));
	UAM_ASSERT_EQ(uam::GoalService::ReviewerModelId(chat, goal), std::string("normal-model"));
	chat.small_model_mode = true;
	UAM_ASSERT_EQ(uam::GoalService::WorkerModelId(chat, goal), std::string("fast-worker"));
	UAM_ASSERT_EQ(uam::GoalService::ReviewerModelId(chat, goal), std::string("smart-reviewer"));
}

UAM_TEST(GoalServiceMaintainsOnlyOneActiveGoalPerChat)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-single-active-goal";
	chat.model_id = "worker-v1";
	chat.reviewer_model_id = "reviewer-chat";
	app.settings.provider_chat_defaults[uam::provider_ids::kCodexCli].reviewer_model_id = "reviewer-v1";
	app.chats.push_back(chat);

	std::string first_goal_id;
	std::string second_goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "First goal.", 0, &first_goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, first_goal_id));
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Second goal.", 0, &second_goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, second_goal_id));

	UAM_ASSERT(first_goal_id.starts_with("goal-"));
	UAM_ASSERT(second_goal_id.starts_with("goal-"));
	UAM_ASSERT(first_goal_id != second_goal_id);
	UAM_ASSERT_EQ(std::ranges::count(app.chats.front().goals, GoalStatus::Active, &Goal::status), static_cast<std::ptrdiff_t>(1));
	UAM_ASSERT_EQ(app.chats.front().goals.front().worker_model_id, std::string("worker-v1"));
	UAM_ASSERT_EQ(app.chats.front().goals.front().reviewer_model_id, std::string("reviewer-chat"));
}

UAM_TEST(GoalMutationsRequireTheOwningChatEvenForDuplicateLegacyIds)
{
	uam::AppState app;
	ChatSession first;
	first.id = "chat-first";
	first.goals.push_back(Goal{.id = "legacy-duplicate", .objective = "First"});
	ChatSession second;
	second.id = "chat-second";
	second.goals.push_back(Goal{.id = "legacy-duplicate", .objective = "Second"});
	app.chats = {first, second};

	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(
	    app, second.id, "legacy-duplicate", GoalStatus::Paused));
	UAM_ASSERT_EQ(app.chats[0].goals[0].status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.chats[1].goals[0].status, GoalStatus::Paused);
	UAM_ASSERT(!uam::GoalService::UpdateGoalStatus(
	    app, "chat-missing", "legacy-duplicate", GoalStatus::Complete));
	UAM_ASSERT(!uam::GoalService::RemoveGoal(app, first.id, "goal-from-another-chat"));
}

UAM_TEST(GoalServiceSmallModelPromptPlansThenExecutesOneDurableStep)
{
	Goal goal;
	goal.objective = "Implement and verify the parser fix.";

	const std::string planning_prompt = uam::GoalService::BuildContinuationPrompt(goal, 0, 0, true);
	UAM_ASSERT(planning_prompt.find("This is the planning turn") != std::string::npos);
	UAM_ASSERT(planning_prompt.find("do not edit files") != std::string::npos);
	UAM_ASSERT(planning_prompt.find("one ordered atomic, verifiable step for every independently verifiable requirement") != std::string::npos);
	UAM_ASSERT(planning_prompt.find("Do not merge or omit requirements to hit an arbitrary step count") != std::string::npos);

	goal.loop_count = 1;
	goal.completed_items = {"Located the shared parser"};
	goal.remaining_items = {"Add the parser guard", "Run the parser test"};
	goal.current_step = "Add the parser guard";
	goal.last_verification = "Parser test reproduces the failure";
	const std::string worker_prompt = uam::GoalService::BuildContinuationPrompt(
	    goal, 120, 1000, true, "Add one guard in parser.cpp and run the focused parser test.");
	UAM_ASSERT(worker_prompt.find("Execute exactly one atomic step") != std::string::npos);
	UAM_ASSERT(worker_prompt.find("Add one guard in parser.cpp") != std::string::npos);
	UAM_ASSERT(worker_prompt.find("Located the shared parser") != std::string::npos);
	UAM_ASSERT(worker_prompt.find("Parser test reproduces the failure") != std::string::npos);

	goal.loop_count = 0;
	const std::string review_prompt = uam::GoalService::BuildReviewPrompt(goal, goal.objective, "1. Inspect parser\n2. Add guard", 0, true);
	UAM_ASSERT(review_prompt.find("mandatory planning turn") != std::string::npos);
	UAM_ASSERT(review_prompt.find("ONLY output must be a single JSON object") != std::string::npos);
	UAM_ASSERT(review_prompt.find("never add, remove, rename, split, merge, duplicate, or reorder steps") != std::string::npos);
}

UAM_TEST(GoalProgressUsesOneStablePlanAndAcceptsExplicitEmptyRemainingWork)
{
	Goal goal;
	goal.completed_items = {"Step 1"};
	goal.remaining_items = {"Step 2", "Step 3"};
	goal.current_step = "Step 2";

	const auto rewritten = uam::GoalService::ParseReviewDecision(R"({
		"decision":"continue",
		"reason":"Rewrote the plan.",
		"nextPrompt":"Continue.",
		"progressUpdate":{
			"completed":["Renamed step"],
			"remaining":["Different total"],
			"currentStep":"Different total",
			"lastVerification":"Checked the diff"
		}
	})");
	UAM_ASSERT(rewritten.has_value());
	uam::acp_detail::ApplyGoalProgressUpdate(goal, *rewritten);
	UAM_ASSERT_EQ(goal.completed_items, (std::vector<std::string>{"Step 1"}));
	UAM_ASSERT_EQ(goal.remaining_items, (std::vector<std::string>{"Step 2", "Step 3"}));
	uam::GoalService::ReviewDecision reordered = *rewritten;
	reordered.completed_items = {"Step 1"};
	reordered.remaining_items = {"Step 3", "Step 2"};
	uam::acp_detail::ApplyGoalProgressUpdate(goal, reordered);
	UAM_ASSERT_EQ(goal.remaining_items, (std::vector<std::string>{"Step 2", "Step 3"}));

	const auto finished = uam::GoalService::ParseReviewDecision(R"({
		"decision":"continue",
		"reason":"All planned work is verified.",
		"nextPrompt":"Report the result.",
		"progressUpdate":{
			"completed":["Step 1","Step 2","Step 3"],
			"remaining":[],
			"currentStep":"",
			"lastVerification":"All focused tests pass"
		}
	})");
	UAM_ASSERT(finished.has_value());
	uam::acp_detail::ApplyGoalProgressUpdate(goal, *finished);
	UAM_ASSERT_EQ(goal.completed_items, (std::vector<std::string>{"Step 1", "Step 2", "Step 3"}));
	UAM_ASSERT(goal.remaining_items.empty());
	UAM_ASSERT(goal.current_step.empty());
}

UAM_TEST(GoalServiceStatusBlockerAndTokenUpdatesMarkParentAndClearActive)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-goal-service";
	app.chats.push_back(chat);

	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Finish the durable goal mode work.", 10, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	app.chats_with_unseen_updates.clear();

	uam::GoalService::RecordTurnCompletion(app, chat.id, goal_id, 4);
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<int64_t>(4));
	UAM_ASSERT(app.chats_with_unseen_updates.contains(chat.id));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);

	uam::GoalService::RecordBlocker(app, chat.id, goal_id, "Need user credentials.");
	uam::GoalService::RecordBlocker(app, chat.id, goal_id, "Need user credentials.");
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);
	uam::GoalService::RecordTurnCompletion(app, chat.id, goal_id, 1);
	uam::GoalService::RecordBlocker(app, chat.id, goal_id, "Need user credentials.");
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());

	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	uam::GoalService::RecordTurnCompletion(app, chat.id, goal_id, 10);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_blocker, std::string("Token budget exceeded."));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
}

UAM_TEST(GoalServicePersistsBlockerKind)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-goal-blocker-kind";
	app.chats.push_back(chat);
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Reconnect safely.", 0, &goal_id));

	uam::GoalService::RecordBlocker(app, chat.id, goal_id,
	                                "The remote turn no longer exists.",
	                                "remote_connection");
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_blocker_kind,
	              std::string("remote_connection"));
}

UAM_TEST(GoalServiceUpdateStatusAndClearActiveGoalWork)
{
	TempDir temp("uam-clear-active-goal");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-clear-active";
	app.chats.push_back(chat);

	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Clear active goal correctly.", 0, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT(uam::GoalService::ClearActiveGoal(app, chat.id));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	UAM_ASSERT(app.chats_with_unseen_updates.contains(chat.id));
	UAM_ASSERT(!uam::GoalService::UpdateGoalStatus(
	    app, chat.id, goal_id, GoalStatus::Active));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	const std::optional<ChatSession> reloaded =
	    ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(reloaded.has_value());
	UAM_ASSERT(reloaded->active_goal_id.empty());
	UAM_ASSERT_EQ(reloaded->goals.front().status, GoalStatus::Paused);

	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	Goal& goal = app.chats.front().goals.front();
	goal.completed_items = {"Verified work"};
	goal.remaining_items = {"Stale work"};
	goal.current_step = "Stale work";
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, chat.id, goal_id, GoalStatus::Complete));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(goal.status, GoalStatus::Complete);
	UAM_ASSERT_EQ(goal.completed_items, (std::vector<std::string>{"Verified work"}));
	UAM_ASSERT(goal.remaining_items.empty());
	UAM_ASSERT(goal.current_step.empty());
}

UAM_TEST(GoalServiceAllowsCompletedGoalRemoval)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-completed-goal-history";
	app.chats.push_back(chat);

	std::string completed_goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Keep the completed goal.", 0, &completed_goal_id));
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, chat.id, completed_goal_id, GoalStatus::Complete));
	UAM_ASSERT(uam::GoalService::RemoveGoal(app, chat.id, completed_goal_id));
	UAM_ASSERT(uam::GoalService::FindGoalById(app, chat.id, completed_goal_id) == nullptr);
}

UAM_TEST(GoalServiceSetActiveGoalReactivatesBlockedGoal)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kOpenCodeCli);
	chat.id = "chat-resume-goal";
	app.chats.push_back(chat);

	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Resume a blocked OpenCode goal.", 0, &goal_id));
	uam::GoalService::RecordBlocker(app, chat.id, goal_id, "Same blocker.");
	uam::GoalService::RecordBlocker(app, chat.id, goal_id, "Same blocker.");
	uam::GoalService::RecordBlocker(app, chat.id, goal_id, "Same blocker.");
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());

	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.chats.front().goals.front().blocked_turn_count, 0);
	UAM_ASSERT(app.chats.front().goals.front().last_blocker.empty());
}

UAM_TEST(GoalServiceSetActiveGoalNeverReactivatesCompletedGoal)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-complete-goal";
	app.chats.push_back(chat);

	std::string completed_goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Completed work.", 0, &completed_goal_id));
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, chat.id, completed_goal_id, GoalStatus::Complete));
	app.chats.front().goals.front().last_verification = "Focused tests passed.";

	std::string active_goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Current work.", 0, &active_goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, active_goal_id));
	std::string error;
	UAM_ASSERT(!uam::GoalService::SetActiveGoal(app, chat.id, completed_goal_id, &error));
	UAM_ASSERT(error.find("cannot be reactivated") != std::string::npos);
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, active_goal_id);
	const Goal* completed_goal = uam::GoalService::FindGoalById(app, chat.id, completed_goal_id);
	UAM_ASSERT(completed_goal != nullptr);
	UAM_ASSERT_EQ(completed_goal->status, GoalStatus::Complete);
	UAM_ASSERT_EQ(completed_goal->last_verification, std::string("Focused tests passed."));
}

UAM_TEST(GoalServiceFindsLatestTerminalGoalAndBuildsReadOnlyState)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-terminal-goal";
	Goal complete;
	complete.id = "goal-complete";
	complete.status = GoalStatus::Complete;
	chat.goals.push_back(complete);
	Goal blocked;
	blocked.id = "goal-blocked";
	blocked.objective = "Wait for approval.";
	blocked.status = GoalStatus::Blocked;
	blocked.last_blocker = "User approval required.";
	blocked.last_diagnostic = "goal_blocked_permission";
	blocked.last_verification = "Remote process is still running.";
	blocked.current_step = "Approve access.";
	blocked.completed_items = {"Connected over SSH"};
	blocked.remaining_items = {"Approve access"};
	chat.goals.push_back(blocked);
	app.chats.push_back(std::move(chat));

	const Goal* latest = uam::GoalService::FindActiveOrLatestTerminalGoal(app, "chat-terminal-goal");
	UAM_ASSERT(latest != nullptr);
	UAM_ASSERT_EQ(latest->id, std::string("goal-blocked"));
	const std::string prompt = uam::GoalService::BuildReadOnlyTerminalPrompt(*latest);
	UAM_ASSERT(prompt.find("read-only") != std::string::npos);
	UAM_ASSERT(prompt.find("Do not reactivate") != std::string::npos);
	UAM_ASSERT(prompt.find("goal_blocked_permission") != std::string::npos);
	UAM_ASSERT(prompt.find("Remote process is still running.") != std::string::npos);
}

UAM_TEST(GoalServicePauseClearsActiveGoalAndResumeReactivates)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-pause-goal";
	app.chats.push_back(chat);

	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Pause and resume goal mode.", 0, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, chat.id, goal_id, GoalStatus::Paused));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	UAM_ASSERT(uam::GoalService::FindActiveGoal(app, chat.id) == nullptr);

	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
}

UAM_TEST(GoalServiceRestartPausesActiveGoalsWithoutDiscardingProgress)
{
	TempDir temp("uam-restart-pauses-goals");
	ChatSession chat;
	chat.id = "chat-restarted-goal";
	chat.active_goal_id = "goal-restarted";
	Goal goal;
	goal.id = chat.active_goal_id;
	goal.status = GoalStatus::Active;
	goal.objective = "Preserve this work.";
	goal.completed_items = {"kept"};
	goal.last_next_prompt = "Continue safely.";
	chat.goals.push_back(goal);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	uam::AppState app;
	app.data_root = temp.root;
	app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);

	UAM_ASSERT_EQ(uam::GoalService::PauseActiveGoalsAfterRestart(app), static_cast<std::size_t>(1));
	UAM_ASSERT(ChatRepository::HydrateChatMessages(temp.root, app.chats.front()));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	UAM_ASSERT_EQ(app.chats.front().goals.front().completed_items.front(), std::string("kept"));
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_next_prompt, std::string("Continue safely."));
	const std::vector<ChatSession> reloaded = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT_EQ(reloaded.front().active_goal_id, std::string{});
	UAM_ASSERT_EQ(reloaded.front().goals.front().status, GoalStatus::Paused);
}

UAM_TEST(GoalServiceRestartDoesNotPublishAnUnpersistedPause)
{
	TempDir temp("uam-restart-pause-save-failure");
	const fs::path blocked_root = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_root, "blocked"));
	uam::AppState app;
	app.data_root = blocked_root;
	ChatSession chat;
	chat.id = "chat-unpersisted-pause";
	chat.active_goal_id = "goal-unpersisted-pause";
	Goal goal;
	goal.id = chat.active_goal_id;
	goal.status = GoalStatus::Active;
	chat.goals.push_back(goal);
	app.chats.push_back(chat);

	std::size_t failed = 0;
	UAM_ASSERT_EQ(uam::GoalService::PauseActiveGoalsAfterRestart(app, &failed), static_cast<std::size_t>(0));
	UAM_ASSERT_EQ(failed, static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal.id);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.status_line, std::string("Failed to persist paused goal after restart."));
}

UAM_TEST(GoalServiceRestartClearsStaleActiveGoalIds)
{
	TempDir temp("uam-restart-clears-stale-active-goal");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-stale-active-goal";
	chat.active_goal_id = "goal-complete";
	Goal goal;
	goal.id = chat.active_goal_id;
	goal.status = GoalStatus::Complete;
	chat.goals.push_back(goal);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	app.chats.push_back(chat);

	UAM_ASSERT_EQ(uam::GoalService::PauseActiveGoalsAfterRestart(app), static_cast<std::size_t>(0));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT(saved->active_goal_id.empty());
}

UAM_TEST(GoalServiceRestartKeepsOwnerActiveWhileRemoteIterationReconnects)
{
	uam::AppState app;
	ChatSession owner;
	owner.id = "remote-goal-owner";
	owner.active_goal_id = "goal-remote";
	Goal goal;
	goal.id = owner.active_goal_id;
	goal.status = GoalStatus::Active;
	owner.goals.push_back(goal);
	ChatSession iteration;
	iteration.id = "remote-goal-iteration";
	iteration.execution_host_id = "ssh-test";
	iteration.remote_turn_reconnect_pending = true;
	iteration.goal_owner_chat_id = " " + owner.id + " ";
	iteration.goal_iteration_goal_id = " " + goal.id + " ";
	app.chats = {owner, iteration};

	UAM_ASSERT_EQ(uam::GoalService::PauseActiveGoalsAfterRestart(app), static_cast<std::size_t>(0));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal.id);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
}

UAM_TEST(AcpGoalPromptWaitsForConfirmedRemoteFreshSessionStop)
{
	TempDir temp("uam-remote-goal-fresh-session-stop");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "remote-goal-fresh-session";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.execution_host_id = "ssh-test";
	app.chats.push_back(chat);

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->running = true;
	session->session_ready = true;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {
	    "cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 0"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 0"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    *session, temp.root, argv, &error));
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::acp_detail::QueueGoalInternalPrompt(
	    app, *raw_session, app.chats.front(), "Review the completed work.", true,
	    "reviewer-model", true));
	UAM_ASSERT(raw_session->remote_stop_pending);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Review the completed work."));
	for (int attempt = 0; attempt < 100 && !app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(app.pending_acp_remote_stops.empty());
	UAM_ASSERT(raw_session->reconnect_pending);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Review the completed work."));
}

UAM_TEST(AcpGoalPromptRecoversAnUnconfirmedRemoteFreshSessionStop)
{
	TempDir temp("uam-remote-goal-unconfirmed-fresh-session-stop");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "remote-goal-unconfirmed-fresh-session";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.execution_host_id = "ssh-test";
	app.chats.push_back(chat);
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->running = true;
	session->session_ready = true;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {
	    "cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 70"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 70"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    *session, temp.root, argv, &error));
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	UAM_ASSERT(uam::acp_detail::QueueGoalInternalPrompt(
	    app, *raw_session, app.chats.front(), "Review after cleanup.", true,
	    "reviewer-model", true));
	for (int attempt = 0; attempt < 100 && !app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(raw_session->remote_stop_unconfirmed);
	UAM_ASSERT(raw_session->recovering_remote_turn);
	UAM_ASSERT(raw_session->reconnect_pending);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Review after cleanup."));
}

UAM_TEST(ClearingGoalReviewStateClearsItsRepairAttempt)
{
	uam::AcpSessionState session;
	session.goal_review_turn = true;
	session.goal_review_scheduled = true;
	session.goal_review_repair_attempts = 1;
	uam::acp_detail::ClearGoalReviewState(session);
	UAM_ASSERT_EQ(session.goal_review_repair_attempts, 0);
}

UAM_TEST(GoalRemovalPreservesCurrentRemoteTurn)
{
	TempDir temp("uam-remote-goal-removal-stop");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "remote-goal-removal";
	chat.provider_id = uam::provider_ids::kClaudeCli;
	chat.execution_host_id = "ssh-test";
	app.chats.push_back(chat);
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Delete safely.", 0, &goal_id,
	                                        "provider", "/goal"));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = uam::provider_ids::kClaudeCli;
	// Before session initialization, cancellation must stop and confirm the remote process.
	session->session_id.clear();
	session->running = true;
	session->processing = true;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {
	    "cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 0"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 0"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    *session, temp.root, argv, &error));
	app.acp_sessions.push_back(std::move(session));

	bool work_changed = false;
	UAM_ASSERT(uam::GoalService::RemoveGoal(app, chat.id, goal_id, &error, &work_changed));
	UAM_ASSERT(!work_changed);
	UAM_ASSERT(app.acp_sessions.front()->processing);
	UAM_ASSERT(!app.acp_sessions.front()->cancel_requested);
	UAM_ASSERT(app.pending_acp_remote_stops.empty());
	UAM_ASSERT(uam::GoalService::FindGoalById(app, chat.id, goal_id) == nullptr);
	(void)uam::StopAcpSession(app, chat.id);
	for (int attempt = 0; attempt < 100 && !app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

}

UAM_TEST(GoalRemovalPreservesPendingLegacyRemoteChildStops)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "remote-goal-pending-removal";
	chat.execution_host_id = "ssh-test";
	app.chats.push_back(chat);
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Delete safely.", 0, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	ChatSession child;
	child.id = "legacy-remote-goal-child";
	child.execution_host_id = "ssh-test";
	child.goal_owner_chat_id = chat.id;
	app.chats.push_back(child);
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = child.id;
	session->remote_stop_pending = true;
	app.acp_sessions.push_back(std::move(session));
	std::string error;
	UAM_ASSERT(uam::GoalService::RemoveGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT(app.acp_sessions.front()->remote_stop_pending);
	UAM_ASSERT(uam::GoalService::FindGoalById(app, chat.id, goal_id) == nullptr);
	app.acp_sessions.front()->remote_stop_pending = false;
	app.acp_sessions.front()->remote_stop_unconfirmed = true;
	UAM_ASSERT(uam::AcpStopInProgress(app, child.id));
	UAM_ASSERT(uam::GoalService::FindGoalById(app, chat.id, goal_id) == nullptr);
}

UAM_TEST(RemoteAttachDoesNotReactivateAnUnrelatedHistoricalGoal)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "remote-normal-turn";
	Goal old_goal;
	old_goal.id = "old-goal";
	old_goal.status = GoalStatus::Blocked;
	old_goal.last_blocker = "Codex process exited during an active turn.";
	chat.goals.push_back(old_goal);
	app.chats.push_back(chat);
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->recovering_remote_turn = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(
	    app, *raw_session, app.chats.front(),
	    R"({"jsonrpc":"2.0","method":"uam/remoteAttached"})"));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
}

UAM_TEST(RemoteRestartRestoresRootGoalReviewIdentity)
{
	TempDir temp("uam-remote-root-goal-review-restore");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "remote-root-review";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.execution_host_id = "missing-remote-host";
	chat.remote_turn_reconnect_pending = true;
	chat.goal_iteration_goal_id = "goal-review";
	chat.goal_iteration_turn_kind = "review";
	chat.goal_iteration_repair_attempts = 1;
	chat.messages_loaded = true;
	chat.messages.push_back(Message{MessageRole::User, "Review privately."});
	chat.messages.push_back(Message{MessageRole::Assistant, "{\"decision\":\"complete\"}"});
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	app.chats.push_back(chat);

	(void)uam::RestoreRemoteAcpSessionsAfterRestart(app);
	uam::AcpSessionState* session = FindTestAcpSession(app, chat.id);
	UAM_ASSERT(session != nullptr);
	UAM_ASSERT_EQ(session->goal_turn_kind, std::string("review"));
	UAM_ASSERT(session->goal_review_turn);
	UAM_ASSERT(session->goal_review_scheduled);
	UAM_ASSERT_EQ(session->goal_review_goal_id, std::string("goal-review"));
	UAM_ASSERT_EQ(session->goal_review_repair_attempts, 1);
}

UAM_TEST(GoalServiceCancellationMatchesPaddedLegacyIterationIds)
{
	uam::AppState app;
	ChatSession owner;
	owner.id = "padded-goal-owner";
	Goal goal;
	goal.id = "padded-goal";
	goal.status = GoalStatus::Paused;
	owner.goals.push_back(goal);
	ChatSession iteration;
	iteration.id = "padded-goal-iteration";
	iteration.goal_owner_chat_id = " " + owner.id + " ";
	iteration.goal_iteration_goal_id = " " + goal.id + " ";
	app.chats = {owner, iteration};
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = iteration.id;
	session->processing = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(uam::GoalService::RemoveGoal(app, owner.id, goal.id, &error));
	UAM_ASSERT(raw_session->processing);
	UAM_ASSERT(!raw_session->cancel_requested);
	UAM_ASSERT(uam::GoalService::FindGoalById(app, owner.id, goal.id) == nullptr);
}

UAM_TEST(GoalServiceUserCancellationStopsAttributedAgentRunsOnly)
{
	TempDir temp("goal-cancels-agent-runs");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession root = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	root.id = "goal-run-root";
	app.chats.push_back(root);
	std::string goal_id;
	std::string other_goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, root.id, "Stop all attributed work.", 0, &goal_id));
	UAM_ASSERT(uam::GoalService::CreateGoal(app, root.id, "Leave unrelated work alone.", 0, &other_goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, root.id, goal_id));

	const auto run = [&](std::string transcript_id, std::string run_goal_id,
	                     std::string status, int depth, std::string parent_id = {})
	{
		AgentRun value;
		value.id = uam::AgentRunLedger::NewRunId();
		value.root_chat_id = root.id;
		value.parent_run_id = std::move(parent_id);
		value.transcript_chat_id = std::move(transcript_id);
		value.goal_id = std::move(run_goal_id);
		value.agent_id = "build";
		value.provider_id = uam::provider_ids::kCodexCli;
		value.definition_snapshot = "builtin:build";
		value.task = "Bounded test task.";
		value.effective_workspace_access = "write";
		value.status = std::move(status);
		value.depth = depth;
		value.created_at = uam::time::TimestampNow();
		value.updated_at = value.created_at;
		return value;
	};
	AgentRun parent = run("goal-parent-chat", goal_id, "running", 1);
	AgentRun child = run("goal-child-chat", goal_id, "queued", 2, parent.id);
	AgentRun unrelated = run("other-run-chat", other_goal_id, "queued", 1);
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, parent));
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, child));
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, unrelated));
	app.agent_runs = {parent, child, unrelated};
	app.queued_agent_run_ids = {child.id, unrelated.id};
	for (const std::string& transcript_id : {parent.transcript_chat_id, child.transcript_chat_id})
	{
		ChatSession transcript;
		transcript.id = transcript_id;
		app.chats.push_back(transcript);
		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = transcript_id;
		app.acp_sessions.push_back(std::move(session));
	}
	auto root_session = std::make_unique<uam::AcpSessionState>();
	root_session->chat_id = root.id;
	root_session->processing = true;
	root_session->queued_user_prompts.push_back({.text = "Must be discarded."});
	uam::AcpSessionState* root_session_ptr = root_session.get();
	app.acp_sessions.push_back(std::move(root_session));
	app.pending_goal_iterations.push_back({root.id, goal_id, "Must not start.",
	                                        "worker_continuation", 0});

	std::string error;
	UAM_ASSERT(uam::GoalService::CancelGoalWork(app, root.id, goal_id, &error));
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, root.id, goal_id, GoalStatus::Paused));
	UAM_ASSERT_EQ(app.agent_runs[0].status, std::string("cancelled"));
	UAM_ASSERT_EQ(app.agent_runs[1].status, std::string("cancelled"));
	UAM_ASSERT_EQ(app.agent_runs[2].status, std::string("queued"));
	UAM_ASSERT(std::ranges::find(app.queued_agent_run_ids, child.id) == app.queued_agent_run_ids.end());
	UAM_ASSERT(std::ranges::find(app.queued_agent_run_ids, unrelated.id) != app.queued_agent_run_ids.end());
	UAM_ASSERT(!root_session_ptr->processing);
	UAM_ASSERT(root_session_ptr->queued_user_prompts.empty());
	UAM_ASSERT(root_session_ptr->goal_resume_suppressed);
	UAM_ASSERT(app.pending_goal_iterations.empty());
	UAM_ASSERT(std::ranges::none_of(app.chats, [&](const ChatSession& chat)
	{
		return chat.id == parent.transcript_chat_id || chat.id == child.transcript_chat_id;
	}));
	UAM_ASSERT(std::ranges::none_of(app.acp_sessions, [&](const auto& session)
	{
		return session != nullptr &&
		       (session->chat_id == parent.transcript_chat_id || session->chat_id == child.transcript_chat_id);
	}));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());

	root_session_ptr->running = true;
	root_session_ptr->session_ready = true;
	root_session_ptr->goal_resume_suppressed = false;
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, root.id, goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, root.id, other_goal_id, &error));
	UAM_ASSERT(root_session_ptr->running);
	UAM_ASSERT(!root_session_ptr->cancel_requested);
	UAM_ASSERT(!root_session_ptr->goal_resume_suppressed);
}

UAM_TEST(GoalRemovalWaitsForManagedCancellationPersistence)
{
	TempDir temp("goal-removal-waits-for-managed-cancel");
	uam::AppState app;
	const fs::path data_root = temp.root;
	app.data_root = data_root;
	ChatSession root;
	root.id = "goal-root";
	ChatSession transcript;
	transcript.id = "goal-managed-transcript";
	app.chats = {root, transcript};
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, root.id, "Cancel safely.", 0, &goal_id));

	AgentRun run;
	run.id = uam::AgentRunLedger::NewRunId();
	run.root_chat_id = root.id;
	run.transcript_chat_id = transcript.id;
	run.goal_id = goal_id;
	run.agent_id = "build";
	run.provider_id = uam::provider_ids::kCodexCli;
	run.definition_snapshot = "Cancel safely.";
	run.task = "Cancel the managed goal run.";
	run.effective_workspace_access = "write";
	run.status = "running";
	run.depth = 1;
	run.created_at = uam::time::TimestampNow();
	run.updated_at = run.created_at;
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
	app.agent_runs.push_back(run);
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = transcript.id;
	session->managed_agent_run_id = run.id;
	app.acp_sessions.push_back(std::move(session));

	const fs::path blocked_root = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_root, "blocked"));
	app.data_root = blocked_root;
	std::string error;
	UAM_ASSERT(!uam::GoalService::RemoveGoal(app, root.id, goal_id, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(uam::GoalService::FindGoalById(app, root.id, goal_id) != nullptr);
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("running"));

	app.data_root = data_root;
	uam::FlushPendingChatSaves(app, true);
	UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(transcript.id));
	UAM_ASSERT(uam::GoalService::RemoveGoal(app, root.id, goal_id, &error));
	UAM_ASSERT(uam::GoalService::FindGoalById(app, root.id, goal_id) == nullptr);
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("cancelled"));
}

UAM_TEST(AcpResumeGoalPersistsActiveStateBeforeQueueingWork)
{
	TempDir temp("uam-resume-goal-fresh-chat");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli, false);
	chat.workspace_directory = temp.root.string();
	chat.id = "chat-resume-work";
	chat.small_model_mode = true;
	std::string goal_id;
	app.chats.push_back(chat);
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Finish the paused work.", 0, &goal_id));
	Goal& goal = app.chats.front().goals.front();
	goal.status = GoalStatus::Paused;
	goal.last_next_prompt = "Run the focused regression tests.";
	goal.worker_model_id.clear();
	goal.reviewer_model_id.clear();
	app.settings.provider_chat_defaults[chat.provider_id].model_id = "fast-worker";
	app.settings.provider_chat_defaults[chat.provider_id].reviewer_model_id = "smart-reviewer";

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->session_ready = true;
	session->goal_resume_suppressed = true;
	session->goal_auto_resume_attempts = 2;
	session->turn_serial = 4;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Run the focused regression tests."));
	UAM_ASSERT_EQ(raw_session->turn_serial, 5);
	UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("worker_continuation"));
	UAM_ASSERT_EQ(raw_session->goal_turn_model_id, std::string("fast-worker"));
	UAM_ASSERT(raw_session->goal_internal_session);
	UAM_ASSERT_EQ(app.chats.front().goals.front().worker_model_id, std::string("fast-worker"));
	UAM_ASSERT_EQ(app.chats.front().goals.front().reviewer_model_id, std::string("smart-reviewer"));
	UAM_ASSERT(!raw_session->goal_resume_suppressed);
	UAM_ASSERT_EQ(raw_session->goal_auto_resume_attempts, 0);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.chats.front().messages.empty());
	const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT_EQ(persisted->active_goal_id, goal_id);
	(void)uam::StopAcpSession(app, chat.id);
}

UAM_TEST(AcpResumeGoalDoesNotQueueWorkWhenActivationCannotBePersisted)
{
	TempDir temp("uam-resume-goal-save-failure");
	uam::AppState app;
	app.data_root = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "blocked"));
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli, false);
	chat.workspace_directory = temp.root.string();
	chat.id = "chat-resume-save-failure";
	app.chats.push_back(chat);
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Remain paused.", 0, &goal_id));
	app.chats.front().goals.front().status = GoalStatus::Paused;
	app.chats.front().goals.front().last_next_prompt = "Do not queue this.";
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->session_ready = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT(error.find("persist") != std::string::npos);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT(!raw_session->processing);
}

UAM_TEST(AcpResumeGoalRestoresBlockedEvidenceWhenQueueingFails)
{
	TempDir temp("uam-resume-blocked-goal-queue-failure");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-resume-blocked-queue-failure";
	chat.workspace_directory = (temp.root / "not-a-directory").string();
	UAM_ASSERT(uam::io::WriteTextFile(chat.workspace_directory, "blocked"));
	app.chats.push_back(chat);
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Keep blocker evidence.", 0, &goal_id));
	Goal& goal = app.chats.front().goals.front();
	goal.status = GoalStatus::Blocked;
	goal.blocked_turn_count = 2;
	goal.last_blocker = "Waiting for user approval.";
	goal.last_blocker_kind = "user_permission_approval";
	goal.last_next_prompt = "Continue after approval.";
	Goal stale_sibling;
	stale_sibling.id = "stale-active-sibling";
	stale_sibling.objective = "Preserve stale sibling state.";
	stale_sibling.status = GoalStatus::Active;
	app.chats.front().goals.push_back(stale_sibling);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));

	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT_EQ(app.chats.front().goals.front().blocked_turn_count, 2);
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_blocker,
	              std::string("Waiting for user approval."));
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_blocker_kind,
	              std::string("user_permission_approval"));
	UAM_ASSERT_EQ(app.chats.front().goals.back().status, GoalStatus::Active);
	const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT_EQ(persisted->goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT_EQ(persisted->goals.front().last_blocker,
	              std::string("Waiting for user approval."));
	UAM_ASSERT_EQ(persisted->goals.back().status, GoalStatus::Active);
}

UAM_TEST(AcpResumeGoalDoesNotCancelAnotherActiveGoal)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-stale-resume";
	app.chats.push_back(chat);
	std::string paused_goal_id;
	std::string active_goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Paused goal.", 0, &paused_goal_id));
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, chat.id, paused_goal_id, GoalStatus::Paused));
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Current goal.", 0, &active_goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, active_goal_id));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->processing = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(app, chat.id, paused_goal_id, &error));
	UAM_ASSERT_EQ(error, std::string("Another goal is already active in this chat."));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, active_goal_id);
	UAM_ASSERT(raw_session->processing);
	UAM_ASSERT(!raw_session->cancel_requested);
}

UAM_TEST(AcpResumeGoalAdoptsTheCurrentlySelectedWorkerAndReviewerModels)
{
	TempDir temp("uam-resume-goal-selected-models");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli, false);
	chat.workspace_directory = temp.root.string();
	chat.id = "chat-resume-selected-models";
	chat.small_model_mode = true;
	chat.model_id = "qwen3.6-35b-a3b";
	chat.reviewer_model_id = "qwen3.8-27b";
	std::string goal_id;
	app.chats.push_back(chat);
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Resume with current selectors.", 0, &goal_id));
	Goal& goal = app.chats.front().goals.front();
	goal.status = GoalStatus::Paused;
	goal.worker_model_id = "ornith-1.5-35b-a3b";
	goal.reviewer_model_id = "old-reviewer";

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->session_ready = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT_EQ(goal.worker_model_id, std::string("qwen3.6-35b-a3b"));
	UAM_ASSERT_EQ(goal.reviewer_model_id, std::string("qwen3.8-27b"));
	UAM_ASSERT_EQ(raw_session->goal_turn_model_id, std::string("qwen3.6-35b-a3b"));
	(void)uam::StopAcpSession(app, chat.id);
}

UAM_TEST(AcpAutomaticResumeDoesNotInventNativeGoalAuthorization)
{
	TempDir temp("uam-provider-goal-busy");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-resume-provider";
	std::string goal_id;
	app.chats.push_back(chat);
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Ship the provider task.", 0, &goal_id, "provider", "/goal"));
	Goal& goal = app.chats.front().goals.front();
	goal.status = GoalStatus::Paused;

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->session_ready = true;
	session->running = true;
	session->provider_id = "codex-cli";
	session->session_id = "thread-1";
	session->processing = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT_EQ(error, std::string("Resume this native goal explicitly to authorize provider execution."));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(goal.status, GoalStatus::Paused);
	UAM_ASSERT(app.chats.front().goal_pending_continuation_id.empty());
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT(raw_session->codex_goal_pending_requests.empty());
	UAM_ASSERT(raw_session->pending_request_methods.empty());
	UAM_ASSERT(raw_session->processing);
}

UAM_TEST(AcpGoalIterationsUsePrivateContextsInOwnerChat)
{
	TempDir temp("uam-goal-private-contexts");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession root = ChatDomainService().CreateNewChat("", "gemini-cli", false);
	root.workspace_directory = temp.root.string();
	root.id = "goal-root";
	root.native_session_id = "root-native-session";
	app.chats.push_back(root);
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, root.id, "Finish the bounded work.", 0, &goal_id));
	app.chats.front().goals.front().status = GoalStatus::Paused;
	app.chats.front().goals.front().last_next_prompt = "Implement one bounded step.";

	std::string error;
	const bool resumed = uam::acp_detail::ResumeGoal(app, root.id, goal_id, &error);
	UAM_ASSERT_EQ(error, std::string{});
	UAM_ASSERT(resumed);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("root-native-session"));
	uam::AcpSessionState* session = FindTestAcpSession(app, root.id);
	UAM_ASSERT(session != nullptr);
	UAM_ASSERT(session->goal_internal_session);
	UAM_ASSERT_EQ(session->goal_turn_kind, std::string("worker_continuation"));
	session->session_ready = true;
	session->processing = true;
	session->queued_prompt.clear();
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->turn_user_message_index = -1;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(),
	                                     R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Implemented the bounded step."}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(),
	                                     R"({"jsonrpc":"2.0","id":10,"result":{}})"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages.front().role, MessageRole::Assistant);
	UAM_ASSERT(session->goal_review_turn);
	UAM_ASSERT(session->goal_internal_session);
	UAM_ASSERT(session->queued_prompt.find("Read-only goal review:") != std::string::npos);

	session->session_ready = true;
	session->processing = true;
	session->queued_prompt.clear();
	session->prompt_request_id = 11;
	session->pending_request_methods[11] = uam::acp_methods::kSessionPrompt;
	session->turn_user_message_index = -1;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(),
	                                     R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"{\"decision\":\"continue\",\"reason\":\"One step remains.\",\"nextPrompt\":\"Implement the final bounded step.\",\"progressUpdate\":{\"remaining\":[\"Implement the final bounded step.\"],\"currentStep\":\"Implement the final bounded step.\"}}"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(),
	                                     R"({"jsonrpc":"2.0","id":11,"result":{}})"));

	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("root-native-session"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.chats.front().messages.back().role, MessageRole::Assistant);
	UAM_ASSERT_EQ(app.chats.front().goals.front().loop_count, 1);
	UAM_ASSERT(session->goal_internal_session);
	UAM_ASSERT_EQ(session->goal_turn_kind, std::string("worker_continuation"));
	UAM_ASSERT_EQ(session->queued_prompt, std::string("Implement the final bounded step."));
	(void)uam::StopAcpSession(app, root.id);
}

UAM_TEST(AcpLegacyGoalIterationSwapsReviewerAndWorkerModelsFromOwner)
{
	TempDir temp("uam-legacy-goal-model-swap");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession owner;
	owner.id = "goal-owner";
	owner.provider_id = "gemini-cli";
	owner.model_id = "ornith-worker";
	owner.small_model_mode = true;
	Goal goal;
	goal.id = "goal-model-swap";
	goal.objective = "Use separate worker and reviewer models.";
	goal.status = GoalStatus::Active;
	goal.worker_model_id = "ornith-worker";
	goal.reviewer_model_id = "qwen-reviewer";
	owner.active_goal_id = goal.id;
	owner.goals.push_back(goal);
	app.chats.push_back(std::move(owner));

	ChatSession iteration;
	iteration.id = "legacy-iteration";
	iteration.provider_id = "gemini-cli";
	iteration.model_id = "qwen-reviewer";
	iteration.goal_owner_chat_id = "goal-owner";
	iteration.goal_iteration_goal_id = goal.id;
	iteration.goal_iteration_turn_kind = "worker_continuation";
	iteration.messages.push_back({MessageRole::User, "Implement one step."});
	iteration.messages.push_back({MessageRole::Assistant, "Implemented one step."});
	app.chats.push_back(std::move(iteration));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "legacy-iteration";
	session->provider_id = "gemini-cli";
	session->turn_user_message_index = 0;
	session->turn_assistant_message_index = 1;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	ChatSession& stored_iteration = app.chats.back();

	uam::acp_detail::ScheduleGoalReviewAfterSuccessfulTurn(app, *raw_session, stored_iteration, nullptr);
	UAM_ASSERT_EQ(raw_session->goal_turn_model_id, std::string("qwen-reviewer"));
	UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("review"));
	UAM_ASSERT(stored_iteration.goal_iteration_turn_kind == "review");
	UAM_ASSERT(std::ranges::any_of(raw_session->diagnostics, [](const auto& diagnostic) {
		return diagnostic.event == "process_launch" &&
		       diagnostic.reason == "starting" &&
		       diagnostic.detail.find("--model qwen-reviewer") != std::string::npos;
	}));

	raw_session->processing = false;
	raw_session->queued_prompt.clear();
	stored_iteration.messages.push_back({MessageRole::Assistant,
	                                    R"({"decision":"continue","reason":"One step remains.","nextPrompt":"Implement the final step.","progressUpdate":{"remaining":["Implement the final step."],"currentStep":"Implement the final step."}})"});
	raw_session->turn_assistant_message_index = 2;
	UAM_ASSERT(uam::acp_detail::HandleGoalReviewCompletion(
	    app, *raw_session, stored_iteration, nullptr));
	UAM_ASSERT_EQ(raw_session->goal_turn_model_id, std::string("ornith-worker"));
	UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("worker_continuation"));
	UAM_ASSERT(raw_session->queued_prompt.find("Implement the final step.") != std::string::npos);
	UAM_ASSERT(std::ranges::any_of(raw_session->diagnostics, [](const auto& diagnostic) {
		return diagnostic.event == "process_launch" &&
		       diagnostic.reason == "starting" &&
		       diagnostic.detail.find("--model ornith-worker") != std::string::npos;
	}));
	(void)uam::StopAcpSession(app, stored_iteration.id);
}

UAM_TEST(ChatRepositoryPersistsPausedGoalProgressState)
{
	TempDir temp("uam-goal-progress-state");
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-goal-progress";
	chat.small_model_mode = true;
	chat.goal_owner_chat_id = " chat-goal-owner ";
	chat.goal_iteration_goal_id = " goal-progress ";
	chat.goal_iteration_turn_kind = "review";
	chat.goal_iteration_repair_attempts = 1;
	Goal goal;
	goal.id = "goal-progress";
	goal.objective = "Track durable progress.";
	goal.status = GoalStatus::Paused;
	goal.token_budget = 100;
	goal.tokens_used = 20;
	goal.completed_items = {"first"};
	goal.remaining_items = {"second"};
	goal.current_step = "second";
	goal.last_verification = "npm test passed";
	goal.last_blocker = "User approval required.";
	goal.last_blocker_kind = "user_permission_approval";
	goal.last_next_prompt = "Do second";
	goal.same_next_prompt_count = 2;
	goal.loop_count = 3;
	goal.created_at = "2026-01-01T00:00:00.000Z";
	goal.updated_at = "2026-01-01T00:00:01.000Z";
	chat.goals.push_back(goal);

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().goals.size(), static_cast<std::size_t>(1));
	const Goal& loaded_goal = loaded.front().goals.front();
	UAM_ASSERT_EQ(loaded_goal.status, GoalStatus::Paused);
	UAM_ASSERT_EQ(loaded_goal.completed_items.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded_goal.completed_items.front(), std::string("first"));
	UAM_ASSERT_EQ(loaded_goal.remaining_items.front(), std::string("second"));
	UAM_ASSERT_EQ(loaded_goal.current_step, std::string("second"));
	UAM_ASSERT_EQ(loaded_goal.last_verification, std::string("npm test passed"));
	UAM_ASSERT_EQ(loaded_goal.last_blocker, std::string("User approval required."));
	UAM_ASSERT_EQ(loaded_goal.last_blocker_kind, std::string("user_permission_approval"));
	UAM_ASSERT_EQ(loaded_goal.last_next_prompt, std::string("Do second"));
	UAM_ASSERT_EQ(loaded_goal.same_next_prompt_count, 2);
	UAM_ASSERT_EQ(loaded_goal.loop_count, 3);
	UAM_ASSERT(loaded.front().small_model_mode);
	UAM_ASSERT_EQ(loaded.front().goal_owner_chat_id, std::string("chat-goal-owner"));
	UAM_ASSERT_EQ(loaded.front().goal_iteration_goal_id, std::string("goal-progress"));
	UAM_ASSERT_EQ(loaded.front().goal_iteration_turn_kind, std::string("review"));
	UAM_ASSERT_EQ(loaded.front().goal_iteration_repair_attempts, 1);
}

UAM_TEST(ChatRepositoryNormalizesLegacyCompletedGoalProgress)
{
	TempDir temp("uam-completed-goal-progress");
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-completed-progress";
	Goal goal;
	goal.id = "goal-completed-progress";
	goal.objective = "Already complete.";
	goal.status = GoalStatus::Complete;
	goal.completed_items = {"Verified work"};
	goal.remaining_items = {"Stale remaining work"};
	goal.current_step = "Stale remaining work";
	chat.goals.push_back(goal);

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().goals.front().completed_items,
	              (std::vector<std::string>{"Verified work"}));
	UAM_ASSERT(loaded.front().goals.front().remaining_items.empty());
	UAM_ASSERT(loaded.front().goals.front().current_step.empty());
}

UAM_TEST(AcpUamReviewedCompletionPersistsVerifiedProgress)
{
	TempDir temp("uam-reviewed-goal-complete");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "reviewed-goal-owner";
	chat.provider_id = "codex-cli";
	chat.active_goal_id = "reviewed-goal";
	Goal goal;
	goal.id = chat.active_goal_id;
	goal.objective = "Finish the verified bounded change.";
	goal.execution_owner = "uam";
	goal.remaining_items = {"Run focused verification"};
	chat.goals.push_back(goal);
	chat.messages.push_back({MessageRole::Assistant,
		R"({"decision":"complete","reason":"The bounded change is verified.","evidence":["Focused verification passed"],"progressUpdate":{"completed":["Bounded change"],"lastVerification":"Focused verification passed"}})"});
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.goal_review_turn = true;
	session.goal_review_goal_id = goal.id;
	session.turn_assistant_message_index = 0;
	UAM_ASSERT(uam::acp_detail::HandleGoalReviewCompletion(app, session, app.chats.front(), nullptr));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Complete);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT(app.chats.front().goals.front().remaining_items.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_verification, std::string("Focused verification passed"));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->goals.front().status, GoalStatus::Complete);
	UAM_ASSERT_EQ(saved->goals.front().execution_owner, std::string("uam"));
}

UAM_TEST(GoalServiceParseReviewDecisionAllowsWrappedStrictJson)
{
	const auto parsed = uam::GoalService::ParseReviewDecision("review:\n{\"decision\":\"continue\",\"reason\":\"more work remains\",\"nextPrompt\":\"Run tests\",\"progressUpdate\":{\"remaining\":[\"Run tests\"],\"currentStep\":\"Run tests\"}}\nthanks");
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->decision, std::string("continue"));
	UAM_ASSERT_EQ(parsed->reason, std::string("more work remains"));
	UAM_ASSERT_EQ(parsed->next_prompt, std::string("Run tests"));
	UAM_ASSERT(!uam::GoalService::ParseReviewDecision("{\"decision\":\"maybe\",\"reason\":\"x\",\"nextPrompt\":\"y\"}").has_value());
}

UAM_TEST(GoalServiceParseReviewDecisionRejectsContinueWithoutNextPrompt)
{
	UAM_ASSERT(!uam::GoalService::ParseReviewDecision(R"({"decision":"continue","reason":"more work remains","nextPrompt":"","progressUpdate":{"remaining":["Run tests"]}})").has_value());
	UAM_ASSERT(!uam::GoalService::ParseReviewDecision(R"({"decision":"continue","reason":"more work remains","nextPrompt":"   ","progressUpdate":{"remaining":["Run tests"]}})").has_value());

	const auto blocked = uam::GoalService::ParseReviewDecision(R"({"decision":"blocked","reason":"Need user input.","nextPrompt":""})");
	UAM_ASSERT(blocked.has_value());
	UAM_ASSERT_EQ(blocked->decision, std::string("blocked"));
	UAM_ASSERT_EQ(blocked->next_prompt, std::string(""));
}

UAM_TEST(GoalServiceParseReviewDecisionRejectsContinueWithoutNonEmptyProgress)
{
	UAM_ASSERT(!uam::GoalService::ParseReviewDecision(
	    R"({"decision":"continue","reason":"more work remains","nextPrompt":"Run tests"})").has_value());
	UAM_ASSERT(!uam::GoalService::ParseReviewDecision(
	    R"({"decision":"continue","reason":"more work remains","nextPrompt":"Run tests","progressUpdate":{}})").has_value());
}

UAM_TEST(GoalServiceParseReviewDecisionRequiresEvidenceForCompleteAndReadsProgress)
{
	UAM_ASSERT(!uam::GoalService::ParseReviewDecision(R"({"decision":"complete","reason":"done","nextPrompt":""})").has_value());

	const auto parsed = uam::GoalService::ParseReviewDecision(R"({
		"decision":"continue",
		"reason":"more work remains",
		"nextPrompt":"Run the focused tests",
		"evidence":["diff reviewed"],
		"blockerKind":"transient",
		"progressUpdate":{
			"completed":["parser guard"],
			"remaining":["runtime guard"],
			"currentStep":"runtime guard",
			"lastVerification":"npm test passed"
		}
	})");
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->blocker_kind, std::string("transient"));
	UAM_ASSERT_EQ(parsed->evidence.front(), std::string("diff reviewed"));
	UAM_ASSERT_EQ(parsed->completed_items.front(), std::string("parser guard"));
	UAM_ASSERT_EQ(parsed->remaining_items.front(), std::string("runtime guard"));
	UAM_ASSERT_EQ(parsed->current_step, std::string("runtime guard"));
	UAM_ASSERT_EQ(parsed->last_verification, std::string("npm test passed"));
}

UAM_TEST(GoalBlockersStopUnlessExplicitlyTransient)
{
	UAM_ASSERT(uam::acp_detail::GoalBlockerStopsImmediately("user_permission_approval"));
	UAM_ASSERT(uam::acp_detail::GoalBlockerStopsImmediately("permission"));
	UAM_ASSERT(uam::acp_detail::GoalBlockerStopsImmediately(""));
	UAM_ASSERT(!uam::acp_detail::GoalBlockerStopsImmediately("transient"));
}

UAM_TEST(GoalServiceParseReviewDecisionRepairsMissingEvidenceArrayBracket)
{
	const auto parsed = uam::GoalService::ParseReviewDecision(
	    R"({"decision":"continue","reason":"More work remains.","nextPrompt":"Continue.","evidence":"first check","second check"],"blockerKind":"","progressUpdate":{"remaining":["Continue"],"currentStep":"Continue"}})");
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->evidence, (std::vector<std::string>{"first check", "second check"}));
}

UAM_TEST(AcpGoalReviewTurnQueuesWorkerContinuationWithoutReviewingReviewOutput)
{
	TempDir temp("uam-goal-review-turn-completion");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Finish the work without looping.";
	goal.status = GoalStatus::Active;
	goal.token_budget = 1000;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->goal_turn_kind = "review";
	session->goal_review_turn = true;
	session->goal_review_scheduled = true;
	session->goal_review_goal_id = "goal-1";
	session->goal_review_user_prompt = "Implement the fix.";
	session->goal_review_assistant_text = "I inspected the runtime.";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"{\"decision\":\"continue\",\"reason\":\"More implementation remains.\",\"nextPrompt\":\"Apply the runtime guard.\",\"evidence\":[\"reviewed worker output\"],\"progressUpdate\":{\"remaining\":[\"Apply the runtime guard.\"],\"currentStep\":\"Apply the runtime guard.\"}}"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));

	UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("worker_continuation"));
	UAM_ASSERT(!raw_session->goal_review_turn);
	UAM_ASSERT(!raw_session->goal_review_scheduled);
	UAM_ASSERT(raw_session->goal_internal_session);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Apply the runtime guard."));
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_next_prompt, std::string("Apply the runtime guard."));
	UAM_ASSERT_EQ(app.chats.front().goals.front().same_next_prompt_count, 1);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	(void)uam::StopAcpSession(app, "chat-1");
}

UAM_TEST(AcpInvalidGoalReviewRetriesOnceThenBlocksInsteadOfCompleting)
{
	TempDir temp("uam-invalid-goal-review");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-invalid-review";
	chat.provider_id = "gemini-cli";
	Goal goal;
	goal.id = "goal-invalid-review";
	goal.objective = "Implement work that must not be marked complete by malformed output.";
	goal.status = GoalStatus::Active;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-invalid-review";
	session->provider_id = "gemini-cli";
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->goal_turn_kind = "review";
	session->goal_review_turn = true;
	session->goal_review_scheduled = true;
	session->goal_review_goal_id = goal.id;
	session->goal_review_user_prompt = "Implement the work.";
	session->goal_review_assistant_text = "I made partial progress.";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"{\"decision\":\"continue\",\"reason\":\"More work remains.\",\"nextPrompt\":\"Continue without progress.\"}"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(raw_session->goal_review_repair_attempts, 1);
	UAM_ASSERT(raw_session->goal_review_turn);
	UAM_ASSERT(raw_session->queued_prompt.find("only repair attempt") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("Do not call tools") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("progressUpdate") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("Implement the work.") == std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("I made partial progress.") == std::string::npos);

	raw_session->queued_prompt.clear();
	raw_session->prompt_request_id = 11;
	raw_session->pending_request_methods[11] = uam::acp_methods::kSessionPrompt;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Still not JSON"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":11,"result":{}})"));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_diagnostic, std::string("goal_blocked_invalid_review"));
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("goal_blocked_invalid_review"));
}

UAM_TEST(AcpBlockedGoalReviewPersistsItsBlockerKind)
{
	TempDir temp("uam-blocked-goal-review-kind");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-blocked-review";
	chat.provider_id = "gemini-cli";
	Goal goal;
	goal.id = "goal-blocked-review";
	goal.objective = "Stop when access is required.";
	goal.status = GoalStatus::Active;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->goal_turn_kind = "review";
	session->goal_review_turn = true;
	session->goal_review_scheduled = true;
	session->goal_review_goal_id = goal.id;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"{\"decision\":\"blocked\",\"reason\":\"Documents access is required.\",\"blockerKind\":\"user_permission_approval\"}"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_blocker_kind,
	              std::string("user_permission_approval"));

	const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(saved.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved.front().goals.front().last_blocker_kind,
	              std::string("user_permission_approval"));
}

UAM_TEST(AcpGoalReviewContinuationHonorsConfiguredLoopCap)
{
	TempDir temp("uam-goal-loop-cap");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	auto run_review = [&](const std::string& chat_id, int max_iterations, int initial_loop_count, bool expect_blocked, const std::string& blocker_kind = "") {
		std::cout << "[CAP_CASE] " << chat_id << '\n';
		uam::AppState app;
		app.data_root = temp.root;
		app.settings.goal_max_loop_iterations = max_iterations;

		ChatSession chat;
		chat.id = chat_id;
		chat.provider_id = "gemini-cli";
		Goal goal;
		goal.id = "goal-1";
		goal.objective = "Finish without an unbounded loop.";
		goal.status = GoalStatus::Active;
		goal.loop_count = initial_loop_count;
		goal.token_budget = 1000;
		goal.tokens_used = 40;
		chat.active_goal_id = goal.id;
		chat.goals.push_back(goal);
		app.chats.push_back(std::move(chat));

		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = chat_id;
		session->session_ready = true;
		session->processing = true;
		session->prompt_request_id = 10;
		session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
		session->goal_turn_kind = "review";
		session->goal_review_turn = true;
		session->goal_review_scheduled = true;
		session->goal_review_goal_id = "goal-1";
		uam::AcpSessionState* raw_session = session.get();
		app.acp_sessions.push_back(std::move(session));
		struct SessionCleanup
		{
			uam::AppState& app;
			const std::string& chat_id;
			~SessionCleanup() { (void)uam::StopAcpSession(app, chat_id); }
		} cleanup{app, chat_id};

		nlohmann::json review = {
			{"decision", blocker_kind.empty() ? "continue" : "blocked"},
			{"reason", "More work remains."},
			{"nextPrompt", "Continue the implementation."},
			{"progressUpdate", {{"remaining", {"Continue the implementation."}}, {"currentStep", "Continue the implementation."}}}
		};
		if (!blocker_kind.empty()) review["blockerKind"] = blocker_kind;
		const nlohmann::json update = {
			{"jsonrpc", "2.0"},
			{"method", "session/update"},
			{"params", {{"update", {{"sessionUpdate", "agent_message_chunk"}, {"content", {{"type", "text"}, {"text", review.dump()}}}}}}}
		};
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), update.dump()));
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));
		UAM_ASSERT_EQ(app.chats.front().goals.front().loop_count, initial_loop_count + 1);
		UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<std::int64_t>(40));
		UAM_ASSERT_EQ(app.chats.front().goals.front().token_budget, static_cast<std::int64_t>(1000));

		if (!expect_blocked)
		{
			UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
			UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("worker_continuation"));
			UAM_ASSERT(raw_session->goal_internal_session);
			if (blocker_kind.empty())
			{
				UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Continue the implementation."));
			}
			else
			{
				UAM_ASSERT(raw_session->queued_prompt.find(app.chats.front().goals.front().objective) != std::string::npos);
				UAM_ASSERT_EQ(app.chats.front().goals.front().current_step, std::string("Continue the implementation."));
				UAM_ASSERT_EQ(app.chats.front().goals.front().last_blocker_kind, blocker_kind);
			}
			UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
			(void)uam::StopAcpSession(app, chat_id);
			return;
		}

		UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
		UAM_ASSERT(app.chats.front().active_goal_id.empty());
		UAM_ASSERT(raw_session->queued_prompt.empty());
		UAM_ASSERT(raw_session->queued_user_prompts.empty());
		const bool terminal_blocker = !blocker_kind.empty() && blocker_kind != "transient";
		const std::string expected_blocker = terminal_blocker ? "More work remains." : "Maximum goal loop iterations reached";
		const std::string expected_diagnostic = terminal_blocker ? "" : "goal_blocked_max_loop_iterations_reached";
		UAM_ASSERT_EQ(app.chats.front().goals.front().last_blocker, expected_blocker);
		UAM_ASSERT_EQ(app.chats.front().goals.front().last_diagnostic, expected_diagnostic);
		if (!terminal_blocker)
		{
			UAM_ASSERT(!raw_session->diagnostics.empty());
			UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, expected_diagnostic);
		}
		if (!blocker_kind.empty()) UAM_ASSERT_EQ(app.chats.front().goals.front().last_blocker_kind, blocker_kind);
		const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
		const auto saved_chat = std::find_if(saved.begin(), saved.end(), [&](const ChatSession& candidate) { return candidate.id == chat_id; });
		UAM_ASSERT(saved_chat != saved.end());
		UAM_ASSERT_EQ(saved_chat->goals.front().status, GoalStatus::Blocked);
		UAM_ASSERT_EQ(saved_chat->goals.front().loop_count, initial_loop_count + 1);
		UAM_ASSERT_EQ(saved_chat->goals.front().tokens_used, static_cast<std::int64_t>(40));
		UAM_ASSERT_EQ(saved_chat->goals.front().token_budget, static_cast<std::int64_t>(1000));
		UAM_ASSERT_EQ(saved_chat->goals.front().last_blocker, expected_blocker);
		UAM_ASSERT_EQ(saved_chat->goals.front().last_diagnostic, expected_diagnostic);
		if (!blocker_kind.empty()) UAM_ASSERT_EQ(saved_chat->goals.front().last_blocker_kind, blocker_kind);
	};

	run_review("below-cap", 2, 0, false);
	run_review("at-cap", 2, 1, true);
	run_review("legacy-zero-is-bounded", 0, 199, true);
	run_review("oversized-cap-is-bounded", 999, 199, true);
	run_review("lower-cap", 1, 0, true);
	run_review("transient-below-cap", 2, 0, false, "transient");
	run_review("transient-at-cap", 2, 1, true, "transient");
	run_review("transient-lower-cap", 1, 0, true, "transient");
	run_review("transient-already-over-lowered-cap", 2, 5, true, "transient");
	run_review("transient-legacy-zero-is-bounded", 0, 199, true, "transient");
	run_review("transient-oversized-cap-is-bounded", 999, 199, true, "transient");
	run_review("terminal-below-cap", 2, 0, true, "user_permission_approval");
	run_review("terminal-at-cap", 1, 0, true, "user_permission_approval");
	run_review("unknown-terminal-at-cap", 1, 0, true, "unknown");
}

UAM_TEST(AcpGoalReviewTurnWithLostReviewBoolDoesNotScheduleReviewOfReviewOutput)
{
	TempDir temp("uam-goal-review-lost-bool");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Avoid reviewing review output.";
	goal.status = GoalStatus::Active;
	goal.token_budget = 1000;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->goal_turn_kind = "review";
	session->goal_review_turn = false;
	session->goal_review_goal_id = "goal-1";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"{\"decision\":\"continue\",\"reason\":\"More work remains.\",\"nextPrompt\":\"Do the next worker task.\",\"evidence\":[\"review text\"],\"progressUpdate\":{\"remaining\":[\"Do the next worker task.\"],\"currentStep\":\"Do the next worker task.\"}}"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));

	UAM_ASSERT(raw_session->goal_turn_kind.empty());
	UAM_ASSERT(!raw_session->goal_review_turn);
	UAM_ASSERT(!raw_session->goal_review_scheduled);
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, 0);
	UAM_ASSERT_EQ(app.chats.front().goals.front().blocked_turn_count, 1);
}

UAM_TEST(AcpGoalSchedulerSkipsMissingUserIndexAndReviewDecisionOutput)
{
	TempDir temp("uam-goal-scheduler-guards");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Only review worker turns.";
	goal.status = GoalStatus::Active;
	goal.token_budget = 1000;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->turn_user_message_index = -1;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Normal worker output."}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));
	UAM_ASSERT(!raw_session->goal_review_scheduled);
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, 0);

	Message user;
	user.role = MessageRole::User;
	user.content = "Worker prompt";
	app.chats.front().messages.push_back(std::move(user));
	raw_session->processing = true;
	raw_session->prompt_request_id = 11;
	raw_session->pending_request_methods[11] = uam::acp_methods::kSessionPrompt;
	raw_session->turn_user_message_index = static_cast<int>(app.chats.front().messages.size()) - 1;
	raw_session->current_assistant_message_index = -1;
	raw_session->turn_assistant_message_index = -1;

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"{\"decision\":\"continue\",\"reason\":\"Still going.\",\"nextPrompt\":\"Repeat this review.\",\"evidence\":[\"review output\"],\"progressUpdate\":{\"remaining\":[\"Repeat this review.\"],\"currentStep\":\"Repeat this review.\"}}"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":11,"result":{}})"));
	UAM_ASSERT(!raw_session->goal_review_scheduled);
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, 0);
}

UAM_TEST(AcpGoalWorkerTurnSchedulesOneReview)
{
	TempDir temp("uam-goal-worker-schedules-review");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "Implement the next fix.";
	chat.messages.push_back(std::move(user));
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Review normal worker turns.";
	goal.status = GoalStatus::Active;
	goal.token_budget = 1000;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->turn_user_message_index = 0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"I changed the runtime guard."}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));

	UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("review"));
	UAM_ASSERT(raw_session->goal_review_turn);
	UAM_ASSERT(raw_session->goal_review_scheduled);
	UAM_ASSERT(raw_session->goal_internal_session);
	UAM_ASSERT(raw_session->queued_prompt.find("Read-only goal review:") != std::string::npos);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(app.chats.front().goals.front().tokens_used > 0);
	(void)uam::StopAcpSession(app, "chat-1");
}

UAM_TEST(AcpGoalRepeatedIdenticalWorkerOutputMakesReviewLoopAware)
{
	TempDir temp("uam-goal-identical-output");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "Implement the next fix.";
	chat.messages.push_back(std::move(user));
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Recover when the model loops.";
	goal.status = GoalStatus::Active;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	std::string final_review_prompt;

	for (int turn = 1; turn <= 3; ++turn)
	{
		raw_session->processing = true;
		raw_session->prompt_request_id = 10 + turn;
		raw_session->pending_request_methods[10 + turn] = uam::acp_methods::kSessionPrompt;
		raw_session->turn_user_message_index = 0;
		raw_session->current_assistant_message_index = -1;
		raw_session->turn_assistant_message_index = -1;

		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Same worker output."}}}})"));
		const std::string result_line = std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(10 + turn) + R"(,"result":{}})";
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), result_line));

		UAM_ASSERT_EQ(app.chats.front().goals.front().same_assistant_text_count, turn);
		UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
		UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
		UAM_ASSERT(raw_session->goal_review_turn);
		final_review_prompt = raw_session->queued_prompt;
		// Reviews before the third identical turn carry no loop notice.
		UAM_ASSERT_EQ(final_review_prompt.find("loopDetection") != std::string::npos, turn >= 3);

		if (turn < 3)
		{
			(void)uam::StopAcpSession(app, "chat-1");
			raw_session->session_ready = true;
		}
	}

	UAM_ASSERT(final_review_prompt.find("identical output for 3 consecutive turns") != std::string::npos);
	UAM_ASSERT(final_review_prompt.find("materially different approach") != std::string::npos);
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, std::string("goal-1"));
	(void)uam::StopAcpSession(app, "chat-1");
}

UAM_TEST(AcpGoalWatchdogResumesStalledLoopAndBlocksAfterRepeatedResumes)
{
	TempDir temp("uam-goal-watchdog");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "opencode-cli";
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Keep the loop moving.";
	goal.status = GoalStatus::Active;
	goal.loop_count = 1;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->processing = false;
	session->last_runtime_activity_time_s = 1.0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	// User-cancelled sessions must not be auto-resumed.
	raw_session->goal_resume_suppressed = true;
	UAM_ASSERT(!uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));
	raw_session->goal_resume_suppressed = false;

	UAM_ASSERT(uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));
	UAM_ASSERT(!raw_session->queued_prompt.empty());
	UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("worker_continuation"));
	UAM_ASSERT(raw_session->goal_internal_session);
	UAM_ASSERT_EQ(raw_session->goal_auto_resume_attempts, 1);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	(void)uam::StopAcpSession(app, "chat-1");

	// A loop that keeps stalling is blocked visibly instead of retried forever.
	raw_session->session_ready = true;
	raw_session->goal_auto_resume_attempts = 3;
	raw_session->last_runtime_activity_time_s = 1.0;
	UAM_ASSERT(uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
}

UAM_TEST(AcpCancelledGoalTurnDoesNotQueueAnotherGoalPrompt)
{
	TempDir temp("uam-goal-cancel");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-goal-cancel";
	chat.provider_id = "codex-cli";
	chat.messages.push_back(Message{MessageRole::User, "Do the work"});
	chat.messages.push_back(Message{MessageRole::Assistant, "Partial result"});
	Goal goal;
	goal.id = "goal-cancel";
	goal.objective = "Finish the work.";
	chat.goals.push_back(goal);
	chat.active_goal_id = goal.id;
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-goal-cancel";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	session->cancel_requested = true;
	session->goal_resume_suppressed = true;
	session->turn_user_message_index = 0;
	session->turn_assistant_message_index = 1;
	session->current_assistant_message_index = 1;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                     R"({"jsonrpc":"2.0","method":"turn/completed","params":{"turn":{"id":"turn-1","status":"interrupted"}}})"));

	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT(!raw_session->processing);
}

UAM_TEST(AcpGoalWatchdogIgnoresFreshGoalsAndBusySessions)
{
	TempDir temp("uam-goal-watchdog-guards");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "opencode-cli";
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Wait for the first user prompt.";
	goal.status = GoalStatus::Active;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->last_runtime_activity_time_s = 1.0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	// Fresh goal (no turns yet): not resumed.
	UAM_ASSERT(!uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));

	// Engaged goal but busy session: not resumed.
	app.chats.front().goals.front().loop_count = 1;
	raw_session->processing = true;
	UAM_ASSERT(!uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));
	raw_session->processing = false;

	// Engaged goal but recent activity: not resumed.
	UAM_ASSERT(!uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 2.0));
}

UAM_TEST(AcpProviderManagedTurnCannotCompleteUnverifiedGoal)
{
	TempDir temp("uam-provider-managed-goal");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-provider-goal";
	chat.provider_id = "codex-cli";
	chat.messages.push_back({MessageRole::User, "/goal Keep this literal", "now"});
	chat.messages.push_back({MessageRole::Assistant, "Completed by provider.", "now"});
	app.chats.push_back(std::move(chat));

	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, app.chats.front().id, "Keep this literal", 100, &goal_id, "provider", "/goal"));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, app.chats.front().id, goal_id));
	Goal& goal = app.chats.front().goals.front();
	goal.loop_count = 7;

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->provider_id = "codex-cli";
	session->session_ready = true;
	session->processing = true;
	session->goal_command_revision = app.chats.front().goal_command_revision;
	session->turn_user_message_index = 0;
	session->turn_assistant_message_index = 1;
	session->last_runtime_activity_time_s = 1.0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(!uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));
	uam::acp_detail::CompletePromptTurnAndHandleGoalLoop(app, *raw_session, app.chats.front(), "ready", nullptr);
	UAM_ASSERT_EQ(goal.status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT(raw_session->queued_user_prompts.empty());
	UAM_ASSERT_EQ(goal.loop_count, 7);

	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, app.chats.front().id, goal_id));
	UAM_ASSERT_EQ(goal.status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);

	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(app.data_root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().goals.front().execution_owner, std::string("provider"));
	UAM_ASSERT_EQ(loaded.front().goals.front().provider_command, std::string("/goal"));
}

// NOTE: The former BuildPrompt/BuildCommand goal-context tests were removed with the
// dead one-shot command pipeline (PR-5). Goal-prompt composition is now exercised by the
// GoalServiceBuildContinuationPrompt* tests and the live ACP goal-review tests above.

UAM_TEST(GoalCommandsDuringTurnPersistImmediatelyAndRejectStaleCompletion)
{
	TempDir temp("uam-goal-active-command");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "goal-active-chat";
	app.chats.push_back(chat);
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Complete the task.", 0, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->turn_serial = 4;
	session->goal_command_revision = app.chats.front().goal_command_revision;
	uam::AcpSessionState* raw = session.get();
	app.acp_sessions.push_back(std::move(session));
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, chat.id, goal_id, GoalStatus::Paused));
	UAM_ASSERT(raw->processing);
	UAM_ASSERT(!raw->cancel_requested);
	std::string error;
	UAM_ASSERT(uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error));
	const std::string resumed_revision = app.chats.front().goal_command_revision;
	UAM_ASSERT_EQ(app.chats.front().goal_pending_continuation_id, goal_id);
	UAM_ASSERT_EQ(raw->turn_serial, 4);
	UAM_ASSERT(raw->queued_prompt.empty());
	UAM_ASSERT(uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT_EQ(app.chats.front().goal_command_revision, resumed_revision);
	const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT_EQ(persisted->goal_pending_continuation_id, goal_id);
	UAM_ASSERT_EQ(persisted->goals.front().status, GoalStatus::Active);
	UAM_ASSERT(uam::GoalService::RemoveGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT(raw->processing);
	uam::acp_detail::CompletePromptTurnAndHandleGoalLoop(app, *raw, app.chats.front(), "ready", nullptr, true);
	UAM_ASSERT(app.chats.front().goals.empty());
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT(app.chats.front().goal_pending_continuation_id.empty());
	UAM_ASSERT(raw->queued_prompt.empty());
	UAM_ASSERT(!raw->goal_review_scheduled);
}

UAM_TEST(GoalRestartDuringTurnReplacesOnePendingContinuation)
{
	TempDir temp("uam-goal-restart-active");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "goal-restart-chat";
	app.chats.push_back(chat);
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Complete the task.", 200, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	app.chats.front().goals.front().tokens_used = 100;
	app.chats.front().goals.front().completed_items = {"old result"};
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->processing = true;
	session->prompt_request_id = 11;
	session->turn_serial = 4;
	app.acp_sessions.push_back(std::move(session));
	std::string error;
	UAM_ASSERT(uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error, true));
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, 0);
	UAM_ASSERT(app.chats.front().goals.front().completed_items.empty());
	UAM_ASSERT_EQ(app.chats.front().goal_pending_continuation_id, goal_id);
	const std::string first = app.chats.front().goal_command_revision;
	UAM_ASSERT(uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error, true));
	UAM_ASSERT(first != app.chats.front().goal_command_revision);
	UAM_ASSERT_EQ(app.acp_sessions.front()->turn_serial, 4);
	UAM_ASSERT_EQ(app.chats.front().goal_pending_continuation_id, goal_id);
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, chat.id, goal_id, GoalStatus::Paused));
	UAM_ASSERT(app.chats.front().goal_pending_continuation_id.empty());
	UAM_ASSERT(app.acp_sessions.front()->processing);
}

UAM_TEST(PausedGoalCannotDispatchItsUnsentStartupPrompt)
{
	TempDir temp("uam-goal-unsent");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	app.chats.push_back(chat);
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Finish.", 100, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.processing = true;
	session.queued_prompt = "unsent continuation";
	session.goal_turn_kind = std::string(uam::acp_detail::kGoalTurnKindWorkerContinuation);
	session.goal_command_revision = app.chats.front().goal_command_revision;
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, chat.id, goal_id, GoalStatus::Paused));
	UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front()));
	UAM_ASSERT(session.queued_prompt.empty());
	UAM_ASSERT(!session.processing);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
}

UAM_TEST(DeferredGoalContinuationWaitsForOwnedStopAndQueuesOnlyOnce)
{
	TempDir temp("uam-goal-deferred-once");
	ScopedEnvVar scoped_path("PATH", InstallSilentGoalProviderShims(temp));
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli, false);
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Finish.", 100, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	app.chats.front().goal_pending_continuation_id = goal_id;
	uam::AsyncAcpProcessStopTask stop;
	stop.chat_id = chat.id;
	stop.finished = std::make_shared<std::atomic_bool>(false);
	app.acp_process_stop_tasks.push_back(std::move(stop));
	UAM_ASSERT(!uam::acp_detail::PollPendingGoalIterations(app));
	UAM_ASSERT(app.acp_sessions.empty());
	app.acp_process_stop_tasks.front().finished->store(true);
	UAM_ASSERT(uam::acp_detail::PollPendingGoalIterations(app));
	UAM_ASSERT(app.chats.front().goal_pending_continuation_id.empty());
	UAM_ASSERT_EQ(app.acp_sessions.size(), 1);
	const int serial = app.acp_sessions.front()->turn_serial;
	UAM_ASSERT(!uam::acp_detail::PollPendingGoalIterations(app));
	UAM_ASSERT_EQ(app.acp_sessions.front()->turn_serial, serial);
	(void)uam::StopAcpSession(app, chat.id);
}

namespace
{
	void PrepareGoalTimingFixture(uam::AppState& app, uam::AcpSessionState& session, const TempDir& temp)
	{
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession chat;
		ConfigureCodexNativeGoalFixture(session, chat);
		session.codex_goal_owner = {};
		chat.workspace_directory = temp.root.string();
		app.chats.push_back(chat);
	}

	int BeginGoalTimingPausedSet(uam::AppState& app, uam::AcpSessionState& session)
	{
		std::string error;
		const int query = session.next_request_id;
		UAM_ASSERT(uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), true, &error, true));
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", query}, {"result", {{"goal", nullptr}}}});
		const int clear = session.next_request_id - 1;
		UAM_ASSERT_EQ(session.pending_request_methods.at(clear), std::string("thread/goal/clear"));
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", clear}, {"result", {{"cleared", false}}}});
		const int paused_set = session.next_request_id - 1;
		UAM_ASSERT_EQ(session.pending_request_methods.at(paused_set), std::string("thread/goal/set"));
		return paused_set;
	}
}

UAM_TEST(CodexNativeGoalPausedAckDoesNotBlockOrStartBeforeDurability)
{
	TempDir temp("native-goal-durable-timing");
	uam::AppState app;
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	PrepareGoalTimingFixture(app, session, temp);
	CodexGoalIncarnationFixtureTransport transport(session, temp);
	const int paused_set = BeginGoalTimingPausedSet(app, session);
	ChatSession blocker;
	blocker.id = "native-goal-unrelated-publication";
	const std::shared_ptr<PreparedChatSave> publication = ChatRepository::PrepareChatSave(temp.root, blocker);
	UAM_ASSERT(publication != nullptr);
	std::promise<void> locked;
	std::future<void> ready = locked.get_future();
	std::jthread holder([&]()
	{
		(void)ChatRepository::PublishPreparedChatSave(publication, true, nullptr, [&]()
		{
			locked.set_value();
			std::this_thread::sleep_for(std::chrono::milliseconds(350));
			return true;
		});
	});
	ready.wait();
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", paused_set}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
	const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - start;
	const int turn_before_durable = session.codex_goal_initial_turn_request_id;
	for (int repeat = 0; repeat < 3; ++repeat)
		uam::acp_detail::StartPendingCodexNativeGoalTurn(app, session, app.chats.front());
	holder.join();
	UAM_ASSERT(elapsed < std::chrono::milliseconds(100));
	UAM_ASSERT_EQ(turn_before_durable, 0);
	UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
	for (int attempt = 0; attempt < 200 && session.codex_goal_initial_turn_request_id == 0; ++attempt)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	UAM_ASSERT(session.codex_goal_initial_turn_request_id != 0);
	const int delivered = session.codex_goal_initial_turn_request_id;
	UAM_ASSERT_EQ(session.pending_request_methods.at(delivered), std::string("turn/start"));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, app.chats.front().id);
	UAM_ASSERT(saved.has_value() && saved->goals.front().native_binding.has_value());
	UAM_ASSERT_EQ(saved->messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved->messages.front().content, saved->goals.front().objective);
	for (int repeat = 0; repeat < 3; ++repeat)
		uam::acp_detail::StartPendingCodexNativeGoalTurn(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, delivered);
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
}

UAM_TEST(CodexNativeGoalDurableCompletionRejectsCancelledOrRecreatedOwner)
{
	for (const bool recreate : {false, true})
	{
		TempDir temp(recreate ? "native-goal-durable-recreated" : "native-goal-durable-cancelled");
		uam::AppState app;
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		uam::AcpSessionState& session = *app.acp_sessions.back();
		PrepareGoalTimingFixture(app, session, temp);
		CodexGoalIncarnationFixtureTransport transport(session, temp);
		const int paused_set = BeginGoalTimingPausedSet(app, session);
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", paused_set}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
		UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
		if (recreate)
		{
			ChatSession replacement;
			replacement.id = app.chats.front().id;
			replacement.provider_id = app.chats.front().provider_id;
			replacement.created_at = app.chats.front().created_at;
			replacement.active_goal_id = app.chats.front().active_goal_id;
			replacement.goal_command_revision = app.chats.front().goal_command_revision;
			replacement.goals = app.chats.front().goals;
			app.chats.front() = replacement;
		}
		else
		{
			session.cancel_requested = true;
			app.chats.front().goal_command_revision = "cancelled-new-revision";
		}
		const std::shared_ptr<const std::uint8_t> current_incarnation = app.chats.front().metadata_incarnation;
		const std::string current_revision = app.chats.front().goal_command_revision;
		for (int attempt = 0; attempt < 100; ++attempt)
		{
			uam::FlushPendingChatSaves(app);
			uam::acp_detail::StartPendingCodexNativeGoalTurn(app, session, app.chats.front());
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
		UAM_ASSERT_EQ(app.chats.front().metadata_incarnation, current_incarnation);
		UAM_ASSERT_EQ(app.chats.front().goal_command_revision, current_revision);
		UAM_ASSERT(std::ranges::none_of(session.pending_request_methods, [](const std::pair<const int, std::string>& request) { return request.second == "turn/start"; }));
	}
}

namespace
{
	int PrepareGoalTimingActivation(uam::AppState& app, uam::AcpSessionState& session)
	{
		const int paused_set = BeginGoalTimingPausedSet(app, session);
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", paused_set}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
		for (int attempt = 0; attempt < 200 && session.codex_goal_initial_turn_request_id == 0; ++attempt)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		const int initial_turn = session.codex_goal_initial_turn_request_id;
		UAM_ASSERT(initial_turn != 0);
		UAM_ASSERT_EQ(session.pending_request_methods.at(initial_turn), std::string("turn/start"));
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", initial_turn}, {"result", {{"turn", {{"id", "timing-native-turn"}}}}}});
		const int activation = WaitForGoalTimingActivation(app, session);
		UAM_ASSERT_EQ(session.pending_request_methods.at(activation), std::string("thread/goal/set"));
		return activation;
	}

	std::chrono::steady_clock::duration GoalReceiptUnderHeldPublication(const TempDir& temp, const std::function<void()>& receipt)
	{
		ChatSession blocker;
		blocker.id = "goal-postinitial-unrelated-publication";
		const std::shared_ptr<PreparedChatSave> publication = ChatRepository::PrepareChatSave(temp.root, blocker);
		UAM_ASSERT(publication != nullptr);
		std::promise<void> locked;
		std::future<void> ready = locked.get_future();
		std::jthread holder([&]()
		{
			(void)ChatRepository::PublishPreparedChatSave(publication, true, nullptr, [&]()
			{
				locked.set_value();
				std::this_thread::sleep_for(std::chrono::milliseconds(350));
				return true;
			});
		});
		ready.wait();
		const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
		receipt();
		const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - start;
		holder.join();
		return elapsed;
	}
}

UAM_TEST(CodexNativeGoalActivationAckKeepsAccountingWithoutBlocking)
{
	TempDir temp("native-goal-activation-timing");
	uam::AppState app;
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	PrepareGoalTimingFixture(app, session, temp);
	CodexGoalIncarnationFixtureTransport transport(session, temp);
	const int activation = PrepareGoalTimingActivation(app, session);
	const std::chrono::steady_clock::duration elapsed = GoalReceiptUnderHeldPublication(temp, [&]()
	{
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", activation}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "active", 10)}}}});
	});
	UAM_ASSERT(elapsed < std::chrono::milliseconds(100));
	UAM_ASSERT(session.codex_goal_native_active);
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<std::int64_t>(30));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	const int request_count = session.next_request_id;
	for (int attempt = 0; attempt < 200; ++attempt)
	{
		uam::FlushPendingChatSaves(app);
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, app.chats.front().id);
		if (saved.has_value() && saved->goals.front().tokens_used == 30) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, app.chats.front().id);
	UAM_ASSERT(saved.has_value() && saved->goals.front().native_binding.has_value());
	UAM_ASSERT_EQ(saved->goals.front().native_binding->last_native_tokens_used, static_cast<std::int64_t>(10));
	UAM_ASSERT_EQ(saved->goals.front().tokens_used, static_cast<std::int64_t>(30));
	UAM_ASSERT_EQ(session.next_request_id, request_count);
	// A duplicate old ACK must not revert current accounting or recreate a transport command.
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", activation}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "active", 0)}}}});
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<std::int64_t>(30));
	UAM_ASSERT_EQ(session.next_request_id, request_count);
}

UAM_TEST(CodexNativeGoalStopAcksKeepOwnershipWithoutBlocking)
{
	for (const bool clear : {false, true})
	{
		TempDir temp(clear ? "native-goal-stop-clear-timing" : "native-goal-stop-pause-timing");
		uam::AppState app;
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		uam::AcpSessionState& session = *app.acp_sessions.back();
		PrepareGoalTimingFixture(app, session, temp);
		CodexGoalIncarnationFixtureTransport transport(session, temp);
		const int activation = PrepareGoalTimingActivation(app, session);
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", activation}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "active", 10)}}}});
		const int stop = session.next_request_id;
		UAM_ASSERT(uam::acp_detail::StopCodexNativeGoal(session, app.chats.front(), true, app.chats.front().active_goal_id, clear));
		UAM_ASSERT_EQ(session.pending_request_methods.at(stop), std::string(clear ? "thread/goal/clear" : "thread/goal/set"));
		const nlohmann::json result = clear ? nlohmann::json{{"cleared", true}} : nlohmann::json{{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 30)}};
		const std::chrono::steady_clock::duration elapsed = GoalReceiptUnderHeldPublication(temp, [&]()
		{
			uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", stop}, {"result", result}});
		});
		UAM_ASSERT(elapsed < std::chrono::milliseconds(100));
		UAM_ASSERT(!session.codex_goal_native_active);
		UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
		UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<std::int64_t>(clear ? 30 : 50));
		UAM_ASSERT_EQ(app.chats.front().goals.front().native_binding.has_value(), !clear);
		for (int attempt = 0; attempt < 200; ++attempt)
		{
			uam::FlushPendingChatSaves(app);
			const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, app.chats.front().id);
			if (saved.has_value() && saved->goals.front().native_binding.has_value() == !clear && saved->goals.front().tokens_used == (clear ? 30 : 50)) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, app.chats.front().id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->goals.front().native_binding.has_value(), !clear);
		UAM_ASSERT_EQ(saved->goals.front().tokens_used, static_cast<std::int64_t>(clear ? 30 : 50));
		// An old stop acknowledgement cannot retire a newly authorized owner.
		app.chats.front().goal_command_revision = "new-post-stop-owner";
		session.codex_goal_owner = uam::acp_detail::CaptureCodexNativeGoalOwner(session, app.chats.front(), app.chats.front().goals.front());
		const uam::AcpSessionState::CodexGoalOwner current = session.codex_goal_owner;
		const int request_count = session.next_request_id;
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", stop}, {"result", result}});
		UAM_ASSERT_EQ(session.codex_goal_owner.revision, current.revision);
		UAM_ASSERT_EQ(session.codex_goal_owner.parent_incarnation, current.parent_incarnation);
		UAM_ASSERT_EQ(session.next_request_id, request_count);
	}
}

namespace
{
	void FailGoalInitialHistorySave(uam::AppState& app, uam::AcpSessionState& session, const TempDir& temp)
	{
		app.data_root = temp.root / "blocked-goal-data-root";
		UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Private fixture blocks full native goal save"));
		const int paused_set = BeginGoalTimingPausedSet(app, session);
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", paused_set}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (session.codex_goal_initial_turn_pending.has_value() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::yield();
		}
		UAM_ASSERT(!session.codex_goal_initial_turn_pending.has_value());
		UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
		UAM_ASSERT(session.goal_resume_suppressed);
		UAM_ASSERT_EQ(app.chats.front().goals.front().last_diagnostic, std::string("native_goal_initial_history_save_failed"));
		UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(app.chats.front().messages.front().content, app.chats.front().goals.front().objective);
		UAM_ASSERT(app.chats.front().messages.front().acp_prompt_not_sent);
		UAM_ASSERT(app.chats.front().goals.front().native_binding.has_value());
		UAM_ASSERT(std::ranges::none_of(session.pending_request_methods, [](const std::pair<const int, std::string>& request) { return request.second == "turn/start"; }));
	}
}

UAM_TEST(CodexNativeGoalFullSaveFailureRetainsUnsentIntent)
{
	TempDir temp("native-goal-full-save-failure");
	uam::AppState app;
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	PrepareGoalTimingFixture(app, session, temp);
	CodexGoalIncarnationFixtureTransport transport(session, temp);
	FailGoalInitialHistorySave(app, session, temp);
	const int requests = session.next_request_id;
	for (int repeat = 0; repeat < 3; ++repeat)
		uam::acp_detail::StartPendingCodexNativeGoalTurn(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.next_request_id, requests);
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
}

UAM_TEST(CodexNativeGoalExplicitRetryDeliversFailedObjectiveExactlyOnce)
{
	for (const bool restart : {false, true})
	{
		TempDir temp(restart ? "native-goal-restart-unsent" : "native-goal-retry-unsent");
		uam::AppState app;
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		uam::AcpSessionState& session = *app.acp_sessions.back();
		PrepareGoalTimingFixture(app, session, temp);
		CodexGoalIncarnationFixtureTransport transport(session, temp);
		FailGoalInitialHistorySave(app, session, temp);
		const std::string original_time = app.chats.front().messages.front().created_at;
		std::error_code error;
		UAM_ASSERT(fs::remove(app.data_root, error));
		UAM_ASSERT(!error);
		if (restart)
		{
			// Recovery persists retained intent, then reloads it without volatile native ownership.
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			const std::optional<ChatSession> reloaded = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
			UAM_ASSERT(reloaded.has_value());
			app.chats.front() = *reloaded;
			session.codex_goal_owner = {};
			session.codex_goal_created_at = 0;
			session.codex_goal_updated_at = 0;
		}
		UAM_ASSERT(uam::GoalService::SetActiveGoal(app, app.chats.front().id, app.chats.front().goals.front().id));
		std::string start_error;
		const int query = session.next_request_id;
		UAM_ASSERT(uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), true, &start_error));
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", query}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
		const int paused_set = session.next_request_id - 1;
		UAM_ASSERT_EQ(session.pending_request_methods.at(paused_set), std::string("thread/goal/set"));
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", paused_set}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
		for (int attempt = 0; attempt < 200 && session.codex_goal_initial_turn_request_id == 0; ++attempt)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		const int delivered = session.codex_goal_initial_turn_request_id;
		UAM_ASSERT(delivered != 0);
		std::string wire;
		nlohmann::json delivered_frame;
		for (int attempt = 0; attempt < 100; ++attempt)
		{
			(void)uam::io::TryReadTextFile(temp.root / "wire.jsonl", wire);
			std::istringstream lines(wire);
			for (std::string line; std::getline(lines, line);)
			{
				const nlohmann::json frame = nlohmann::json::parse(line, nullptr, false);
				if (frame.is_object() && frame.value("id", -1) == delivered) delivered_frame = frame;
			}
			if (!delivered_frame.is_null()) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(delivered_frame.is_object());
		UAM_ASSERT_EQ(delivered_frame["method"], nlohmann::json("turn/start"));
		UAM_ASSERT_EQ(delivered_frame["params"]["input"].size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(delivered_frame["params"]["input"][0]["text"], nlohmann::json(app.chats.front().goals.front().objective));
		UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(app.chats.front().messages.front().created_at, original_time);
		UAM_ASSERT(!app.chats.front().messages.front().acp_prompt_not_sent);
		const int requests = session.next_request_id;
		for (int repeat = 0; repeat < 3; ++repeat)
			uam::acp_detail::StartPendingCodexNativeGoalTurn(app, session, app.chats.front());
		UAM_ASSERT_EQ(session.next_request_id, requests);
	}
}

UAM_TEST(CodexNativeGoalInitialDeliveryWitnessRoundTripsStrictly)
{
	TempDir temp("native-goal-delivery-witness");
	uam::AcpSessionState session;
	ChatSession chat;
	ConfigureCodexNativeGoalFixture(session, chat);
	chat.messages.push_back({MessageRole::User, chat.goals.front().objective, "2026-10-13T12:00:00.001Z"});
	Goal& goal = chat.goals.front();
	goal.native_binding = NativeGoalBinding{1, "codex-cli", "local", "thread-1", chat.id, goal.id, goal.objective, 100, 101, 20, 100, 80, 0};
	for (const NativeGoalInitialDelivery::State state : {NativeGoalInitialDelivery::State::Pending, NativeGoalInitialDelivery::State::Attempted, NativeGoalInitialDelivery::State::Accepted})
	{
		chat.messages.front().acp_prompt_not_sent = state != NativeGoalInitialDelivery::State::Accepted;
		goal.native_binding->initial_delivery = NativeGoalInitialDelivery{0, chat.messages.front().created_at, state,
			state == NativeGoalInitialDelivery::State::Accepted ? "owned-initial-turn" : ""};
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(loaded.has_value());
		UAM_ASSERT_EQ(loaded->goals.front().native_binding, goal.native_binding);
	}
	std::string original;
	const fs::path primary = temp.root / "chats" / (chat.id + ".json");
	UAM_ASSERT(uam::io::TryReadTextFile(primary, original));
	const nlohmann::json valid = nlohmann::json::parse(original);
	for (int malformed = 0; malformed < 5; ++malformed)
	{
		nlohmann::json changed = valid;
		nlohmann::json& witness = changed["goals"][0]["nativeBinding"]["initialDelivery"];
		if (malformed == 0) witness["userMessageIndex"] = -1;
		if (malformed == 1) witness.erase("userMessageCreatedAt");
		if (malformed == 2) witness["state"] = "future-unknown-state";
		if (malformed == 3) witness["turnId"] = "";
		if (malformed == 4) { witness["state"] = "pending"; witness["turnId"] = "unexpected-turn"; }
		UAM_ASSERT(uam::io::WriteTextFile(primary, changed.dump()));
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(loaded.has_value() && loaded->goals.front().native_binding.has_value());
		UAM_ASSERT_EQ(loaded->goals.front().native_binding->version, 0);
	}
	goal.native_binding->initial_delivery.reset();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::optional<ChatSession> legacy = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(legacy.has_value() && legacy->goals.front().native_binding.has_value());
	UAM_ASSERT_EQ(legacy->goals.front().native_binding->version, 1);
	UAM_ASSERT(!legacy->goals.front().native_binding->initial_delivery.has_value());
}

UAM_TEST(CodexNativeGoalAmbiguousOrUnownedInitialDeliveryCannotResume)
{
	for (int invalid = 0; invalid < 3; ++invalid)
	{
		TempDir temp("native-goal-delivery-hold");
		uam::AppState app;
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		uam::AcpSessionState& session = *app.acp_sessions.back();
		PrepareGoalTimingFixture(app, session, temp);
		CodexGoalIncarnationFixtureTransport transport(session, temp);
		ChatSession& chat = app.chats.front();
		Goal& goal = chat.goals.front();
		chat.messages.push_back({MessageRole::User, goal.objective, "2026-10-13T12:00:00.001Z"});
		chat.messages.front().acp_prompt_not_sent = true;
		goal.native_binding = NativeGoalBinding{1, "codex-cli", "local", "thread-1", chat.id, goal.id, goal.objective, 100, 101, 20, 100, 80, 0};
		if (invalid != 2)
			goal.native_binding->initial_delivery = NativeGoalInitialDelivery{0,
				invalid == 1 ? "different-row-time" : chat.messages.front().created_at,
				invalid == 0 ? NativeGoalInitialDelivery::State::Attempted : NativeGoalInitialDelivery::State::Pending, ""};
		const int requests = session.next_request_id;
		std::string error;
		UAM_ASSERT(!uam::acp_detail::StartCodexNativeGoal(app, session, chat, goal, true, &error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(session.next_request_id, requests);
		UAM_ASSERT(session.codex_goal_pending_requests.empty());
		UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
		UAM_ASSERT_EQ(chat.messages.size(), static_cast<std::size_t>(1));
		UAM_ASSERT(chat.messages.front().acp_prompt_not_sent);
		UAM_ASSERT_EQ(goal.status, GoalStatus::Active);
		UAM_ASSERT(session.goal_resume_suppressed);
	}
}

UAM_TEST(CodexNativeGoalPausedNotificationPreservesAcceptedDelivery)
{
	TempDir temp("native-goal-accepted-notification");
	uam::AppState app;
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	PrepareGoalTimingFixture(app, session, temp);
	CodexGoalIncarnationFixtureTransport transport(session, temp);
	const int paused_set = BeginGoalTimingPausedSet(app, session);
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", paused_set}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
	for (int attempt = 0; attempt < 200 && session.codex_goal_initial_turn_request_id == 0; ++attempt)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	const int initial = session.codex_goal_initial_turn_request_id;
	UAM_ASSERT(initial != 0);
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", initial}, {"result", {{"turn", {{"id", "accepted-before-notification"}}}}}});
	UAM_ASSERT(session.codex_goal_initial_turn_pending.has_value());
	UAM_ASSERT_EQ(session.codex_goal_initial_turn_pending->action, uam::AcpSessionState::CodexGoalOwner::Action::Activate);
	nlohmann::json updated = NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0);
	updated["updatedAt"] = 102;
	ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpHandleMessage(app, session, app.chats.front(),
		{{"method", "thread/goal/updated"}, {"params", {{"threadId", session.session_id}, {"goal", updated}}}}, nullptr);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, app.chats.front().goals.front().id);
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	const Goal& goal = app.chats.front().goals.front();
	UAM_ASSERT(goal.native_binding.has_value() && goal.native_binding->initial_delivery.has_value());
	UAM_ASSERT_EQ(goal.native_binding->initial_delivery->state, NativeGoalInitialDelivery::State::Accepted);
	UAM_ASSERT_EQ(goal.native_binding->initial_delivery->turn_id, std::string("accepted-before-notification"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(!app.chats.front().messages.front().acp_prompt_not_sent);
	UAM_ASSERT(!session.codex_goal_initial_turn_pending.has_value());
	// A changed accounting snapshot may hold activation; it cannot turn accepted work back into unsent intent.
	UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
	const int requests = session.next_request_id;
	for (int repeat = 0; repeat < 3; ++repeat)
		uam::acp_detail::StartPendingCodexNativeGoalTurn(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.next_request_id, requests);
}

UAM_TEST(CodexNativeGoalResumeSaveFailurePreservesAcceptedWitness)
{
	TempDir temp("native-goal-resume-accepted-save-failure");
	uam::AppState app;
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	PrepareGoalTimingFixture(app, session, temp);
	CodexGoalIncarnationFixtureTransport transport(session, temp);
	const int activation = PrepareGoalTimingActivation(app, session);
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", activation}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "active", 0)}}}});
	ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpHandleMessage(app, session, app.chats.front(),
		{{"method", "turn/completed"}, {"params", {{"threadId", session.session_id}, {"turn", {{"id", "timing-native-turn"}, {"status", "completed"}}}}}}, nullptr);
	UAM_ASSERT(session.codex_turn_id.empty());
	UAM_ASSERT(app.chats.front().goals.front().native_binding->initial_delivery.has_value());
	const NativeGoalInitialDelivery accepted = *app.chats.front().goals.front().native_binding->initial_delivery;
	UAM_ASSERT_EQ(accepted.state, NativeGoalInitialDelivery::State::Accepted);
	UAM_ASSERT(!app.chats.front().messages.front().acp_prompt_not_sent);
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, app.chats.front().id, app.chats.front().goals.front().id, GoalStatus::Paused, true));
	int pause = -1;
	for (const std::pair<const std::string, uam::AcpSessionState::CodexGoalOwner>& pending : session.codex_goal_pending_requests)
		if (pending.second.action == uam::AcpSessionState::CodexGoalOwner::Action::StopPause) pause = std::stoi(pending.first);
	UAM_ASSERT(pause >= 0);
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", pause}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, app.chats.front().id, app.chats.front().goals.front().id));
	Goal other;
	other.id = "unrelated-goal-preserved";
	other.status = GoalStatus::Blocked;
	other.last_diagnostic = "unrelated-diagnostic";
	app.chats.front().goals.push_back(other);
	app.data_root = temp.root / "blocked-accepted-resume";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Private accepted-resume failure fixture"));
	std::string error;
	const int query = session.next_request_id;
	UAM_ASSERT(uam::acp_detail::StartCodexNativeGoal(app, session, app.chats.front(), app.chats.front().goals.front(), true, &error));
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", query}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
	const int paused_set = session.next_request_id - 1;
	UAM_ASSERT_EQ(session.pending_request_methods.at(paused_set), std::string("thread/goal/set"));
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", paused_set}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
	for (int attempt = 0; attempt < 200 && session.codex_goal_initial_turn_pending.has_value(); ++attempt)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	UAM_ASSERT(!session.codex_goal_initial_turn_pending.has_value());
	UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
	UAM_ASSERT_EQ(app.chats.front().goals.front().native_binding->initial_delivery, std::optional<NativeGoalInitialDelivery>(accepted));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(!app.chats.front().messages.front().acp_prompt_not_sent);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.back().status, other.status);
	UAM_ASSERT_EQ(app.chats.front().goals.back().last_diagnostic, other.last_diagnostic);
	UAM_ASSERT(session.goal_resume_suppressed);
	UAM_ASSERT(std::ranges::none_of(session.pending_request_methods, [](const std::pair<const int, std::string>& request) { return request.second == "turn/start"; }));
}

namespace
{
	int PrepareGoalTimingFirstTurn(uam::AppState& app, uam::AcpSessionState& session)
	{
		const int paused_set = BeginGoalTimingPausedSet(app, session);
		uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", paused_set}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
		for (int attempt = 0; attempt < 200 && session.codex_goal_initial_turn_request_id == 0; ++attempt)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(session.codex_goal_initial_turn_request_id != 0);
		return session.codex_goal_initial_turn_request_id;
	}
}

UAM_TEST(CodexNativeGoalOldAcceptedReceiptCannotRetireSameCommandNewPhase)
{
	TempDir temp("native-goal-new-phase-cookie");
	uam::AppState app;
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	PrepareGoalTimingFixture(app, session, temp);
	CodexGoalIncarnationFixtureTransport transport(session, temp);
	const int initial = PrepareGoalTimingFirstTurn(app, session);
	uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", initial}, {"result", {{"turn", {{"id", "old-accepted-phase"}}}}}});
	UAM_ASSERT(session.codex_goal_initial_turn_pending.has_value());
	const uam::AcpSessionState::CodexGoalOwner old = *session.codex_goal_initial_turn_pending;
	uam::AcpSessionState::CodexGoalOwner replacement = uam::acp_detail::CaptureCodexNativeGoalOwner(app, session, app.chats.front(), app.chats.front().goals.front());
	UAM_ASSERT(replacement.initial_history_incarnation != old.initial_history_incarnation);
	const std::shared_ptr<const std::uint8_t> new_cookie = replacement.initial_history_incarnation;
	const std::string new_configuration = replacement.delivery_configuration;
	// Preserve every old scalar field while installing the fresh actual factory's attempt identity.
	replacement = old;
	replacement.initial_history_incarnation = new_cookie;
	replacement.delivery_configuration = new_configuration;
	session.codex_goal_initial_turn_pending = replacement;
	session.codex_goal_owner.initial_history_incarnation = new_cookie;
	session.goal_resume_suppressed = false;
	const std::string diagnostic = app.chats.front().goals.front().last_diagnostic;
	const int requests = session.next_request_id;
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	UAM_ASSERT(session.codex_goal_initial_turn_pending.has_value());
	UAM_ASSERT_EQ(session.codex_goal_initial_turn_pending->initial_history_incarnation, new_cookie);
	UAM_ASSERT_EQ(session.codex_goal_owner.initial_history_incarnation, new_cookie);
	UAM_ASSERT(!session.goal_resume_suppressed);
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_diagnostic, diagnostic);
	UAM_ASSERT_EQ(session.next_request_id, requests);
	UAM_ASSERT_EQ(app.chats.front().goals.front().native_binding->initial_delivery->state, NativeGoalInitialDelivery::State::Accepted);
}

UAM_TEST(CodexNativeGoalPendingReceiptsRejectChangedDeliveryConfiguration)
{
	for (const bool accepted : {false, true})
		for (int changed = 0; changed < 3; ++changed)
		{
			TempDir temp("native-goal-delivery-configuration");
			uam::AppState app;
			app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
			uam::AcpSessionState& session = *app.acp_sessions.back();
			PrepareGoalTimingFixture(app, session, temp);
			CodexGoalIncarnationFixtureTransport transport(session, temp);
			if (accepted)
			{
				const int initial = PrepareGoalTimingFirstTurn(app, session);
				uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", initial}, {"result", {{"turn", {{"id", "configured-accepted-phase"}}}}}});
			}
			else
			{
				const int paused_set = BeginGoalTimingPausedSet(app, session);
				uam::acp_detail::HandleAcpResponse(app, session, app.chats.front(), {{"id", paused_set}, {"result", {{"goal", NativeGoalFixtureSnapshot(app.chats.front(), "paused", 0)}}}});
			}
			UAM_ASSERT(session.codex_goal_initial_turn_pending.has_value());
			const int requests = session.next_request_id;
			if (changed == 0) app.chats.front().workspace_directory = (temp.root / "different-workspace").string();
			if (changed == 1) app.chats.front().model_id = "different-goal-model";
			if (changed == 2) app.chats.front().uam_agent_id = "different-desired-agent";
			for (int attempt = 0; attempt < 100; ++attempt)
			{
				uam::FlushPendingChatSaves(app);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			UAM_ASSERT(!session.codex_goal_initial_turn_pending.has_value());
			UAM_ASSERT_EQ(session.next_request_id, requests);
			UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
			UAM_ASSERT(std::ranges::none_of(session.codex_goal_pending_requests, [](const std::pair<const std::string, uam::AcpSessionState::CodexGoalOwner>& pending)
			{
				return pending.second.action == uam::AcpSessionState::CodexGoalOwner::Action::Activate ||
					pending.second.action == uam::AcpSessionState::CodexGoalOwner::Action::InitialTurn;
			}));
			UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
			UAM_ASSERT_EQ(app.chats.front().messages.front().acp_prompt_not_sent, !accepted);
			UAM_ASSERT_EQ(app.chats.front().goals.front().native_binding->initial_delivery->state,
				accepted ? NativeGoalInitialDelivery::State::Accepted : NativeGoalInitialDelivery::State::Pending);
		}
}


UAM_TEST(GoalPromptCompactionPreservesDistinctTasksAndVerificationRules)
{
	Goal goal;
	goal.objective = "Verify the database migration.";
	const std::string continuation = uam::GoalService::BuildContinuationPrompt(goal, 12, 100);
	UAM_ASSERT(continuation.size() < 400);
	const std::string no_duplicate = uam::GoalService::BuildContinuationPrompt(goal, 12, 100, false, "", false);
	UAM_ASSERT(no_duplicate.find(goal.objective) == std::string::npos);
	UAM_ASSERT(no_duplicate.find("verifying every requirement against current state") != std::string::npos);
	UAM_ASSERT(continuation.find("verifying every requirement against current state") != std::string::npos);
	UAM_ASSERT(continuation.find("used=12 budget=100 remaining=88") != std::string::npos);
	const std::string review = uam::GoalService::BuildReviewPrompt(goal, "  " + goal.objective + "\n", "Tests passed.", 0);
	UAM_ASSERT(review.find("recentUserPrompt") == std::string::npos);
	UAM_ASSERT(review.find(goal.objective) != std::string::npos);
	UAM_ASSERT(review.find("Tests passed.") != std::string::npos);
	UAM_ASSERT(review.find("Do not output") == std::string::npos);
	UAM_ASSERT(review.find("do not edit files or run mutating commands") != std::string::npos);
	const std::string distinct = uam::GoalService::BuildReviewPrompt(goal, "Check rollback safety too.", "Tests passed.", 3);
	UAM_ASSERT(distinct.find("<recentUserPrompt>\nCheck rollback safety too.") != std::string::npos);
	UAM_ASSERT(distinct.find("<loopDetection>") != std::string::npos);
	for (int index = 0; index < 15; ++index)
	{
		goal.completed_items.push_back("Verified step " + std::to_string(index));
		goal.remaining_items.push_back("Pending step " + std::to_string(index));
	}
	const std::string full_ledger = uam::GoalService::BuildReviewPrompt(goal, goal.objective, "Checked one step.", 0, true);
	UAM_ASSERT(full_ledger.find("Verified step 0\n") != std::string::npos);
	UAM_ASSERT(full_ledger.find("Pending step 14\n") != std::string::npos);
	goal.completed_items.clear();
	goal.remaining_items.clear();
	goal.status = GoalStatus::Complete;
	const std::string terminal = uam::GoalService::BuildReadOnlyTerminalPrompt(goal);
	UAM_ASSERT(terminal.find("lastBlocker") == std::string::npos);
	UAM_ASSERT(terminal.find("remainingItems") == std::string::npos);
	UAM_ASSERT(terminal.find("Do not reactivate") != std::string::npos);
}
