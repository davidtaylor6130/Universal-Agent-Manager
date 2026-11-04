#include "test_harness.h"
#include "app/worktree_launch_preparation_service.h"
#include "app/vcs_commit_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "remote/runner_protocol.h"
#include "remote/runner_client.h"
#include "remote/runner_state.h"

#include <iostream>
#include <fstream>

using namespace uam_test;

std::optional<int> RunRemoteWorktreeReceiptFixture(int argc, char** argv)
{
	if (argc != 2 || std::string_view(argv[1]) != "--uam-test-remote-worktree-receipt") return std::nullopt;
#if !defined(_WIN32) && UAM_ENABLE_SSH
	uam::remote::RunnerState state;
	const std::string outcome = uam::env::GetNonEmptyString("UAM_PRIVATE_WORKTREE_OUTCOME").value_or("");
	const std::string operation = uam::env::GetNonEmptyString("UAM_PRIVATE_WORKTREE_OPERATION").value_or("");
	const std::string token = uam::env::GetNonEmptyString("UAM_PRIVATE_WORKTREE_TOKEN").value_or("");
	const std::string target = uam::env::GetNonEmptyString("UAM_PRIVATE_WORKTREE_TARGET").value_or("");
	int removal_attempts = 0;
	nlohmann::json request;
	while (uam::remote::ReadFrame(std::cin, request) == uam::remote::FrameReadResult::Ok)
	{
		const std::string type = request.value("type", "");
		nlohmann::json response;
		if (type == "process.remove" && request.value("sessionId", "").starts_with("worktree-create-") && outcome.starts_with("cleanup-"))
		{
			++removal_attempts;
			if (const std::optional<std::string> log = uam::env::GetNonEmptyString("UAM_PRIVATE_WORKTREE_CLEANUP_LOG"))
			{ std::ofstream output(*log, std::ios::app); output << "remove\n"; }
			if (outcome == "cleanup-error" || (outcome == "cleanup-retry" && removal_attempts == 1))
				response = {{"id", request["id"]}, {"ok", false}, {"error", {{"code", "fixture_cleanup_failure"}, {"message", "Private settled cleanup failure"}}}};
			else
			{
				response = uam::remote::HandleRunnerRequest(request, std::string(uam::constants::kAppVersion).substr(1), &state);
				if (outcome == "cleanup-missing")
					response = {{"id", request["id"]}, {"ok", false}, {"error", {{"code", "session_not_found"}, {"message", "Private settled process is absent"}}}};
			}
		}
		else if (type == "process.start" && outcome == "start-reject")
			response = {{"id", request["id"]}, {"ok", false}, {"error", {{"code", "private_start_rejected"}, {"message", "Private start authority rejected"}}}};
		else if (!operation.empty() && request.value("sessionId", "") == operation)
		{
			if (request.value("controlToken", "") != token) return 3;
			if (type == "process.poll" && outcome == "missing")
				response = {{"id", request["id"]}, {"ok", false}, {"error", {{"code", "session_not_found"}, {"message", "Private operation is absent"}}}};
			else if (type == "process.poll")
				response = {{"id", request["id"]}, {"ok", true}, {"result", {{"running", outcome == "running"}, {"exitCode", outcome == "nonzero" ? 7 : 0}, {"stdoutBase64", ""}, {"stderrBase64", ""}}}};
			else if (type == "process.remove") response = {{"id", request["id"]}, {"ok", true}, {"result", nlohmann::json::object()}};
			else return 4;
		}
		else if (type == "path.inspect" && outcome == "inspection-error" && request.value("path", "") == target)
			response = {{"id", request["id"]}, {"ok", false}, {"error", {{"code", "connection_lost"}, {"message", "Private worktree inspection transport failure"}}}};
		else response = uam::remote::HandleRunnerRequest(request, std::string(uam::constants::kAppVersion).substr(1), &state);
		if (type == "hello" && outcome == "no-acks") response["capabilities"]["processOutputAcknowledgement"] = false;
		if (!uam::remote::WriteFrame(std::cout, response)) return 5;
	}
	return 0;
#else
	return std::nullopt;
#endif
}

#if !defined(_WIN32) && UAM_ENABLE_SSH
namespace
{
	struct PrivateWorktreeResidentRunner
	{
		TempDir temp{"uwr"};
		uam::platform::StdioProcessPlatformFields process;
		ScopedEnvVar socket_env{"UAM_PRIVATE_WORKTREE_SOCKET", (temp.root / "s").string()};

		explicit PrivateWorktreeResidentRunner(const fs::path& fake_ssh)
		{
			IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
			const fs::path runner = service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
			const fs::path socket = temp.root / "s";
			UAM_ASSERT(socket.string().size() < 104);
			std::string error;
			UAM_ASSERT(service.StartStdioProcess(process, temp.root, {runner.string(), "serve", "--socket", socket.string()}, &error));
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!fs::exists(socket) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
			UAM_ASSERT(fs::exists(socket));
			UAM_ASSERT(uam::io::WriteTextFile(fake_ssh, "#!/bin/sh\nexec \"$UAM_PRIVATE_WORKTREE_RUNNER\" bridge --socket \"$UAM_PRIVATE_WORKTREE_SOCKET\"\n"));
			fs::permissions(fake_ssh, fs::perms::owner_all);
		}

		~PrivateWorktreeResidentRunner()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(process, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(process);
		}
	};

	struct RemoteWorktreeFixture
	{
		TempDir temp{"uam-remote-worktree"};
		uam::AppState app;
		fs::path source;
		std::unique_ptr<ScopedEnvVar> path_env, runner_env;

		RemoteWorktreeFixture()
		{
			source = fs::canonical(temp.root) / "source";
			fs::create_directories(source);
			UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(source.string())));
			UAM_ASSERT(RunGitForTest(source, "config user.email private@example.test"));
			UAM_ASSERT(RunGitForTest(source, "config user.name Private"));
			UAM_ASSERT(uam::io::WriteTextFile(source / "source.txt", "committed source\n"));
			UAM_ASSERT(RunGitForTest(source, "add source.txt"));
			UAM_ASSERT(RunGitForTest(source, "commit -m private-base"));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "ssh", "#!/bin/sh\nexec \"$UAM_PRIVATE_WORKTREE_RUNNER\" bridge-direct\n"));
			fs::permissions(temp.root / "ssh", fs::perms::owner_all);
			const fs::path runner = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
			UAM_ASSERT(fs::exists(runner));
			path_env = std::make_unique<ScopedEnvVar>("PATH", temp.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin"));
			runner_env = std::make_unique<ScopedEnvVar>("UAM_PRIVATE_WORKTREE_RUNNER", runner.string());
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ExecutionHost host;
			host.id = "private-ssh-worktree";
			host.transport = "ssh";
			host.ssh_alias = "private-worktree-fixture";
			host.platform = "linux";
			host.runner_version = std::string(uam::constants::kAppVersion).substr(1);
			host.runner_protocol_version = 3;
			host.runner_status = "ready";
			app.settings.execution_hosts.push_back(host);
			ChatSession chat = ChatDomainService().CreateNewChat("", "gemini-cli");
			chat.id = "private-remote-worktree-chat";
			chat.execution_host_id = host.id;
			chat.workspace_directory = source.string();
			chat.messages.push_back({.role = MessageRole::User, .content = "Preserve private history", .created_at = "2026-10-15T00:00:00Z"});
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
		}

		~RemoteWorktreeFixture()
		{
			for (const std::pair<const std::string, std::shared_ptr<uam::WorktreeLaunchPreparationTask>>& entry : app.worktree_launch_preparations)
			{
				entry.second->cancellation->request_stop();
				if (entry.second->worker.valid()) entry.second->worker.wait();
			}
		}

		std::shared_ptr<uam::WorktreeLaunchPreparationTask> Start(bool explicit_create = false)
		{
			const uam::WorktreeLaunchPreparationResult result = uam::PrepareWorktreeForLaunch(app, app.chats.front(), explicit_create);
			UAM_ASSERT_EQ(result.state, uam::WorktreeLaunchPreparationState::Pending);
			UAM_ASSERT(result.cancellation);
			return app.worktree_launch_preparations.at(app.chats.front().id);
		}

		void Worker(const std::shared_ptr<uam::WorktreeLaunchPreparationTask>& task)
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (task->worker.valid() && task->worker.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
		}

		bool Settle(const std::shared_ptr<uam::WorktreeLaunchPreparationTask>& task)
		{
			Worker(task);
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!task->saved.has_value() && task->error.empty() && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::PollWorktreeLaunchPreparations(app);
				(void)uam::FlushPendingChatSaves(app);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			UAM_ASSERT(task->saved.has_value() || !task->error.empty());
			return task->saved.value_or(false);
		}

		void UseReceiptRunner()
		{
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "ssh",
			    "#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"$UAM_PRIVATE_WORKTREE_BRIDGE_LOG\"\nexec \"$UAM_PRIVATE_WORKTREE_FIXTURE\" --uam-test-remote-worktree-receipt\n"));
			fs::permissions(temp.root / "ssh", fs::perms::owner_all);
		}
	};
}
#endif

UAM_TEST(RemoteWorktreeAutomaticPreparationRequiresOwnedSavedIsolation)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "source.txt", "dirty source survivor\n"));
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
	UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
	fixture.Worker(task);
	fixture.app.chats.front().messages.push_back({.role = MessageRole::User, .content = "Newer live history"});
	UAM_ASSERT(fixture.Settle(task));
	const ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(chat));
	UAM_ASSERT(chat.workspace_worktree_directory != chat.workspace_directory);
	UAM_ASSERT_EQ(ReadFile(fs::path(chat.workspace_worktree_directory) / "source.txt"), std::string("committed source\n"));
	UAM_ASSERT_EQ(ReadFile(fixture.source / "source.txt"), std::string("dirty source survivor\n"));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->workspace_worktree_directory == chat.workspace_worktree_directory && saved->messages.size() == 2);
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Ready);
		UAM_ASSERT(!uam::IsWorktreeLaunchPreparationPending(fixture.app, chat.id));
		UAM_ASSERT(fixture.app.worktree_operation_chat_ids.empty());
		UAM_ASSERT(fixture.app.worktree_launch_preparations.at(chat.id) == task);
	}
	const uam::GitWorktreeStatus status = uam::GitWorktreeService().Status(fixture.app, chat);
	UAM_ASSERT(status.error.empty() && status.is_git_repository && status.isolated && !status.worktree_missing);
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
#endif
}

UAM_TEST(RemoteWorktreeFailedMessageRetryRetainsRawIntentThroughPendingCancellation)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.messages.front().interrupted = true;
	chat.messages.front().acp_prompt_not_sent = true;
	chat.approval_mode.clear();
	chat.memory_enabled = false;
	fixture.app.settings.central_provider_configuration.enabled = false;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	std::string error;
	UAM_ASSERT(uam::RetryFailedAcpMessage(fixture.app, chat.id, 0, &error));
	UAM_ASSERT(error.empty());
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(fixture.app, chat.id);
	UAM_ASSERT(session && session->queued_user_prompts.size() == 1);
	const uam::AcpQueuedUserPromptState original = session->queued_user_prompts.front();
	UAM_ASSERT_EQ(original.text, chat.messages.front().content);
	UAM_ASSERT_EQ(original.existing_user_message_index, 0);
	UAM_ASSERT_EQ(original.existing_user_message_created_at, chat.messages.front().created_at);
	std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->acp_queued_prompts.size() == 1 && saved->acp_queued_prompts.front() == original);
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *session, chat));
	UAM_ASSERT(session->worktree_launch_pending && !session->running && session->prompt_request_id == 0);
	UAM_ASSERT(uam::IsWorktreeLaunchPreparationPending(fixture.app, chat.id));
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.Worker(task);
	uam::CancelWorktreeLaunchPreparation(fixture.app, chat.id);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	(void)uam::FlushPendingChatSaves(fixture.app);
	UAM_ASSERT_EQ(session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT(session->queued_user_prompts.front() == original);
	UAM_ASSERT(chat.workspace_worktree_directory.empty() && fixture.app.cli_terminals.empty());
	UAM_ASSERT(!session->running && !session->processing && session->prompt_request_id == 0);
	saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->acp_queued_prompts.size() == 1 && saved->acp_queued_prompts.front() == original);
	UAM_ASSERT_EQ(saved->acp_dispatched_queued_prompt_count, std::size_t{0});
#endif
}

UAM_TEST(RemoteWorktreeExplicitCreationWorksWithoutAutomaticWorktrees)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	fixture.app.settings.git_auto_worktree = false;
	fixture.app.chats.front().workspace_isolation_kind.clear();
	const uam::GitWorktreeStatus before = uam::GitWorktreeService().Status(fixture.app, fixture.app.chats.front());
	UAM_ASSERT(before.error.empty() && before.is_git_repository && !before.isolated);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start(true);
	UAM_ASSERT(fixture.Settle(task));
	UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(fixture.app.chats.front()));
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front(), true).state, uam::WorktreeLaunchPreparationState::Ready);
#endif
}

UAM_TEST(RemoteWorktreeRejectsLinkedSourceWithoutArtifacts)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	const fs::path linked = fs::canonical(fixture.temp.root) / "linked-source";
	fs::create_directory_symlink(fixture.source, linked);
	fixture.app.chats.front().workspace_directory = linked.string();
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
	UAM_ASSERT(!fixture.Settle(task));
	UAM_ASSERT(!task->error.empty());
	UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
	UAM_ASSERT(!fs::exists(task->receipt_file));
	UAM_ASSERT_EQ(ReadFile(fixture.source / "source.txt"), std::string("committed source\n"));
#endif
}

UAM_TEST(RemoteWorktreeCachedReadyRevokesChangedInheritedFolder)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	fixture.app.folders.push_back({"private-inherited-folder", "Private", fixture.source.string(), false, chat.execution_host_id});
	chat.folder_id = fixture.app.folders.back().id;
	chat.workspace_directory.clear();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	const uam::VcsCommitStatus vcs = uam::VcsCommitService().Status(fixture.app, chat);
	UAM_ASSERT(vcs.available);
	UAM_ASSERT_EQ(vcs.workspace_directory, fixture.source.string());
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
	UAM_ASSERT(fixture.Settle(task));
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	const std::string original_worktree = chat.workspace_worktree_directory;
	fixture.app.folders.back().directory = (fixture.source.parent_path() / "different-source").string();
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->cancellation->stop_requested());
	UAM_ASSERT(!fixture.app.worktree_launch_preparations.contains(chat.id));
	UAM_ASSERT_EQ(chat.workspace_worktree_directory, original_worktree);
	UAM_ASSERT(fs::exists(fs::path(original_worktree) / "source.txt"));
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
#endif
}

UAM_TEST(RemoteWorktreeAutomaticPlainDirectorySkipsInapplicablePull)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	for (const bool pull : {false, true})
	{
		RemoteWorktreeFixture fixture;
		const fs::path plain = fs::canonical(fixture.temp.root) / "plain-source";
		fs::create_directory(plain);
		UAM_ASSERT(uam::io::WriteTextFile(plain / "sentinel.txt", "Plain source must remain unchanged.\n"));
		fixture.app.settings.git_pull_before_chat = pull;
		fixture.app.chats.front().workspace_directory = plain.string();
		fixture.app.chats.front().native_session_id = "private-existing-native";
		const std::vector<Message> messages = fixture.app.chats.front().messages;
		ChatSession neighbor = ChatDomainService().CreateNewChat("", "gemini-cli");
		neighbor.id = "unrelated-absolute-remote-chat";
		neighbor.execution_host_id = fixture.app.chats.front().execution_host_id;
		neighbor.workspace_directory = fixture.source.string();
		fixture.app.chats.push_back(neighbor);
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.front()));
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.back()));
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
		UAM_ASSERT(fixture.Settle(task));
		const ChatSession& chat = fixture.app.chats.front();
		UAM_ASSERT(chat.workspace_isolation_kind.empty() && chat.workspace_worktree_directory.empty());
		UAM_ASSERT(chat.workspace_source_directory.empty() && chat.workspace_branch_name.empty() && chat.workspace_base_ref.empty());
		UAM_ASSERT_EQ(chat.workspace_directory, plain.string());
		UAM_ASSERT_EQ(chat.native_session_id, std::string("private-existing-native"));
		UAM_ASSERT_EQ(chat.messages, messages);
		UAM_ASSERT(!fs::exists(task->receipt_file) && !fs::exists(plain / ".git"));
		UAM_ASSERT_EQ(ReadFile(plain / "sentinel.txt"), std::string("Plain source must remain unchanged.\n"));
		UAM_ASSERT_EQ(std::distance(fs::directory_iterator(plain), fs::directory_iterator{}), 1);
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
		UAM_ASSERT(saved && saved->workspace_isolation_kind.empty() && saved->workspace_worktree_directory.empty());
		UAM_ASSERT_EQ(saved->native_session_id, chat.native_session_id);
		UAM_ASSERT_EQ(saved->messages, messages);
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Ready);
		UAM_ASSERT(!uam::IsWorktreeLaunchPreparationPending(fixture.app, chat.id));
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
	}
#endif
}

UAM_TEST(RemoteWorktreePlainExplicitOverrideAndCorruptGitStillFailClosed)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	for (const std::string scenario : {"explicit", "base-override", "corrupt-git", "corrupt-ancestor-git"})
	{
		RemoteWorktreeFixture fixture;
		const fs::path plain = fs::canonical(fixture.temp.root) / "plain-source";
		fs::create_directory(plain);
		UAM_ASSERT(uam::io::WriteTextFile(plain / "sentinel.txt", "Untouched plain source."));
		fixture.app.chats.front().workspace_directory = plain.string();
		fixture.app.settings.git_pull_before_chat = true;
		if (scenario == "base-override") fixture.app.chats.front().git_base_branch_override = "main";
		const fs::path marker = scenario == "corrupt-ancestor-git" ? fs::canonical(fixture.temp.root) / ".git" : plain / ".git";
		if (scenario == "corrupt-git" || scenario == "corrupt-ancestor-git")
			UAM_ASSERT(uam::io::WriteTextFile(marker, "gitdir: missing-private-git-metadata\n"));
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start(scenario == "explicit");
		UAM_ASSERT(!fixture.Settle(task));
		UAM_ASSERT(!task->error.empty());
		UAM_ASSERT_EQ(fixture.app.chats.front().workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
		UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
		UAM_ASSERT(!fs::exists(task->receipt_file));
		UAM_ASSERT_EQ(ReadFile(plain / "sentinel.txt"), std::string("Untouched plain source."));
		if (scenario == "corrupt-git" || scenario == "corrupt-ancestor-git")
			UAM_ASSERT_EQ(ReadFile(marker), std::string("gitdir: missing-private-git-metadata\n"));
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
	}
#endif
}

UAM_TEST(RemoteWorktreeOptionalPullUsesCurrentHeadWithoutFetching)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	const fs::path upstream = fs::canonical(fixture.temp.root) / "private-upstream.git";
	UAM_ASSERT(RunTestCommand("git clone --bare " + ShellQuoteForTest(fixture.source.string()) + " " + ShellQuoteForTest(upstream.string())));
	UAM_ASSERT(RunGitForTest(fixture.source, "remote add origin " + ShellQuoteForTest(upstream.string())));
	IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
	const ProcessExecutionResult head = process.ExecuteCommand("git -C " + ShellQuoteForTest(fixture.source.string()) + " rev-parse HEAD", 10000);
	const ProcessExecutionResult branch = process.ExecuteCommand("git -C " + ShellQuoteForTest(fixture.source.string()) + " symbolic-ref --short HEAD", 10000);
	UAM_ASSERT(head.ok && head.exit_code == 0 && branch.ok && branch.exit_code == 0);
	const std::string branch_name = uam::strings::Trim(branch.output);
	UAM_ASSERT(RunGitForTest(fixture.source, "config " + ShellQuoteForTest("branch." + branch_name + ".remote") + " origin"));
	UAM_ASSERT(RunGitForTest(fixture.source, "config " + ShellQuoteForTest("branch." + branch_name + ".merge") + " " + ShellQuoteForTest("refs/heads/" + branch_name)));
	const std::string source_config = ReadFile(fixture.source / ".git" / "config");
	const std::string upstream_head = ReadFile(upstream / "HEAD");
	UAM_ASSERT(!fs::exists(fixture.source / ".git" / "FETCH_HEAD"));
	fixture.app.settings.git_pull_before_chat = true;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
	UAM_ASSERT(fixture.Settle(task));
	const ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(chat));
	UAM_ASSERT_EQ(chat.workspace_base_ref, uam::strings::Trim(head.output));
	UAM_ASSERT_EQ(ReadFile(fs::path(chat.workspace_worktree_directory) / "source.txt"), std::string("committed source\n"));
	UAM_ASSERT(task->prepared.has_value());
	const std::string warning = uam::strings::TrimAndLowerAscii(task->prepared->operation.status.warning);
	UAM_ASSERT(warning.find("pull") != std::string::npos && warning.find("skip") != std::string::npos);
	UAM_ASSERT(!fs::exists(fixture.source / ".git" / "FETCH_HEAD"));
	UAM_ASSERT_EQ(ReadFile(fixture.source / ".git" / "config"), source_config);
	UAM_ASSERT_EQ(ReadFile(upstream / "HEAD"), upstream_head);
	const ProcessExecutionResult after = process.ExecuteCommand("git -C " + ShellQuoteForTest(fixture.source.string()) + " rev-parse HEAD", 10000);
	UAM_ASSERT(after.ok && after.exit_code == 0);
	UAM_ASSERT_EQ(after.output, head.output);
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
#endif
}

UAM_TEST(RemoteWorktreePlainCancellationAndSaveRetryPreservePendingMetadata)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	for (const bool canceled : {true, false})
	{
		RemoteWorktreeFixture fixture;
		const fs::path plain = fs::canonical(fixture.temp.root) / "plain-source";
		fs::create_directory(plain);
		UAM_ASSERT(uam::io::WriteTextFile(plain / "sentinel.txt", "Untouched plain source."));
		fixture.app.chats.front().workspace_directory = plain.string();
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.front()));
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
		fixture.Worker(task);
		const fs::path preserved = fixture.temp.root / "preserved-data";
		if (canceled)
		{
			uam::CancelWorktreeLaunchPreparation(fixture.app, fixture.app.chats.front().id, task->cancellation);
			(void)uam::PollWorktreeLaunchPreparations(fixture.app);
			UAM_ASSERT(!task->merged && !task->saved.value_or(false));
		}
		else
		{
			fs::rename(fixture.app.data_root, preserved);
			UAM_ASSERT(uam::io::WriteTextFile(fixture.app.data_root, "Private unavailable storage."));
			UAM_ASSERT(!fixture.Settle(task));
			UAM_ASSERT(!task->error.empty());
			fs::remove(fixture.app.data_root);
			fs::rename(preserved, fixture.app.data_root);
		}
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
		UAM_ASSERT(saved && saved->workspace_isolation_kind == uam::paths::kPendingGitWorktreeIsolationKind);
		UAM_ASSERT(saved->workspace_worktree_directory.empty() && !fs::exists(task->receipt_file));
		UAM_ASSERT_EQ(ReadFile(plain / "sentinel.txt"), std::string("Untouched plain source."));
		if (!canceled)
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Failed);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> retry = fixture.Start();
		UAM_ASSERT(fixture.Settle(retry));
		UAM_ASSERT(fixture.app.chats.front().workspace_isolation_kind.empty());
		UAM_ASSERT(!fs::exists(plain / ".git") && !fs::exists(retry->receipt_file));
		UAM_ASSERT_EQ(ReadFile(plain / "sentinel.txt"), std::string("Untouched plain source."));
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
	}
#endif
}

UAM_TEST(RemoteWorktreeProtocolTwoNeverStartsPreparation)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	RemoteWorktreeFixture fixture;
	fixture.app.settings.execution_hosts.back().runner_protocol_version = 2;
	const uam::WorktreeLaunchPreparationResult result = uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front());
	UAM_ASSERT_EQ(result.state, uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT(!result.error.empty());
	UAM_ASSERT(fixture.app.worktree_launch_preparations.empty() && fixture.app.worktree_operation_chat_ids.empty());
#endif
}

UAM_TEST(RemoteWorktreeStaleHostAndRecreatedChatCannotPublish)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	for (const bool host_changed : {false, true})
	{
		RemoteWorktreeFixture fixture;
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
		fixture.Worker(task);
		if (host_changed) fixture.app.settings.execution_hosts.back().ssh_alias = "private-changed-host";
		else fixture.app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT(task->cancellation->stop_requested());
		UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
		UAM_ASSERT(!task->saved.value_or(false));
		UAM_ASSERT(fs::exists(task->receipt_file));
		UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{1});
	}
#endif
}

UAM_TEST(RemoteWorktreeCanceledCreationRetainsReceiptWithoutPublication)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
	fixture.Worker(task);
	uam::CancelWorktreeLaunchPreparation(fixture.app, fixture.app.chats.front().id);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->cancellation->stop_requested());
	UAM_ASSERT(!task->merged && !task->saved.value_or(false));
	UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
	UAM_ASSERT(fs::exists(task->receipt_file));
#endif
}

UAM_TEST(RemoteWorktreeRestartRecoversOriginalArtifactsWithoutRecreating)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
	fixture.Worker(task);
	const fs::path preserved = fixture.temp.root / "preserved-data";
	fs::rename(fixture.app.data_root, preserved);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.app.data_root, "private unavailable storage"));
	UAM_ASSERT(!fixture.Settle(task));
	UAM_ASSERT(!task->error.empty());
	fs::remove(fixture.app.data_root);
	fs::rename(preserved, fixture.app.data_root);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
	UAM_ASSERT(saved && saved->workspace_worktree_directory.empty());
	const std::string original_worktree = task->prepared->metadata.workspace_worktree_directory;
	fixture.app.worktree_launch_preparations.clear();
	fixture.app.pending_chat_save_at_by_chat_id.clear();
	fixture.app.pending_chat_save_tasks.clear();
	fixture.app.chats.front() = *saved;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> recovery = fixture.Start();
	UAM_ASSERT(fixture.Settle(recovery));
	UAM_ASSERT_EQ(fixture.app.chats.front().workspace_worktree_directory, original_worktree);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{1});
#endif
}

UAM_TEST(RemoteWorktreeInitializingCheckoutNeverRecoversReady)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> first = fixture.Start();
	fixture.Worker(first);
	const nlohmann::json receipt = nlohmann::json::parse(ReadFile(first->receipt_file));
	const fs::path worktree = uam::paths::PathFromUtf8(receipt.at("worktree").get<std::string>());
	const std::string marker = uam::strings::Trim(ReadFile(worktree / ".git"));
	UAM_ASSERT(marker.starts_with("gitdir: "));
	const fs::path admin = uam::paths::PathFromUtf8(marker.substr(8));
	UAM_ASSERT(uam::io::WriteTextFile(admin / "locked", "initializing\n"));
	fixture.app.worktree_launch_preparations.clear();
	fixture.app.worktree_operation_chat_ids.clear();
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> recovery = fixture.Start();
	UAM_ASSERT(!fixture.Settle(recovery));
	UAM_ASSERT(!recovery->error.empty());
	UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
	UAM_ASSERT(fs::exists(admin / "locked") && fs::exists(worktree / "source.txt"));
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
#endif
}

UAM_TEST(RemoteWorktreeCompletedReceiptSurvivesHelperMetadataUpgrade)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> first = fixture.Start();
	UAM_ASSERT(fixture.Settle(first));
	const std::string original = fixture.app.chats.front().workspace_worktree_directory;
	ExecutionHost& host = fixture.app.settings.execution_hosts.back();
	host.runner_directory = "private/upgraded-helper";
	host.architecture = "private-upgraded-architecture";
	host.instruction_file = "private-upgraded-instructions.md";
	host.instruction_text = "Private upgraded instruction content";
	fixture.app.worktree_launch_preparations.clear();
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> recovery = fixture.Start();
	UAM_ASSERT(fixture.Settle(recovery));
	UAM_ASSERT_EQ(fixture.app.chats.front().workspace_worktree_directory, original);
	UAM_ASSERT_EQ(ReadFile(fs::path(original) / "source.txt"), std::string("committed source\n"));
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
#endif
}

UAM_TEST(RemoteWorktreeDefiniteFailedReceiptAllowsFreshPreservedAttempt)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> first = fixture.Start();
	fixture.Worker(first);
	nlohmann::json receipt = nlohmann::json::parse(ReadFile(first->receipt_file));
	const fs::path old_worktree = uam::paths::PathFromUtf8(receipt.at("worktree").get<std::string>());
	const std::string old_branch = receipt.at("branch").get<std::string>();
	// Reproduce a recorded no-start failure after an unrelated directory occupies the selected leaf.
	UAM_ASSERT(RunGitForTest(fixture.source, "worktree remove --force " + ShellQuoteForTest(old_worktree.string())));
	fs::create_directories(old_worktree);
	UAM_ASSERT(uam::io::WriteTextFile(old_worktree / "foreign-survivor.txt", "preserve foreign destination\n"));
	receipt["operationState"] = "failed";
	receipt["stableHost"] = nlohmann::json::array({"previous-host", "ssh", "previous-alias", "linux"}).dump();
	receipt["requestedSource"] = "/previous/private/workspace";
	UAM_ASSERT(uam::io::WriteTextFile(first->receipt_file, receipt.dump()));
	fixture.app.worktree_launch_preparations.clear();
	fixture.app.worktree_operation_chat_ids.clear();
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> recovery = fixture.Start();
	UAM_ASSERT(fixture.Settle(recovery));
	UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory != old_worktree.string());
	UAM_ASSERT(fixture.app.chats.front().workspace_branch_name != old_branch);
	UAM_ASSERT_EQ(ReadFile(old_worktree / "foreign-survivor.txt"), std::string("preserve foreign destination\n"));
	UAM_ASSERT(RunGitForTest(fixture.source, "show-ref --verify --quiet " + ShellQuoteForTest("refs/heads/" + old_branch)));
	UAM_ASSERT(!fs::exists(first->receipt_file));
	UAM_ASSERT(fs::exists(fs::path(first->receipt_file.string() + ".failed")));
#endif
}

UAM_TEST(RemoteWorktreePendingReceiptObservesOriginalHelperAndNeverInfersSuccess)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	for (const std::string outcome : {"running", "nonzero", "missing"})
	{
		RemoteWorktreeFixture fixture;
		fixture.app.settings.execution_hosts.back().runner_directory = "private/original-helper";
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> first = fixture.Start();
		UAM_ASSERT(fixture.Settle(first));
		const std::string original_worktree = fixture.app.chats.front().workspace_worktree_directory;
		nlohmann::json receipt = nlohmann::json::parse(ReadFile(first->receipt_file));
		receipt["operationState"] = "pending";
		UAM_ASSERT(uam::io::WriteTextFile(first->receipt_file, receipt.dump()));
		fixture.app.worktree_launch_preparations.clear();
		fixture.app.settings.execution_hosts.back().runner_directory = "private/new-helper";
		fixture.app.settings.execution_hosts.back().instruction_text = "Private new instructions";
		const fs::path bridge_log = fixture.temp.root / "bridge.log";
		ScopedEnvVar executable("UAM_PRIVATE_WORKTREE_FIXTURE", PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().string());
		ScopedEnvVar log("UAM_PRIVATE_WORKTREE_BRIDGE_LOG", bridge_log.string());
		ScopedEnvVar mode("UAM_PRIVATE_WORKTREE_OUTCOME", outcome);
		ScopedEnvVar operation("UAM_PRIVATE_WORKTREE_OPERATION", receipt.at("operationId").get<std::string>());
		ScopedEnvVar token("UAM_PRIVATE_WORKTREE_TOKEN", receipt.at("controlToken").get<std::string>());
		fixture.UseReceiptRunner();
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> recovery = fixture.Start();
		UAM_ASSERT(!fixture.Settle(recovery));
		UAM_ASSERT(!recovery->error.empty());
		UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(first->receipt_file)).at("operationState"), nlohmann::json("pending"));
		const std::string requests = ReadFile(bridge_log);
		UAM_ASSERT(requests.find("private/original-helper") != std::string::npos);
		UAM_ASSERT(requests.find("private/new-helper") == std::string::npos);
		UAM_ASSERT_EQ(fixture.app.chats.front().workspace_worktree_directory, original_worktree);
		UAM_ASSERT_EQ(ReadFile(fs::path(original_worktree) / "source.txt"), std::string("committed source\n"));
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
	}
#endif
}

UAM_TEST(RemoteWorktreeStatusInspectionFailureDoesNotClaimMissingDirectory)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> first = fixture.Start();
	UAM_ASSERT(fixture.Settle(first));
	ScopedEnvVar executable("UAM_PRIVATE_WORKTREE_FIXTURE", PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().string());
	ScopedEnvVar log("UAM_PRIVATE_WORKTREE_BRIDGE_LOG", (fixture.temp.root / "bridge.log").string());
	ScopedEnvVar mode("UAM_PRIVATE_WORKTREE_OUTCOME", "inspection-error");
	ScopedEnvVar target("UAM_PRIVATE_WORKTREE_TARGET", fixture.app.chats.front().workspace_worktree_directory);
	fixture.UseReceiptRunner();
	const uam::GitWorktreeStatus status = uam::GitWorktreeService().Status(fixture.app, fixture.app.chats.front());
	UAM_ASSERT(status.isolated && !status.error.empty() && !status.worktree_missing);
	UAM_ASSERT(fs::exists(fs::path(fixture.app.chats.front().workspace_worktree_directory) / "source.txt"));
#endif
}

UAM_TEST(RemoteWorktreePrestartCollisionRecordsFailureWithoutStartingGit)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	ChatSession metadata = uam::CaptureVcsChatContext(fixture.app.chats.front());
	std::vector<std::string> states;
	fs::path occupied;
	std::string branch;
	const uam::GitWorktreeIntentRecorder record = [&](const uam::GitWorktreeCreationIntent& intent, std::string& error)
	{
		states.push_back(intent.operation_state);
		if (intent.operation_state == "pending")
		{
			occupied = intent.worktree;
			branch = intent.branch;
			fs::create_directories(occupied);
			if (!uam::io::WriteTextFile(occupied / "foreign-survivor.txt", "private collision survivor\n"))
			{ error = "Private collision setup failed"; return false; }
		}
		return true;
	};
	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PrepareForChat(fixture.app, metadata, record, {});
	UAM_ASSERT(!result.ok && !result.message.empty());
	UAM_ASSERT(states == std::vector<std::string>({"pending", "failed"}));
	UAM_ASSERT_EQ(ReadFile(occupied / "foreign-survivor.txt"), std::string("private collision survivor\n"));
	UAM_ASSERT(!RunGitForTest(fixture.source, "show-ref --verify --quiet " + ShellQuoteForTest("refs/heads/" + branch)));
	UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
#endif
}

UAM_TEST(RemoteWorktreeObservedFailedMissingCheckoutArchivesReceipt)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> first = fixture.Start();
	fixture.Worker(first);
	nlohmann::json receipt = nlohmann::json::parse(ReadFile(first->receipt_file));
	const std::string old_worktree = receipt.at("worktree").get<std::string>();
	const std::string old_branch = receipt.at("branch").get<std::string>();
	UAM_ASSERT(RunGitForTest(fixture.source, "worktree remove --force " + ShellQuoteForTest(old_worktree)));
	receipt["operationState"] = "pending";
	UAM_ASSERT(uam::io::WriteTextFile(first->receipt_file, receipt.dump()));
	fixture.app.worktree_launch_preparations.clear();
	fixture.app.worktree_operation_chat_ids.clear();
	ScopedEnvVar executable("UAM_PRIVATE_WORKTREE_FIXTURE", PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().string());
	ScopedEnvVar log("UAM_PRIVATE_WORKTREE_BRIDGE_LOG", (fixture.temp.root / "bridge.log").string());
	ScopedEnvVar mode("UAM_PRIVATE_WORKTREE_OUTCOME", "nonzero");
	ScopedEnvVar operation("UAM_PRIVATE_WORKTREE_OPERATION", receipt.at("operationId").get<std::string>());
	ScopedEnvVar token("UAM_PRIVATE_WORKTREE_TOKEN", receipt.at("controlToken").get<std::string>());
	fixture.UseReceiptRunner();
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> recovery = fixture.Start();
	UAM_ASSERT(!fixture.Settle(recovery));
	UAM_ASSERT(!fs::exists(first->receipt_file) && fs::exists(fs::path(first->receipt_file.string() + ".failed")));
	UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Failed);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> fresh = fixture.Start();
	UAM_ASSERT(fixture.Settle(fresh));
	UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory != old_worktree);
	UAM_ASSERT(RunGitForTest(fixture.source, "show-ref --verify --quiet " + ShellQuoteForTest("refs/heads/" + old_branch)));
#endif
}

UAM_TEST(RemoteWorktreeCompletionSaveFailureRetainsOriginalProcessForRecovery)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	PrivateWorktreeResidentRunner resident(fixture.temp.root / "ssh");
	const ExecutionHost& host = fixture.app.settings.execution_hosts.back();
	ChatSession metadata = uam::CaptureVcsChatContext(fixture.app.chats.front());
	metadata.created_at = fixture.app.chats.front().created_at;
	const fs::path receipt_file = fixture.app.data_root / ".worktree-launch" / "private-retained-operation.json";
	uam::GitWorktreeCreationIntent retained;
	const uam::GitWorktreeIntentRecorder record = [&](const uam::GitWorktreeCreationIntent& intent, std::string& error)
	{
		if (intent.operation_state == "completed")
		{ error = "Private completion receipt save failure"; return false; }
		retained = intent;
		const nlohmann::json receipt = {{"chatId", metadata.id}, {"createdAt", metadata.created_at},
		    {"remoteHost", uam::RemoteWorktreeHostIdentity(host)}, {"stableHost", uam::RemoteWorktreeStableHostIdentity(host)},
		    {"operationState", intent.operation_state}, {"operationId", intent.operation_id}, {"controlToken", intent.control_token},
		    {"source", uam::paths::Utf8PathString(intent.source)}, {"repository", uam::paths::Utf8PathString(intent.repository)},
		    {"worktree", uam::paths::Utf8PathString(intent.worktree)}, {"branch", intent.branch}, {"baseSha", intent.base_sha}};
		return uam::io::WriteTextFile(receipt_file, receipt.dump());
	};
	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PrepareForChat(fixture.app, metadata, record, {});
	UAM_ASSERT(!result.ok && result.message.find("Private completion receipt save failure") != std::string::npos);
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(receipt_file)).at("operationState"), nlohmann::json("pending"));
	uam::remote::RunnerClient observer(PlatformServicesFactory::Instance().process_service,
	    uam::remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version), host.runner_version, host.runner_protocol_version);
	std::string error;
	UAM_ASSERT(observer.Connect(&error));
	observer.SetProcessControlToken(retained.operation_id, retained.control_token);
	uam::remote::ProcessPollResult process;
	UAM_ASSERT(observer.PollProcess(retained.operation_id, process, &error));
	UAM_ASSERT(!process.running && process.exit_code == 0);
	UAM_ASSERT(uam::GitWorktreeService().ValidateRemotePreparedWorkspace(host, fixture.app.data_root, receipt_file, metadata, error));
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(receipt_file)).at("operationState"), nlohmann::json("completed"));
	bool missing = false;
	UAM_ASSERT(!observer.PollProcess(retained.operation_id, process, &error, {}, nullptr, nullptr, &missing));
	UAM_ASSERT(missing);
	UAM_ASSERT_EQ(ReadFile(retained.worktree / "source.txt"), std::string("committed source\n"));
	UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
#endif
}

UAM_TEST(RemoteWorktreeCommandReportsUnattemptedPrestartFailures)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	RemoteWorktreeFixture fixture;
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const ExecutionHost& host = fixture.app.settings.execution_hosts.back();
	for (const bool cancel : {false, true})
	{
		uam::remote::RunnerClient client(service, {"/usr/bin/false"});
		std::stop_source cancellation;
		if (cancel) cancellation.request_stop();
		bool attempted = true;
		const ProcessExecutionResult result = client.ExecuteCommand("private-no-start", fixture.source, {"git", "--version"},
		    cancel ? 1000 : 0, cancellation.get_token(), {}, "private-no-start-token", &attempted);
		UAM_ASSERT(!attempted && !result.ok);
		UAM_ASSERT(cancel ? result.canceled : result.timed_out);
	}
	{
		uam::remote::RunnerClient client(service, {"/usr/bin/false"});
		bool attempted = true;
		const ProcessExecutionResult result = client.ExecuteCommand("private-no-start", fixture.source, {"git", "--version"}, 1000, {}, {}, "private-no-start-token", &attempted);
		UAM_ASSERT(!attempted && !result.ok && !result.error.empty());
	}
	ScopedEnvVar executable("UAM_PRIVATE_WORKTREE_FIXTURE", service.ResolveCurrentExecutablePath().string());
	ScopedEnvVar log("UAM_PRIVATE_WORKTREE_BRIDGE_LOG", (fixture.temp.root / "bridge.log").string());
	for (const std::string mode : {"no-acks", "start-reject"})
	{
		ScopedEnvVar outcome("UAM_PRIVATE_WORKTREE_OUTCOME", mode);
		fixture.UseReceiptRunner();
		uam::remote::RunnerClient client(service,
		    uam::remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version), host.runner_version, host.runner_protocol_version);
		bool attempted = mode == "no-acks";
		const ProcessExecutionResult result = client.ExecuteCommand("private-no-start", fixture.source, {"git", "--version"}, 1000, {}, {}, "private-no-start-token", &attempted);
		UAM_ASSERT(attempted == (mode == "start-reject"));
		UAM_ASSERT(!result.ok && !result.error.empty());
	}
#endif
}

UAM_TEST(RemoteWorktreeSettledCleanupFailuresDoNotRejectCompletedWorkspace)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	for (const std::string mode : {"cleanup-error", "cleanup-retry", "cleanup-missing"})
	{
		RemoteWorktreeFixture fixture;
		ScopedEnvVar executable("UAM_PRIVATE_WORKTREE_FIXTURE", PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().string());
		ScopedEnvVar bridge_log("UAM_PRIVATE_WORKTREE_BRIDGE_LOG", (fixture.temp.root / "bridge.log").string());
		ScopedEnvVar outcome("UAM_PRIVATE_WORKTREE_OUTCOME", mode);
		const fs::path cleanup_log = fixture.temp.root / "cleanup.log";
		ScopedEnvVar log("UAM_PRIVATE_WORKTREE_CLEANUP_LOG", cleanup_log.string());
		fixture.UseReceiptRunner();
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
		UAM_ASSERT(fixture.Settle(task));
		UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(task->receipt_file)).at("operationState"), nlohmann::json("completed"));
		UAM_ASSERT_EQ(ReadFile(cleanup_log), mode == "cleanup-missing" ? std::string("remove\n") : std::string("remove\nremove\n"));
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Ready);
		UAM_ASSERT_EQ(ReadFile(fs::path(fixture.app.chats.front().workspace_worktree_directory) / "source.txt"), std::string("committed source\n"));
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
	}
#endif
}

UAM_TEST(RemoteWorktreeExplicitRequestDoesNotReusePlainSuccess)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	RemoteWorktreeFixture fixture;
	const fs::path plain = fs::canonical(fixture.temp.root) / "plain-source";
	fs::create_directory(plain);
	fixture.app.chats.front().workspace_directory = plain.string();
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> automatic = fixture.Start();
	UAM_ASSERT(fixture.Settle(automatic));
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> explicit_request = fixture.Start(true);
	UAM_ASSERT(!fixture.Settle(explicit_request));
	UAM_ASSERT(!explicit_request->error.empty());
	UAM_ASSERT(fixture.app.chats.front().workspace_isolation_kind.empty());
	UAM_ASSERT(!fs::exists(plain / ".git") && !fs::exists(explicit_request->receipt_file));
#endif
}

UAM_TEST(RemoteWorktreeDamagedBareRepositoryIsNotPlain)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	for (const std::string missing : {"HEAD", "objects"})
	{
		RemoteWorktreeFixture fixture;
		const fs::path bare = fs::canonical(fixture.temp.root) / "damaged-bare.git";
		UAM_ASSERT(RunTestCommand("git clone --bare " + ShellQuoteForTest(fixture.source.string()) + " " + ShellQuoteForTest(bare.string())));
		fs::remove_all(bare / missing);
		fixture.app.chats.front().workspace_directory = bare.string();
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.Start();
		UAM_ASSERT(!fixture.Settle(task));
		UAM_ASSERT(!task->error.empty());
		UAM_ASSERT_EQ(fixture.app.chats.front().workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
		UAM_ASSERT(!fs::exists(task->receipt_file));
		UAM_ASSERT(fs::exists(bare / "config") && fs::exists(bare / "refs"));
	}
#endif
}

UAM_TEST(RemotePlainWorkspacePreparationKeepsAcceptedInputUnheldUntilRealFailure)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	if (!GitAvailableForTests()) return;
	for (const bool malformed_git : {false, true})
	{
		RemoteWorktreeFixture fixture;
		const fs::path plain = fs::canonical(fixture.temp.root) / "plain-acp-source";
		fs::create_directory(plain);
		UAM_ASSERT(uam::io::WriteTextFile(plain / "sentinel.txt", "Preserve the unstarted source."));
		if (malformed_git)
			UAM_ASSERT(uam::io::WriteTextFile(plain / ".git", "gitdir: missing-private-metadata\n"));
		fixture.app.settings.git_pull_before_chat = true;
		fixture.app.settings.central_provider_configuration.enabled = false;
		ChatSession& chat = fixture.app.chats.front();
		chat.workspace_directory = plain.string();
		chat.approval_mode.clear();
		chat.memory_enabled = false;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
		const std::vector<Message> messages = chat.messages;
		std::string error;
		if (!uam::SendAcpPrompt(fixture.app, chat.id, "Continue with this work", &error))
			throw TestFailure("Plain workspace ACP admission failed: " + error);
		uam::AcpSessionState* session = uam::FindAcpSessionForChat(fixture.app, chat.id);
		UAM_ASSERT(session && session->queued_user_prompts.size() == 1);
		const uam::AcpQueuedUserPromptState raw = session->queued_user_prompts.front();
		UAM_ASSERT_EQ(raw.text, std::string("Continue with this work"));
		UAM_ASSERT(!raw.prepared_for_delivery);
		UAM_ASSERT(!raw.id.empty() && raw.revision > 0);
		UAM_ASSERT_EQ(chat.acp_queued_prompts, std::vector<uam::AcpQueuedUserPromptState>{raw});
		UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{0});
		UAM_ASSERT(!session->queue_held && !chat.acp_queue_held);
		std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
		UAM_ASSERT(saved && saved->acp_queued_prompts == std::vector<uam::AcpQueuedUserPromptState>{raw});

		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		uam::WorktreeLaunchPreparationState state = uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, *session, chat, &error);
		UAM_ASSERT_EQ(state, uam::WorktreeLaunchPreparationState::Pending);
		UAM_ASSERT(session->worktree_launch_pending);
		UAM_ASSERT_EQ(session->lifecycle_state, std::string("waiting_workspace"));
		UAM_ASSERT(!session->queue_held && !chat.acp_queue_held);
		while (state == uam::WorktreeLaunchPreparationState::Pending && std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::FlushPendingChatSaves(fixture.app);
			// Poll preparation only: reaching Ready must not launch or drain a provider here.
			state = uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, *session, chat, &error);
			UAM_ASSERT_EQ(session->queued_user_prompts.size(), std::size_t{1});
			UAM_ASSERT(session->queued_user_prompts.front() == raw);
			UAM_ASSERT(!session->running && session->prompt_request_id == 0);
			if (state == uam::WorktreeLaunchPreparationState::Pending)
			{
				UAM_ASSERT(!session->queue_held && !chat.acp_queue_held);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
		if (state == uam::WorktreeLaunchPreparationState::Pending)
			throw TestFailure("Plain workspace ACP preparation exceeded 30 seconds.");
		UAM_ASSERT_EQ(state, malformed_git ? uam::WorktreeLaunchPreparationState::Failed : uam::WorktreeLaunchPreparationState::Ready);
		UAM_ASSERT_EQ(session->queue_held, malformed_git);
		UAM_ASSERT_EQ(chat.acp_queue_held, malformed_git);
		UAM_ASSERT(!session->worktree_launch_pending);
		if (malformed_git)
		{
			UAM_ASSERT(!session->last_error.empty() && !error.empty());
			UAM_ASSERT_EQ(chat.workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
			UAM_ASSERT_EQ(ReadFile(plain / ".git"), std::string("gitdir: missing-private-metadata\n"));
		}
		else
		{
			UAM_ASSERT(session->last_error.empty() && error.empty());
			UAM_ASSERT(chat.workspace_isolation_kind.empty());
			UAM_ASSERT(!fs::exists(plain / ".git"));
		}
		while ((fixture.app.pending_chat_save_at_by_chat_id.contains(chat.id) || fixture.app.pending_chat_save_tasks.contains(chat.id)) && std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::FlushPendingChatSaves(fixture.app);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(chat.id) && !fixture.app.pending_chat_save_tasks.contains(chat.id));
		UAM_ASSERT_EQ(chat.acp_queued_prompts, std::vector<uam::AcpQueuedUserPromptState>{raw});
		UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{0});
		UAM_ASSERT_EQ(chat.messages, messages);
		saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
		UAM_ASSERT(saved && saved->acp_queued_prompts == std::vector<uam::AcpQueuedUserPromptState>{raw});
		UAM_ASSERT_EQ(saved->acp_dispatched_queued_prompt_count, std::size_t{0});
		UAM_ASSERT_EQ(saved->acp_queue_held, malformed_git);
		UAM_ASSERT_EQ(saved->messages, messages);
		UAM_ASSERT_EQ(saved->workspace_isolation_kind, chat.workspace_isolation_kind);
		UAM_ASSERT(!session->running && session->prompt_request_id == 0);
		UAM_ASSERT(chat.remote_pending_requests.empty() && chat.remote_prompt_delivery_payload.empty());
		UAM_ASSERT(chat.workspace_worktree_directory.empty() && fixture.app.cli_terminals.empty());
		UAM_ASSERT_EQ(ReadFile(plain / "sentinel.txt"), std::string("Preserve the unstarted source."));
	}
#endif
}
