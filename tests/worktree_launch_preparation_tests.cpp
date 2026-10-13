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
		UAM_ASSERT(fixture.app.worktree_retention_gate.TryAcquireCleanup());
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
