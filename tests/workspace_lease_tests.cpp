#include "test_harness.h"
#include "common/paths/workspace_lease.h"
#include "app/worktree_retention_service.h"

#include <thread>

using namespace uam_test;

UAM_TEST(WorkspaceLeaseProtectsOutstandingMutationsThroughCrossThreadCompletion)
{
	uam::WorkspaceLeaseGate gate;
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> request = gate.TryAcquireMutation();
	UAM_ASSERT(request && !request->IsExclusive());
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> completion = request;
	request.reset();
	UAM_ASSERT(!gate.TryAcquireCleanup());
	std::jthread worker([lease = std::move(completion)]() mutable { lease.reset(); });
	worker.join();
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> cleanup = gate.TryAcquireCleanup();
	UAM_ASSERT(cleanup && cleanup->IsExclusive());
	UAM_ASSERT(!gate.TryAcquireCleanup());
	UAM_ASSERT(!gate.TryAcquireMutation());
	cleanup.reset();
	UAM_ASSERT(gate.TryAcquireMutation());
}

UAM_TEST(WorkspaceLeaseStateSurvivesCopiesAndOwnerDestruction)
{
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> cleanup;
	{
		uam::WorkspaceLeaseGate owner;
		uam::WorkspaceLeaseGate snapshot = owner;
		std::shared_ptr<uam::WorkspaceLeaseGate::Lease> request = snapshot.TryAcquireMutation();
		UAM_ASSERT(!owner.TryAcquireCleanup());
		request.reset();
		cleanup = owner.TryAcquireCleanup();
		UAM_ASSERT(cleanup);
		UAM_ASSERT(!snapshot.TryAcquireMutation());
	}
	std::jthread worker([lease = std::move(cleanup)]() mutable { lease.reset(); });
	worker.join();
}

UAM_TEST(WorkspaceLeaseRoutesPreserveOrdinaryChatInteraction)
{
	for (const std::string_view action : {"importLocalChats", "moveChatWorkspace", "createSession", "resolveStartupNativeImports", "branchFromMessage", "deleteFolders"})
		UAM_ASSERT(uam::RequiresWorkspaceMutationLease(action));
	for (const std::string_view action : {"getChatMessages", "selectSession", "sendAcpMessage", "cancelAcpTurn", "stopAcpSession", "setChatModel"})
		UAM_ASSERT(!uam::RequiresWorkspaceMutationLease(action));
}

UAM_TEST(WorktreeRetentionBackgroundServiceRevalidatesImportedReferencesBeforeRemoval)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-worktree-retention-service");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "baseline\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatFolder folder;
	folder.id = "folder-retention";
	folder.directory = repo.string();
	app.folders.push_back(folder);
	ChatSession chat;
	chat.id = "chat-retention-service";
	chat.folder_id = folder.id;
	chat.workspace_directory = repo.string();
	UAM_ASSERT(uam::GitWorktreeService().CreateForChat(app, chat).ok);
	const fs::path worktree = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
	app.chats.push_back(chat);
	const fs::file_time_type old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 15);
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(repo / ".git")) fs::last_write_time(entry.path(), old);
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(worktree)) fs::last_write_time(entry.path(), old);
	fs::last_write_time(worktree, old);
	uam::WorktreeRetentionService service;
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now() + std::chrono::seconds(40);
	const std::function<void()> wait_for_worker = [&]()
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (service.WorkerPending() && std::chrono::steady_clock::now() < deadline)
		{
			service.Poll(app, now);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(!service.WorkerPending());
	};
	service.Poll(app, now);
	wait_for_worker();
	UAM_ASSERT(fs::exists(worktree));
	UAM_ASSERT(!service.Pending());
	app.chats.clear();
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> import = app.worktree_retention_gate.TryAcquireMutation();
	UAM_ASSERT(import);
	now += std::chrono::hours(7);
	service.Poll(app, now);
	wait_for_worker();
	UAM_ASSERT(service.Pending());
	UAM_ASSERT(fs::exists(worktree));
	// Publication occurs while the import still holds its lease, before cleanup may acquire exclusivity.
	app.chats.push_back(chat);
	import.reset();
	service.Poll(app, now);
	wait_for_worker();
	UAM_ASSERT(fs::exists(worktree));
	UAM_ASSERT(!service.Pending());
	app.chats.clear();
	now += std::chrono::hours(7);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	service.Poll(app, now);
	UAM_ASSERT(!service.Pending());
	UAM_ASSERT(fs::exists(worktree));
	app.acp_sessions.clear();
	app.folders.clear();
	service.Poll(app, now);
	wait_for_worker();
	UAM_ASSERT(!fs::exists(worktree));
	UAM_ASSERT(app.worktree_retention_gate.TryAcquireMutation());
	UAM_ASSERT(RunGitForTest(repo, "show-ref --verify " + ShellQuoteForTest("refs/heads/" + chat.workspace_branch_name)));
	service.Stop();
}
