#include "test_harness.h"
#include "app/worktree_launch_preparation_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/paths/app_paths.h"
#include "app/agent_run_ledger.h"
#include "app/agent_run_scheduler.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "common/constants/app_constants.h"
#include "remote/runner_client.h"
#include <algorithm>

using namespace uam_test;

namespace
{
	struct WorkspacePromptFixture
	{
		TempDir temp{"uam-acp-workspace-launch"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;
		WorkspacePromptFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			ChatSession chat;
			chat.id = "private-workspace-launch";
			chat.provider_id = "gemini-cli";
			chat.workspace_directory = temp.root.string();
			chat.approval_mode.clear();
			chat.memory_enabled = false;
			chat.messages.push_back({.role = MessageRole::User, .content = "Saved private history"});
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
			owned->chat_id = app.chats.front().id;
			owned->provider_id = app.chats.front().provider_id;
			session = owned.get();
			app.acp_sessions.push_back(std::move(owned));
		}
	};
}

namespace
{
	std::shared_ptr<uam::WorktreeLaunchPreparationTask> SeedOwnedAcpWorkspaceTask(WorkspacePromptFixture& fixture)
	{
		uam::AppState& app = fixture.app;
		ChatSession& chat = app.chats.front();
		fixture.session->running = false;
		chat.svn_update_pending = true;
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = std::make_shared<uam::WorktreeLaunchPreparationTask>();
		task->data_root = app.data_root;
		task->original = chat;
		task->resolved_source = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat));
		task->base_branch = app.settings.git_default_base_branch;
		task->pull_before_chat = app.settings.git_pull_before_chat;
		task->git_enabled = app.settings.git_enabled;
		task->automatic_worktree = app.settings.git_auto_worktree;
		task->svn_enabled = app.settings.svn_enabled;
		task->svn_update_before_chat = app.settings.svn_update_before_chat;
		task->update_only = true;
		app.worktree_launch_preparations.emplace(chat.id, task);
		return task;
	}
}

UAM_TEST(AcpWorkspacePendingFirstDispatchRetainsDurableFifoHead)
{
	WorkspacePromptFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = fixture.app.chats.front();
	uam::AcpQueuedUserPromptState prompt;
	prompt.id = "private-workspace-fifo";
	prompt.text = "Accepted workspace input";
	session.queued_user_prompts.push_back(prompt);
	chat.acp_queued_prompts = {prompt};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, session, chat));
	UAM_ASSERT(session.worktree_launch_pending && session.worktree_launch_cancellation == task->cancellation);
	UAM_ASSERT(!session.processing && !session.running && session.prompt_request_id == 0);
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(session.reconnect_attempts, 0);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->acp_queued_prompts.size() == 1 && saved->svn_update_pending);
}

UAM_TEST(AcpWorkspaceDiscoveryCannotLaunchBeforeOwnedPreparation)
{
	WorkspacePromptFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
	fixture.session->model_discovery_only = true;
	const std::shared_ptr<const char> incarnation = fixture.session->permission_review_incarnation;
	UAM_ASSERT(uam::acp_detail::StartAcpProcessForChat(fixture.app, *fixture.session, fixture.app.chats.front()));
	UAM_ASSERT(!fixture.session->running && fixture.session->worktree_launch_pending);
	UAM_ASSERT(fixture.session->permission_review_incarnation == incarnation);
	UAM_ASSERT(fixture.session->worktree_launch_cancellation == task->cancellation);
}

UAM_TEST(AcpWorkspaceFailureOrRevocationHoldsInputWithoutReplacement)
{
	for (const bool revoked : {false, true})
	{
		WorkspacePromptFixture fixture;
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = fixture.app.chats.front();
		uam::AcpQueuedUserPromptState prompt;
		prompt.id = "private-held-workspace";
		prompt.text = "Retain failed workspace input";
		session.queued_user_prompts.push_back(prompt);
		chat.acp_queued_prompts = {prompt};
		session.worktree_launch_pending = true;
		session.worktree_launch_cancellation = task->cancellation;
		session.reconnect_pending = true;
		if (revoked) task->cancellation->request_stop();
		else task->error = "Private workspace preparation failed";
		UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, session, chat), uam::WorktreeLaunchPreparationState::Failed);
		UAM_ASSERT(session.queue_held && chat.acp_queue_held && !session.reconnect_pending);
		UAM_ASSERT(!session.worktree_launch_pending && !session.running && !session.processing);
		UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT(!session.last_error.empty());
		UAM_ASSERT(fixture.app.worktree_launch_preparations.empty());
		// The failure settles: later polls neither restart preparation nor log reconnect attempts.
		for (int poll = 0; poll < 3; ++poll) (void)uam::PollAllAcpSessions(fixture.app);
		UAM_ASSERT(session.queue_held && !session.reconnect_pending && !session.worktree_launch_pending && !session.running);
		UAM_ASSERT(fixture.app.worktree_launch_preparations.empty());
		UAM_ASSERT(std::ranges::none_of(session.diagnostics, [](const uam::AcpDiagnosticEntryState& entry) { return entry.event == "reconnect"; }));
	}
}


UAM_TEST(AcpReconnectParkedOnWorkspaceLogsWaitingNotStarted)
{
	// A reconnect that only parks on workspace preparation started nothing; its diagnostic must not claim a reconnect.
	WorkspacePromptFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = fixture.app.chats.front();
	uam::AcpQueuedUserPromptState prompt;
	prompt.id = "private-parked-workspace";
	prompt.text = "Input waiting for its workspace";
	session.queued_user_prompts.push_back(prompt);
	chat.acp_queued_prompts = {prompt};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	session.reconnect_pending = true;
	session.reconnect_not_before_time_s = 0.0;
	for (int poll = 0; poll < 5 && !session.worktree_launch_pending; ++poll) (void)uam::PollAllAcpSessions(fixture.app);
	UAM_ASSERT(session.worktree_launch_pending && !session.running);
	const auto logged = [&session](const char* reason) { return std::ranges::any_of(session.diagnostics, [reason](const uam::AcpDiagnosticEntryState& entry) { return entry.event == "reconnect" && entry.reason == reason; }); };
	UAM_ASSERT(logged("waiting_workspace"));
	UAM_ASSERT(!logged("started"));
	UAM_ASSERT(task->cancellation && !task->cancellation->stop_requested());
}

UAM_TEST(AcpPreparedSendToIdleWorktreeChatWaitsForItsOwnSave)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	// A Send to an idle local worktree chat with central provider setup prepares its agent, then resumes bound to its
	// own unsent row. That resume schedules a save, which reports the worktree Pending. The row-bound input was refused
	// and marked unsent, and the finished workspace check then launched the provider with nothing to send.
	TempDir temp("uam-prepared-worktree-send");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const ExecutionHost host = uam::execution_hosts::LocalHost();
	app.settings.execution_hosts = {host};
	app.settings.central_provider_configuration.enabled = true;
	app.settings.provider_chat_defaults["claude-cli"].feature_preference = "uam";
	auto& version = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("claude-cli", host.id)];
	version.checked = version.supported = true;
	version.installed_version = "2.1.31";
	version.execution_host = host;
	fs::create_directories(temp.root / "source");
	fs::create_directories(temp.root / "worktree");
	ChatSession initial;
	initial.id = "private-prepared-worktree-send";
	initial.provider_id = "claude-cli";
	initial.native_session_id = "0b6a0d63-6a8e-4d4c-9d43-2f0c8f2a51e7";
	initial.uam_agent_id = "build";
	initial.approval_mode.clear();
	initial.memory_enabled = false;
	initial.workspace_directory = initial.workspace_source_directory = (temp.root / "source").string();
	initial.workspace_worktree_directory = (temp.root / "worktree").string();
	initial.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	initial.workspace_branch_name = "build/private-prepared-send";
	initial.messages.push_back({.role = MessageRole::User, .content = "Saved private history"});
	app.chats.push_back(std::move(initial));
	ChatSession& chat = app.chats.front();
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	struct Cleanup
	{
		uam::AppState& app;
		~Cleanup() { uam::FastStopAcpSessionsForExit(app); }
	} cleanup{app};
	std::string error;
	const std::string text = "Continue with the prepared input";
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, text, &error));
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(app, chat.id);
	UAM_ASSERT(session != nullptr && session->pending_agent_input && !session->running);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (session->pending_agent_input && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(!session->pending_agent_input && !session->running);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(chat.messages.back().content, text);
	// The input waits for its workspace instead of settling as unsent.
	UAM_ASSERT(chat.messages.back().acp_prompt_not_sent && !chat.messages.back().interrupted);
	UAM_ASSERT_EQ(session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(session->queued_user_prompts.front().existing_user_message_index, 1);
	UAM_ASSERT(!session->queue_held && session->lifecycle_state != uam::acp_detail::kAcpLifecycleError);
	UAM_ASSERT(session->worktree_launch_pending);
	// Its owned save keeps the input queued for the launch that follows.
	const auto save_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while ((app.pending_chat_save_at_by_chat_id.contains(chat.id) || app.pending_chat_save_tasks.contains(chat.id)) &&
	       std::chrono::steady_clock::now() < save_deadline)
	{
		(void)uam::FlushPendingChatSaves(app, true);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(chat.id) && !app.pending_chat_save_tasks.contains(chat.id));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved && saved->acp_queued_prompts.size() == 1 && !saved->acp_queue_held);
	UAM_ASSERT(!saved->messages.back().interrupted);
#endif
}


UAM_TEST(AcpRetryInIdleWorktreeChatWaitsForPendingSave)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	// Retry of an unsent message in an idle local worktree chat while a chat save is pending, such as the one a refused
	// Send schedules. The worktree reports Pending until that save lands, and the row-bound Retry was refused with
	// "Workspace preparation is still pending."; it now waits for the workspace like other workspace preparation.
	TempDir temp("uam-retry-worktree-save");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const ExecutionHost host = uam::execution_hosts::LocalHost();
	app.settings.execution_hosts = {host};
	app.settings.central_provider_configuration.enabled = true;
	app.settings.provider_chat_defaults["claude-cli"].feature_preference = "uam";
	auto& version = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("claude-cli", host.id)];
	version.checked = version.supported = true;
	version.installed_version = "2.1.31";
	version.execution_host = host;
	fs::create_directories(temp.root / "source");
	fs::create_directories(temp.root / "worktree");
	ChatSession initial;
	initial.id = "private-retry-worktree-save";
	initial.provider_id = "claude-cli";
	initial.uam_agent_id = "build";
	initial.approval_mode.clear();
	initial.memory_enabled = false;
	initial.created_at = initial.updated_at = "2026-11-17 12:32:00";
	initial.workspace_directory = initial.workspace_source_directory = (temp.root / "source").string();
	initial.workspace_worktree_directory = (temp.root / "worktree").string();
	initial.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	initial.workspace_branch_name = "build/private-retry";
	Message unsent{.role = MessageRole::User, .content = "Retry this unsent input"};
	unsent.created_at = "2026-11-17 12:32:40";
	unsent.acp_prompt_not_sent = unsent.interrupted = true;
	initial.messages.push_back(unsent);
	app.chats.push_back(std::move(initial));
	ChatSession& chat = app.chats.front();
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	struct Cleanup
	{
		uam::AppState& app;
		~Cleanup() { uam::FastStopAcpSessionsForExit(app); }
	} cleanup{app};
	uam::acp_detail::ScheduleChatSave(app, chat, 0.0);
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(chat.id));
	std::string error;
	const bool retried = uam::RetryFailedAcpMessage(app, chat.id, 0, &error);
	UAM_ASSERT_EQ(error, std::string());
	UAM_ASSERT(retried);
	const uam::AcpSessionState* session = uam::FindAcpSessionForChat(app, chat.id);
	UAM_ASSERT(session != nullptr && !session->running && !session->queue_held);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	UAM_ASSERT(chat.messages.front().acp_prompt_not_sent && !chat.messages.front().interrupted);
	UAM_ASSERT_EQ(session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(session->queued_user_prompts.front().existing_user_message_index, 0);
	UAM_ASSERT(session->lifecycle_state != uam::acp_detail::kAcpLifecycleError);
#endif
}

namespace
{
	// Binds the fixture session to a running managed run so the scheduler owns its outcome.
	std::string BindManagedRunForLaunchFailure(uam::AppState& app, uam::AcpSessionState& session, const std::string& transcript_id)
	{
		AgentRun run;
		run.id = uam::AgentRunLedger::NewRunId();
		run.root_chat_id = "managed-launch-root";
		run.transcript_chat_id = transcript_id;
		run.agent_id = "reviewer";
		run.provider_id = session.provider_id;
		run.definition_snapshot = "Review safely.";
		run.task = "Review.";
		run.effective_workspace_access = "read";
		run.status = "running";
		run.depth = 1;
		run.created_at = run.updated_at = uam::time::TimestampNow();
		UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
		app.agent_runs.push_back(run);
		session.managed_agent_run_id = run.id;
		return run.id;
	}

	void AssertManagedLaunchFailed(uam::AppState& app)
	{
		(void)uam::AgentRunScheduler::PollAtForTests(app, 1000);
		UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("failed"));
		UAM_ASSERT_EQ(app.agent_runs.front().diagnostic_code, std::string("managed_launch_failed"));
		UAM_ASSERT(!uam::strings::Contains(app.agent_runs.front().diagnostic, "Retry"));
	}
}

UAM_TEST(AcpWorkspaceFailureFailsManagedRunInsteadOfHolding)
{
	WorkspacePromptFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = fixture.app.chats.front();
	(void)BindManagedRunForLaunchFailure(fixture.app, session, chat.id);
	session.worktree_launch_pending = true;
	session.worktree_launch_cancellation = task->cancellation;
	task->error = "Private workspace preparation failed";
	UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, session, chat), uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT(session.queue_held && !session.managed_launch_attempted);
	AssertManagedLaunchFailed(fixture.app);
}

UAM_TEST(AcpRemoteStopAttachSkipsAFailingWorkspace)
{
	// Stop always works: a restored remote turn whose worktree no longer validates (its branch was switched) must still
	// reach the host to stop, and only a real launch reports the workspace error.
	WorkspacePromptFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = fixture.app.chats.front();
	chat.execution_host_id = "ssh-unconfigured";
	task->error = "This chat's worktree is on another branch.";
	session.processing = session.recovering_remote_turn = session.remote_stop_unconfirmed = true;
	chat.remote_turn_reconnect_pending = chat.remote_stop_cleanup_pending = true;
	std::string error;
	(void)uam::acp_detail::StartAcpProcessForChat(fixture.app, session, chat, &error);
	// A launch that runs a provider still reports this error (AcpWorkspaceFailureFailsManagedRunInsteadOfHolding).
	UAM_ASSERT(!task->error_reported);
	UAM_ASSERT(error != task->error && session.last_error != task->error);
}

UAM_TEST(AcpRemoteStopDuringAWorkspaceCheckKeepsItsReconnect)
{
	// Stop pressed while a restored turn's worktree check still runs: that check must not hold back or cancel the stop.
	WorkspacePromptFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = fixture.app.chats.front();
	chat.execution_host_id = "ssh-unconfigured";
	chat.remote_turn_reconnect_pending = true;
	session.processing = session.recovering_remote_turn = true;
	session.worktree_launch_pending = true;
	session.worktree_launch_cancellation = task->cancellation;
	std::string error;
	(void)uam::CancelAcpTurn(fixture.app, chat.id, &error);
	UAM_ASSERT(session.remote_stop_unconfirmed && chat.remote_stop_cleanup_pending);
	UAM_ASSERT(session.reconnect_pending);
	UAM_ASSERT(!session.worktree_launch_pending);
	UAM_ASSERT(task->cancellation->stop_requested());
}

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_SSH && defined(__APPLE__)
#include "common/chat/acp_prompt_recovery.h"
namespace
{
	struct DirectWorkspaceWorkerFixture
	{
		TempDir temp{"uam-direct-remote-workspace-worker"};
		TempDir runner_temp{"uwr"};
		struct ResidentProcess
		{
			uam::platform::StdioProcessPlatformFields fields;
			~ResidentProcess()
			{
				auto& process = PlatformServicesFactory::Instance().process_service;
				process.StopStdioProcess(fields, true);
				process.CloseStdioProcessHandles(fields);
			}
		} resident;
		uam::AppState app;
		fs::path repository = fs::canonical(temp.root) / "repository";
		fs::path entered = temp.root / "validation-entered";
		fs::path release = temp.root / "validation-release";
		fs::path wire = temp.root / "provider-wire.jsonl";
		std::unique_ptr<ScopedEnvVar> path, gate_root, primary_env, home, xdg, runner_env, socket_env;
		std::shared_ptr<uam::WorktreeLaunchPreparationTask> task;
		int callbacks = 0;
		std::optional<bool> receipt;
		const std::string fresh = "Exactly one input after genuine workspace validation";

		DirectWorkspaceWorkerFixture()
		{
			fs::create_directories(temp.root / "private-home");
			fs::create_directories(temp.root / "private-config");
			home = std::make_unique<ScopedEnvVar>("HOME", (temp.root / "private-home").string());
			xdg = std::make_unique<ScopedEnvVar>("XDG_CONFIG_HOME", (temp.root / "private-config").string());
			// Resolve the real executable through the same public login-shell service
			// before installing our wrapper. All repository commands forward here.
			auto& process = PlatformServicesFactory::Instance().process_service;
			const auto resolved_git = process.ExecuteCommand("command -v git", 10000);
			UAM_ASSERT(resolved_git.ok && resolved_git.exit_code == 0 && !resolved_git.timed_out && !resolved_git.output_truncated && !resolved_git.canceled);
			const fs::path real_git = uam::paths::PathFromUtf8(uam::strings::Trim(resolved_git.output));
			UAM_ASSERT(real_git.is_absolute() && fs::is_regular_file(real_git));
			fs::create_directories(repository);
			UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repository.string())));
			UAM_ASSERT(RunGitForTest(repository, "config user.email uam@example.test"));
			UAM_ASSERT(RunGitForTest(repository, "config user.name UAM"));
			UAM_ASSERT(uam::io::WriteTextFile(repository / "source.txt", "Private source survivor\n"));
			UAM_ASSERT(RunGitForTest(repository, "add source.txt"));
			UAM_ASSERT(RunGitForTest(repository, "commit -m baseline"));
			const fs::path bin = temp.root / "bin";
			fs::create_directories(bin);
			const fs::path runner = process.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
			UAM_ASSERT(fs::is_regular_file(runner));
			const fs::path socket = runner_temp.root / "s";
			UAM_ASSERT(socket.string().size() < 104);
			runner_env = std::make_unique<ScopedEnvVar>("UAM_TEST_DIRECT_RUNNER", runner.string());
			socket_env = std::make_unique<ScopedEnvVar>("UAM_TEST_DIRECT_SOCKET", socket.string());
			gate_root = std::make_unique<ScopedEnvVar>("UAM_TEST_DIRECT_WORKSPACE", temp.root.string());
			// The resident's children inherit this private path when it starts.
			primary_env = std::make_unique<ScopedEnvVar>("UAM_TEST_DIRECT_PRIMARY",
			    AppPaths::UamChatFilePath(temp.root / "data", "private-direct-workspace-chat").string());
			path = std::make_unique<ScopedEnvVar>("PATH", bin.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin"));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "private-home" / ".profile",
			    "export PATH=" + ShellQuoteForTest(bin.string()) + ":\"$PATH\"\n"));
			// Every apparent SSH connection is a local private Unix-socket bridge.
			// The special witness exits locally before any bridge or barrier exists.
			const fs::path ssh = bin / "ssh";
			UAM_ASSERT(uam::io::WriteTextFile(ssh, R"SH(#!/bin/sh
if [ "$1" = --uam-private-witness ]; then printf '%s\n' private-ssh; exit 0; fi
case " $* " in *' private-workspace-fixture '*) ;; *) exit 93 ;; esac
printf bridge > "$UAM_TEST_DIRECT_WORKSPACE/ssh-bridge-witness"
if [ -f "$UAM_TEST_DIRECT_WORKSPACE/validation-arm" ]; then
  printf entered > "$UAM_TEST_DIRECT_WORKSPACE/validation-entered"
  while [ ! -f "$UAM_TEST_DIRECT_WORKSPACE/validation-release" ]; do /bin/sleep 0.02; done
fi
exec "$UAM_TEST_DIRECT_RUNNER" bridge --socket "$UAM_TEST_DIRECT_SOCKET"
)SH"));
			fs::permissions(ssh, fs::perms::owner_all);
			const auto resolved_ssh = process.ExecuteCommand("command -v ssh", 10000);
			UAM_ASSERT(resolved_ssh.ok && resolved_ssh.exit_code == 0 && !resolved_ssh.timed_out && !resolved_ssh.output_truncated && !resolved_ssh.canceled);
			UAM_ASSERT_EQ(uam::paths::PathFromUtf8(uam::strings::Trim(resolved_ssh.output)), ssh);
			const auto ssh_witness = process.ExecuteCommand("ssh --uam-private-witness", 10000);
			UAM_ASSERT(ssh_witness.ok && ssh_witness.exit_code == 0 && !ssh_witness.timed_out && !ssh_witness.output_truncated && !ssh_witness.canceled);
			UAM_ASSERT_EQ(uam::strings::Trim(ssh_witness.output), std::string("private-ssh"));
			std::string runner_error;
			UAM_ASSERT(process.StartStdioProcess(resident.fields, runner_temp.root, {runner.string(), "serve", "--socket", socket.string()}, &runner_error));
			Wait([&]() { return fs::exists(socket); });
			ExecutionHost host;
			host.id = "private-direct-workspace";
			host.transport = "ssh";
			host.ssh_alias = "private-workspace-fixture";
			host.platform = "linux";
			host.runner_version = std::string(uam::constants::kAppVersion).substr(1);
			host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
			host.runner_status = "ready";
			// Build and save a genuine remote-owned receipt using the public service.
			// A fresh AppState below models restart; do not erase/forge a task to pass Send.
			{
				uam::AppState builder;
				builder.data_root = temp.root / "data";
				builder.settings.execution_hosts = {uam::execution_hosts::LocalHost(), host};
				builder.settings.central_provider_configuration.enabled = false;
				builder.settings.git_enabled = builder.settings.git_auto_worktree = true;
				builder.settings.git_pull_before_chat = false;
				builder.settings.provider_chat_defaults["gemini-cli"].feature_preference = "provider";
				builder.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
				ChatSession chat = ChatDomainService().CreateNewChat({}, "gemini-cli", false);
				chat.id = "private-direct-workspace-chat";
				chat.execution_host_id = host.id;
				chat.routing_mode = "explicit";
				chat.workspace_directory = repository.string();
				chat.workspace_isolation_kind = uam::paths::kPendingGitWorktreeIsolationKind;
				chat.memory_enabled = chat.uam_control_enabled = false;
				chat.approval_mode.clear();
				chat.messages.push_back({.role = MessageRole::User, .content = "Previously delivered private history"});
				builder.chats.push_back(std::move(chat));
				UAM_ASSERT(ChatRepository::SaveChat(builder.data_root, builder.chats.front()));
				UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(builder, builder.chats.front()).state, uam::WorktreeLaunchPreparationState::Pending);
				const auto initial = builder.worktree_launch_preparations.at(builder.chats.front().id);
				try
				{
					Wait([&]()
					{
						(void)uam::PollWorktreeLaunchPreparations(builder);
						(void)uam::FlushPendingChatSaves(builder);
						return initial->saved.has_value() && builder.pending_chat_save_tasks.empty();
					});
					UAM_ASSERT(initial->saved.value_or(false) && initial->prepared && initial->prepared->operation.ok);
				}
				catch (...)
				{
					initial->cancellation->request_stop();
					if (initial->worker.valid()) initial->worker.wait();
					(void)uam::PollWorktreeLaunchPreparations(builder);
					while (uam::acp_detail::HasActiveChatSaveWorker(builder, builder.chats.front().id))
						(void)uam::FlushPendingChatSaves(builder);
					throw; // Preserve the original setup failure after actual work retires.
				}
				UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(builder.chats.front()));
				UAM_ASSERT(fs::is_regular_file(initial->receipt_file));
				const auto loaded = ChatRepository::LoadLocalChat(builder.data_root, builder.chats.front().id);
				UAM_ASSERT(loaded && loaded->workspace_worktree_directory == builder.chats.front().workspace_worktree_directory);
				app.data_root = builder.data_root;
				app.settings = builder.settings;
				app.provider_profiles = builder.provider_profiles;
				app.chats.push_back(*loaded);
			}
			UAM_ASSERT(app.worktree_launch_preparations.empty() && app.worktree_operation_chat_ids.empty());
			UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.acp_sessions.empty());
			UAM_ASSERT(Chat().routing_mode == "explicit" && uam::paths::IsGitWorktreeIsolated(Chat()));
			UAM_ASSERT(fs::exists(temp.root / "ssh-bridge-witness"));
			const fs::path peer = temp.root / "private-gemini";
			UAM_ASSERT(uam::io::WriteTextFile(peer, R"SH(#!/bin/sh
if [ "$1" = --version ]; then
  printf version > "$UAM_TEST_DIRECT_WORKSPACE/gemini-version-witness"
  printf '%s\n' '0.55.1'
  exit 0
fi
while IFS= read -r line; do
  printf '%s\n' "$line" >> "$UAM_TEST_DIRECT_WORKSPACE/provider-wire.jsonl"
  id=$(printf '%s\n' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*) printf '{"jsonrpc":"2.0","id":%s,"result":{"protocolVersion":1,"agentCapabilities":{"loadSession":true}}}\n' "$id" ;;
    *'"method":"session/new"'*|*'"method":"session/load"'*) printf '{"jsonrpc":"2.0","id":%s,"result":{"sessionId":"private-workspace-session"}}\n' "$id" ;;
    *'"method":"session/set_mode"'*|*'"method":"session/set_model"'*|*'"method":"session/set_config_option"'*) printf '{"jsonrpc":"2.0","id":%s,"result":{}}\n' "$id" ;;
    *'"method":"session/prompt"'*)
      grep -Fq 'Exactly one input after genuine workspace validation' "$UAM_TEST_DIRECT_PRIMARY" || exit 91
      grep -Fq 'workspace_worktree_directory' "$UAM_TEST_DIRECT_PRIMARY" || exit 92
      printf '{"jsonrpc":"2.0","id":%s,"result":{"stopReason":"end_turn"}}\n' "$id" ;;
  esac
done
)SH"));
			fs::permissions(peer, fs::perms::owner_all);
			// A fallback version probe must never resolve the installed provider.
			const fs::path version_shim = bin / "gemini";
			UAM_ASSERT(uam::io::WriteTextFile(version_shim, "#!/bin/sh\nexec /bin/sh " + ShellQuoteForTest(peer.string()) + " \"$@\"\n"));
			fs::permissions(version_shim, fs::perms::owner_all);
			app.provider_profiles.front().interactive_command = uam::shell::JoinEscapedArgs({"/bin/sh", peer.string()});
			auto& version = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("gemini-cli", host.id)];
			version.checked = version.supported = true;
			version.installed_version = "0.55.1";
			version.execution_host = host;
			UAM_ASSERT_EQ(AppPaths::UamChatFilePath(app.data_root, Chat().id),
			    temp.root / "data" / "chats" / "private-direct-workspace-chat.json");
			// Verify resolution before executing even a version command, so a failed
			// login-profile prerequisite cannot invoke an installed provider.
			const auto resolved_peer = process.ExecuteCommand("command -v gemini", 10000);
			UAM_ASSERT(resolved_peer.ok && resolved_peer.exit_code == 0 && !resolved_peer.timed_out && !resolved_peer.output_truncated && !resolved_peer.canceled);
			UAM_ASSERT_EQ(uam::paths::PathFromUtf8(uam::strings::Trim(resolved_peer.output)), version_shim);
			const auto version_witness = process.ExecuteCommand("gemini --version", 10000);
			UAM_ASSERT(version_witness.ok && version_witness.exit_code == 0 && !version_witness.timed_out && !version_witness.output_truncated && !version_witness.canceled);
			UAM_ASSERT_EQ(ReadFile(temp.root / "gemini-version-witness"), std::string("version"));
			UAM_ASSERT_EQ(uam::strings::Trim(version_witness.output), std::string("0.55.1"));
			UAM_ASSERT(!fs::exists(entered) && !fs::exists(wire));
			// The actual validation retry can block only after both public witnesses.
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "validation-arm", "armed"));
		}
		ChatSession& Chat() { return app.chats.front(); }
		static void Wait(const std::function<bool()>& done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!done() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			UAM_ASSERT(done());
		}
		void Start()
		{
			std::string error;
			const bool accepted = uam::SendAcpPrompt(app, Chat().id, fresh, &error);
			const auto found = app.worktree_launch_preparations.find(Chat().id);
			if (found != app.worktree_launch_preparations.end()) task = found->second;
			UAM_ASSERT(accepted);
			UAM_ASSERT(error.empty());
			UAM_ASSERT(task && task->remote_host && task->worker.valid());
			UAM_ASSERT(uam::DeferChatSaveResult(app, Chat().id, [this](bool saved) { ++callbacks; receipt = saved; }));
			Wait([&]() { return fs::exists(entered); });
			auto* session = uam::FindAcpSessionForChat(app, Chat().id);
			UAM_ASSERT(session && session->worktree_launch_pending && !session->running && !session->processing);
			UAM_ASSERT(task->worker.valid() && task->worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready);
			UAM_ASSERT(session->worktree_launch_cancellation == task->cancellation);
			UAM_ASSERT_EQ(session->queued_user_prompts.size(), std::size_t{1});
			UAM_ASSERT(session->queued_user_prompts.front().direct_admission && session->queued_user_prompts.front().text == fresh);
			UAM_ASSERT_EQ(Chat().acp_queued_prompts, (std::vector<uam::AcpQueuedUserPromptState>{session->queued_user_prompts.front()}));
			UAM_ASSERT(callbacks == 0 && !receipt.has_value() && !fs::exists(wire));
			UAM_ASSERT_EQ(Chat().messages.size(), std::size_t{1});
		}
		void Release() { UAM_ASSERT(uam::io::WriteTextFile(release, "release")); }
		void Tick()
		{
			(void)uam::PollWorktreeLaunchPreparations(app);
			(void)uam::PollAllAcpSessions(app);
			(void)uam::FlushPendingChatSaves(app);
		}
		std::vector<nlohmann::json> Prompts()
		{
			std::vector<nlohmann::json> result;
			std::istringstream lines(fs::exists(wire) ? ReadFile(wire) : std::string{});
			for (std::string line; std::getline(lines, line);)
			{
				const auto request = nlohmann::json::parse(line);
				if (request.value("method", "") == "session/prompt") result.push_back(request);
			}
			return result;
		}
		~DirectWorkspaceWorkerFixture()
		{
			// Release genuine process work even on assertion unwind. Never replace a
			// worker result, clear pending tasks/timers, or invoke a live provider.
			(void)uam::io::WriteTextFile(release, "release");
			for (const auto& entry : app.worktree_launch_preparations)
			{
				entry.second->cancellation->request_stop();
				if (entry.second->worker.valid()) entry.second->worker.wait();
			}
			(void)uam::PollWorktreeLaunchPreparations(app);
			// Save futures are detached packaged tasks: destroying AppState alone
			// does not join them. Observe/harvest actual work while DATA and callback
			// storage still exist; do not clear inert retry timers or task entries.
			while (uam::acp_detail::HasActiveChatSaveWorker(app, Chat().id))
			{
				(void)uam::FlushPendingChatSaves(app);
				std::this_thread::yield();
			}
			// A ready worktree result can precede destruction of the worker's
			// captured lease. Acquire the real exclusive guard before TempDir.
			std::shared_ptr<uam::WorkspaceLeaseGate::Lease> retired;
			while (!(retired = app.worktree_retention_gate.TryAcquireCleanup()))
				std::this_thread::yield();
			for (const auto& session : app.acp_sessions)
				if (session)
				{
					auto& process = PlatformServicesFactory::Instance().process_service;
					process.StopStdioProcess(*session, true);
					process.CloseStdioProcessHandles(*session);
				}
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(resident.fields, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(resident.fields);
			// No owner polling follows this observed worker retirement barrier.
		}
	};
}

UAM_TEST(AcpWorkspaceRemoteValidationAndSaveGateDirectInputExactlyOnce)
{
	UAM_ASSERT(GitAvailableForTests());
	DirectWorkspaceWorkerFixture fixture;
	fixture.Start();
	auto& chat = fixture.Chat();
	const auto original_rows = chat.messages;
	const auto original = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, chat.id));
	fixture.Release();
	DirectWorkspaceWorkerFixture::Wait([&]() { return fixture.task->worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(fixture.task->merged && fixture.task->saving && !fixture.task->saved.has_value());
	UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(chat));
	UAM_ASSERT_EQ(chat.workspace_worktree_directory, fixture.task->prepared->metadata.workspace_worktree_directory);
	UAM_ASSERT(chat.messages == original_rows && fixture.callbacks == 0 && !fs::exists(fixture.wire));
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, chat.id)), original);
	// Gate two is real metadata publication, independent of completed Git work.
	auto* session = uam::FindAcpSessionForChat(fixture.app, chat.id);
	UAM_ASSERT(session && !uam::DrainNextQueuedAcpUserPrompt(fixture.app, *session, chat));
	UAM_ASSERT(!session->running && !fs::exists(fixture.wire));
	DirectWorkspaceWorkerFixture::Wait([&]()
	{
		fixture.Tick();
		return fixture.callbacks == 1 && fixture.receipt == std::optional<bool>{true} &&
		    fixture.Prompts().size() == 1 && !uam::FindAcpSessionForChat(fixture.app, chat.id)->processing;
	});
	UAM_ASSERT(fixture.task->saved == std::optional<bool>{true});
	const auto durable = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(durable && durable->workspace_worktree_directory == chat.workspace_worktree_directory);
	UAM_ASSERT_EQ(std::ranges::count(durable->messages, fixture.fresh, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(chat.messages, fixture.fresh, &Message::content), 1);
	UAM_ASSERT(durable->messages.front() == original_rows.front());
	UAM_ASSERT_EQ(fixture.Prompts().size(), std::size_t{1});
	UAM_ASSERT(fixture.Prompts().front().at("params").at("prompt").dump().find(fixture.fresh) != std::string::npos);
	UAM_ASSERT_EQ(fixture.callbacks, 1);
	UAM_ASSERT_EQ(ReadFile(fixture.repository / "source.txt"), std::string("Private source survivor\n"));
}

UAM_TEST(AcpWorkspaceRemoteValidationRejectsStaleWorkspaceAndHost)
{
	UAM_ASSERT(GitAvailableForTests());
	for (const std::string_view variation : {"workspace", "host"})
	{
		std::cerr << "PRIVATE_WORKSPACE_STALE variation=" << (variation == "workspace" ? 0 : 1) << std::endl;
		DirectWorkspaceWorkerFixture fixture;
		fixture.Start();
		auto* session = uam::FindAcpSessionForChat(fixture.app, fixture.Chat().id);
		UAM_ASSERT(session && session->queued_user_prompts.size() == 1);
		const auto captured = session->queued_user_prompts.front();
		UAM_ASSERT(captured.direct_admission && !captured.id.empty() && captured.revision > 0 && captured.text == fixture.fresh);
		const auto original_owner = ChatRepository::QueueStorageOwner(fixture.Chat());
		const auto native_before = fixture.Chat().native_session_id;
		const auto old_rows = fixture.Chat().messages;
		const auto isolated_directory = fixture.Chat().workspace_worktree_directory;
		if (variation == "workspace") fixture.Chat().workspace_directory = (fixture.temp.root / "new-owner-workspace").string();
		else if (variation == "host") fixture.Chat().execution_host_id = "removed-private-host";
		fixture.Release();
		DirectWorkspaceWorkerFixture::Wait([&]()
		{
			fixture.Tick();
			return fixture.callbacks == 1 && !fixture.task->worker.valid() &&
			    fixture.app.pending_chat_save_tasks.empty() && fixture.app.pending_chat_save_at_by_chat_id.empty();
		});
		// This plain callback reports real storage, not the selection-qualified
		// BeginDurableAdmission result. A saved retained intent may be held.
		const auto durable = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
		const auto current_owner = ChatRepository::QueueStorageOwner(fixture.Chat());
		const bool live_held = session->queue_held && fixture.Chat().acp_queue_held;
		const bool durable_held = durable && durable->acp_queue_held;
		const bool live_exact = session->queued_user_prompts == std::deque<uam::AcpQueuedUserPromptState>{captured} &&
		    fixture.Chat().acp_queued_prompts == std::vector<uam::AcpQueuedUserPromptState>{captured};
		const bool durable_exact = durable && durable->acp_queued_prompts == std::vector<uam::AcpQueuedUserPromptState>{captured};
		if (fixture.callbacks != 1 || !fixture.receipt.has_value() || !live_held || !durable_held || !live_exact || !durable_exact)
		    std::cerr << "PRIVATE_WORKSPACE_STALE failure variation=" << (variation == "workspace" ? 0 : 1)
		        << " callbacks=" << fixture.callbacks << " receipt_present=" << fixture.receipt.has_value()
		        << " receipt_saved=" << fixture.receipt.value_or(false) << " live_held=" << live_held
		        << " durable_held=" << durable_held << " live_count=" << session->queued_user_prompts.size()
		        << " durable_count=" << (durable ? durable->acp_queued_prompts.size() : 0)
		        << " live_exact=" << live_exact << " durable_exact=" << durable_exact << std::endl;
		UAM_ASSERT_EQ(fixture.callbacks, 1);
		UAM_ASSERT(fixture.receipt.has_value());
		UAM_ASSERT(live_held && live_exact);
		UAM_ASSERT(durable_held && durable_exact);
		// Whole-prompt equality above includes id/revision, original payload,
		// attachments/skills and every captured agent/security policy field.
		UAM_ASSERT(!fixture.Chat().acp_queue_storage_error && !durable->acp_queue_storage_error);
		UAM_ASSERT_EQ(fixture.Chat().acp_dispatched_queued_prompt_count, std::size_t{0});
		UAM_ASSERT_EQ(durable->acp_dispatched_queued_prompt_count, std::size_t{0});
		UAM_ASSERT_EQ(ChatRepository::QueueStorageOwner(*durable), current_owner);
		UAM_ASSERT(current_owner != original_owner); // Persisted owner reflects this arm's real selection DATA drift.
		UAM_ASSERT_EQ(durable->execution_host_id, fixture.Chat().execution_host_id);
		UAM_ASSERT_EQ(durable->workspace_directory, fixture.Chat().workspace_directory);
		UAM_ASSERT_EQ(durable->workspace_worktree_directory, isolated_directory);
		UAM_ASSERT_EQ(durable->messages, old_rows);
		UAM_ASSERT_EQ(durable->native_session_id, native_before);
		UAM_ASSERT_EQ(fixture.Chat().native_session_id, native_before);
		UAM_ASSERT(!session->running && !session->processing && !session->initialized && !session->session_ready);
		UAM_ASSERT(!session->reconnect_pending && session->last_process_id.empty() && !session->stdin_writer);
		UAM_ASSERT(fixture.task->cancellation->stop_requested());
		const auto remaining = fixture.app.worktree_launch_preparations.find(fixture.Chat().id);
		UAM_ASSERT(remaining == fixture.app.worktree_launch_preparations.end() || remaining->second != fixture.task);
		for (int tick = 0; tick < 16; ++tick)
		{
			UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *session, fixture.Chat()));
			UAM_ASSERT_EQ(fixture.callbacks, 1);
			UAM_ASSERT(!session->running && !session->processing && !session->stdin_writer);
			UAM_ASSERT_EQ(session->queued_user_prompts, (std::deque<uam::AcpQueuedUserPromptState>{captured}));
			UAM_ASSERT_EQ(fixture.Chat().acp_queued_prompts, (std::vector<uam::AcpQueuedUserPromptState>{captured}));
		}
		UAM_ASSERT(fixture.Prompts().empty() && !fs::exists(fixture.wire));
		UAM_ASSERT_EQ(fixture.Chat().messages, old_rows);
		UAM_ASSERT_EQ(fixture.Chat().workspace_worktree_directory, isolated_directory);
		UAM_ASSERT(fs::is_directory(uam::paths::PathFromUtf8(isolated_directory)));
		UAM_ASSERT_EQ(ReadFile(fixture.repository / "source.txt"), std::string("Private source survivor\n"));
	}
}

UAM_TEST(AcpWorkspaceRemoteValidationStopPreservesUnsentPolicyOnce)
{
	UAM_ASSERT(GitAvailableForTests());
	DirectWorkspaceWorkerFixture fixture;
	// Genuine supported selection before Send: SnapshotSelectedUamAgent captures
	// the built-in definition; never inject a queue policy or recovery receipt.
	fixture.app.settings.provider_chat_defaults["gemini-cli"].feature_preference = "uam";
	fixture.Chat().uam_agent_id = "build";
	fixture.Start();
	const auto old_rows = fixture.Chat().messages;
	const auto isolated_directory = fixture.Chat().workspace_worktree_directory;
	const auto native_before = fixture.Chat().native_session_id;
	auto* session = uam::FindAcpSessionForChat(fixture.app, fixture.Chat().id);
	UAM_ASSERT(session && session->queued_user_prompts.size() == 1);
	const auto captured = session->queued_user_prompts.front();
	UAM_ASSERT(captured.append_user_message && captured.direct_admission);
	UAM_ASSERT(!captured.id.empty() && captured.revision >= 1);
	UAM_ASSERT(uam::prompt_recovery::HasCapturedPolicy(captured));
	UAM_ASSERT(captured.uam_agent_id == "build" && !captured.uam_agent_definition_snapshot.empty());
	UAM_ASSERT(!session->running && !session->initialized && !session->session_ready);
	(void)uam::StopAcpSession(fixture.app, fixture.Chat().id);
	fixture.Release();
	DirectWorkspaceWorkerFixture::Wait([&]()
	{
		fixture.Tick();
		return fixture.callbacks == 1 && !fixture.task->worker.valid() &&
		    fixture.app.pending_chat_save_tasks.empty() && fixture.app.pending_chat_save_at_by_chat_id.empty();
	});
	// This true callback acknowledges durable post-Stop storage, not delivery.
	UAM_ASSERT_EQ(fixture.callbacks, 1);
	UAM_ASSERT(fixture.receipt == std::optional<bool>{true});
	UAM_ASSERT(!fs::exists(fixture.wire) && fixture.Prompts().empty());
	UAM_ASSERT(!session->running && !session->initialized && !session->session_ready && !session->processing);
	UAM_ASSERT(!session->stdin_writer && session->pending_request_methods.empty());
	UAM_ASSERT(!session->pending_local_launch && !session->reconnect_pending);
	UAM_ASSERT(session->queued_user_prompts.empty() && fixture.Chat().acp_queued_prompts.empty());
	UAM_ASSERT_EQ(fixture.Chat().native_session_id, native_before);
	UAM_ASSERT_EQ(fixture.Chat().messages.size(), old_rows.size() + 1);
	UAM_ASSERT(std::equal(old_rows.begin(), old_rows.end(), fixture.Chat().messages.begin()));
	const auto& unsent = fixture.Chat().messages.back();
	UAM_ASSERT(unsent.role == MessageRole::User && unsent.content == captured.text);
	UAM_ASSERT(unsent.interrupted && unsent.acp_prompt_not_sent && !unsent.acp_prompt_discarded);
	UAM_ASSERT(!unsent.created_at.empty());
	UAM_ASSERT_EQ(unsent.attachments, captured.attachments);
	UAM_ASSERT_EQ(unsent.markdown_store_files, captured.markdown_store_files);
	UAM_ASSERT_EQ(unsent.markdown_store_prompt_blocks, captured.markdown_store_prompt_blocks);
	UAM_ASSERT_EQ(unsent.priority_steer, captured.priority_steer);
	UAM_ASSERT_EQ(std::ranges::count(fixture.Chat().messages, captured.text, &Message::content), 1);
	UAM_ASSERT_EQ(uam::KnownUnsentAcpMessageIndices(fixture.app, fixture.Chat()),
	    (std::vector<int>{static_cast<int>(old_rows.size())}));
	UAM_ASSERT_EQ(fixture.Chat().acp_unsent_prompt_recoveries.size(), std::size_t{1});
	const auto& recovery = fixture.Chat().acp_unsent_prompt_recoveries.front();
	UAM_ASSERT(uam::prompt_recovery::BindingMatches(fixture.Chat(), recovery));
	UAM_ASSERT_EQ(recovery.admission_id, captured.id);
	UAM_ASSERT_EQ(recovery.admission_revision, captured.revision);
	UAM_ASSERT_EQ(recovery.user_message_index, static_cast<int>(old_rows.size()));
	UAM_ASSERT_EQ(recovery.user_message_created_at, unsent.created_at);
	UAM_ASSERT_EQ(recovery.uam_agent_id, captured.uam_agent_id);
	UAM_ASSERT_EQ(recovery.uam_agent_definition_hash, captured.uam_agent_definition_hash);
	UAM_ASSERT_EQ(recovery.uam_agent_definition_snapshot, captured.uam_agent_definition_snapshot);
	UAM_ASSERT_EQ(recovery.uam_agent_instructions, captured.uam_agent_instructions);
	UAM_ASSERT_EQ(recovery.uam_agent_skills, captured.uam_agent_skills);
	UAM_ASSERT_EQ(recovery.uam_agent_delegates, captured.uam_agent_delegates);
	UAM_ASSERT_EQ(recovery.uam_agent_workspace_access, captured.uam_agent_workspace_access);
	UAM_ASSERT_EQ(recovery.uam_agent_execution_capability, captured.uam_agent_execution_capability);
	const auto durable = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(durable && durable->messages == fixture.Chat().messages && durable->acp_queued_prompts.empty());
	UAM_ASSERT_EQ(durable->acp_unsent_prompt_recoveries, fixture.Chat().acp_unsent_prompt_recoveries);
	UAM_ASSERT(uam::prompt_recovery::BindingMatches(*durable, durable->acp_unsent_prompt_recoveries.front()));
	const auto raw = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.Chat().id)));
	UAM_ASSERT_EQ(raw.at("acp_unsent_prompt_recoveries").size(), std::size_t{1});
	UAM_ASSERT_EQ(raw.at("acp_unsent_prompt_recoveries").at(0).at("admission_id").get<std::string>(), captured.id);
	UAM_ASSERT_EQ(fixture.Chat().workspace_worktree_directory, isolated_directory);
	UAM_ASSERT(fs::is_directory(uam::paths::PathFromUtf8(isolated_directory)));
	UAM_ASSERT_EQ(ReadFile(fixture.repository / "source.txt"), std::string("Private source survivor\n"));
}
#endif

UAM_TEST(AcpHeldWorkspaceWaitSettlesFinishedPreparation)
{
	// A held queue must not leave a finished preparation parked on "waiting for workspace" with no error (beafe2).
	for (const bool failed : {true, false})
	{
		WorkspacePromptFixture fixture;
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = fixture.app.chats.front();
		uam::AcpQueuedUserPromptState prompt;
		prompt.id = "private-held-finished-workspace";
		prompt.text = "Held input across preparation";
		session.queued_user_prompts.push_back(prompt);
		chat.acp_queued_prompts = {prompt};
		session.queue_held = chat.acp_queue_held = true;
		session.worktree_launch_pending = true;
		session.worktree_launch_cancellation = task->cancellation;
		session.lifecycle_state = "waiting_workspace";
		if (failed) task->error = "Private preparation failure";
		else task->saved = true;
		for (int poll = 0; poll < 3; ++poll) (void)uam::PollAllAcpSessions(fixture.app);
		UAM_ASSERT(!session.worktree_launch_pending && !session.running && !session.reconnect_pending);
		UAM_ASSERT(session.queue_held && chat.acp_queue_held);
		UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(session.lifecycle_state, std::string(failed ? "error" : "stopped"));
		UAM_ASSERT_EQ(session.last_error.empty(), !failed);
	}
}

#if UAM_ENABLE_SSH
namespace
{
	ExecutionHost ReadyPrivateRemoteHost()
	{
		ExecutionHost host;
		host.id = "private-remote-preparation";
		host.transport = "ssh";
		host.ssh_alias = "private-preparation";
		host.platform = "windows";
		host.runner_version = std::string(uam::constants::kAppVersion).substr(1);
		host.runner_protocol_version = 3;
		host.runner_status = "ready";
		UAM_ASSERT(uam::remote::CanAttemptRunnerConnection(host));
		return host;
	}

	std::shared_ptr<uam::WorktreeLaunchPreparationTask> SeedRemoteWorkspaceTask(WorkspacePromptFixture& fixture)
	{
		const ExecutionHost host = ReadyPrivateRemoteHost();
		fixture.app.settings.execution_hosts.push_back(host);
		fixture.app.chats.front().execution_host_id = host.id;
		fixture.app.chats.front().workspace_directory = "C:/Users/private/Source";
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
		task->remote_host = host;
		return task;
	}
}

UAM_TEST(RemoteWorkspacePreparationDeadlineCancelsUnansweredWorker)
{
	WorkspacePromptFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedRemoteWorkspaceTask(fixture);
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = fixture.app.chats.front();
	std::promise<uam::PreparedLaunchWorktree> worker;
	task->worker = worker.get_future();
	fixture.app.worktree_operation_chat_ids.insert(chat.id);
	uam::AcpQueuedUserPromptState prompt;
	prompt.id = "private-held-remote-input";
	prompt.text = "Held input across an unanswered helper";
	session.queued_user_prompts.push_back(prompt);
	chat.acp_queued_prompts = {prompt};
	session.queue_held = chat.acp_queue_held = true;
	session.worktree_launch_pending = true;
	session.worktree_launch_cancellation = task->cancellation;
	session.lifecycle_state = "waiting_workspace";
	task->worker_deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(!task->cancellation->stop_requested());
	// An unanswered helper (the host rebooted) is canceled at the deadline and reported, never awaited forever.
	task->worker_deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
	UAM_ASSERT(uam::PollWorktreeLaunchPreparations(fixture.app));
	UAM_ASSERT(task->cancellation->stop_requested());
	// The held session reports at once, while the unanswered worker is still running.
	(void)uam::PollAllAcpSessions(fixture.app);
	UAM_ASSERT_EQ(session.lifecycle_state, std::string("error"));
	UAM_ASSERT(session.last_error.find("timed out") != std::string::npos);
	UAM_ASSERT(session.queue_held && !session.worktree_launch_pending && !session.running);
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	// The worker keeps its operation fence until it really exits; then everything is released.
	UAM_ASSERT(fixture.app.worktree_operation_chat_ids.contains(chat.id));
	worker.set_value({{.message = "Remote command canceled."}, task->original});
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(!fixture.app.worktree_operation_chat_ids.contains(chat.id));
	UAM_ASSERT(!fixture.app.worktree_launch_preparations.contains(chat.id));
	UAM_ASSERT(!task->lease && !task->remote_host_lease);
}

UAM_TEST(RemoteWorkspaceReferenceAdmissionSkipsOnlySettledFamilyChildren)
{
	enum class Blocker { None, ActiveRun, QueuedInput, Unrelated, ForeignRoot, MissingRun, PersistedOnly, SavedDelivery, SavedInteraction };
	for (const Blocker blocker : {Blocker::None, Blocker::ActiveRun, Blocker::QueuedInput, Blocker::Unrelated, Blocker::ForeignRoot, Blocker::MissingRun,
	         Blocker::PersistedOnly, Blocker::SavedDelivery, Blocker::SavedInteraction})
	{
		WorkspacePromptFixture fixture;
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedRemoteWorkspaceTask(fixture);
		uam::AppState& app = fixture.app;
		const ChatSession parent = app.chats.front();
		uam::PreparedLaunchWorktree prepared{{.ok = true}, task->original};
		prepared.metadata.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
		prepared.metadata.workspace_source_directory = "C:/Users/private/Source";
		prepared.metadata.workspace_worktree_directory = "C:/Users/private/uam-worktree-0123456789abcdef";
		prepared.metadata.workspace_branch_name = "uam/0123456789abcdef";
		prepared.metadata.workspace_base_ref = std::string(40, 'a');
		const auto child = [&](const std::string& id, const std::string& run_id)
		{
			ChatSession value;
			value.id = id;
			value.provider_id = "claude-cli";
			value.execution_host_id = parent.execution_host_id;
			value.agent_run_id = run_id;
			value.workspace_isolation_kind = uam::paths::kPendingGitWorktreeIsolationKind;
			// The saved child names the parent's worktree, spelled as Windows wrote it.
			value.workspace_directory = "C:/Users/private\\uam-worktree-0123456789abcdef";
			value.workspace_source_directory = "C:/Users/private/Source";
			return value;
		};
		const auto run = [&](const std::string& id, const std::string& transcript, const std::string& status, const std::string& root)
		{
			AgentRun value;
			value.id = id;
			value.root_chat_id = root;
			value.transcript_chat_id = transcript;
			value.provider_id = "claude-cli";
			value.status = status;
			return value;
		};
		// A finished v2 child is inert after restart: only the worker's read-only ledger observation still names it.
		app.chats.push_back(child("private-settled-v2", "run-settled-v2"));
		prepared.reference_runs.push_back(run("run-settled-v2", "private-settled-v2", "failed", parent.id));
		app.chats.push_back(child("private-settled-v1", "run-settled-v1"));
		app.agent_runs.push_back(run("run-settled-v1", "private-settled-v1", "completed", parent.id));
		prepared.references.push_back(child("private-settled-saved", "run-settled-saved"));
		prepared.reference_runs.push_back(run("run-settled-saved", "private-settled-saved", "cancelled", parent.id));
		std::string expected;
		switch (blocker)
		{
		case Blocker::None: break;
		case Blocker::ActiveRun:
			app.chats.push_back(child(expected = "private-active-child", "run-active"));
			app.agent_runs.push_back(run("run-active", expected, "running", parent.id));
			// A stale finished observation never overrides the live ledger.
			prepared.reference_runs.push_back(run("run-active", expected, "completed", parent.id));
			break;
		case Blocker::QueuedInput:
			app.chats.push_back(child(expected = "private-queued-child", "run-queued"));
			app.chats.back().acp_queued_prompts.emplace_back().id = "private-queued-input";
			app.agent_runs.push_back(run("run-queued", expected, "completed", parent.id));
			break;
		case Blocker::Unrelated:
			app.chats.push_back(child(expected = "private-unrelated-chat", ""));
			break;
		case Blocker::ForeignRoot:
			app.chats.push_back(child(expected = "private-foreign-child", "run-foreign"));
			prepared.reference_runs.push_back(run("run-foreign", expected, "failed", "private-other-parent"));
			break;
		case Blocker::MissingRun:
			app.chats.push_back(child(expected = "private-unknown-child", "run-pruned"));
			break;
		case Blocker::PersistedOnly:
			prepared.references.push_back(child(expected = "private-persisted-chat", ""));
			break;
		case Blocker::SavedDelivery:
		case Blocker::SavedInteraction:
			// A saved-only finished child still holding delivery or interaction authority is not settled.
			prepared.references.push_back(child(expected = "private-saved-authority-child", "run-saved-authority"));
			if (blocker == Blocker::SavedDelivery) prepared.references.back().remote_prompt_delivery_id = "private-delivery";
			else prepared.references.back().remote_interaction_responses.emplace_back();
			prepared.reference_runs.push_back(run("run-saved-authority", expected, "failed", parent.id));
			break;
		}
		task->prepared = std::move(prepared);
		(void)uam::PollWorktreeLaunchPreparations(app);
		if (blocker == Blocker::None)
		{
			UAM_ASSERT(task->error.empty());
			UAM_ASSERT(task->merged);
			UAM_ASSERT_EQ(app.chats.front().workspace_worktree_directory, std::string("C:/Users/private/uam-worktree-0123456789abcdef"));
			continue;
		}
		UAM_ASSERT(!task->merged);
		UAM_ASSERT(task->error.find(expected) != std::string::npos);
		UAM_ASSERT(task->error.find("private-settled") == std::string::npos);
		UAM_ASSERT(app.chats.front().workspace_worktree_directory.empty());
	}
}
UAM_TEST(RemoteWorkspaceReferenceAdmissionSkipsExpiredRunChildBySavedRootLink)
{
	// Issue #998: a finished child whose run record expired is recognised only by the root link saved at its birth.
	enum class Case { Linked, LinkedSavedOnly, Legacy, OtherBirth, OtherRoot, UnreadableLedger, QueuedInput, RecordNamesChat };
	for (const Case kind : {Case::Linked, Case::LinkedSavedOnly, Case::Legacy, Case::OtherBirth, Case::OtherRoot, Case::UnreadableLedger, Case::QueuedInput, Case::RecordNamesChat})
	{
		WorkspacePromptFixture fixture;
		// The birth the link names must exist before the task captures the parent's identity.
		fixture.app.chats.front().created_at = "2026-01-02 03:04:05";
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedRemoteWorkspaceTask(fixture);
		uam::AppState& app = fixture.app;
		UAM_ASSERT_EQ(task->original.created_at, app.chats.front().created_at);
		const ChatSession parent = app.chats.front();
		uam::PreparedLaunchWorktree prepared{{.ok = true}, task->original};
		prepared.metadata.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
		prepared.metadata.workspace_source_directory = "C:/Users/private/Source";
		prepared.metadata.workspace_worktree_directory = "C:/Users/private/uam-worktree-0123456789abcdef";
		prepared.metadata.workspace_branch_name = "uam/0123456789abcdef";
		prepared.metadata.workspace_base_ref = std::string(40, 'a');
		prepared.reference_runs_complete = kind != Case::UnreadableLedger;
		ChatSession child;
		child.id = "private-expired-child";
		child.provider_id = "claude-cli";
		child.execution_host_id = parent.execution_host_id;
		child.agent_run_id = "run-expired";
		child.workspace_isolation_kind = uam::paths::kPendingGitWorktreeIsolationKind;
		child.workspace_directory = "C:/Users/private\\uam-worktree-0123456789abcdef";
		child.workspace_source_directory = "C:/Users/private/Source";
		if (kind != Case::Legacy)
		{
			child.managed_root_chat_id = kind == Case::OtherRoot ? "private-other-root" : parent.id;
			child.managed_root_created_at = kind == Case::OtherBirth ? "2026-01-02 03:04:06" : parent.created_at;
		}
		if (kind == Case::QueuedInput) child.acp_queued_prompts.emplace_back().id = "private-queued-input";
		if (kind == Case::RecordNamesChat)
		{
			AgentRun other;
			other.id = "run-other";
			other.root_chat_id = parent.id;
			other.transcript_chat_id = child.id;
			other.status = "running";
			prepared.reference_runs.push_back(other);
		}
		// The link survives the saved round trip that remote admission reads.
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, child));
		const auto saved = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, child.id);
		UAM_ASSERT(saved && saved->managed_root_chat_id == child.managed_root_chat_id && saved->managed_root_created_at == child.managed_root_created_at);
		UAM_ASSERT(saved->subagent_parent_chat_id.empty() && saved->subagent_parent_created_at.empty());
		if (kind == Case::LinkedSavedOnly) prepared.references.push_back(*saved);
		else app.chats.push_back(child);
		task->prepared = std::move(prepared);
		(void)uam::PollWorktreeLaunchPreparations(app);
		if (kind == Case::Linked || kind == Case::LinkedSavedOnly)
		{
			UAM_ASSERT(task->error.empty());
			UAM_ASSERT(task->merged);
			UAM_ASSERT_EQ(app.chats.front().workspace_worktree_directory, std::string("C:/Users/private/uam-worktree-0123456789abcdef"));
			continue;
		}
		UAM_ASSERT(!task->merged);
		UAM_ASSERT(task->error.find("private-expired-child") != std::string::npos);
		UAM_ASSERT(app.chats.front().workspace_worktree_directory.empty());
	}
}
#endif
