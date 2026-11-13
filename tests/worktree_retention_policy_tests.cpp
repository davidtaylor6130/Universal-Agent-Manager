#include "test_harness.h"
#include "app/worktree_retention_service.h"
#include "common/config/settings_frontend_json.h"
#include "common/utils/hash_utils.h"

#include <thread>

using namespace uam_test;

namespace
{
	void AgeWorktree(const fs::path& repo, const fs::path& worktree)
	{
		const fs::file_time_type old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 15);
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(repo / ".git")) fs::last_write_time(entry.path(), old);
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(worktree)) fs::last_write_time(entry.path(), old);
		fs::last_write_time(worktree, old);
	}

	void WaitForRetention(uam::WorktreeRetentionService& service, const uam::AppState& app, std::chrono::steady_clock::time_point now)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (service.WorkerPending() && std::chrono::steady_clock::now() < deadline)
		{
			service.Poll(app, now);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(!service.WorkerPending());
	}
}

UAM_TEST(WorktreeRetentionPolicySettingsRoundTripAndBounds)
{
	TempDir temp("uam-retention-settings");
	AppSettings settings;
	UAM_ASSERT(settings.worktree_cleanup_enabled);
	UAM_ASSERT_EQ(settings.worktree_retention_days, 14);
	settings.worktree_cleanup_enabled = false;
	settings.worktree_retention_days = 30;
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.ini", settings));
	AppSettings loaded;
	SettingsStore::Load(temp.root / "settings.ini", loaded);
	UAM_ASSERT(!loaded.worktree_cleanup_enabled);
	UAM_ASSERT_EQ(loaded.worktree_retention_days, 30);
	const nlohmann::json frontend = uam::settings_frontend_json::SerializeCommonSettingsFields(loaded);
	UAM_ASSERT_EQ(frontend.at("worktreeCleanupEnabled"), nlohmann::json(false));
	UAM_ASSERT_EQ(frontend.at("worktreeRetentionDays"), nlohmann::json(30));
	for (const int days : {-1, 0, 3651, 2147483647})
	{
		settings.worktree_retention_days = days;
		UAM_ASSERT(SettingsStore::Save(temp.root / "settings.ini", settings));
		SettingsStore::Load(temp.root / "settings.ini", loaded);
		UAM_ASSERT_EQ(loaded.worktree_retention_days, 14);
	}
}

UAM_TEST(WorktreeRetentionPolicyThresholdAndQueuedRevocation)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-retention-policy");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "keep baseline\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "retention-policy-chat";
	chat.workspace_directory = repo.string();
	const uam::GitWorktreeService git;
	UAM_ASSERT(git.CreateForChat(app, chat).ok);
	UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(app.data_root, chat.id).Failed());
	const fs::path worktree = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
	AgeWorktree(repo, worktree);
	uam::GitWorktreeRetentionCandidate candidate;
	UAM_ASSERT(git.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate, {}, 1));
	UAM_ASSERT(!git.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate, {}, 30));
	UAM_ASSERT(!git.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate, {}, 0));
	UAM_ASSERT(!git.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate, {}, 2147483647));
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> mutation = app.worktree_retention_gate.TryAcquireMutation();
	UAM_ASSERT(mutation);
	uam::WorktreeRetentionService service;
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now() + std::chrono::seconds(40);
	service.Poll(app, now);
	WaitForRetention(service, app, now);
	UAM_ASSERT(service.Pending());
	app.settings.worktree_retention_days = 30;
	service.Poll(app, now);
	WaitForRetention(service, app, now);
	UAM_ASSERT(!service.Pending());
	UAM_ASSERT(fs::exists(worktree));
	app.settings.worktree_retention_days = 1;
	service.Poll(app, now);
	WaitForRetention(service, app, now);
	UAM_ASSERT(service.Pending());
	app.settings.worktree_cleanup_enabled = false;
	service.Poll(app, now);
	mutation.reset();
	service.Poll(app, now + std::chrono::hours(7));
	UAM_ASSERT(!service.Pending());
	UAM_ASSERT(fs::exists(worktree));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(worktree / "app.txt"), "keep baseline\n");
	UAM_ASSERT(RunGitForTest(repo, "show-ref --verify " + ShellQuoteForTest("refs/heads/" + chat.workspace_branch_name)));
	service.Stop();
}

namespace
{
	void AgeTree(const fs::path& root)
	{
		const fs::file_time_type old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 15);
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) fs::last_write_time(entry.path(), old);
		fs::last_write_time(root, old);
	}

	/// Runs whole retention passes until nothing more is queued: discovery, then one removal per poll.
	void RunRetentionPasses(uam::WorktreeRetentionService& service, const uam::AppState& app, std::chrono::steady_clock::time_point& now)
	{
		for (int pass = 0; pass < 8; ++pass)
		{
			now += std::chrono::hours(7);
			service.Poll(app, now);
			WaitForRetention(service, app, now);
			while (service.Pending())
			{
				service.Poll(app, now);
				WaitForRetention(service, app, now);
			}
		}
	}

	struct PlainFolderSnapshot
	{
		fs::path folder;
		fs::path repository;
		fs::path worktree;
		ChatSession chat;
	};

	PlainFolderSnapshot CreatePlainFolderSnapshot(uam::AppState& app, const fs::path& base, const std::string& id)
	{
		PlainFolderSnapshot snapshot;
		snapshot.folder = base / (id + "-folder");
		fs::create_directories(snapshot.folder);
		UAM_ASSERT(uam::io::WriteTextFile(snapshot.folder / "notes.txt", "plain folder\n"));
		snapshot.chat.id = id;
		snapshot.chat.workspace_directory = snapshot.folder.string();
		const uam::GitWorktreeOperationResult created = uam::GitWorktreeService().CreateForChat(app, snapshot.chat);
		UAM_ASSERT(created.ok);
		UAM_ASSERT(created.status.managed_repository);
		snapshot.worktree = uam::paths::PathFromUtf8(snapshot.chat.workspace_worktree_directory);
		snapshot.repository = app.data_root / "worktrees" /
		    uam::hashing::Hex64(uam::hashing::Fnv1a64(uam::paths::NormalizedPortablePathString(snapshot.folder))) /
		    ("managed-repository-" + uam::hashing::Hex64(uam::hashing::Fnv1a64(id)));
		UAM_ASSERT(fs::is_directory(snapshot.repository / ".git"));
		UAM_ASSERT(fs::is_regular_file(snapshot.worktree / ".git"));
		return snapshot;
	}
}

UAM_TEST(WorktreeRetentionRemovesOrphanedPlainFolderSnapshotsOnlyWhenNothingIsLost)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-retention-snapshot");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	PlainFolderSnapshot live = CreatePlainFolderSnapshot(app, temp.root, "snapshot-live");
	PlainFolderSnapshot orphan = CreatePlainFolderSnapshot(app, temp.root, "snapshot-orphan");
	PlainFolderSnapshot committed = CreatePlainFolderSnapshot(app, temp.root, "snapshot-committed");
	PlainFolderSnapshot dirty = CreatePlainFolderSnapshot(app, temp.root, "snapshot-dirty");
	// Agent work committed on the snapshot branch is kept with its repository, like any branch.
	UAM_ASSERT(uam::io::WriteTextFile(committed.worktree / "agent.txt", "agent work\n"));
	UAM_ASSERT(RunGitForTest(committed.worktree, "add agent.txt"));
	UAM_ASSERT(RunGitForTest(committed.worktree, "-c user.name=UAM -c user.email=uam@example.test commit -m checkpoint"));
	app.chats.push_back(live.chat);
	for (const PlainFolderSnapshot* snapshot : {&orphan, &committed, &dirty})
		UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(app.data_root, snapshot->chat.id).Failed());
	for (const PlainFolderSnapshot* snapshot : {&live, &orphan, &committed, &dirty})
	{
		AgeTree(snapshot->repository);
		AgeTree(snapshot->worktree);
	}

	// Patches older than the retention period go; recent ones and anything that is not a plain patch file stay.
	const fs::path patches = app.data_root / "worktrees" / "patches";
	fs::create_directories(patches);
	UAM_ASSERT(uam::io::WriteTextFile(patches / "old-chat-1.patch", "old\n"));
	UAM_ASSERT(uam::io::WriteTextFile(patches / "new-chat-2.patch", "new\n"));
	UAM_ASSERT(uam::io::WriteTextFile(patches / "notes.txt", "keep\n"));
	fs::last_write_time(patches / "old-chat-1.patch", fs::file_time_type::clock::now() - std::chrono::hours(24 * 15));
	fs::last_write_time(patches / "notes.txt", fs::file_time_type::clock::now() - std::chrono::hours(24 * 15));

	uam::WorktreeRetentionService service;
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	// The dirty snapshot gains an untracked file after aging, so its repository is never clean.
	UAM_ASSERT(uam::io::WriteTextFile(dirty.repository / "unsaved.txt", "unsaved\n"));
	AgeTree(dirty.repository);
	RunRetentionPasses(service, app, now);
	// Clean orphaned worktrees go first; removing one touches its repository, which then ages again before it can go.
	UAM_ASSERT(!fs::exists(orphan.worktree) && fs::exists(orphan.repository / ".git"));
	for (const PlainFolderSnapshot* snapshot : {&live, &orphan, &committed, &dirty}) AgeTree(snapshot->repository);
	RunRetentionPasses(service, app, now);
	service.Stop();

	// A live chat keeps its worktree and snapshot.
	UAM_ASSERT(fs::exists(live.worktree / ".git") && fs::exists(live.repository / ".git"));
	// An orphan with nothing beyond its snapshot loses its worktree, then its repository.
	UAM_ASSERT(!fs::exists(orphan.worktree));
	UAM_ASSERT(!fs::exists(orphan.repository));
	// Committed agent work keeps the repository and its branch once the clean worktree is gone.
	UAM_ASSERT(!fs::exists(committed.worktree));
	UAM_ASSERT(fs::exists(committed.repository / ".git"));
	UAM_ASSERT(RunGitForTest(committed.repository, "show-ref --verify " + ShellQuoteForTest("refs/heads/" + committed.chat.workspace_branch_name)));
	// Unsaved files keep the repository.
	UAM_ASSERT(fs::exists(dirty.repository / "unsaved.txt"));
	UAM_ASSERT(fs::exists(orphan.folder / "notes.txt") && fs::exists(dirty.folder / "notes.txt"));

	UAM_ASSERT(!fs::exists(patches / "old-chat-1.patch"));
	UAM_ASSERT(fs::exists(patches / "new-chat-2.patch"));
	UAM_ASSERT(fs::exists(patches / "notes.txt"));
}

UAM_TEST(WorktreeRetentionRefusesSnapshotsOutsideTheirOwnedLayout)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-retention-snapshot-layout");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	PlainFolderSnapshot snapshot = CreatePlainFolderSnapshot(app, temp.root, "snapshot-layout");
	UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(app.data_root, snapshot.chat.id).Failed());
	const uam::GitWorktreeService git;
	uam::GitWorktreeRetentionCandidate candidate;
	AgeTree(snapshot.repository);
	AgeTree(snapshot.worktree);
	// Its linked worktree still exists, so the snapshot itself is not a candidate yet.
	UAM_ASSERT(!git.InspectManagedSnapshotRetentionCandidate(app.data_root, snapshot.repository, {}, candidate, {}, 1));
	UAM_ASSERT(git.InspectRetentionCandidate(app.data_root, snapshot.repository, snapshot.worktree, {}, candidate, {}, 1));
	// A protected path inside the worktree keeps it.
	UAM_ASSERT(!git.InspectRetentionCandidate(app.data_root, snapshot.repository, snapshot.worktree, {snapshot.worktree / "notes.txt"}, candidate, {}, 1));
	UAM_ASSERT(RunGitForTest(snapshot.repository, "worktree remove " + ShellQuoteForTest(snapshot.worktree.string())));
	AgeTree(snapshot.repository);
	UAM_ASSERT(git.InspectManagedSnapshotRetentionCandidate(app.data_root, snapshot.repository, {}, candidate, {}, 1));
	UAM_ASSERT(!git.InspectManagedSnapshotRetentionCandidate(app.data_root, snapshot.repository, {}, candidate, {}, 30));
	UAM_ASSERT(!git.InspectManagedSnapshotRetentionCandidate(app.data_root, snapshot.repository, {snapshot.repository}, candidate, {}, 1));
	// A copy with the same shape outside `worktrees/<key>/` is never treated as owned.
	const fs::path foreign = temp.root / "elsewhere" / snapshot.repository.parent_path().filename() / snapshot.repository.filename();
	fs::create_directories(foreign.parent_path());
	fs::copy(snapshot.repository, foreign, fs::copy_options::recursive);
	AgeTree(foreign);
	UAM_ASSERT(!git.InspectManagedSnapshotRetentionCandidate(app.data_root, foreign, {}, candidate, {}, 1));
	// Removal needs the exclusive lease.
	UAM_ASSERT(git.InspectManagedSnapshotRetentionCandidate(app.data_root, snapshot.repository, {}, candidate, {}, 1));
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> mutation = app.worktree_retention_gate.TryAcquireMutation();
	UAM_ASSERT(mutation);
	UAM_ASSERT(!git.RemoveManagedSnapshotRetentionCandidate(app.data_root, candidate, {}, *mutation, {}, 1));
	mutation.reset();
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> cleanup = app.worktree_retention_gate.TryAcquireCleanup();
	UAM_ASSERT(cleanup);
	UAM_ASSERT(git.RemoveManagedSnapshotRetentionCandidate(app.data_root, candidate, {}, *cleanup, {}, 1));
	UAM_ASSERT(!fs::exists(snapshot.repository));
	UAM_ASSERT(fs::exists(foreign / ".git"));
}
