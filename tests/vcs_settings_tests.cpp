#include "test_harness.h"
#include "app/git_worktree_service.h"
#include "app/vcs_commit_service.h"
#include "common/config/settings_frontend_json.h"
#include "common/paths/workspace_root.h"

using namespace uam_test;

UAM_TEST(TemporarySideChatNeverInheritsAutomaticWorktreeCreation)
{
	TempDir temp("uam-side-chat-workspace");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession parent = ChatDomainService().CreateNewChat("", "codex-cli");
	parent.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	parent.workspace_directory = (temp.root / "source").string();
	parent.workspace_source_directory = parent.workspace_directory;
	parent.workspace_worktree_directory = (temp.root / "parent-worktree").string();
	parent.workspace_branch_name = "build/parent";
	fs::create_directories(parent.workspace_worktree_directory);
	ChatDomainService().AddMessage(parent, MessageRole::User, "Keep using this workspace");
	app.chats.push_back(parent);
	std::string side_id;
	UAM_ASSERT(uam::CreateTemporarySideChat(app, parent.id, &side_id));
	const ChatSession* side = ChatDomainService().FindChatById(app, side_id);
	UAM_ASSERT(side != nullptr);
	UAM_ASSERT(side->workspace_isolation_kind.empty());
	UAM_ASSERT(side->workspace_worktree_directory.empty());
	UAM_ASSERT(side->workspace_branch_name.empty());
	UAM_ASSERT_EQ(uam::paths::ResolveWorkspaceRootPath(app, *side), fs::path(parent.workspace_worktree_directory));
	UAM_ASSERT_EQ(app.chats.front().workspace_branch_name, parent.workspace_branch_name);
	const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(app.data_root, side_id);
	UAM_ASSERT(persisted.has_value() && persisted->workspace_isolation_kind.empty());
	UAM_ASSERT(!fs::exists(app.data_root / "worktrees"));
}

UAM_TEST(PersistedTemporarySideChatSkipsFirstLaunchWorktreePreparation)
{
	TempDir temp("uam-side-chat-legacy-workspace");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession side = ChatDomainService().CreateNewChat("", "codex-cli");
	side.temporary_parent_chat_id = "parent";
	side.workspace_directory = (temp.root / "unavailable-parent-workspace").string();
	const ChatSession before = side;
	std::string error;
	UAM_ASSERT(uam::GitWorktreeService().PrepareForFirstLaunch(app, side, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(side.workspace_directory, before.workspace_directory);
	UAM_ASSERT_EQ(side.workspace_isolation_kind, before.workspace_isolation_kind);
	UAM_ASSERT(side.workspace_worktree_directory.empty());
	UAM_ASSERT(!fs::exists(app.data_root / "worktrees"));
}

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

UAM_TEST(SvnUpdateBeforeChatSettingsDefaultOffAndPersistThroughJson)
{
	TempDir temp("uam-svn-update-settings");
	AppSettings settings;
	const nlohmann::json defaults = uam::settings_frontend_json::SerializeCommonSettingsFields(settings);
	UAM_ASSERT(defaults.contains("svnUpdateBeforeChat"));
	UAM_ASSERT_EQ(defaults.at("svnUpdateBeforeChat").get<bool>(), false);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "settings.txt", "svn_enabled=true\nsvn_update_before_chat=true\n"));
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", settings).loaded);
	UAM_ASSERT_EQ(uam::settings_frontend_json::SerializeCommonSettingsFields(settings).at("svnUpdateBeforeChat").get<bool>(), true);
	UAM_ASSERT(SettingsStore::Save(temp.root / "roundtrip.txt", settings));
	AppSettings restored;
	UAM_ASSERT(SettingsStore::Load(temp.root / "roundtrip.txt", restored).loaded);
	UAM_ASSERT_EQ(uam::settings_frontend_json::SerializeCommonSettingsFields(restored).at("svnUpdateBeforeChat").get<bool>(), true);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "legacy.txt", "svn_enabled=true\n"));
	AppSettings legacy;
	UAM_ASSERT(SettingsStore::Load(temp.root / "legacy.txt", legacy).loaded);
	UAM_ASSERT_EQ(uam::settings_frontend_json::SerializeCommonSettingsFields(legacy).at("svnUpdateBeforeChat").get<bool>(), false);
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

UAM_TEST(ManagedSnapshotSettingsDefaultOnAndPersistIndependently)
{
	TempDir temp("uam-snapshot-settings");
	AppSettings settings;
	const nlohmann::json defaults = uam::settings_frontend_json::SerializeCommonSettingsFields(settings);
	UAM_ASSERT(defaults.value("gitSnapshotSvn", false));
	UAM_ASSERT(defaults.value("gitSnapshotPlainFolders", false));
	for (const bool svn : {false, true})
	{
		const fs::path path = temp.root / "settings.txt";
		UAM_ASSERT(uam::io::WriteTextFile(path, std::string("git_snapshot_svn=") + (svn ? "true" : "false") + "\ngit_snapshot_plain_folders=" + (svn ? "false" : "true") + "\n"));
		UAM_ASSERT(SettingsStore::Load(path, settings).loaded);
		UAM_ASSERT(SettingsStore::Save(path, settings));
		AppSettings loaded;
		UAM_ASSERT(SettingsStore::Load(path, loaded).loaded);
		const nlohmann::json projected = uam::settings_frontend_json::SerializeCommonSettingsFields(loaded);
		UAM_ASSERT_EQ(projected.value("gitSnapshotSvn", !svn), svn);
		UAM_ASSERT_EQ(projected.value("gitSnapshotPlainFolders", svn), !svn);
	}
}
