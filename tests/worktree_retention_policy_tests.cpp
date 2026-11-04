#include "test_harness.h"
#include "app/worktree_retention_service.h"
#include "common/config/settings_frontend_json.h"

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
