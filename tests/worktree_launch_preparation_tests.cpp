#include "test_harness.h"
#include "app/worktree_launch_preparation_service.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

namespace
{
	struct LaunchFixture
	{
		TempDir temporary{"uam-async-worktree-launch"};
		uam::AppState app;
		fs::path repository = temporary.root / "repo";
		fs::path foreign = temporary.root / "foreign";

		LaunchFixture()
		{
			fs::create_directories(repository);
			UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repository.string())));
			UAM_ASSERT(RunGitForTest(repository, "config user.email uam@example.test"));
			UAM_ASSERT(RunGitForTest(repository, "config user.name UAM"));
			UAM_ASSERT(uam::io::WriteTextFile(repository / "source.txt", "source survivor\n"));
			UAM_ASSERT(RunGitForTest(repository, "add source.txt"));
			UAM_ASSERT(RunGitForTest(repository, "commit -m base"));
			UAM_ASSERT(RunGitForTest(repository, "worktree add -b foreign-branch " + ShellQuoteForTest(foreign.string())));
			UAM_ASSERT(uam::io::WriteTextFile(foreign / "private.txt", "foreign survivor\n"));
			app.data_root = temporary.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
			chat.workspace_directory = repository.string();
			chat.messages.push_back({MessageRole::User, "saved before preparation"});
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			app.chats.push_back(std::move(chat));
		}

		void WaitForWorker(const std::shared_ptr<uam::WorktreeLaunchPreparationTask>& task)
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (task->worker.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
		}

		void WaitForSave(const std::shared_ptr<uam::WorktreeLaunchPreparationTask>& task)
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (!task->saved.has_value() && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(app);
				(void)uam::PollWorktreeLaunchPreparations(app);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			UAM_ASSERT(task->saved.has_value());
		}

		void AssertForeignSurvivors()
		{
			UAM_ASSERT_EQ(uam::io::ReadTextFile(foreign / "private.txt"), "foreign survivor\n");
			UAM_ASSERT_EQ(uam::io::ReadTextFile(repository / "source.txt"), "source survivor\n");
			UAM_ASSERT(RunGitForTest(repository, "show-ref --verify refs/heads/foreign-branch"));
		}
	};
}

UAM_TEST(WorktreeLaunchPreparationAsyncOrderingPreservesLiveMetadata)
{
	if (!GitAvailableForTests()) return;
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	const uam::WorktreeLaunchPreparationResult initial = uam::PrepareWorktreeForLaunch(fixture.app, chat);
	UAM_ASSERT_EQ(initial.state, uam::WorktreeLaunchPreparationState::Pending);
	UAM_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
	uam::CancelWorktreeLaunchPreparation(fixture.app, chat.id, std::make_shared<std::stop_source>());
	UAM_ASSERT(!initial.cancellation->stop_requested());
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).cancellation, initial.cancellation);
	fixture.WaitForWorker(task);
	chat.messages.push_back({MessageRole::User, "added during preparation"});
	chat.title = "Current title";
	chat.memory_level = "off";
	chat.goal_command_revision = "current-goal-revision";
	uam::AcpQueuedUserPromptState queued;
	queued.id = "retained-outbox";
	queued.text = "Retained next prompt";
	chat.acp_queued_prompts.push_back(queued);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->merged);
	UAM_ASSERT(fs::exists(task->receipt_file));
	UAM_ASSERT(!task->saved.has_value());
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const std::optional<ChatSession> before = ChatRepository::LoadLocalChatSummaryReadOnly(fixture.app.data_root, chat.id);
	UAM_ASSERT(before && before->workspace_worktree_directory.empty());
	UAM_ASSERT(!fixture.app.worktree_retention_gate.TryAcquireCleanup());
	fixture.WaitForSave(task);
	UAM_ASSERT(*task->saved);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved);
	UAM_ASSERT_EQ(saved->workspace_worktree_directory, chat.workspace_worktree_directory);
	UAM_ASSERT_EQ(saved->messages.back().content, "added during preparation");
	UAM_ASSERT_EQ(saved->title, "Current title");
	UAM_ASSERT_EQ(saved->memory_level, "off");
	UAM_ASSERT_EQ(saved->goal_command_revision, "current-goal-revision");
	UAM_ASSERT_EQ(saved->acp_queued_prompts.front().id, "retained-outbox");
	UAM_ASSERT(fixture.app.worktree_retention_gate.TryAcquireCleanup());
	fixture.AssertForeignSurvivors();
}

UAM_TEST(WorktreeLaunchPreparationSaveFailureKeepsArtifactsForRetry)
{
	if (!GitAvailableForTests()) return;
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(task);
	const fs::path primary = AppPaths::UamChatFilePath(fixture.app.data_root, chat.id);
	const fs::path preserved = fixture.temporary.root / "preserved-chat.json";
	fs::copy_file(primary, preserved);
	fs::remove(primary);
	fs::create_directory(primary);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->merged);
	const std::string receipt = uam::io::ReadTextFile(task->receipt_file);
	fixture.WaitForSave(task);
	UAM_ASSERT(!*task->saved);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT(fs::exists(uam::paths::PathFromUtf8(chat.workspace_worktree_directory)));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(task->receipt_file), receipt);
	UAM_ASSERT_EQ(chat.messages.back().content, "saved before preparation");
	fs::remove(primary);
	fs::copy_file(preserved, primary);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	fixture.WaitForSave(task);
	UAM_ASSERT(*task->saved);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	fixture.AssertForeignSurvivors();
}

UAM_TEST(WorktreeLaunchPreparationStaleAndCanceledOwnersCannotPublish)
{
	if (!GitAvailableForTests()) return;
	for (const std::string_view change : {"stop", "delete", "recreate", "same-stamp", "root", "provider", "workspace", "base", "native"})
	{
		LaunchFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		const uam::WorktreeLaunchPreparationResult initial = uam::PrepareWorktreeForLaunch(fixture.app, chat);
		UAM_ASSERT_EQ(initial.state, uam::WorktreeLaunchPreparationState::Pending);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
		if (change == "stop") initial.cancellation->request_stop();
		else if (change == "delete")
		{
			uam::WorkspaceDeletionTask deletion;
			UAM_ASSERT(!uam::PrepareChatDeletion(fixture.app, {chat.id}, deletion));
			UAM_ASSERT(initial.cancellation->stop_requested());
		}
		else if (change == "recreate") chat.created_at += "-replacement";
		else if (change == "same-stamp") chat.metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		else if (change == "root") fixture.app.data_root /= "replacement-root";
		else if (change == "provider") chat.provider_id = "gemini-cli";
		else if (change == "workspace") chat.workspace_directory = (fixture.temporary.root / "replacement-workspace").string();
		else if (change == "base") fixture.app.settings.git_default_base_branch = "replacement-base";
		else chat.native_session_id = "replacement-native";
		UAM_ASSERT(!fixture.app.worktree_retention_gate.TryAcquireCleanup());
		fixture.WaitForWorker(task);
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT(fixture.app.worktree_launch_preparations.empty() || change == "stop" || change == "delete");
		UAM_ASSERT(chat.workspace_worktree_directory.empty());
		UAM_ASSERT(!task->merged);
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
		if (task->prepared && task->prepared->operation.ok)
		{
			UAM_ASSERT(fs::exists(task->receipt_file));
			UAM_ASSERT(fs::exists(uam::paths::PathFromUtf8(task->prepared->metadata.workspace_worktree_directory)));
		}
		UAM_ASSERT(fixture.app.worktree_retention_gate.TryAcquireCleanup());
		fixture.AssertForeignSurvivors();
	}
}

UAM_TEST(WorktreeLaunchPreparationBlockedOwnershipStorageIsSafe)
{
	if (!GitAvailableForTests()) return;
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const fs::path blocked = fixture.app.data_root / ".worktree-launch";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "existing file must survive\n"));
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(task);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT(chat.workspace_worktree_directory.empty());
	UAM_ASSERT(!fs::exists(fixture.app.data_root / "worktrees"));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(blocked), "existing file must survive\n");
	fixture.AssertForeignSurvivors();
}

UAM_TEST(WorktreeLaunchPreparationCanceledManagedRetryValidatesOwnership)
{
	if (!GitAvailableForTests()) return;
	for (bool tamper : {false, true})
	{
		LaunchFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		const fs::path source = fixture.temporary.root / "managed-source";
		UAM_ASSERT(uam::io::WriteTextFile(source / "baseline.txt", "managed baseline"));
		chat.workspace_directory = source.string();
		const uam::WorktreeLaunchPreparationResult initial = uam::PrepareWorktreeForLaunch(fixture.app, chat);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
		fixture.WaitForWorker(task);
		initial.cancellation->request_stop();
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT(task->prepared && task->prepared->operation.ok && !task->merged);
		const std::string artifact = task->prepared->metadata.workspace_worktree_directory;
		if (tamper) UAM_ASSERT(uam::io::WriteTextFile(task->receipt_file, "{}"));
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
		fixture.WaitForWorker(task);
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		if (tamper)
		{
			UAM_ASSERT(!task->merged);
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Failed);
		}
		else
		{
			UAM_ASSERT(task->merged);
			fixture.WaitForSave(task);
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
			UAM_ASSERT_EQ(chat.workspace_worktree_directory, artifact);
		}
		UAM_ASSERT(fs::exists(uam::paths::PathFromUtf8(artifact)));
		fixture.AssertForeignSurvivors();
	}
}

UAM_TEST(WorktreeLaunchPreparationAcpAdmissionWaitsForOwnedSave)
{
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = std::make_shared<uam::WorktreeLaunchPreparationTask>();
	fixture.app.worktree_launch_preparations.emplace(chat.id, task);
	const std::size_t messages = chat.messages.size();
	std::string error;
	UAM_ASSERT(!uam::SendAcpPrompt(fixture.app, chat.id, "must wait", &error));
	UAM_ASSERT(error.find("Workspace preparation") != std::string::npos);
	UAM_ASSERT_EQ(chat.messages.size(), messages);
	UAM_ASSERT(chat.acp_queued_prompts.empty());
	task->saved = true;
	fixture.app.worktree_operation_chat_ids.insert(chat.id);
	error.clear();
	UAM_ASSERT(!uam::SendAcpPrompt(fixture.app, chat.id, "must still wait", &error));
	UAM_ASSERT(error.find("Workspace preparation") != std::string::npos);
	UAM_ASSERT_EQ(chat.messages.size(), messages);
	UAM_ASSERT(chat.acp_queued_prompts.empty());
}

UAM_TEST(WorktreeLaunchPreparationFailedSaveRetryRevalidatesArtifacts)
{
	if (!GitAvailableForTests()) return;
	for (bool remove_binding : {false, true})
	{
		LaunchFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
		fixture.WaitForWorker(task);
		const fs::path primary = AppPaths::UamChatFilePath(fixture.app.data_root, chat.id);
		const fs::path saved = fixture.temporary.root / "preserved.json";
		fs::copy_file(primary, saved);
		fs::remove(primary);
		fs::create_directory(primary);
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		fixture.WaitForSave(task);
		UAM_ASSERT(!*task->saved);
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Failed);
		const fs::path artifact = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
		if (remove_binding)
		{
			fs::remove(artifact / ".git");
			UAM_ASSERT(uam::io::WriteTextFile(artifact / "foreign.txt", "foreign survivor"));
		}
		else UAM_ASSERT(RunGitForTest(artifact, "-c user.name=Fixture -c user.email=fixture@example.test commit --allow-empty -m foreign"));
		fs::remove(primary);
		fs::copy_file(saved, primary);
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
		fixture.WaitForWorker(task);
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Failed);
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
		// A ready packaged-task future exposes its result before the detached
		// callable has necessarily destroyed its captured mutation lease. Observe
		// the actual cleanup resource; never treat result readiness as retirement.
		std::shared_ptr<uam::WorkspaceLeaseGate::Lease> cleanup;
		const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!cleanup && std::chrono::steady_clock::now() < cleanup_deadline)
		{
			cleanup = fixture.app.worktree_retention_gate.TryAcquireCleanup();
			if (!cleanup) std::this_thread::yield();
		}
		UAM_ASSERT(cleanup); // Retain the genuinely acquired exclusive guard through the original survivor checks.
		if (remove_binding) UAM_ASSERT_EQ(uam::io::ReadTextFile(artifact / "foreign.txt"), "foreign survivor");
		fixture.AssertForeignSurvivors();
	}
}

UAM_TEST(WorktreeLaunchPreparationMergedPolicyChangeRevokesAndRecovers)
{
	if (!GitAvailableForTests()) return;
	for (const std::string_view policy : {"git", "auto", "base"})
	{
		LaunchFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		const uam::WorktreeLaunchPreparationResult initial = uam::PrepareWorktreeForLaunch(fixture.app, chat);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
		fixture.WaitForWorker(task);
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT(task->merged);
		if (policy == "git") fixture.app.settings.git_enabled = false;
		if (policy == "auto") fixture.app.settings.git_auto_worktree = false;
		if (policy == "base") fixture.app.settings.git_default_base_branch = "different";
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT(initial.cancellation->stop_requested());
		fixture.WaitForSave(task);
		UAM_ASSERT(*task->saved);
		UAM_ASSERT(fixture.app.worktree_launch_preparations.empty());
		UAM_ASSERT(fixture.app.worktree_retention_gate.TryAcquireCleanup());
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
		fixture.AssertForeignSurvivors();
	}
}

UAM_TEST(WorktreeLaunchPreparationRetrySupportsLinkedSourceCheckout)
{
	if (!GitAvailableForTests()) return;
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.workspace_directory = fixture.foreign.string();
	const uam::WorktreeLaunchPreparationResult initial = uam::PrepareWorktreeForLaunch(fixture.app, chat);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(task);
	initial.cancellation->request_stop();
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->prepared && task->prepared->operation.ok && !task->merged);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	fixture.WaitForWorker(task);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->merged);
	fixture.WaitForSave(task);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	fixture.AssertForeignSurvivors();
}

UAM_TEST(WorktreeLaunchPreparationFailedSavePreservesNewerEdits)
{
	if (!GitAvailableForTests()) return;
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(task);
	const fs::path primary = AppPaths::UamChatFilePath(fixture.app.data_root, chat.id);
	const fs::path preserved = fixture.temporary.root / "preserved.json";
	fs::copy_file(primary, preserved);
	fs::remove(primary);
	fs::create_directory(primary);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	fixture.WaitForSave(task);
	UAM_ASSERT(!*task->saved);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Failed);
	const fs::path artifact = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
	fs::remove(artifact / ".git");
	UAM_ASSERT(uam::io::WriteTextFile(artifact / "foreign.txt", "foreign survivor"));
	chat.title = "newer title";
	chat.memory_level = "off";
	chat.goal_command_revision = "newer goal";
	chat.messages.push_back({MessageRole::User, "newer body"});
	uam::acp_detail::ScheduleChatSave(fixture.app, chat, 0.0);
	const std::shared_ptr<std::optional<bool>> foreign_saved = std::make_shared<std::optional<bool>>();
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, chat.id, [foreign_saved](bool saved) { *foreign_saved = saved; }));
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	fixture.WaitForWorker(task);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT(chat.workspace_worktree_directory.empty());
	UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT_EQ(chat.title, "newer title");
	UAM_ASSERT_EQ(chat.memory_level, "off");
	UAM_ASSERT_EQ(chat.goal_command_revision, "newer goal");
	UAM_ASSERT_EQ(chat.messages.back().content, "newer body");
	fs::remove(primary);
	fs::copy_file(preserved, primary);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!foreign_saved->has_value() && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	UAM_ASSERT(foreign_saved->has_value() && **foreign_saved);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->workspace_worktree_directory.empty());
	UAM_ASSERT_EQ(saved->title, "newer title");
	UAM_ASSERT_EQ(saved->messages.back().content, "newer body");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(artifact / "foreign.txt"), "foreign survivor");
	fixture.AssertForeignSurvivors();
}

UAM_TEST(WorktreeLaunchPreparationExistingIsolationWaitsForPortOperation)
{
	if (!GitAvailableForTests()) return;
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT(uam::GitWorktreeService().CreateForChat(fixture.app, chat).ok);
	const std::string workspace = chat.workspace_worktree_directory;
	fixture.app.worktree_operation_chat_ids.insert(chat.id);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	UAM_ASSERT(fixture.app.worktree_launch_preparations.empty());
	UAM_ASSERT_EQ(chat.workspace_worktree_directory, workspace);
	fixture.app.worktree_operation_chat_ids.erase(chat.id);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	fixture.AssertForeignSurvivors();
}

UAM_TEST(WorktreeLaunchPreparationRestartRecoversOwnedReceipts)
{
	if (!GitAvailableForTests()) return;
	for (const std::string_view source_kind : {"git", "managed", "subdirectory"})
	{
		LaunchFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		if (source_kind == "managed")
		{
			const fs::path source = fixture.temporary.root / "managed-source";
			UAM_ASSERT(uam::io::WriteTextFile(source / "baseline.txt", "managed baseline"));
			chat.workspace_directory = source.string();
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
		}
		if (source_kind == "subdirectory")
		{
			fs::create_directories(fixture.repository / "nested-source");
			chat.workspace_directory = (fixture.repository / "nested-source").string();
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
		}
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> interrupted = fixture.app.worktree_launch_preparations.at(chat.id);
		fixture.WaitForWorker(interrupted);
		const uam::PreparedLaunchWorktree prepared = interrupted->worker.get();
		UAM_ASSERT(prepared.operation.ok);
		fixture.app.worktree_launch_preparations.clear();
		fixture.app.worktree_operation_chat_ids.clear();
		interrupted->lease.reset();
		chat = *ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
		UAM_ASSERT(chat.workspace_worktree_directory.empty());
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> recovered = fixture.app.worktree_launch_preparations.at(chat.id);
		fixture.WaitForWorker(recovered);
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT(recovered->merged);
		UAM_ASSERT_EQ(recovered->receipt_file, interrupted->receipt_file);
		UAM_ASSERT_EQ(chat.workspace_worktree_directory, prepared.metadata.workspace_worktree_directory);
		UAM_ASSERT_EQ(chat.workspace_branch_name, prepared.metadata.workspace_branch_name);
		fixture.WaitForSave(recovered);
		UAM_ASSERT(*recovered->saved);
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
		fixture.AssertForeignSurvivors();
	}
}

UAM_TEST(WorktreeLaunchPreparationRestartRejectsChangedOwnership)
{
	if (!GitAvailableForTests()) return;
	for (const std::string_view change : {"malformed", "oversized", "created", "source", "repository", "head", "shared", "policy", "legacy", "symlink", "ambiguous"})
	{
		LaunchFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> interrupted = fixture.app.worktree_launch_preparations.at(chat.id);
		fixture.WaitForWorker(interrupted);
		const uam::PreparedLaunchWorktree prepared = interrupted->worker.get();
		UAM_ASSERT(prepared.operation.ok);
		fixture.app.worktree_launch_preparations.clear();
		fixture.app.worktree_operation_chat_ids.clear();
		interrupted->lease.reset();
		const fs::path artifact = uam::paths::PathFromUtf8(prepared.metadata.workspace_worktree_directory);
		nlohmann::json receipt = nlohmann::json::parse(uam::io::ReadTextFile(interrupted->receipt_file));
		if (change == "malformed") UAM_ASSERT(uam::io::WriteTextFile(interrupted->receipt_file, "{}"));
		if (change == "oversized")
		{
			UAM_ASSERT(uam::io::WriteTextFile(interrupted->receipt_file, receipt.dump() + std::string(64 * 1024, ' ')));
			std::string validation_error;
			UAM_ASSERT(!uam::GitWorktreeService().ValidatePreparedWorkspace(fixture.app.data_root, interrupted->receipt_file, prepared.metadata, validation_error));
			UAM_ASSERT(validation_error.find("size limit") != std::string::npos);
		}
		if (change == "created") chat.created_at += "-replacement";
		if (change == "source") chat.workspace_directory = fixture.foreign.string();
		if (change == "repository")
		{
			receipt["repository"] = fixture.foreign.string();
			UAM_ASSERT(uam::io::WriteTextFile(interrupted->receipt_file, receipt.dump()));
		}
		if (change == "legacy")
		{
			receipt.erase("launchPolicy");
			UAM_ASSERT(uam::io::WriteTextFile(interrupted->receipt_file, receipt.dump()));
		}
		if (change == "policy") fixture.app.settings.git_default_base_branch = "changed-base";
		if (change == "head") UAM_ASSERT(RunGitForTest(artifact, "-c user.name=Fixture -c user.email=fixture@example.test commit --allow-empty -m foreign"));
		if (change == "shared")
		{
			ChatSession other = ChatDomainService().CreateNewChat("", "codex-cli");
			other.workspace_directory = artifact.string();
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, other));
		}
		if (change == "symlink")
		{
			const fs::path target = fixture.temporary.root / "receipt.json";
			fs::rename(interrupted->receipt_file, target);
			fs::create_symlink(target, interrupted->receipt_file);
		}
		if (change == "ambiguous") fs::copy_file(interrupted->receipt_file, interrupted->receipt_file.parent_path() / (interrupted->receipt_file.stem().string() + "-duplicate.json"));
		const std::string preserved_receipt = uam::io::ReadTextFile(interrupted->receipt_file);
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> rejected = fixture.app.worktree_launch_preparations.at(chat.id);
		fixture.WaitForWorker(rejected);
		const uam::WorktreeLaunchPreparationResult refusal = uam::PrepareWorktreeForLaunch(fixture.app, chat);
		UAM_ASSERT_EQ(refusal.state, uam::WorktreeLaunchPreparationState::Failed);
		if (change == "oversized") UAM_ASSERT(refusal.error.find("size limit") != std::string::npos);
		UAM_ASSERT(chat.workspace_worktree_directory.empty());
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
		UAM_ASSERT(fs::exists(artifact));
		UAM_ASSERT_EQ(uam::io::ReadTextFile(interrupted->receipt_file), preserved_receipt);
		UAM_ASSERT(RunGitForTest(fixture.repository, "show-ref --verify refs/heads/" + prepared.metadata.workspace_branch_name));
		fixture.AssertForeignSurvivors();
	}
}

#if !defined(_WIN32)
namespace
{
	struct FakeSvnLaunchFixture
	{
		TempDir temporary{"uam-fake-svn-launch"};
		uam::AppState app;
		fs::path source = temporary.root / "source";
		fs::path log = source / "updates.log";
		std::unique_ptr<ScopedEnvVar> path;
		std::unique_ptr<ScopedEnvVar> root;
		std::unique_ptr<ScopedEnvVar> mode;

		FakeSvnLaunchFixture(const std::string& behavior = "success")
		{
			fs::create_directories(source / ".svn");
			UAM_ASSERT(uam::io::WriteTextFile(source / "source.txt", "baseline"));
			const fs::path binary = temporary.root / "bin" / "svn";
			UAM_ASSERT(uam::io::WriteTextFile(binary, R"SH(#!/bin/sh
case "$1" in
info)
    case "$3" in
    wc-root) printf '%s\n' "$UAM_TEST_SVN_ROOT" ;;
    url) printf '%s\n' 'file:///private-fixture/trunk' ;;
    repos-uuid) printf '%s\n' 'private-fixture-uuid' ;;
    *) exit 2 ;;
    esac ;;
status)
    if [ -f "$UAM_TEST_SVN_ROOT/locked" ]; then printf '%s\n' '  L     .'; fi
    if [ "$UAM_TEST_SVN_MODE" = conflict ] && [ -f "$UAM_TEST_SVN_ROOT/updates.log" ]; then
        printf '%s\n' 'C       source.txt'
    fi ;;
update)
    printf '%s\n' update >> "$UAM_TEST_SVN_ROOT/updates.log"
    if [ "$UAM_TEST_SVN_MODE" = failure ]; then printf '%s\n' "svn: E155004: Run 'svn cleanup' to remove locks" >&2; exit 1; fi
    if [ "$UAM_TEST_SVN_MODE" = truncated ]; then /usr/bin/head -c 5000000 /dev/zero; exit 0; fi
    if [ "$UAM_TEST_SVN_MODE" = blocked ]; then
        while [ ! -f "$UAM_TEST_SVN_ROOT/release" ]; do /bin/sleep 0.02; done
    fi
    printf '%s' updated > "$UAM_TEST_SVN_ROOT/source.txt" ;;
*) exit 2 ;;
esac
)SH"));
			fs::permissions(binary, fs::perms::owner_all);
			const char* existing = std::getenv("PATH");
			path = std::make_unique<ScopedEnvVar>("PATH", binary.parent_path().string() + ":" + (existing ? existing : ""));
			root = std::make_unique<ScopedEnvVar>("UAM_TEST_SVN_ROOT", source.string());
			mode = std::make_unique<ScopedEnvVar>("UAM_TEST_SVN_MODE", behavior);
			app.data_root = temporary.root / "data";
			app.settings.git_enabled = false;
			app.settings.svn_update_before_chat = true;
			ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli", false);
			chat.workspace_directory = source.string();
			chat.svn_update_pending = true;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			app.chats.push_back(std::move(chat));
		}

		std::shared_ptr<uam::WorktreeLaunchPreparationTask> Start()
		{
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(app, app.chats.front()).state, uam::WorktreeLaunchPreparationState::Pending);
			return app.worktree_launch_preparations.at(app.chats.front().id);
		}

		void Wait(const std::shared_ptr<uam::WorktreeLaunchPreparationTask>& task)
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (task->worker.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			(void)uam::PollWorktreeLaunchPreparations(app);
		}

		void Save(const std::shared_ptr<uam::WorktreeLaunchPreparationTask>& task)
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (!task->saved && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(app);
				(void)uam::PollWorktreeLaunchPreparations(app);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			UAM_ASSERT(task->saved && *task->saved);
			UAM_ASSERT(!app.chats.front().svn_update_pending);
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(app, app.chats.front()).state, uam::WorktreeLaunchPreparationState::Ready);
		}
	};
}
#endif

UAM_TEST(SvnUpdateBeforeChatRunsOnceBeforeManagedIsolation)
{
#if !defined(_WIN32)
	if (!GitAvailableForTests()) return;
	FakeSvnLaunchFixture fixture;
	fixture.app.settings.git_enabled = true;
	fixture.app.chats.front().workspace_isolation_kind = uam::paths::kPendingGitWorktreeIsolationKind;
	const auto task = fixture.Start();
	fixture.Wait(task);
	UAM_ASSERT(task->merged);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(uam::paths::PathFromUtf8(fixture.app.chats.front().workspace_worktree_directory) / "source.txt"), "updated");
	fixture.Save(task);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.log), "update\n");
	UAM_ASSERT(!ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id)->svn_update_pending);
#endif
}

UAM_TEST(SvnUpdateBeforeChatOffResumeAndNonSvnSkipMutation)
{
#if !defined(_WIN32)
	for (const std::string& control : {"off", "disabled", "resume", "non-svn"})
	{
		FakeSvnLaunchFixture fixture;
		if (control == "off") fixture.app.settings.svn_update_before_chat = false;
		if (control == "disabled") fixture.app.settings.svn_enabled = false;
		if (control == "resume") fixture.app.chats.front().svn_update_pending = false;
		if (control == "non-svn")
		{
			const fs::path plain = fixture.temporary.root / "plain";
			fs::create_directories(plain);
			fixture.app.chats.front().workspace_directory = plain.string();
		}
		if (control == "resume") UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Ready);
		else
		{
			const auto task = fixture.Start();
			fixture.Wait(task);
			UAM_ASSERT(task->merged);
			fixture.Save(task);
		}
		UAM_ASSERT(!fs::exists(fixture.log));
		UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.source / "source.txt"), "baseline");
	}
#endif
}

UAM_TEST(SvnUpdateBeforeChatFailureAndConflictBlockLaunch)
{
#if !defined(_WIN32)
	for (const std::string& behavior : {"failure", "conflict"})
	{
		FakeSvnLaunchFixture fixture(behavior);
		const auto task = fixture.Start();
		fixture.Wait(task);
		UAM_ASSERT(!task->merged);
		UAM_ASSERT(fixture.app.chats.front().svn_update_pending);
		UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Failed);
		UAM_ASSERT(!task->error.empty());
		// svn's own E-code reaches the existing launch error instead of a generic failure.
		if (behavior == "failure") UAM_ASSERT(task->error.find("E155004: Run 'svn cleanup' to remove locks") != std::string::npos);
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	}
#endif
}

UAM_TEST(SvnUpdateBeforeChatCancellationAndStaleSourceCannotPublish)
{
#if !defined(_WIN32)
	for (const std::string& behavior : {"canceled", "stale", "truncated"})
	{
		FakeSvnLaunchFixture fixture(behavior == "truncated" ? "truncated" : "blocked");
		const auto task = fixture.Start();
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!fs::exists(fixture.log))
		{
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		ChatSession competing = fixture.app.chats.front();
		competing.id = "same-checkout-chat";
		fixture.app.chats.push_back(competing);
		if (behavior != "truncated")
		{
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.back()).state, uam::WorktreeLaunchPreparationState::Pending);
			UAM_ASSERT(!fixture.app.worktree_launch_preparations.contains(competing.id));
		}
		if (behavior == "canceled") task->cancellation->request_stop();
		else if (behavior == "stale")
		{
			fixture.app.chats.front().workspace_directory = (fixture.temporary.root / "changed-source").string();
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "release", "release"));
		}
		fixture.Wait(task);
		UAM_ASSERT(!task->merged);
		UAM_ASSERT(fixture.app.chats.front().svn_update_pending);
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
		UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.log), "update\n");
		if (behavior != "stale")
		{
			if (behavior == "truncated")
				UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Failed);
			// The interrupted receipt is verified first: a locked working copy still blocks another update.
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "locked", "locked"));
			const auto locked = fixture.Start();
			fixture.Wait(locked);
			UAM_ASSERT(!locked->merged);
			UAM_ASSERT(locked->error.find("Run svn cleanup") != std::string::npos);
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Failed);
			UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.log), "update\n");
			// A consistent working copy recovers the interrupted receipt by retrying the update once.
			fs::remove(fixture.source / "locked");
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "release", "release"));
			const auto retry = fixture.Start();
			fixture.Wait(retry);
			UAM_ASSERT_EQ(retry->merged, behavior == "canceled");
			UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.log), "update\nupdate\n");
		}
	}
#endif
}

UAM_TEST(SvnUpdateBeforeChatReceiptRecoveryNeverRepeatsCompletedUpdate)
{
#if !defined(_WIN32)
	FakeSvnLaunchFixture fixture;
	const auto task = fixture.Start();
	fixture.Wait(task);
	UAM_ASSERT(task->merged);
	fixture.Save(task);
	fixture.app.chats.front().svn_update_pending = true;
	const auto recovered = fixture.Start();
	fixture.Wait(recovered);
	UAM_ASSERT(recovered->merged);
	fixture.Save(recovered);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.log), "update\n");
	{
		ScopedEnvVar conflict("UAM_TEST_SVN_MODE", "conflict");
		fixture.app.chats.front().svn_update_pending = true;
		const auto conflicted = fixture.Start();
		fixture.Wait(conflicted);
		UAM_ASSERT(!conflicted->merged);
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Failed);
	}
	const fs::path changed = fixture.temporary.root / "changed-source";
	fs::create_directories(changed);
	fixture.app.chats.front().workspace_directory = changed.string();
	const auto wrong_source = fixture.Start();
	fixture.Wait(wrong_source);
	UAM_ASSERT(!wrong_source->merged);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.log), "update\n");
#endif
}

UAM_TEST(ManagedSnapshotDisabledAutomaticLaunchUsesOriginalFolder)
{
	TempDir temp("uam-snapshot-async-disabled");
	uam::AppState app;
	app.data_root = temp.root / "data";
	const fs::path source = temp.root / "source";
	fs::create_directories(source);
	const fs::path settings = temp.root / "settings.txt";
	UAM_ASSERT(uam::io::WriteTextFile(settings, "git_snapshot_plain_folders=false\n"));
	UAM_ASSERT(SettingsStore::Load(settings, app.settings).loaded);
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
	chat.workspace_directory = source.string();
	app.chats.push_back(chat);
	const uam::WorktreeLaunchPreparationResult result = uam::PrepareWorktreeForLaunch(app, app.chats.front());
	UAM_ASSERT_EQ(result.state, uam::WorktreeLaunchPreparationState::Ready);
	UAM_ASSERT(app.chats.front().workspace_isolation_kind.empty());
	UAM_ASSERT_EQ(app.chats.front().workspace_directory, source.string());
	UAM_ASSERT(app.worktree_launch_preparations.empty());
	UAM_ASSERT(!fs::exists(app.data_root / "worktrees"));
}

UAM_TEST(ManagedSnapshotPolicyChangeRevokesPreparedPublication)
{
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const fs::path source = fixture.temporary.root / "plain";
	fs::create_directories(source);
	UAM_ASSERT(uam::io::WriteTextFile(source / "keep.txt", "original"));
	chat.workspace_directory = source.string();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(task);
	const fs::path settings = fixture.temporary.root / "settings.txt";
	UAM_ASSERT(uam::io::WriteTextFile(settings, "git_snapshot_plain_folders=false\n"));
	UAM_ASSERT(SettingsStore::Load(settings, fixture.app.settings).loaded);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(!task->merged);
	UAM_ASSERT(task->cancellation->stop_requested());
	UAM_ASSERT(chat.workspace_worktree_directory.empty());
	UAM_ASSERT_EQ(ReadFile(source / "keep.txt"), std::string("original"));
	fixture.AssertForeignSurvivors();
}

UAM_TEST(ManagedSnapshotLegacyReceiptUsesEnabledPolicyDefaults)
{
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const fs::path source = fixture.temporary.root / "legacy-plain";
	fs::create_directories(source);
	UAM_ASSERT(uam::io::WriteTextFile(source / "keep.txt", "original"));
	chat.workspace_directory = source.string();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> interrupted = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(interrupted);
	const uam::PreparedLaunchWorktree prepared = interrupted->worker.get();
	UAM_ASSERT(prepared.operation.ok);
	nlohmann::json receipt = nlohmann::json::parse(ReadFile(interrupted->receipt_file));
	UAM_ASSERT(receipt["launchPolicy"].value("snapshotSvn", false));
	UAM_ASSERT(receipt["launchPolicy"].value("snapshotPlainFolders", false));
	receipt["launchPolicy"].erase("snapshotSvn");
	receipt["launchPolicy"].erase("snapshotPlainFolders");
	UAM_ASSERT(uam::io::WriteTextFile(interrupted->receipt_file, receipt.dump()));
	fixture.app.worktree_launch_preparations.clear();
	fixture.app.worktree_operation_chat_ids.clear();
	interrupted->lease.reset();
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> recovered = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(recovered);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(recovered->merged);
	UAM_ASSERT_EQ(chat.workspace_worktree_directory, prepared.metadata.workspace_worktree_directory);
	fixture.WaitForSave(recovered);
	UAM_ASSERT(*recovered->saved);
	fixture.AssertForeignSurvivors();
}

UAM_TEST(ManagedSnapshotOptOutPreservesInterruptedCreationReceipt)
{
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const fs::path source = fixture.temporary.root / "interrupted-plain";
	fs::create_directories(source);
	UAM_ASSERT(uam::io::WriteTextFile(source / "keep.txt", "original"));
	chat.workspace_directory = source.string();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> interrupted = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(interrupted);
	const uam::PreparedLaunchWorktree prepared = interrupted->worker.get();
	UAM_ASSERT(prepared.operation.ok);
	const std::string receipt = ReadFile(interrupted->receipt_file);
	UAM_ASSERT(uam::io::WriteTextFile(fs::path(prepared.metadata.workspace_worktree_directory) / "owned-change.txt", "preserve me"));
	fixture.app.worktree_launch_preparations.clear();
	fixture.app.worktree_operation_chat_ids.clear();
	interrupted->lease.reset();
	fixture.app.settings.git_snapshot_plain_folders = false;
	std::string error;
	UAM_ASSERT(!uam::GitWorktreeService().PrepareForFirstLaunch(fixture.app, chat, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(chat.workspace_isolation_kind, uam::paths::kPendingGitWorktreeIsolationKind);
	int intents = 0;
	UAM_ASSERT(!uam::GitWorktreeService().PrepareForChat(fixture.app, chat,
	    [&intents](const uam::GitWorktreeCreationIntent&, std::string&) { ++intents; return true; }, {}, false).ok);
	UAM_ASSERT_EQ(intents, 0);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT_EQ(ReadFile(interrupted->receipt_file), receipt);
	UAM_ASSERT_EQ(ReadFile(fs::path(prepared.metadata.workspace_worktree_directory) / "owned-change.txt"), std::string("preserve me"));
	UAM_ASSERT_EQ(ReadFile(source / "keep.txt"), std::string("original"));
	fixture.AssertForeignSurvivors();
}

UAM_TEST(ManagedSnapshotOptOutRejectsPartialOwnershipMetadata)
{
	TempDir temp("uam-snapshot-partial-ownership");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.git_snapshot_plain_folders = false;
	const fs::path source = temp.root / "plain";
	fs::create_directories(source);
	for (std::string ChatSession::* field : {&ChatSession::workspace_source_directory, &ChatSession::workspace_worktree_directory, &ChatSession::workspace_branch_name, &ChatSession::workspace_base_ref})
	{
		ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
		chat.workspace_directory = source.string();
		chat.*field = "retained-owned-metadata";
		std::string error;
		UAM_ASSERT(!uam::GitWorktreeService().PrepareForFirstLaunch(app, chat, &error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(chat.workspace_isolation_kind, uam::paths::kPendingGitWorktreeIsolationKind);
		UAM_ASSERT_EQ(chat.*field, std::string("retained-owned-metadata"));
		app.chats = {chat};
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(app, app.chats.front()).state, uam::WorktreeLaunchPreparationState::Failed);
		UAM_ASSERT(!fs::exists(app.data_root / "worktrees"));
	}
}


namespace
{
	std::string FirstSendGitOutput(const fs::path& source, const std::string& arguments)
	{
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
		    "git -C " + ShellQuoteForTest(source.string()) + " " + arguments, 10000);
		UAM_ASSERT(result.ok && result.exit_code == 0);
		return result.output;
	}

	fs::path SetFirstSendUnbornSource(LaunchFixture& fixture)
	{
		const fs::path source = fixture.temporary.root / "unborn-source";
		fs::create_directories(source);
		UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(source.string())));
		UAM_ASSERT(uam::io::WriteTextFile(source / "unborn.txt", "uncommitted first-send survivor\n"));
		ChatSession& chat = fixture.app.chats.front();
		chat.workspace_directory = fs::canonical(source).string();
		fixture.app.settings.git_enabled = true;
		fixture.app.settings.git_auto_worktree = true;
		fixture.app.settings.git_pull_before_chat = false;
		fixture.app.settings.git_default_base_branch.clear();
		fixture.app.settings.git_snapshot_plain_folders = true;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
		UAM_ASSERT(FirstSendGitOutput(source, "for-each-ref").empty());
		return fs::canonical(source);
	}

	void AssertFirstSendUnbornSourcePreserved(LaunchFixture& fixture, const fs::path& source,
	    const std::string& original_head)
	{
		UAM_ASSERT_EQ(ReadFile(source / ".git/HEAD"), original_head);
		UAM_ASSERT(FirstSendGitOutput(source, "for-each-ref").empty());
		const ProcessExecutionResult head = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
		    "git -C " + ShellQuoteForTest(source.string()) + " rev-parse --verify HEAD", 10000);
		UAM_ASSERT(!head.timed_out && !head.canceled && head.exit_code > 0);
		UAM_ASSERT_EQ(ReadFile(source / "unborn.txt"), std::string("uncommitted first-send survivor\n"));
		fixture.AssertForeignSurvivors();
	}

	uam::GitWorktreeCreationIntent CaptureFirstSendRejectedIntent(LaunchFixture& fixture)
	{
		ChatSession snapshot = fixture.app.chats.front();
		uam::GitWorktreeCreationIntent captured;
		int intents = 0;
		const auto result = uam::GitWorktreeService().PrepareForChat(fixture.app, snapshot,
		    [&captured, &intents](const uam::GitWorktreeCreationIntent& intent, std::string& error)
		    {
			    ++intents;
			    captured = intent;
			    error = "fixture rejects the pre-mutation creation intent";
			    return false;
		    }, {});
		UAM_ASSERT(!result.ok);
		UAM_ASSERT_EQ(intents, 1);
		UAM_ASSERT(result.message.find("fixture rejects") != std::string::npos);
		UAM_ASSERT(!captured.worktree.empty());
		UAM_ASSERT(!fs::exists(captured.worktree));
		UAM_ASSERT(!fs::is_symlink(captured.worktree));
		UAM_ASSERT(uam::paths::IsSameOrInsideRoot(fixture.app.data_root, captured.worktree));
		return captured;
	}

	fs::path SeedFirstSendLegacyEarlyReceipt(LaunchFixture& fixture,
	    const uam::GitWorktreeCreationIntent& captured)
	{
		const ChatSession& chat = fixture.app.chats.front();
		const AppSettings& settings = fixture.app.settings;
		// Historical on-disk early intent, not fabricated completed creation authority.
		// The owned target itself came from a rejecting public recorder above.
		const nlohmann::json policy = {{"version", 1}, {"requestedSource", chat.workspace_directory},
		    {"baseBranch", settings.git_default_base_branch}, {"pullBeforeChat", settings.git_pull_before_chat},
		    {"gitEnabled", settings.git_enabled}, {"automaticWorktree", settings.git_auto_worktree},
		    {"snapshotSvn", settings.git_snapshot_svn}, {"snapshotPlainFolders", settings.git_snapshot_plain_folders},
		    {"baseOverride", chat.git_base_branch_override ? nlohmann::json(*chat.git_base_branch_override) : nlohmann::json(nullptr)}};
		const nlohmann::json receipt = {{"chatId", chat.id}, {"createdAt", chat.created_at},
		    {"source", chat.workspace_directory}, {"repository", chat.workspace_directory},
		    {"worktree", uam::paths::Utf8PathString(captured.worktree)}, {"branch", ""}, {"baseSha", ""},
		    {"managedRepository", false}, {"launchPolicy", policy}};
		const fs::path path = fixture.app.data_root / ".worktree-launch" /
		    (uam::hashing::Hex64(uam::hashing::Fnv1a64(chat.id)) + "-legacy-first-send.json");
		UAM_ASSERT(uam::io::WriteTextFile(path, receipt.dump()));
		return path;
	}

	void AssertFirstSendOwnedBaseline(LaunchFixture& fixture, const fs::path& source)
	{
		const ChatSession& chat = fixture.app.chats.front();
		const fs::path worktree = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
		UAM_ASSERT(!worktree.empty() && worktree != source);
		UAM_ASSERT(uam::paths::IsSameOrInsideRoot(fixture.app.data_root / "worktrees", worktree));
		UAM_ASSERT(!chat.workspace_branch_name.empty() && !chat.workspace_base_ref.empty());
		UAM_ASSERT_EQ(ReadFile(worktree / "unborn.txt"), std::string("uncommitted first-send survivor\n"));
		UAM_ASSERT(uam::GitWorktreeService().Status(fixture.app, chat).managed_repository);
		const fs::path common = uam::paths::PathFromUtf8(uam::strings::Trim(
		    FirstSendGitOutput(worktree, "rev-parse --path-format=absolute --git-common-dir")));
		UAM_ASSERT(uam::paths::IsSameOrInsideRoot(fixture.app.data_root / "worktrees", common));
		UAM_ASSERT(common != source / ".git");
	}
}

UAM_TEST(WorktreeLaunchEarlyMissingBaseReceiptRetriesAfterLocalRefRepair)
{
	LaunchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	fixture.app.settings.git_default_base_branch = "first-send-missing-base";
	fixture.app.settings.git_pull_before_chat = false;
	uam::AcpQueuedUserPromptState queued;
	queued.id = "first-send-authored-queue";
	queued.text = "Preserve my unsent authored request.";
	chat.acp_queued_prompts.push_back(queued);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const auto failed = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(failed);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	const auto refusal = uam::PrepareWorktreeForLaunch(fixture.app, chat);
	UAM_ASSERT_EQ(refusal.state, uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT(!refusal.error.empty());
	const fs::path original_receipt = failed->receipt_file;
	const std::string receipt = ReadFile(original_receipt);
	const auto early = nlohmann::json::parse(receipt);
	UAM_ASSERT(early.at("branch").get<std::string>().empty() && early.at("baseSha").get<std::string>().empty());
	const fs::path absent_target = uam::paths::PathFromUtf8(early.at("worktree").get<std::string>());
	UAM_ASSERT(!fs::exists(absent_target) && !fs::is_symlink(absent_target));
	UAM_ASSERT(chat.workspace_worktree_directory.empty());
	UAM_ASSERT(RunGitForTest(fixture.repository, "branch first-send-missing-base"));
	UAM_ASSERT_EQ(ReadFile(original_receipt), receipt);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const auto retry = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(retry);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(retry->merged);
	UAM_ASSERT_EQ(retry->receipt_file, original_receipt);
	const auto advanced = nlohmann::json::parse(ReadFile(original_receipt));
	UAM_ASSERT_EQ(advanced.at("branch").get<std::string>(), chat.workspace_branch_name);
	UAM_ASSERT_EQ(advanced.at("baseSha").get<std::string>(), chat.workspace_base_ref);
	UAM_ASSERT(!chat.workspace_branch_name.empty() && !chat.workspace_base_ref.empty());
	UAM_ASSERT(!advanced.at("managedRepository").get<bool>());
	std::string validation_error;
	UAM_ASSERT(uam::GitWorktreeService().ValidatePreparedWorkspace(
	    fixture.app.data_root, original_receipt, chat, validation_error));
	UAM_ASSERT(validation_error.empty());
	fixture.WaitForSave(retry);
	UAM_ASSERT(*retry->saved);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	UAM_ASSERT_EQ(chat.messages.front().content, std::string("saved before preparation"));
	UAM_ASSERT_EQ(chat.acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, queued.id);
	UAM_ASSERT_EQ(chat.acp_queued_prompts.front().text, queued.text);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->workspace_worktree_directory == chat.workspace_worktree_directory);
	UAM_ASSERT_EQ(saved->messages.front().content, std::string("saved before preparation"));
	UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(saved->acp_queued_prompts.front().text, queued.text);
	fixture.AssertForeignSurvivors();
}

UAM_TEST(WorktreeLaunchLegacyEarlyUnbornReceiptRecoversPrivateBaseline)
{
	LaunchFixture fixture;
	const fs::path source = SetFirstSendUnbornSource(fixture);
	const std::string original_head = ReadFile(source / ".git/HEAD");
	const auto intent = CaptureFirstSendRejectedIntent(fixture);
	const fs::path path = SeedFirstSendLegacyEarlyReceipt(fixture, intent);
	UAM_ASSERT(nlohmann::json::parse(ReadFile(path)).at("branch").get<std::string>().empty());
	ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const auto task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(task);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->merged);
	UAM_ASSERT_EQ(task->receipt_file, path);
	const auto advanced = nlohmann::json::parse(ReadFile(path));
	UAM_ASSERT(advanced.at("managedRepository").get<bool>());
	UAM_ASSERT_EQ(advanced.at("branch").get<std::string>(), chat.workspace_branch_name);
	UAM_ASSERT_EQ(advanced.at("baseSha").get<std::string>(), chat.workspace_base_ref);
	UAM_ASSERT(!chat.workspace_branch_name.empty() && !chat.workspace_base_ref.empty());
	std::string validation_error;
	UAM_ASSERT(uam::GitWorktreeService().ValidatePreparedWorkspace(
	    fixture.app.data_root, path, chat, validation_error));
	UAM_ASSERT(validation_error.empty());
	fixture.WaitForSave(task);
	UAM_ASSERT(*task->saved);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	AssertFirstSendOwnedBaseline(fixture, source);
	AssertFirstSendUnbornSourcePreserved(fixture, source, original_head);
}

UAM_TEST(WorktreeLaunchLegacyEarlyUnbornReceiptPreservesOccupiedTarget)
{
	LaunchFixture fixture;
	const fs::path source = SetFirstSendUnbornSource(fixture);
	const std::string original_head = ReadFile(source / ".git/HEAD");
	const auto intent = CaptureFirstSendRejectedIntent(fixture);
	const fs::path path = SeedFirstSendLegacyEarlyReceipt(fixture, intent);
	const std::string receipt = ReadFile(path);
	UAM_ASSERT(uam::io::WriteTextFile(intent.worktree / "foreign.txt", "foreign target survivor\n"));
	ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const auto task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(task);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	const auto refusal = uam::PrepareWorktreeForLaunch(fixture.app, chat);
	UAM_ASSERT_EQ(refusal.state, uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT(!refusal.error.empty() && !task->merged);
	UAM_ASSERT(chat.workspace_worktree_directory.empty());
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT_EQ(ReadFile(path), receipt);
	UAM_ASSERT_EQ(ReadFile(intent.worktree / "foreign.txt"), std::string("foreign target survivor\n"));
	AssertFirstSendUnbornSourcePreserved(fixture, source, original_head);
}

UAM_TEST(WorktreeLaunchFreshUnbornAutoIsolationPreservesExplicitPolicyBoundaries)
{
	for (const std::string_view control : {"automatic", "explicit-base", "snapshot-opt-out"})
	{
		LaunchFixture fixture;
		const fs::path source = SetFirstSendUnbornSource(fixture);
		const std::string original_head = ReadFile(source / ".git/HEAD");
		ChatSession& chat = fixture.app.chats.front();
		if (control == "explicit-base") chat.git_base_branch_override = "missing-explicit-base";
		if (control == "snapshot-opt-out") fixture.app.settings.git_snapshot_plain_folders = false;
		const auto initial = uam::PrepareWorktreeForLaunch(fixture.app, chat);
		if (control == "automatic")
		{
			UAM_ASSERT_EQ(initial.state, uam::WorktreeLaunchPreparationState::Pending);
			const auto task = fixture.app.worktree_launch_preparations.at(chat.id);
			fixture.WaitForWorker(task);
			(void)uam::PollWorktreeLaunchPreparations(fixture.app);
			UAM_ASSERT(task->merged);
			fixture.WaitForSave(task);
			UAM_ASSERT(*task->saved);
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
			AssertFirstSendOwnedBaseline(fixture, source);
		}
		else
		{
			if (initial.state == uam::WorktreeLaunchPreparationState::Pending)
			{
				const auto task = fixture.app.worktree_launch_preparations.at(chat.id);
				fixture.WaitForWorker(task);
				(void)uam::PollWorktreeLaunchPreparations(fixture.app);
				UAM_ASSERT(!task->merged);
			}
			const auto refusal = uam::PrepareWorktreeForLaunch(fixture.app, chat);
			UAM_ASSERT_EQ(refusal.state, uam::WorktreeLaunchPreparationState::Failed);
			UAM_ASSERT(!refusal.error.empty());
			UAM_ASSERT(chat.workspace_worktree_directory.empty());
			UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
		}
		AssertFirstSendUnbornSourcePreserved(fixture, source, original_head);
	}
}


namespace
{
	struct UnbornGlobalBaseSourceWitness
	{
		struct File { fs::path path; bool existed; std::string bytes; };
		std::vector<File> files;
		std::string original_refs;
		explicit UnbornGlobalBaseSourceWitness(const fs::path& source)
		{
			original_refs = FirstSendGitOutput(source, "for-each-ref " + ShellQuoteForTest("--format=%(refname) %(objectname)"));
			for (const std::string_view relative : {".git/HEAD", ".git/index", ".git/config", ".git/FETCH_HEAD", "unborn.txt"})
			{
				const fs::path path = source / relative;
				const bool exists = fs::exists(path);
				files.push_back({path, exists, exists ? ReadFile(path) : std::string{}});
			}
		}
		void AssertPreserved(const fs::path& source) const
		{
			for (const File& file : files)
			{
				UAM_ASSERT_EQ(fs::exists(file.path), file.existed);
				if (file.existed) UAM_ASSERT_EQ(ReadFile(file.path), file.bytes);
			}
			const auto head = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
			    "git -C " + ShellQuoteForTest(source.string()) + " rev-parse --verify HEAD", 10000);
			UAM_ASSERT(!head.timed_out && !head.canceled && !head.output_truncated && head.exit_code > 0);
		}
	};

	uam::AcpQueuedUserPromptState SeedUnbornGlobalBaseAuthoredQueue(LaunchFixture& fixture)
	{
		uam::AcpQueuedUserPromptState queued;
		queued.id = "unborn-global-base-authored-queue";
		queued.text = "Keep this authored request through global base selection.";
		fixture.app.chats.front().acp_queued_prompts.push_back(queued);
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.front()));
		return queued;
	}

	void AssertUnbornGlobalBaseAuthoredQueue(const ChatSession& chat, const uam::AcpQueuedUserPromptState& queued)
	{
		UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
		UAM_ASSERT_EQ(chat.messages.front().content, std::string("saved before preparation"));
		UAM_ASSERT_EQ(chat.acp_queued_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, queued.id);
		UAM_ASSERT_EQ(chat.acp_queued_prompts.front().text, queued.text);
	}
}

UAM_TEST(WorktreeLaunchUnbornHeadHonorsExistingGlobalCommittedBase)
{
	LaunchFixture fixture;
	const fs::path source = SetFirstSendUnbornSource(fixture);
	// Import only a committed ref from another fixture-owned local repository. HEAD stays unborn.
	UAM_ASSERT(RunGitForTest(source, "fetch " + ShellQuoteForTest(fixture.repository.string()) + " HEAD:refs/heads/chosen-global-base"));
	UAM_ASSERT(RunGitForTest(source, "add unborn.txt"));
	UAM_ASSERT(uam::io::WriteTextFile(source / "unborn.txt", "unrelated staged plus dirty unborn source bytes\n"));
	fixture.app.settings.git_default_base_branch = "chosen-global-base";
	const std::string selected = uam::strings::Trim(FirstSendGitOutput(source, "rev-parse --verify refs/heads/chosen-global-base"));
	const auto queued = SeedUnbornGlobalBaseAuthoredQueue(fixture);
	const UnbornGlobalBaseSourceWitness witness(source);
	ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT(!chat.git_base_branch_override.has_value());
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const auto task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(task);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->prepared && task->prepared->operation.ok && task->merged);
	// A snapshot of current unborn files is a wrong base even if it creates a valid owned worktree.
	UAM_ASSERT_EQ(chat.workspace_base_ref, selected);
	UAM_ASSERT(!task->prepared->operation.status.managed_repository);
	const fs::path worktree = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
	UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(chat) && fs::is_directory(worktree) && worktree != source);
	UAM_ASSERT_EQ(ReadFile(worktree / "source.txt"), std::string("source survivor\n"));
	UAM_ASSERT(!fs::exists(worktree / "unborn.txt"));
	std::string error;
	UAM_ASSERT(uam::GitWorktreeService().ValidatePreparedWorkspace(fixture.app.data_root, task->receipt_file, chat, error));
	UAM_ASSERT(error.empty());
	fixture.WaitForSave(task);
	UAM_ASSERT(*task->saved);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	witness.AssertPreserved(source);
	// Normal Git worktree creation adds only its own branch; pre-existing refs remain byte-identical values.
	const std::string own_ref = "refs/heads/" + chat.workspace_branch_name;
	UAM_ASSERT_EQ(uam::strings::Trim(FirstSendGitOutput(source, "rev-parse --verify " + ShellQuoteForTest(own_ref))), selected);
	std::string remaining_refs;
	const std::string all_refs = FirstSendGitOutput(source, "for-each-ref " + ShellQuoteForTest("--format=%(refname) %(objectname)"));
	const std::string own_line = own_ref + " " + selected + "\n";
	const std::size_t own_position = all_refs.find(own_line);
	UAM_ASSERT(own_position != std::string::npos && (own_position == 0 || all_refs[own_position - 1] == '\n'));
	remaining_refs = all_refs;
	remaining_refs.erase(own_position, own_line.size());
	UAM_ASSERT_EQ(remaining_refs, witness.original_refs);
	AssertUnbornGlobalBaseAuthoredQueue(chat, queued);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->workspace_base_ref == selected);
	AssertUnbornGlobalBaseAuthoredQueue(*saved, queued);
	fixture.AssertForeignSurvivors();
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
}

UAM_TEST(WorktreeLaunchUnbornHeadRefusesMissingGlobalBaseWithoutSnapshot)
{
	LaunchFixture fixture;
	const fs::path source = SetFirstSendUnbornSource(fixture);
	UAM_ASSERT(RunGitForTest(source, "add unborn.txt"));
	fixture.app.settings.git_default_base_branch = "missing-global-base";
	const auto queued = SeedUnbornGlobalBaseAuthoredQueue(fixture);
	const UnbornGlobalBaseSourceWitness witness(source);
	ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT(!chat.git_base_branch_override.has_value());
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const auto task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(task);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->prepared && !task->prepared->operation.ok && !task->merged);
	const auto refused = uam::PrepareWorktreeForLaunch(fixture.app, chat);
	UAM_ASSERT_EQ(refused.state, uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT(refused.error.find("missing-global-base") != std::string::npos);
	UAM_ASSERT(chat.workspace_worktree_directory.empty() && chat.workspace_branch_name.empty() && chat.workspace_base_ref.empty());
	UAM_ASSERT_EQ(chat.workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT(!task->saved.has_value());
	const fs::path artifact_root = fixture.app.data_root / "worktrees";
	if (fs::exists(artifact_root))
	{
		for (const auto& entry : fs::recursive_directory_iterator(artifact_root))
		{
			UAM_ASSERT(entry.path().filename().string().find("managed-repository-") != 0);
			UAM_ASSERT(entry.is_directory() && !entry.is_symlink());
		}
	}
	witness.AssertPreserved(source);
	UAM_ASSERT_EQ(FirstSendGitOutput(source, "for-each-ref " + ShellQuoteForTest("--format=%(refname) %(objectname)")), witness.original_refs);
	AssertUnbornGlobalBaseAuthoredQueue(chat, queued);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->workspace_worktree_directory.empty());
	AssertUnbornGlobalBaseAuthoredQueue(*saved, queued);
	fixture.AssertForeignSurvivors();
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
}


UAM_TEST(WorktreeLaunchUnbornIgnoredDependenciesPreserveQueuedInput)
{
	LaunchFixture fixture;
	const fs::path source = SetFirstSendUnbornSource(fixture);
	const fs::path plugin = source / ".build/checkouts/swift-argument-parser/Plugins";
	fs::create_directories(plugin);
	UAM_ASSERT(uam::io::WriteTextFile(source / ".gitignore", ".build/\n"));
	fs::create_directory_symlink(fixture.foreign, plugin / "GenerateCommon");
	const std::string original_head = ReadFile(source / ".git/HEAD");
	ChatSession& chat = fixture.app.chats.front();
	uam::AcpQueuedUserPromptState queued; queued.id = "one-authored-workspace-input";
	queued.text = "Prepare this workspace once; do not send from this fixture.";
	chat.acp_queued_prompts = {queued}; chat.acp_queue_held = true;
	const auto messages = chat.messages;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const auto task = fixture.app.worktree_launch_preparations.at(chat.id);
	fixture.WaitForWorker(task); (void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->merged);
	fixture.WaitForSave(task); UAM_ASSERT(*task->saved);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	UAM_ASSERT_EQ(ReadFile(fs::path(chat.workspace_worktree_directory) / "unborn.txt"), std::string("uncommitted first-send survivor\n"));
	UAM_ASSERT(!fs::exists(fs::path(chat.workspace_worktree_directory) / ".build"));
	UAM_ASSERT_EQ(chat.messages, messages);
	UAM_ASSERT_EQ(chat.acp_queued_prompts, (std::vector<uam::AcpQueuedUserPromptState>{queued}));
	UAM_ASSERT(chat.acp_queue_held && chat.acp_dispatched_queued_prompt_count == 0);
	UAM_ASSERT(fixture.app.acp_sessions.empty());
	AssertFirstSendUnbornSourcePreserved(fixture, source, original_head);
}
