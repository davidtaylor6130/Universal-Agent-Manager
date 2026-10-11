#include "test_harness.h"
#include "common/paths/workspace_lease.h"
#include "app/worktree_retention_service.h"
#include "app/runtime_orchestration_services.h"

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
	(void)ChatRepository::DeleteChatStorageFiles(app.data_root, chat.id);
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> import = app.worktree_retention_gate.TryAcquireMutation();
	UAM_ASSERT(import);
	now += std::chrono::hours(7);
	service.Poll(app, now);
	wait_for_worker();
	UAM_ASSERT(service.Pending());
	UAM_ASSERT(fs::exists(worktree));
	// Publication occurs while the import still holds its lease, before cleanup may acquire exclusivity.
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	app.chats.push_back(chat);
	import.reset();
	service.Poll(app, now);
	wait_for_worker();
	UAM_ASSERT(fs::exists(worktree));
	UAM_ASSERT(!service.Pending());
	app.chats.clear();
	(void)ChatRepository::DeleteChatStorageFiles(app.data_root, chat.id);
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

UAM_TEST(WorktreeRetentionPreservesArchivedAgentTranscriptWorkspaceAfterParentMoves)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-worktree-agent-reference");
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
	folder.id = "agent-reference-folder";
	folder.directory = repo.string();
	app.folders.push_back(folder);
	ChatSession parent;
	parent.id = "agent-reference-parent";
	parent.folder_id = folder.id;
	parent.workspace_directory = repo.string();
	UAM_ASSERT(uam::GitWorktreeService().CreateForChat(app, parent).ok);
	const fs::path worktree = uam::paths::PathFromUtf8(parent.workspace_worktree_directory);
	ChatSession transcript;
	transcript.id = "agent-reference-transcript";
	transcript.folder_id = folder.id;
	transcript.agent_run_id = "agent-reference-run";
	transcript.workspace_directory = worktree.string();
	transcript.workspace_source_directory = repo.string();
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, transcript));
	parent.workspace_directory = repo.string();
	parent.workspace_worktree_directory.clear();
	parent.workspace_source_directory.clear();
	parent.workspace_branch_name.clear();
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, parent));
	ChatHistorySyncService().LoadSidebarChats(app);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.front().id, parent.id);
	AgentRun run;
	run.id = transcript.agent_run_id;
	run.root_chat_id = parent.id;
	run.transcript_chat_id = transcript.id;
	run.status = "completed";
	app.agent_runs.push_back(run);
	const fs::file_time_type old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 15);
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(repo / ".git")) fs::last_write_time(entry.path(), old);
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(worktree)) fs::last_write_time(entry.path(), old);
	fs::last_write_time(worktree, old);
	uam::WorktreeRetentionService service;
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now() + std::chrono::seconds(40);
	const std::function<void()> scan = [&]()
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		service.Poll(app, now);
		while (service.WorkerPending() && std::chrono::steady_clock::now() < deadline)
		{
			service.Poll(app, now);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(!service.WorkerPending());
	};
	scan();
	UAM_ASSERT(fs::exists(worktree));
	UAM_ASSERT(!service.Pending());
	// A persisted transcript remains a reference even if its agent ledger is missing.
	app.agent_runs.clear();
	now += std::chrono::hours(7);
	scan();
	UAM_ASSERT(fs::exists(worktree));
	// A ledger pointing to unreadable storage is unknown ownership, not permission to remove it.
	app.agent_runs.push_back(run);
	(void)ChatRepository::DeleteChatStorageFiles(app.data_root, transcript.id);
	now += std::chrono::hours(7);
	scan();
	UAM_ASSERT(fs::exists(worktree));
	app.agent_runs.clear();
	now += std::chrono::hours(7);
	scan();
	UAM_ASSERT(!fs::exists(worktree));
	service.Stop();
}

UAM_TEST(WorktreeRetentionMetadataUsesLargeChatCacheWithoutRepairingMissingOwnership)
{
	TempDir temp("uam-retention-metadata-bounds");
	ChatSession chat;
	chat.id = "retention-large-transcript";
	chat.created_at = "2026-10-11T00:00:00.000Z";
	chat.updated_at = chat.created_at;
	chat.workspace_directory = (temp.root / "workspace").string();
	chat.messages.push_back({MessageRole::Assistant, std::string(33U * 1024U * 1024U, 'x'), "now"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const fs::path summary = AppPaths::UamChatSummaryFilePath(temp.root, chat.id);
	const std::uintmax_t bytes = fs::file_size(primary);
	UAM_ASSERT(bytes > 32U * 1024U * 1024U);
	const fs::file_time_type primary_modified = fs::last_write_time(primary);
	const fs::file_time_type summary_modified = fs::last_write_time(summary);
	const std::optional<ChatSession> metadata = ChatRepository::LoadLocalChatSummaryReadOnly(temp.root, chat.id);
	UAM_ASSERT(metadata);
	UAM_ASSERT_EQ(metadata->workspace_directory, chat.workspace_directory);
	UAM_ASSERT(metadata->messages.empty());
	UAM_ASSERT_EQ(fs::last_write_time(primary), primary_modified);
	UAM_ASSERT_EQ(fs::last_write_time(summary), summary_modified);
	fs::remove(summary);
	UAM_ASSERT(!ChatRepository::LoadLocalChatSummaryReadOnly(temp.root, chat.id));
	UAM_ASSERT(!fs::exists(summary));
	UAM_ASSERT_EQ(fs::file_size(primary), bytes);
	UAM_ASSERT_EQ(fs::last_write_time(primary), primary_modified);
	for (const std::string& field : {"execution_host_id", "workspace_directory", "workspace_worktree_directory", "workspace_source_directory"})
	{
		const std::string id = "retention-invalid-ownership";
		const fs::path malformed = AppPaths::UamChatFilePath(temp.root, id);
		UAM_ASSERT(uam::io::WriteTextFile(malformed, "{\"id\":\"" + id + "\",\"" + field + "\":42}"));
		UAM_ASSERT(!ChatRepository::LoadLocalChatSummaryReadOnly(temp.root, id));
	}
}
