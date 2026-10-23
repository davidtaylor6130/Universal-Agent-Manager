#include "test_harness.h"
#include "app/worktree_launch_preparation_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/paths/app_paths.h"

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
	}
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
