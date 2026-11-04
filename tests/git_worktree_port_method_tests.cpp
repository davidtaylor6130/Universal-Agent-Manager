#include "test_harness.h"

using namespace uam_test;

namespace
{
	std::string PortGitOutput(const fs::path& repository, const std::string& arguments)
	{
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
		    "git -C " + ShellQuoteForTest(repository.string()) + " " + arguments, 120000);
		UAM_ASSERT(result.ok && result.exit_code == 0);
		return uam::strings::Trim(result.output);
	}

	struct PortFixture
	{
		TempDir temporary{"uam-staged-port"};
		uam::AppState app;
		fs::path source = temporary.root / "source";
		fs::path worktree;
		std::string source_head;
		std::string chat_head;
		std::string chat_branch;

		explicit PortFixture(const std::string& method)
		{
			fs::create_directories(source);
			UAM_ASSERT(RunGitForTest(source, "init"));
			UAM_ASSERT(RunGitForTest(source, "config user.email fixture@example.test"));
			UAM_ASSERT(RunGitForTest(source, "config user.name Fixture"));
			UAM_ASSERT(uam::io::WriteTextFile(source / ".gitignore", "private.txt\n"));
			UAM_ASSERT(uam::io::WriteTextFile(source / "survivor.txt", "source survivor\n"));
			UAM_ASSERT(RunGitForTest(source, "add ."));
			UAM_ASSERT(RunGitForTest(source, "commit -m baseline"));
			UAM_ASSERT(RunGitForTest(source, "branch survivor"));
			app.data_root = temporary.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
			chat.workspace_directory = source.string();
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(uam::GitWorktreeService().CreateForChat(app, app.chats.front()).ok);
			worktree = uam::paths::PathFromUtf8(app.chats.front().workspace_worktree_directory);
			chat_branch = app.chats.front().workspace_branch_name;
			UAM_ASSERT(uam::io::WriteTextFile(worktree / "chat.txt", "committed chat change\n"));
			UAM_ASSERT(RunGitForTest(worktree, "-c user.email=fixture@example.test -c user.name=Fixture add chat.txt"));
			UAM_ASSERT(RunGitForTest(worktree, "-c user.email=fixture@example.test -c user.name=Fixture commit -m chat"));
			UAM_ASSERT(uam::io::WriteTextFile(worktree / "private.txt", "worktree survivor\n"));
			UAM_ASSERT(uam::io::WriteTextFile(source / "private.txt", "source private survivor\n"));
			source_head = PortGitOutput(source, "rev-parse HEAD");
			chat_head = PortGitOutput(worktree, "rev-parse HEAD");
			const fs::path settings = temporary.root / "settings.txt";
			UAM_ASSERT(uam::io::WriteTextFile(settings, "git_enabled=true\ngit_port_method=" + method + "\n"));
			UAM_ASSERT(SettingsStore::Load(settings, app.settings).loaded);
		}

		void AssertRetained()
		{
			UAM_ASSERT_EQ(PortGitOutput(source, "rev-parse HEAD"), source_head);
			UAM_ASSERT_EQ(PortGitOutput(worktree, "rev-parse HEAD"), chat_head);
			UAM_ASSERT_EQ(PortGitOutput(source, "rev-parse refs/heads/" + chat_branch), chat_head);
			UAM_ASSERT_EQ(app.chats.front().workspace_worktree_directory, worktree.string());
			UAM_ASSERT_EQ(uam::io::ReadTextFile(source / "survivor.txt"), "source survivor\n");
			UAM_ASSERT_EQ(uam::io::ReadTextFile(source / "private.txt"), "source private survivor\n");
			UAM_ASSERT_EQ(uam::io::ReadTextFile(worktree / "private.txt"), "worktree survivor\n");
			UAM_ASSERT(RunGitForTest(source, "show-ref --verify refs/heads/survivor"));
		}
	};
}

UAM_TEST(GitWorktreePortMergeStagesOwnedCommitWithoutCommitting)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("merge");
	UAM_ASSERT(uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front()).ok);
	UAM_ASSERT_EQ(PortGitOutput(fixture.source, "rev-parse MERGE_HEAD"), fixture.chat_head);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.source / "chat.txt"), "committed chat change\n");
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortSquashStagesOwnedCommitWithoutCommitting)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("squash");
	UAM_ASSERT(uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front()).ok);
	const fs::path squash_message = uam::paths::PathFromUtf8(PortGitOutput(fixture.source, "rev-parse --path-format=absolute --git-path SQUASH_MSG"));
	UAM_ASSERT(fs::exists(squash_message));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.source / "chat.txt"), "committed chat change\n");
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortCancellationBeforeMutationPreservesSource)
{
	PortFixture fixture("merge");
	std::stop_source cancel;
	cancel.request_stop();
	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PortCommittedChanges(fixture.app, fixture.app.chats.front(), cancel.get_token());
	UAM_ASSERT(!result.ok);
	UAM_ASSERT(result.operation_receipt.empty());
	UAM_ASSERT(!fs::exists(fixture.source / "chat.txt"));
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortRecreatedChatCancelsOwnedRequest)
{
	PortFixture fixture("squash");
	ChatSession& chat = fixture.app.chats.front();
	const std::shared_ptr<uam::AppState::GitPortRequest> request = std::make_shared<uam::AppState::GitPortRequest>();
	request->root = fixture.app.data_root;
	request->incarnation = chat.metadata_incarnation;
	request->workspace = chat.workspace_directory;
	request->source = chat.workspace_source_directory;
	request->worktree = chat.workspace_worktree_directory;
	request->branch = chat.workspace_branch_name;
	request->base = chat.workspace_base_ref;
	fixture.app.git_port_requests[chat.id] = request;
	chat.metadata_incarnation = std::make_shared<const uint8_t>(0);
	uam::PollGitPortRequests(fixture.app);
	UAM_ASSERT(request->cancellation->stop_requested());
	UAM_ASSERT(fixture.app.git_port_requests.empty());
	UAM_ASSERT(!fs::exists(fixture.source / "chat.txt"));
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortMergeRefusesDirtySourceBeforeReceipt)
{
	PortFixture fixture("merge");
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "survivor.txt", "new unrelated change\n"));
	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front());
	UAM_ASSERT(!result.ok && result.operation_receipt.empty());
	UAM_ASSERT(uam::io::ReadTextFile(fixture.source / "survivor.txt") == "new unrelated change\n");
	UAM_ASSERT(PortGitOutput(fixture.source, "rev-parse HEAD") == fixture.source_head);
}

UAM_TEST(GitWorktreePortMergeRetainsOwnedConflictRecovery)
{
	PortFixture fixture("merge");
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "chat.txt", "source conflict\n"));
	UAM_ASSERT(RunGitForTest(fixture.source, "add chat.txt"));
	UAM_ASSERT(RunGitForTest(fixture.source, "commit -m source-conflict"));
	fixture.source_head = PortGitOutput(fixture.source, "rev-parse HEAD");
	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front());
	UAM_ASSERT(!result.ok && result.port_state == "conflicts");
	UAM_ASSERT(fs::exists(result.operation_receipt));
	UAM_ASSERT(PortGitOutput(fixture.source, "rev-parse MERGE_HEAD") == fixture.chat_head);
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortSkippedWorkerSettlesExpiredHandler)
{
	PortFixture fixture("merge");
	const std::shared_ptr<uam::AppState::GitPortRequest> request = std::make_shared<uam::AppState::GitPortRequest>();
	request->lifetime_bound = true;
	request->lease = fixture.app.worktree_retention_gate.TryAcquireMutation();
	UAM_ASSERT(request->lease);
	fixture.app.git_port_requests[fixture.app.chats.front().id] = request;
	fixture.app.worktree_operation_chat_ids.insert(fixture.app.chats.front().id);
	uam::PollGitPortRequests(fixture.app);
	UAM_ASSERT(request->cancellation->stop_requested());
	UAM_ASSERT(request->worker_state.load() == uam::AppState::GitPortRequest::WorkerState::Finished);
	UAM_ASSERT(fixture.app.git_port_requests.empty());
	UAM_ASSERT(fixture.app.worktree_operation_chat_ids.empty());
	UAM_ASSERT(fixture.app.worktree_retention_gate.TryAcquireCleanup());
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortDestroyedHandlerWaitsForRunningWorker)
{
	PortFixture fixture("squash");
	const std::shared_ptr<uam::AppState::GitPortRequest> request = std::make_shared<uam::AppState::GitPortRequest>();
	request->lifetime_bound = true;
	request->lease = fixture.app.worktree_retention_gate.TryAcquireMutation();
	UAM_ASSERT(request->lease);
	request->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Running);
	fixture.app.git_port_requests[fixture.app.chats.front().id] = request;
	fixture.app.worktree_operation_chat_ids.insert(fixture.app.chats.front().id);
	uam::PollGitPortRequests(fixture.app);
	UAM_ASSERT(request->cancellation->stop_requested());
	UAM_ASSERT(fixture.app.git_port_requests.at(fixture.app.chats.front().id) == request);
	UAM_ASSERT(fixture.app.worktree_operation_chat_ids.contains(fixture.app.chats.front().id));
	UAM_ASSERT(!fixture.app.worktree_retention_gate.TryAcquireCleanup());
	request->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Finished);
	uam::PollGitPortRequests(fixture.app);
	UAM_ASSERT(fixture.app.git_port_requests.empty() && fixture.app.worktree_operation_chat_ids.empty());
	UAM_ASSERT(fixture.app.worktree_retention_gate.TryAcquireCleanup());
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortSettlementPreservesReplacementRequest)
{
	PortFixture fixture("merge");
	const ChatSession& chat = fixture.app.chats.front();
	const std::shared_ptr<uam::AppState::GitPortRequest> old = std::make_shared<uam::AppState::GitPortRequest>();
	old->cancellation->request_stop();
	old->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Finished);
	const std::shared_ptr<uam::AppState::GitPortRequest> replacement = std::make_shared<uam::AppState::GitPortRequest>();
	replacement->root = fixture.app.data_root;
	replacement->incarnation = chat.metadata_incarnation;
	replacement->workspace = chat.workspace_directory;
	replacement->source = chat.workspace_source_directory;
	replacement->worktree = chat.workspace_worktree_directory;
	replacement->branch = chat.workspace_branch_name;
	replacement->base = chat.workspace_base_ref;
	replacement->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Running);
	fixture.app.git_port_requests[chat.id] = replacement;
	fixture.app.worktree_operation_chat_ids.insert(chat.id);
	uam::PollGitPortRequests(fixture.app);
	UAM_ASSERT(fixture.app.git_port_requests.at(chat.id) == replacement);
	UAM_ASSERT(!replacement->cancellation->stop_requested());
	UAM_ASSERT(fixture.app.worktree_operation_chat_ids.contains(chat.id));
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreeSharedChatRefusesDestructiveOperations)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("patch");
	ChatSession alias = fixture.app.chats.front();
	alias.id += "-alias";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, alias));
	// Deliberately omit the persisted alias from the target-only async snapshot.
	UAM_ASSERT(uam::io::WriteTextFile(fixture.worktree / "chat.txt", "unsaved shared edits\n"));
	const uam::GitWorktreeOperationResult discard = uam::GitWorktreeService().DiscardChatChanges(fixture.app, fixture.app.chats.front());
	UAM_ASSERT(!discard.ok);
	UAM_ASSERT(discard.message.find("Another chat") != std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.worktree / "chat.txt"), "unsaved shared edits\n");
	const uam::GitWorktreeOperationResult port = uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front());
	UAM_ASSERT(!port.ok);
	UAM_ASSERT(!fs::exists(fixture.source / "chat.txt"));
	std::string removal_error;
	UAM_ASSERT(!uam::GitWorktreeService().RemoveUnusedBranchWorktree(fixture.app, fixture.app.chats.front(), &removal_error));
	UAM_ASSERT(removal_error.find("Another chat") != std::string::npos);
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreeUnsavedSharedChatRefusesDiscard)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("patch");
	ChatSession alias = fixture.app.chats.front();
	alias.id += "-unsaved-alias";
	fixture.app.chats.push_back(alias);
	UAM_ASSERT(!uam::GitWorktreeService().DiscardChatChanges(fixture.app, fixture.app.chats.front()).ok);
	fixture.AssertRetained();
}
