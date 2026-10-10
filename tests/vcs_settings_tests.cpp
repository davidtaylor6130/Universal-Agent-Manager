#include "test_harness.h"
#include "app/git_worktree_service.h"
#include "app/vcs_commit_service.h"
#include "common/config/settings_frontend_json.h"
#include "common/paths/workspace_root.h"

using namespace uam_test;

UAM_TEST(VcsSettingsPersistAndDefaultExistingInstallationsToEnabled)
{
	TempDir temp("uam-vcs-settings");
	AppSettings settings;
	UAM_ASSERT(settings.git_enabled && settings.svn_enabled);
	settings.git_enabled = false; settings.svn_enabled = true;
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	AppSettings restored;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", restored).loaded);
	UAM_ASSERT(!restored.git_enabled && restored.svn_enabled);
	const nlohmann::json json = uam::settings_frontend_json::SerializeCommonSettingsFields(restored);
	UAM_ASSERT_EQ(json["gitEnabled"].get<bool>(), false); UAM_ASSERT_EQ(json["svnEnabled"].get<bool>(), true);
	restored.git_enabled = true; restored.svn_enabled = false;
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", restored));
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", settings).loaded);
	UAM_ASSERT(settings.git_enabled && !settings.svn_enabled);
}

UAM_TEST(VcsSettingsDisabledTypedOperationsStopBeforeLocalOrRemoteAccess)
{
	uam::AppState app; app.settings.git_enabled = false; app.settings.svn_enabled = false;
	ChatSession chat; chat.id = "disabled-vcs"; chat.execution_host_id = "missing-remote-host";
	const uam::VcsCommitService service;
	const uam::VcsCommitStatus status = service.Status(app, chat);
	UAM_ASSERT(!status.available); UAM_ASSERT(status.vcs_types.empty()); UAM_ASSERT(status.warning.find("disabled") != std::string::npos);
	for (const uam::VcsType type : {uam::VcsType::Git, uam::VcsType::Svn})
	{
		std::string error;
		UAM_ASSERT(service.Diff(app, chat, "file.cpp", type, &error).empty()); UAM_ASSERT(error.find("disabled") != std::string::npos);
		UAM_ASSERT(service.Commit(app, chat, type, "Commit", {"file.cpp"}).error.find("disabled") != std::string::npos);
		UAM_ASSERT(service.GenerateMessage(app, chat, type, {"file.cpp"}).error.find("disabled") != std::string::npos);
	}
	chat.execution_host_id = "local";
	UAM_ASSERT(!service.Status(app, chat).available);
	std::string reason;
	const uam::GitWorktreeService worktrees;
	UAM_ASSERT(!worktrees.CanCheckpointTurn(app, chat, &reason)); UAM_ASSERT(reason.find("disabled") != std::string::npos);
	UAM_ASSERT(!worktrees.CreateForChat(app, chat).ok);
	ChatSession branch; UAM_ASSERT(!worktrees.CreateBranchForChat(app, chat, branch, "").ok);
	UAM_ASSERT(!worktrees.DiscardChatChanges(app, chat).ok); UAM_ASSERT(!worktrees.PortChatChanges(app, chat).ok);
	UAM_ASSERT(!worktrees.CreateTurnCheckpoint(app, chat, 0).ok); UAM_ASSERT(!worktrees.PreviewTurnRollback(app, chat, 0).ok);
}

UAM_TEST(VcsSettingsFilterDetectedSystemsAndRestoreThemWhenEnabled)
{
	TempDir temp("uam-vcs-filter");
	uam::AppState app; ChatSession chat; chat.workspace_directory = temp.root.string();
	fs::create_directories(temp.root / ".svn");
	app.settings.git_enabled = false;
	const uam::VcsCommitService service;
	UAM_ASSERT(service.Status(app, chat).vcs_types == std::vector<uam::VcsType>{uam::VcsType::Svn});
	app.settings.svn_enabled = false; UAM_ASSERT(!service.Status(app, chat).available);
	if (!GitAvailableForTests()) return;
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(temp.root.string())));
	app.settings.git_enabled = true;
	UAM_ASSERT(service.Status(app, chat).vcs_types == std::vector<uam::VcsType>{uam::VcsType::Git});
	app.settings.svn_enabled = true;
	UAM_ASSERT_EQ(service.Status(app, chat).vcs_types.size(), std::size_t{2});
}

UAM_TEST(VcsSettingsDisabledGitKeepsOwnedPathsAndSkipsOnlyUncreatedDefaults)
{
	TempDir temp("uam-vcs-default-worktree");
	uam::AppState app; app.settings.git_enabled = false;
	ChatSession pending; pending.workspace_directory = temp.root.string(); pending.workspace_isolation_kind = uam::paths::kPendingGitWorktreeIsolationKind;
	const uam::GitWorktreeService service;
	UAM_ASSERT(service.PrepareForFirstLaunch(app, pending));
	UAM_ASSERT(pending.workspace_isolation_kind.empty()); UAM_ASSERT_EQ(pending.workspace_directory, temp.root.string());
	ChatSession isolated; isolated.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	isolated.workspace_worktree_directory = temp.root.string(); isolated.workspace_directory = temp.root.string(); isolated.workspace_branch_name = "owned-branch";
	UAM_ASSERT(service.PrepareForFirstLaunch(app, isolated));
	UAM_ASSERT_EQ(isolated.workspace_isolation_kind, uam::paths::kGitWorktreeIsolationKind);
	UAM_ASSERT_EQ(isolated.workspace_worktree_directory, temp.root.string());
	const uam::GitWorktreeStatus status = service.Status(app, isolated);
	UAM_ASSERT(status.isolated); UAM_ASSERT_EQ(status.branch_name, "owned-branch"); UAM_ASSERT(!status.worktree_missing);
}
