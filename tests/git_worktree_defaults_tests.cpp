#include "test_harness.h"
#include "common/config/settings_frontend_json.h"

using namespace uam_test;

UAM_TEST(GitWorktreeDefaultsRoundTripAndLegacyDefaults)
{
	TempDir temp("uam-worktree-default-settings");
	AppSettings settings;
	UAM_ASSERT(settings.git_auto_worktree);
	UAM_ASSERT(settings.git_default_base_branch.empty());
	settings.git_auto_worktree = false;
	settings.git_default_base_branch = " feature/base ";
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", loaded).loaded);
	UAM_ASSERT(!loaded.git_auto_worktree);
	UAM_ASSERT_EQ(loaded.git_default_base_branch, "feature/base");
	const nlohmann::json frontend = uam::settings_frontend_json::SerializeCommonSettingsFields(loaded);
	UAM_ASSERT_EQ(frontend.at("gitAutoWorktree").get<bool>(), false);
	UAM_ASSERT_EQ(frontend.at("gitDefaultBaseBranch").get<std::string>(), "feature/base");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "legacy.txt", "git_enabled=true\nsvn_enabled=true\n"));
	AppSettings legacy;
	UAM_ASSERT(SettingsStore::Load(temp.root / "legacy.txt", legacy).loaded);
	UAM_ASSERT(legacy.git_auto_worktree);
	UAM_ASSERT(legacy.git_default_base_branch.empty());
}

UAM_TEST(GitWorktreeAutomaticOptOutPreservesCreationAndOwnedIsolation)
{
	TempDir temp("uam-worktree-auto-optout");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.git_auto_worktree = false;
	ChatSession chat = ChatDomainService().CreateNewChat("folder", "codex-cli", app.settings.git_enabled && app.settings.git_auto_worktree);
	UAM_ASSERT(chat.workspace_isolation_kind.empty());
	UAM_ASSERT_EQ(chat.folder_id, "folder");
	UAM_ASSERT_EQ(chat.provider_id, "codex-cli");
	UAM_ASSERT(!chat.id.empty());
	UAM_ASSERT_EQ(chat.branch_root_chat_id, chat.id);
	ChatSession pending = ChatDomainService().CreateNewChat("folder", "codex-cli");
	UAM_ASSERT_EQ(pending.workspace_isolation_kind, uam::paths::kPendingGitWorktreeIsolationKind);
	pending.workspace_directory = (temp.root / "absent-source").string();
	UAM_ASSERT(uam::GitWorktreeService().PrepareForFirstLaunch(app, pending));
	UAM_ASSERT(pending.workspace_isolation_kind.empty());
	UAM_ASSERT_EQ(pending.workspace_directory, (temp.root / "absent-source").string());
	ChatSession isolated = pending;
	isolated.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	isolated.workspace_worktree_directory = (temp.root / "owned-worktree").string();
	isolated.workspace_source_directory = pending.workspace_directory;
	isolated.workspace_branch_name = "owned-branch";
	UAM_ASSERT(uam::GitWorktreeService().PrepareForFirstLaunch(app, isolated));
	UAM_ASSERT_EQ(isolated.workspace_isolation_kind, uam::paths::kGitWorktreeIsolationKind);
	UAM_ASSERT_EQ(isolated.workspace_worktree_directory, (temp.root / "owned-worktree").string());
	UAM_ASSERT_EQ(isolated.workspace_branch_name, "owned-branch");
	UAM_ASSERT(!fs::exists(app.data_root / "worktrees"));
}

UAM_TEST(GitWorktreeDefaultBaseUsesLocalCommitAndPreservesExplicitBranches)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-worktree-default-base");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "base\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m base"));
	UAM_ASSERT(RunGitForTest(repo, "branch feature/base"));
	UAM_ASSERT(RunGitForTest(repo, "tag tag-only"));
	const ProcessExecutionResult base = PlatformServicesFactory::Instance().process_service.ExecuteCommand("git -C " + ShellQuoteForTest(repo.string()) + " rev-parse HEAD", 10000);
	UAM_ASSERT(base.ok && base.exit_code == 0);
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "head\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m head"));
	const ProcessExecutionResult head = PlatformServicesFactory::Instance().process_service.ExecuteCommand("git -C " + ShellQuoteForTest(repo.string()) + " rev-parse HEAD", 10000);
	UAM_ASSERT(head.ok && head.exit_code == 0);
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.git_auto_worktree = false;
	app.settings.git_default_base_branch = "feature/base";
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli", false);
	chat.workspace_directory = repo.string();
	const uam::GitWorktreeService git;
	UAM_ASSERT(git.CreateForChat(app, chat).ok);
	UAM_ASSERT_EQ(chat.workspace_base_ref, uam::strings::Trim(base.output));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(uam::paths::PathFromUtf8(chat.workspace_worktree_directory) / "app.txt"), "base\n");
	for (const std::string& branch : {std::string("absent"), std::string("tag-only"), std::string("--help"), std::string("feature/../escape")})
	{
		uam::AppState invalid;
		invalid.data_root = temp.root / ("invalid-" + uam::hashing::Hex64(uam::hashing::Fnv1a64(branch)));
		invalid.settings.git_default_base_branch = branch;
		ChatSession rejected = ChatDomainService().CreateNewChat("", "codex-cli");
		rejected.workspace_directory = repo.string();
		const uam::GitWorktreeOperationResult result = git.CreateForChat(invalid, rejected);
		UAM_ASSERT(!result.ok);
		UAM_ASSERT(!result.message.empty());
		UAM_ASSERT(rejected.workspace_worktree_directory.empty());
		UAM_ASSERT(!fs::exists(invalid.data_root / "worktrees"));
	}
	app.settings.git_default_base_branch = "absent";
	ChatSession checkpoint = ChatDomainService().CreateNewChat("", "codex-cli", false);
	checkpoint.workspace_directory = repo.string();
	UAM_ASSERT(git.CreateBranchForChat(app, chat, checkpoint, uam::strings::Trim(head.output)).ok);
	UAM_ASSERT_EQ(checkpoint.workspace_base_ref, uam::strings::Trim(head.output));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(uam::paths::PathFromUtf8(checkpoint.workspace_worktree_directory) / "app.txt"), "head\n");
	app.settings.git_default_base_branch.clear();
	ChatSession current = ChatDomainService().CreateNewChat("", "codex-cli", false);
	current.workspace_directory = repo.string();
	UAM_ASSERT(git.CreateForChat(app, current).ok);
	UAM_ASSERT_EQ(current.workspace_base_ref, uam::strings::Trim(head.output));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(repo / "app.txt"), "head\n");
}

UAM_TEST(GitWorktreeDefaultBasePreservesManagedAndSvnSnapshots)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-worktree-managed-default-base");
	for (const bool svn : {false, true})
	{
		const fs::path source = temp.root / (svn ? "svn-source" : "plain-source");
		fs::create_directories(source);
		if (svn) fs::create_directories(source / ".svn");
		UAM_ASSERT(uam::io::WriteTextFile(source / "content.txt", "preserved baseline\n"));
		uam::AppState app;
		app.data_root = temp.root / (svn ? "svn-data" : "plain-data");
		app.settings.git_default_base_branch = "missing-git-only-branch";
		app.settings.git_auto_worktree = false;
		ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli", false);
		chat.workspace_directory = source.string();
		const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().CreateForChat(app, chat);
		UAM_ASSERT(result.ok);
		UAM_ASSERT(result.status.managed_repository);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(uam::paths::PathFromUtf8(chat.workspace_worktree_directory) / "content.txt"), "preserved baseline\n");
		UAM_ASSERT_EQ(uam::io::ReadTextFile(source / "content.txt"), "preserved baseline\n");
		if (svn) UAM_ASSERT(fs::is_directory(source / ".svn"));
	}
}

UAM_TEST(ManagedSnapshotPolicySeparatesSvnAndPlainFoldersBeforeOwnedIntent)
{
	UAM_ASSERT(GitAvailableForTests());
	for (const bool svn : {false, true})
	{
		TempDir temp("uam-snapshot-policy-disabled");
		const fs::path source = temp.root / "source";
		fs::create_directories(svn ? source / ".svn" : source);
		UAM_ASSERT(uam::io::WriteTextFile(source / "keep.txt", "original"));
		uam::AppState app;
		app.data_root = temp.root / "data";
		const fs::path settings = temp.root / "settings.txt";
		UAM_ASSERT(uam::io::WriteTextFile(settings, std::string("svn_enabled=false\ngit_snapshot_svn=") + (svn ? "false" : "true") + "\ngit_snapshot_plain_folders=" + (svn ? "true" : "false") + "\n"));
		UAM_ASSERT(SettingsStore::Load(settings, app.settings).loaded);
		ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
		chat.workspace_directory = source.string();
		const ChatSession original = chat;
		int intents = 0;
		const uam::GitWorktreeIntentRecorder record = [&intents](const uam::GitWorktreeCreationIntent&, std::string&) { ++intents; return true; };
		const uam::GitWorktreeOperationResult refused = uam::GitWorktreeService().PrepareForChat(app, chat, record, {}, true);
		UAM_ASSERT(!refused.ok);
		UAM_ASSERT(refused.message.find("snapshot") != std::string::npos);
		UAM_ASSERT_EQ(intents, 0);
		UAM_ASSERT(!fs::exists(app.data_root / "worktrees"));
		UAM_ASSERT(uam::GitWorktreeService().PrepareForFirstLaunch(app, chat));
		UAM_ASSERT(chat.workspace_isolation_kind.empty());
		UAM_ASSERT(chat.workspace_worktree_directory.empty());
		UAM_ASSERT_EQ(chat.workspace_directory, original.workspace_directory);
		UAM_ASSERT_EQ(ReadFile(source / "keep.txt"), std::string("original"));
		UAM_ASSERT(!fs::exists(source / ".git"));
	}
}

UAM_TEST(ManagedSnapshotOptOutKeepsExistingOwnedWorktree)
{
	UAM_ASSERT(GitAvailableForTests());
	TempDir temp("uam-snapshot-policy-existing");
	uam::AppState app;
	app.data_root = temp.root / "data";
	const fs::path source = temp.root / "source";
	fs::create_directories(source);
	UAM_ASSERT(uam::io::WriteTextFile(source / "keep.txt", "original"));
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
	chat.workspace_directory = source.string();
	UAM_ASSERT(uam::GitWorktreeService().CreateForChat(app, chat).ok);
	const ChatSession owned = chat;
	const fs::path settings = temp.root / "settings.txt";
	UAM_ASSERT(uam::io::WriteTextFile(settings, "git_snapshot_svn=false\ngit_snapshot_plain_folders=false\n"));
	UAM_ASSERT(SettingsStore::Load(settings, app.settings).loaded);
	UAM_ASSERT(uam::GitWorktreeService().PrepareForFirstLaunch(app, chat));
	UAM_ASSERT(uam::GitWorktreeService().CreateForChat(app, chat).ok);
	UAM_ASSERT_EQ(chat.workspace_worktree_directory, owned.workspace_worktree_directory);
	UAM_ASSERT_EQ(chat.workspace_branch_name, owned.workspace_branch_name);
	UAM_ASSERT_EQ(ReadFile(fs::path(chat.workspace_worktree_directory) / "keep.txt"), std::string("original"));
}

UAM_TEST(ManagedSnapshotIndependentOptInAndRealGitRemainAvailable)
{
	UAM_ASSERT(GitAvailableForTests());
	for (const std::string_view kind : {"svn", "plain", "git"})
	{
		TempDir temp("uam-snapshot-policy-enabled");
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.settings.git_snapshot_svn = kind == "svn";
		app.settings.git_snapshot_plain_folders = kind == "plain";
		app.settings.svn_enabled = false;
		const fs::path source = temp.root / "source";
		fs::create_directories(kind == "svn" ? source / ".svn" : source);
		UAM_ASSERT(uam::io::WriteTextFile(source / "keep.txt", "original"));
		if (kind == "git")
		{
			UAM_ASSERT(RunGitForTest(source, "init --template="));
			UAM_ASSERT(RunGitForTest(source, "config user.email uam@example.test"));
			UAM_ASSERT(RunGitForTest(source, "config user.name UAM"));
			UAM_ASSERT(RunGitForTest(source, "add keep.txt"));
			UAM_ASSERT(RunGitForTest(source, "commit -m baseline"));
		}
		ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
		chat.workspace_directory = source.string();
		UAM_ASSERT(uam::GitWorktreeService().CreateForChat(app, chat).ok);
		UAM_ASSERT(!chat.workspace_worktree_directory.empty());
		UAM_ASSERT_EQ(ReadFile(fs::path(chat.workspace_worktree_directory) / "keep.txt"), std::string("original"));
		UAM_ASSERT_EQ(fs::exists(source / ".git"), kind == "git");
	}
}

UAM_TEST(ManagedSnapshotOptOutDoesNotBypassUnknownWorkspaceValidation)
{
	TempDir temp("uam-snapshot-policy-unknown");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.git_snapshot_svn = false;
	app.settings.git_snapshot_plain_folders = false;
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
	chat.workspace_directory = (temp.root / "missing").string();
	UAM_ASSERT(uam::ManagedSnapshotDisabledReason(app, chat).empty());
	std::string error;
	UAM_ASSERT(!uam::GitWorktreeService().PrepareForFirstLaunch(app, chat, &error));
	UAM_ASSERT(!error.empty());
	const fs::path source = temp.root / "malformed";
	fs::create_directories(source);
	UAM_ASSERT(uam::io::WriteTextFile(source / ".git", "gitdir: missing\n"));
	chat.workspace_directory = source.string();
	UAM_ASSERT(uam::ManagedSnapshotDisabledReason(app, chat).empty());
	int intents = 0;
	const uam::GitWorktreeOperationResult refused = uam::GitWorktreeService().PrepareForChat(app, chat,
	    [&intents](const uam::GitWorktreeCreationIntent&, std::string&) { ++intents; return true; }, {}, true);
	UAM_ASSERT(!refused.ok);
	UAM_ASSERT(!refused.message.empty());
	UAM_ASSERT_EQ(intents, 0);
	UAM_ASSERT(!fs::exists(app.data_root / "worktrees"));
	UAM_ASSERT_EQ(chat.workspace_isolation_kind, uam::paths::kPendingGitWorktreeIsolationKind);
}


UAM_TEST(ManagedSnapshotUnbornSkipsIgnoredDependencyLinks)
{
	UAM_ASSERT(GitAvailableForTests());
	TempDir temp("uam-unborn-ignored-dependencies");
	const fs::path source = temp.root / "source";
	const fs::path dependency = source / ".build/checkouts/swift-argument-parser/Plugins";
	fs::create_directories(dependency);
	fs::create_directories(temp.root / "external");
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(source.string())));
	UAM_ASSERT(uam::io::WriteTextFile(source / ".gitignore", ".build/\n"));
	UAM_ASSERT(uam::io::WriteTextFile(source / "app.txt", "authored source\n"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "external/private.txt", "must not snapshot\n"));
	fs::create_directory_symlink(temp.root / "external", dependency / "GenerateCommon");
	const std::string original_head = ReadFile(source / ".git/HEAD");
	uam::AppState app; app.data_root = temp.root / "data";
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
	chat.workspace_directory = source.string();
	const auto result = uam::GitWorktreeService().PrepareForChat(app, chat,
	    [](const uam::GitWorktreeCreationIntent&, std::string&) { return true; }, {});
	if (!result.ok) throw std::runtime_error(result.message);
	UAM_ASSERT(result.ok);
	const fs::path isolated = chat.workspace_worktree_directory;
	UAM_ASSERT_EQ(ReadFile(isolated / "app.txt"), std::string("authored source\n"));
	UAM_ASSERT(!fs::exists(isolated / ".build"));
	UAM_ASSERT_EQ(ReadFile(source / ".git/HEAD"), original_head);
	UAM_ASSERT_EQ(ReadFile(temp.root / "external/private.txt"), std::string("must not snapshot\n"));
	UAM_ASSERT(fs::is_symlink(fs::symlink_status(dependency / "GenerateCommon")));
}

UAM_TEST(ManagedSnapshotPreservesRelativeSymlinkNodes)
{
	UAM_ASSERT(GitAvailableForTests());
	for (const int kind : {0, 1, 2})
	{
		TempDir temp("uam-managed-relative-link");
		const fs::path source = temp.root / "source";
		fs::create_directories(source / "Sources/Common");
		fs::create_directories(source / "Plugins");
		if (kind == 1) fs::create_directories(source / ".svn");
		if (kind == 2) UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(source.string())));
		UAM_ASSERT(uam::io::WriteTextFile(source / "Sources/Common/code.swift", "original source\n"));
		fs::create_directory_symlink("../Sources/Common", source / "Plugins/GenerateCommon");
		fs::create_symlink("Sources/Common/code.swift", source / "entry.swift");
		uam::AppState app; app.data_root = temp.root / "data";
		ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli"); chat.workspace_directory = source.string();
		const auto result = uam::GitWorktreeService().PrepareForChat(app, chat,
		    [](const uam::GitWorktreeCreationIntent&, std::string&) { return true; }, {});
		if (!result.ok) throw std::runtime_error(result.message);
	UAM_ASSERT(result.ok);
		const fs::path isolated = chat.workspace_worktree_directory;
		UAM_ASSERT(fs::is_symlink(fs::symlink_status(isolated / "Plugins/GenerateCommon")));
		UAM_ASSERT_EQ(fs::read_symlink(isolated / "Plugins/GenerateCommon"), fs::path("../Sources/Common"));
		UAM_ASSERT_EQ(fs::read_symlink(isolated / "entry.swift"), fs::path("Sources/Common/code.swift"));
		UAM_ASSERT_EQ(ReadFile(isolated / "Plugins/GenerateCommon/code.swift"), std::string("original source\n"));
		const auto mode = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
		    "git -C " + ShellQuoteForTest(isolated.string()) + " ls-files --stage -- Plugins/GenerateCommon entry.swift", 10000);
		UAM_ASSERT(mode.ok && mode.exit_code == 0 && mode.output.starts_with("120000 "));
		UAM_ASSERT(mode.output.find("\n120000 ") != std::string::npos);
		// The guarded UAM file writer deliberately refuses linked ancestors; a normal
		// filesystem write still must resolve into the isolated target, never the source.
		{ std::ofstream edit(isolated / "Plugins/GenerateCommon/code.swift"); edit << "isolated edit\n"; UAM_ASSERT(edit.good()); }
		UAM_ASSERT_EQ(ReadFile(source / "Sources/Common/code.swift"), std::string("original source\n"));
		UAM_ASSERT_EQ(fs::read_symlink(source / "Plugins/GenerateCommon"), fs::path("../Sources/Common"));
	}
}

UAM_TEST(ManagedSnapshotPreservesBrokenInternalSymlink)
{
	UAM_ASSERT(GitAvailableForTests());
	TempDir temp("uam-managed-broken-link");
	const fs::path source = temp.root / "source"; fs::create_directories(source);
	fs::create_symlink("generated/missing.swift", source / "future.swift");
	uam::AppState app; app.data_root = temp.root / "data";
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli"); chat.workspace_directory = source.string();
	const auto result = uam::GitWorktreeService().CreateForChat(app, chat);
	if (!result.ok) throw std::runtime_error(result.message);
	UAM_ASSERT(result.ok);
	const fs::path isolated = chat.workspace_worktree_directory;
	UAM_ASSERT(fs::is_symlink(fs::symlink_status(isolated / "future.swift")));
	UAM_ASSERT_EQ(fs::read_symlink(isolated / "future.swift"), fs::path("generated/missing.swift"));
	UAM_ASSERT(!fs::exists(isolated / "future.swift"));
	UAM_ASSERT(fs::is_symlink(fs::symlink_status(source / "future.swift")));
}

UAM_TEST(ManagedSnapshotRejectsEscapingSymlinkNodes)
{
	UAM_ASSERT(GitAvailableForTests());
	for (const int kind : {0, 1, 2})
	{
		TempDir temp("uam-managed-escaping-link");
		const fs::path source = temp.root / "source"; fs::create_directories(source);
		fs::create_directories(temp.root / "outside");
		UAM_ASSERT(uam::io::WriteTextFile(temp.root / "outside/keep.txt", "external survivor"));
		if (kind == 0) fs::create_directory_symlink("../outside", source / "escape");
		if (kind == 1) fs::create_directory_symlink(temp.root / "outside", source / "escape");
		if (kind == 2) { fs::create_directory_symlink("../outside", source / "indirect"); fs::create_directory_symlink("indirect", source / "escape"); }
		uam::AppState app; app.data_root = temp.root / "data";
		ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli"); chat.workspace_directory = source.string();
		const auto result = uam::GitWorktreeService().CreateForChat(app, chat);
		UAM_ASSERT(!result.ok && !result.message.empty());
		UAM_ASSERT(chat.workspace_worktree_directory.empty());
		UAM_ASSERT_EQ(ReadFile(temp.root / "outside/keep.txt"), std::string("external survivor"));
		UAM_ASSERT(fs::is_symlink(fs::symlink_status(source / "escape")));
	}
}


UAM_TEST(ManagedSnapshotUnbornUsesWorkingTreeAfterStagedDeletion)
{
	UAM_ASSERT(GitAvailableForTests()); TempDir temp("uam-snapshot-staged-deletion");
	const fs::path source = temp.root / "source"; fs::create_directories(source);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(source.string())));
	UAM_ASSERT(uam::io::WriteTextFile(source / "deleted.txt", "old staged bytes"));
	UAM_ASSERT(uam::io::WriteTextFile(source / "keep.txt", "current source"));
	UAM_ASSERT(RunGitForTest(source, "add deleted.txt")); fs::remove(source / "deleted.txt");
	uam::AppState app; app.data_root = temp.root / "data";
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli"); chat.workspace_directory = source.string();
	const auto result = uam::GitWorktreeService().PrepareForChat(app, chat,
	    [](const uam::GitWorktreeCreationIntent&, std::string&) { return true; }, {});
	UAM_ASSERT(result.ok);
	const fs::path isolated = chat.workspace_worktree_directory;
	UAM_ASSERT(!fs::exists(isolated / "deleted.txt"));
	UAM_ASSERT_EQ(ReadFile(isolated / "keep.txt"), std::string("current source"));
	UAM_ASSERT(!fs::exists(source / "deleted.txt"));
}

UAM_TEST(ManagedSnapshotUnbornRetainsNestedRepositoryFiles)
{
	UAM_ASSERT(GitAvailableForTests()); TempDir temp("uam-snapshot-nested-source");
	const fs::path source = temp.root / "source"; fs::create_directories(source / "nested");
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(source.string())));
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest((source / "nested").string())));
	UAM_ASSERT(uam::io::WriteTextFile(source / "nested/keep.txt", "nested current source"));
	uam::AppState app; app.data_root = temp.root / "data";
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli"); chat.workspace_directory = source.string();
	const auto result = uam::GitWorktreeService().PrepareForChat(app, chat,
	    [](const uam::GitWorktreeCreationIntent&, std::string&) { return true; }, {});
	UAM_ASSERT(result.ok);
	const fs::path isolated = chat.workspace_worktree_directory;
	UAM_ASSERT_EQ(ReadFile(isolated / "nested/keep.txt"), std::string("nested current source"));
	UAM_ASSERT(!fs::exists(isolated / "nested/.git"));
	UAM_ASSERT(fs::is_directory(source / "nested/.git"));
}

UAM_TEST(ManagedSnapshotUnbornRefusesStagedPathBehindReplacedLink)
{
	UAM_ASSERT(GitAvailableForTests()); TempDir temp("uam-snapshot-staged-parent-link");
	const fs::path source = temp.root / "source"; fs::create_directories(source / "vendor");
	fs::create_directories(temp.root / "outside");
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(source.string())));
	UAM_ASSERT(uam::io::WriteTextFile(source / ".gitignore", "vendor\n"));
	UAM_ASSERT(uam::io::WriteTextFile(source / "vendor/a.txt", "old staged bytes"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "outside/a.txt", "outside canary must not be imported"));
	UAM_ASSERT(RunGitForTest(source, "add -f vendor/a.txt"));
	fs::remove_all(source / "vendor"); fs::create_directory_symlink(temp.root / "outside", source / "vendor");
	uam::AppState app; app.data_root = temp.root / "data";
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli"); chat.workspace_directory = source.string();
	const auto result = uam::GitWorktreeService().PrepareForChat(app, chat,
	    [](const uam::GitWorktreeCreationIntent&, std::string&) { return true; }, {});
	UAM_ASSERT(!result.ok);
	UAM_ASSERT(chat.workspace_worktree_directory.empty());
	UAM_ASSERT_EQ(ReadFile(temp.root / "outside/a.txt"), std::string("outside canary must not be imported"));
	UAM_ASSERT(fs::is_symlink(fs::symlink_status(source / "vendor")));
}


UAM_TEST(ManagedSnapshotAcceptsEquivalentSourceRootSpellings)
{
	UAM_ASSERT(GitAvailableForTests());
	for (const int kind : {0, 1, 2})
	{
		for (const int spelling : {0, 1, 2})
		{
			TempDir temp("uam-snapshot-root-spelling");
			const fs::path source = temp.root / "source";
			fs::create_directories(source / "Sources/Common");
			fs::create_directories(source / "Plugins");
			if (kind == 1) fs::create_directories(source / ".svn");
			if (kind == 2) UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(source.string())));
			UAM_ASSERT(uam::io::WriteTextFile(source / "Sources/Common/code.swift", "authored bytes\n"));
			fs::create_directory_symlink("../Sources/Common", source / "Plugins/GenerateCommon");
			fs::path stored = source / "";
			if (spelling == 1) stored = source / ".";
			if (spelling == 2) stored = source / "Sources/..";
			uam::AppState app; app.data_root = temp.root / "data";
			ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
			chat.workspace_directory = stored.string();
			const auto result = uam::GitWorktreeService().PrepareForChat(app, chat,
			    [](const uam::GitWorktreeCreationIntent&, std::string&) { return true; }, {});
			if (!result.ok) throw std::runtime_error(result.message);
			UAM_ASSERT(result.ok);
			const fs::path isolated = chat.workspace_worktree_directory;
			UAM_ASSERT_EQ(ReadFile(isolated / "Sources/Common/code.swift"), std::string("authored bytes\n"));
			UAM_ASSERT(fs::is_symlink(fs::symlink_status(isolated / "Plugins/GenerateCommon")));
			UAM_ASSERT_EQ(fs::read_symlink(isolated / "Plugins/GenerateCommon"), fs::path("../Sources/Common"));
			UAM_ASSERT_EQ(ReadFile(isolated / "Plugins/GenerateCommon/code.swift"), std::string("authored bytes\n"));
			UAM_ASSERT_EQ(ReadFile(source / "Sources/Common/code.swift"), std::string("authored bytes\n"));
			UAM_ASSERT_EQ(fs::read_symlink(source / "Plugins/GenerateCommon"), fs::path("../Sources/Common"));
		}
	}
}
