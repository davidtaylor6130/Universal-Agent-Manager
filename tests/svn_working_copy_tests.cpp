#include "test_harness.h"
#include "app/worktree_launch_preparation_service.h"

#include <algorithm>
#include <set>

using namespace uam_test;

// Real-svn cases against disposable file:// repositories: update before a new chat, tree conflicts, locks and
// svn:needs-lock, mixed revisions, keyword expansion and porting a snapshot's changes back into the working copy.
namespace
{
	ProcessExecutionResult RunSvnIn(const fs::path& cwd, std::vector<std::string> args)
	{
		args.insert(args.begin() + 1, "--non-interactive");
		return uam::RunSvn(cwd, args);
	}

	void Svn(const fs::path& cwd, std::vector<std::string> args)
	{
		const std::string command = args.front();
		const ProcessExecutionResult result = RunSvnIn(cwd, std::move(args));
		if (!result.ok || result.exit_code != 0) throw TestFailure("svn " + command + " failed: " + result.output + result.error);
	}

	std::string SvnOutput(const fs::path& cwd, std::vector<std::string> args)
	{
		const std::string command = args.front();
		const ProcessExecutionResult result = RunSvnIn(cwd, std::move(args));
		if (!result.ok || result.exit_code != 0) throw TestFailure("svn " + command + " failed: " + result.output + result.error);
		return uam::strings::Trim(result.output);
	}

	const uam::VcsChangedFile* Row(const uam::VcsCommitStatus& status, const std::string& path)
	{
		const auto found = std::ranges::find_if(status.changed_files, [&](const uam::VcsChangedFile& file) { return file.path == path; });
		return found == status.changed_files.end() ? nullptr : &*found;
	}

	/// `svn status` puts a tree conflict in its seventh column.
	bool TreeConflicted(const uam::VcsCommitStatus& status, const std::string& path)
	{
		const uam::VcsChangedFile* row = Row(status, path);
		return row != nullptr && row->status.size() == 7 && row->status[6] == 'C';
	}

	/// Snapshot cases also need Git; under UAM_REQUIRE_SVN=1 a missing Git fails them rather than skipping silently.
	bool GitForRealSvn()
	{
		if (GitAvailableForTests()) return true;
		if (uam::env::GetNonEmptyString("UAM_REQUIRE_SVN").value_or("") == "1") throw TestFailure("UAM_REQUIRE_SVN=1, but git is not installed");
		std::fprintf(stderr, "SKIP real-svn case: git is not installed\n");
		return false;
	}

	bool Writable(const fs::path& path)
	{
		return (fs::status(path).permissions() & fs::perms::owner_write) != fs::perms::none;
	}

	/// A repository with two working copies of its root: `checkout` is the chat's workspace and `other` stands for
	/// another user committing to the same repository.
	struct SvnWorkingCopies
	{
		TempDir temp{"uam-svn-working-copy"};
		fs::path repository = temp.root / "repository";
		fs::path checkout = temp.root / "checkout";
		fs::path other = temp.root / "other";
		std::string url;
		uam::AppState app;
		ChatSession chat;
		uam::VcsCommitService vcs;

		SvnWorkingCopies()
		{
			UAM_ASSERT(RunTestCommand("svnadmin create " + ShellQuoteForTest(uam::paths::Utf8PathString(repository))));
			url = SvnFileUrlForTest(repository);
			Svn(temp.root, {"checkout", url, checkout.string()});
			Svn(temp.root, {"checkout", url, other.string()});
			app.settings.svn_enabled = true;
			app.settings.git_enabled = false;
			chat.workspace_directory = checkout.string();
		}

		/// Writes the files, schedules the new ones and commits them from `copy`.
		void CommitFiles(const fs::path& copy, const std::vector<std::pair<std::string, std::string>>& files, const std::string& message)
		{
			for (const auto& [name, text] : files)
			{
				const fs::path path = copy / uam::paths::PathFromUtf8(name);
				const bool added = !fs::exists(path);
				UAM_ASSERT(uam::io::WriteTextFile(path, text));
				if (added) Svn(copy, {"add", "--parents", "--", name + "@"});
			}
			Svn(copy, {"commit", "-m", message});
		}

		std::string HeadRevision() const { return SvnOutput(temp.root, {"info", "--show-item", "revision", url + "@HEAD"}); }

		/// Every node's working revision, so a mixed-revision working copy shows more than one.
		std::set<std::string> WorkingRevisions() const
		{
			std::set<std::string> revisions;
			std::istringstream lines(SvnOutput(checkout, {"info", "--depth", "infinity", "--show-item", "revision", "--", "."}));
			for (std::string line; std::getline(lines, line);)
			{
				std::istringstream fields(line);
				std::string revision;
				if (fields >> revision) revisions.insert(revision);
			}
			return revisions;
		}

		/// Leaves `checkout` mixed: its root at the first commit, `a.txt` at the second.
		void MakeMixedRevisions()
		{
			CommitFiles(checkout, {{"a.txt", "one\n"}, {"b.txt", "b\n"}}, "baseline");
			Svn(checkout, {"update"});
			CommitFiles(checkout, {{"a.txt", "one\ntwo\n"}}, "edit a");
			UAM_ASSERT_EQ(WorkingRevisions(), (std::set<std::string>{"1", "2"}));
		}
	};

	/// Drives the real new-chat preparation for one chat whose SVN update is pending.
	struct SvnLaunch
	{
		SvnWorkingCopies& copies;
		std::string chat_id;

		explicit SvnLaunch(SvnWorkingCopies& working_copies) : copies(working_copies)
		{
			copies.app.data_root = copies.temp.root / "data";
			copies.app.settings.svn_update_before_chat = true;
			ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli", false);
			chat.workspace_directory = copies.checkout.string();
			chat.svn_update_pending = true;
			UAM_ASSERT(ChatRepository::SaveChat(copies.app.data_root, chat));
			chat_id = chat.id;
			copies.app.chats.push_back(std::move(chat));
		}

		ChatSession& Chat() { return copies.app.chats.front(); }

		/// Starts preparation and waits for its worker; returns the task for inspection.
		std::shared_ptr<uam::WorktreeLaunchPreparationTask> Run()
		{
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(copies.app, Chat()).state, uam::WorktreeLaunchPreparationState::Pending);
			const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = copies.app.worktree_launch_preparations.at(chat_id);
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			while (task->worker.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			(void)uam::PollWorktreeLaunchPreparations(copies.app);
			return task;
		}

		void AwaitSave(const std::shared_ptr<uam::WorktreeLaunchPreparationTask>& task)
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (!task->saved && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(copies.app);
				(void)uam::PollWorktreeLaunchPreparations(copies.app);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			UAM_ASSERT(task->saved && *task->saved);
		}
	};

	/// A chat whose workspace is a local Git snapshot of `checkout`, as SVN workspaces get by default.
	struct SvnSnapshotChat
	{
		SvnWorkingCopies& copies;

		explicit SvnSnapshotChat(SvnWorkingCopies& working_copies) : copies(working_copies)
		{
			copies.app.settings.git_enabled = true;
			copies.app.settings.git_port_method = GitPortMethod::Patch;
			copies.app.data_root = copies.temp.root / "data";
			copies.app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatFolder folder;
			folder.id = "folder";
			folder.directory = copies.checkout.string();
			copies.app.folders.push_back(folder);
			ChatSession chat = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
			chat.workspace_directory = copies.checkout.string();
			copies.app.chats.push_back(chat);
			const uam::GitWorktreeOperationResult created = uam::GitWorktreeService().CreateForChat(copies.app, Chat());
			if (!created.ok) throw TestFailure("snapshot: " + created.message);
			UAM_ASSERT(created.status.managed_repository && created.status.is_svn_workspace);
		}

		ChatSession& Chat() { return copies.app.chats.front(); }
		fs::path Worktree() { return uam::paths::PathFromUtf8(Chat().workspace_worktree_directory); }
	};
} // namespace

UAM_TEST(SvnRealUpdateBeforeNewChatBringsAMixedWorkingCopyToOneRevision)
{
	if (!RealSvnAvailable()) return;
	SvnWorkingCopies copies;
	copies.MakeMixedRevisions();
	Svn(copies.other, {"update"});
	copies.CommitFiles(copies.other, {{"b.txt", "b\nincoming\n"}}, "incoming edit");
	SvnLaunch launch(copies);
	const auto task = launch.Run();
	UAM_ASSERT(task->error.empty());
	launch.AwaitSave(task);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(copies.app, launch.Chat()).state, uam::WorktreeLaunchPreparationState::Ready);
	UAM_ASSERT(!launch.Chat().svn_update_pending);
	UAM_ASSERT(!ChatRepository::LoadLocalChat(copies.app.data_root, launch.chat_id)->svn_update_pending);
	// The update brought in the other user's commit and left no node behind.
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "b.txt"), std::string("b\nincoming\n"));
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "a.txt"), std::string("one\ntwo\n"));
	UAM_ASSERT_EQ(copies.WorkingRevisions(), (std::set<std::string>{"3"}));
}

UAM_TEST(SvnRealUpdateBeforeNewChatStopsAtATreeConflictAndKeepsTheEdit)
{
	if (!RealSvnAvailable()) return;
	SvnWorkingCopies copies;
	copies.CommitFiles(copies.checkout, {{"f.txt", "base\n"}}, "baseline");
	Svn(copies.other, {"update"});
	Svn(copies.other, {"delete", "--", "f.txt"});
	Svn(copies.other, {"commit", "-m", "delete f"});
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "f.txt", "mine\n"));
	SvnLaunch launch(copies);

	// The incoming deletion meets the local edit: svn postpones it as a tree conflict and the chat does not start.
	const auto task = launch.Run();
	UAM_ASSERT_EQ(task->error, std::string("Resolve the conflicts left by the SVN update before starting the chat."));
	const uam::WorktreeLaunchPreparationResult refused = uam::PrepareWorktreeForLaunch(copies.app, launch.Chat());
	UAM_ASSERT_EQ(refused.state, uam::WorktreeLaunchPreparationState::Failed);
	UAM_ASSERT(launch.Chat().svn_update_pending);
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "f.txt"), std::string("mine\n"));
	UAM_ASSERT(TreeConflicted(copies.vcs.Status(copies.app, copies.chat, uam::VcsType::Svn, false), "f.txt"));

	// A retry finds the conflict before updating again: a newer commit is not brought in and the edit stays.
	copies.CommitFiles(copies.other, {{"later.txt", "later\n"}}, "later commit");
	const auto retried = launch.Run();
	UAM_ASSERT_EQ(retried->error, std::string("Resolve SVN conflicts before starting the chat."));
	UAM_ASSERT(launch.Chat().svn_update_pending);
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "f.txt"), std::string("mine\n"));
	UAM_ASSERT_EQ(SvnOutput(copies.checkout, {"info", "--show-item", "revision", "--", "."}), std::string("2"));
	UAM_ASSERT(!fs::exists(copies.checkout / "later.txt"));
}

UAM_TEST(SvnRealTreeConflictsAreReportedAndKeptOutOfCommits)
{
	if (!RealSvnAvailable()) return;
	SvnWorkingCopies copies;
	copies.CommitFiles(copies.checkout, {{"f.txt", "base\n"}, {"d/g.txt", "base\n"}, {"keep.txt", "base\n"}}, "baseline");
	Svn(copies.other, {"update"});
	Svn(copies.other, {"delete", "--", "f.txt", "d"});
	Svn(copies.other, {"commit", "-m", "delete f and d"});
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "f.txt", "mine\n"));
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "d" / "g.txt", "mine\n"));
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "keep.txt", "kept edit\n"));

	const uam::VcsUpdateResult update = copies.vcs.Update(copies.app, copies.chat, uam::VcsType::Svn);
	if (!update.ok) throw TestFailure(update.error);
	UAM_ASSERT(update.conflicts);
	UAM_ASSERT_EQ(update.revision, std::string("2"));
	// Update never resolves: both local edits survive the incoming deletion.
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "f.txt"), std::string("mine\n"));
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "d" / "g.txt"), std::string("mine\n"));

	const uam::VcsCommitStatus status = copies.vcs.Status(copies.app, copies.chat, uam::VcsType::Svn, false);
	UAM_ASSERT(status.error.empty());
	UAM_ASSERT(TreeConflicted(status, "f.txt"));
	UAM_ASSERT(TreeConflicted(status, "d"));
	// The tree-conflict detail lines below each row are not rows of their own.
	UAM_ASSERT(std::ranges::none_of(status.changed_files, [](const uam::VcsChangedFile& file) { return file.path.find("incoming") != std::string::npos; }));

	for (const std::string& selected : {"f.txt", "d", "d/g.txt"})
	{
		const uam::VcsCommitResult refused = copies.vcs.Commit(copies.app, copies.chat, uam::VcsType::Svn, "publish conflicted", {selected});
		UAM_ASSERT(!refused.ok);
		UAM_ASSERT_EQ(refused.error, std::string("Resolve SVN conflicts, obstructions or external paths before committing."));
	}
	UAM_ASSERT_EQ(copies.HeadRevision(), std::string("2"));
	// A path outside the conflicts still publishes.
	const uam::VcsCommitResult unrelated = copies.vcs.Commit(copies.app, copies.chat, uam::VcsType::Svn, "publish keep", {"keep.txt"});
	if (!unrelated.ok) throw TestFailure(unrelated.error);
	UAM_ASSERT_EQ(copies.HeadRevision(), std::string("3"));
	UAM_ASSERT(TreeConflicted(unrelated.status, "f.txt"));
}

UAM_TEST(SvnRealLocksDecideWhoCanCommitAndNeedsLockFilesStayReadOnly)
{
	if (!RealSvnAvailable()) return;
	SvnWorkingCopies copies;
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "f.txt", "base\n"));
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "n.bin", "base\n"));
	Svn(copies.checkout, {"add", "--", "f.txt", "n.bin"});
	// Not "*": svn.exe on Windows expands an unquoted wildcard argument against the folder, which set the property on
	// f.txt as well. svn stores any value of this boolean property as "*".
	Svn(copies.checkout, {"propset", "svn:needs-lock", "yes", "n.bin"});
	Svn(copies.checkout, {"commit", "-m", "baseline"});
	Svn(copies.other, {"update"});

	// Another working copy holds the lock: committing an edit fails with svn's reason and keeps the edit.
	Svn(copies.other, {"lock", "-m", "theirs", "--", "f.txt"});
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "f.txt", "mine\n"));
	const uam::VcsCommitResult blocked = copies.vcs.Commit(copies.app, copies.chat, uam::VcsType::Svn, "edit locked file", {"f.txt"});
	UAM_ASSERT(!blocked.ok);
	UAM_ASSERT(blocked.error.find("E195022") != std::string::npos || blocked.error.find("E160037") != std::string::npos);
	UAM_ASSERT_EQ(copies.HeadRevision(), std::string("1"));
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "f.txt"), std::string("mine\n"));
	const uam::VcsCommitStatus after_block = copies.vcs.Status(copies.app, copies.chat, uam::VcsType::Svn, false);
	const uam::VcsChangedFile* edited = Row(after_block, "f.txt");
	UAM_ASSERT(edited != nullptr && edited->status == "M");
	Svn(copies.other, {"unlock", "--", "f.txt"});
	const uam::VcsCommitResult released = copies.vcs.Commit(copies.app, copies.chat, uam::VcsType::Svn, "edit unlocked file", {"f.txt"});
	if (!released.ok) throw TestFailure(released.error);
	UAM_ASSERT_EQ(copies.HeadRevision(), std::string("2"));

	// svn:needs-lock keeps the file read-only until this working copy takes the lock; committing releases it.
	UAM_ASSERT(!Writable(copies.checkout / "n.bin"));
	UAM_ASSERT(Row(copies.vcs.Status(copies.app, copies.chat, uam::VcsType::Svn, false), "n.bin") == nullptr);
	Svn(copies.checkout, {"lock", "--", "n.bin"});
	UAM_ASSERT(Writable(copies.checkout / "n.bin"));
	// A held lock alone is not a change: it is not listed, and selecting it publishes nothing and keeps the lock
	// instead of reporting a commit that svn never made.
	UAM_ASSERT(Row(copies.vcs.Status(copies.app, copies.chat, uam::VcsType::Svn, false), "n.bin") == nullptr);
	const uam::VcsCommitResult lock_only = copies.vcs.Commit(copies.app, copies.chat, uam::VcsType::Svn, "lock only", {"n.bin"});
	UAM_ASSERT(!lock_only.ok);
	UAM_ASSERT_EQ(lock_only.error, std::string("A selected SVN path is no longer changed: n.bin"));
	UAM_ASSERT_EQ(copies.HeadRevision(), std::string("2"));
	UAM_ASSERT(RunSvnIn(copies.checkout, {"status", "--", "n.bin"}).output.starts_with("     K"));
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "n.bin", "locked edit\n"));
	const uam::VcsCommitStatus locked_edit = copies.vcs.Status(copies.app, copies.chat, uam::VcsType::Svn, false);
	const uam::VcsChangedFile* locked = Row(locked_edit, "n.bin");
	UAM_ASSERT(locked != nullptr && locked->status == "M    K");
	const uam::VcsCommitResult committed = copies.vcs.Commit(copies.app, copies.chat, uam::VcsType::Svn, "edit needs-lock file", {"n.bin"});
	if (!committed.ok) throw TestFailure(committed.error);
	UAM_ASSERT_EQ(copies.HeadRevision(), std::string("3"));
	UAM_ASSERT(Row(committed.status, "n.bin") == nullptr);
	UAM_ASSERT(!Writable(copies.checkout / "n.bin"));
	UAM_ASSERT_EQ(SvnOutput(copies.temp.root, {"cat", "--", copies.url + "/n.bin@HEAD"}), std::string("locked edit"));
}

UAM_TEST(SvnRealMixedRevisionsUpdateToOneRevisionAndRefuseOutOfDatePublication)
{
	if (!RealSvnAvailable()) return;
	SvnWorkingCopies copies;
	copies.MakeMixedRevisions();

	// History lists the commit made from this working copy; the working revision it reports is the root's.
	const uam::VcsHistoryPage page = copies.vcs.History(copies.app, copies.chat, uam::VcsType::Svn, {});
	if (!page.error.empty()) throw TestFailure(page.error);
	UAM_ASSERT(page.entries.size() == 2 && page.entries.front().revision == "2");
	UAM_ASSERT_EQ(page.working_revision, std::string("1"));

	// A property change on the root is out of date while the root is older than a committed child: svn refuses it,
	// nothing is published and the change stays for after an update.
	Svn(copies.checkout, {"propset", "uam:fixture", "mixed", "."});
	const uam::VcsCommitResult refused = copies.vcs.Commit(copies.app, copies.chat, uam::VcsType::Svn, "root property", {"."});
	UAM_ASSERT(!refused.ok);
	UAM_ASSERT(refused.error.find("out of date") != std::string::npos || refused.error.find("E155011") != std::string::npos || refused.error.find("E160028") != std::string::npos);
	UAM_ASSERT_EQ(copies.HeadRevision(), std::string("2"));
	UAM_ASSERT_EQ(SvnOutput(copies.checkout, {"propget", "uam:fixture", "."}), std::string("mixed"));

	const uam::VcsUpdateResult update = copies.vcs.Update(copies.app, copies.chat, uam::VcsType::Svn);
	if (!update.ok) throw TestFailure(update.error);
	UAM_ASSERT(!update.conflicts);
	UAM_ASSERT_EQ(update.revision, std::string("2"));
	UAM_ASSERT_EQ(copies.WorkingRevisions(), (std::set<std::string>{"2"}));
	const uam::VcsCommitResult published = copies.vcs.Commit(copies.app, copies.chat, uam::VcsType::Svn, "root property", {"."});
	if (!published.ok) throw TestFailure(published.error);
	UAM_ASSERT_EQ(copies.HeadRevision(), std::string("3"));
}

UAM_TEST(SvnRealExpandedKeywordsAreNotChangesAndStayOutOfDiffs)
{
	if (!RealSvnAvailable()) return;
	SvnWorkingCopies copies;
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "k.txt", "$Revision$\n$Id$\nbody\n"));
	Svn(copies.checkout, {"add", "--", "k.txt"});
	Svn(copies.checkout, {"propset", "svn:keywords", "Revision Id", "k.txt"});
	Svn(copies.checkout, {"commit", "-m", "keywords"});
	const std::string expanded = ReadFile(copies.checkout / "k.txt");
	UAM_ASSERT(expanded.starts_with("$Revision: 1 $\n$Id: k.txt 1 "));
	UAM_ASSERT(Row(copies.vcs.Status(copies.app, copies.chat, uam::VcsType::Svn, true), "k.txt") == nullptr);

	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "k.txt", expanded.substr(0, expanded.rfind("body")) + "body changed\n"));
	const uam::VcsCommitStatus status = copies.vcs.Status(copies.app, copies.chat, uam::VcsType::Svn, true);
	const uam::VcsChangedFile* row = Row(status, "k.txt");
	UAM_ASSERT(row != nullptr && row->status == "M");
	UAM_ASSERT_EQ(row->additions, 1);
	UAM_ASSERT_EQ(row->deletions, 1);
	std::string error;
	const std::string patch = copies.vcs.Diff(copies.app, copies.chat, "k.txt", uam::VcsType::Svn, &error);
	if (!error.empty()) throw TestFailure(error);
	UAM_ASSERT(patch.find("\n+body changed") != std::string::npos && patch.find("\n-body") != std::string::npos);
	std::istringstream lines(patch);
	for (std::string line; std::getline(lines, line);)
		if ((line.starts_with('+') || line.starts_with('-')) && !line.starts_with("+++") && !line.starts_with("---"))
			UAM_ASSERT(line.find("$Revision") == std::string::npos && line.find("$Id") == std::string::npos);

	// The repository keeps the keywords unexpanded; the working copy shows the new revision after the commit.
	const uam::VcsCommitResult committed = copies.vcs.Commit(copies.app, copies.chat, uam::VcsType::Svn, "edit body", {"k.txt"});
	if (!committed.ok) throw TestFailure(committed.error);
	UAM_ASSERT_EQ(SvnOutput(copies.temp.root, {"cat", "--ignore-keywords", "--", copies.url + "/k.txt@HEAD"}), std::string("$Revision$\n$Id$\nbody changed"));
	UAM_ASSERT(ReadFile(copies.checkout / "k.txt").starts_with("$Revision: 2 $\n"));
}

// POSIX-only because these managed-snapshot cases have not been run on Windows; Windows has no real-svn coverage of
// SVN snapshots yet (recorded in docs/engineering/testing.md).
#ifndef _WIN32
UAM_TEST(SvnRealSnapshotChangesPortBackIntoTheWorkingCopy)
{
	if (!RealSvnAvailable() || !GitForRealSvn()) return;
	SvnWorkingCopies copies;
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "a.txt", "a\n"));
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "k.txt", "$Revision$\nbody\n"));
	UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "n.bin", "base\n"));
	Svn(copies.checkout, {"add", "--", "a.txt", "k.txt", "n.bin"});
	Svn(copies.checkout, {"propset", "svn:keywords", "Revision", "k.txt"});
	Svn(copies.checkout, {"propset", "svn:needs-lock", "yes", "n.bin"});
	Svn(copies.checkout, {"commit", "-m", "baseline"});
	Svn(copies.checkout, {"update"});
	SvnSnapshotChat snapshot(copies);
	const fs::path worktree = snapshot.Worktree();
	// The snapshot holds the files as the working copy shows them, keywords expanded.
	UAM_ASSERT_EQ(ReadFile(worktree / "k.txt"), std::string("$Revision: 1 $\nbody\n"));

	UAM_ASSERT(uam::io::WriteTextFile(worktree / "a.txt", "a\nported\n"));
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "k.txt", "$Revision: 1 $\nbody ported\n"));
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "n.bin", "ported without a lock\n"));
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "new.txt", "new\n"));
	const uam::GitWorktreeOperationResult ported = uam::GitWorktreeService().PortChatChanges(copies.app, snapshot.Chat());
	if (!ported.ok) throw TestFailure("port: " + ported.message);
	UAM_ASSERT(snapshot.Chat().workspace_worktree_directory.empty());

	UAM_ASSERT_EQ(ReadFile(copies.checkout / "a.txt"), std::string("a\nported\n"));
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "k.txt"), std::string("$Revision: 1 $\nbody ported\n"));
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "new.txt"), std::string("new\n"));
	// Porting writes through svn:needs-lock without taking the lock; the file is an ordinary local edit to SVN.
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "n.bin"), std::string("ported without a lock\n"));
	ChatSession source;
	source.workspace_directory = copies.checkout.string();
	const uam::VcsCommitStatus status = copies.vcs.Status(copies.app, source, uam::VcsType::Svn, true);
	UAM_ASSERT(Row(status, "a.txt") && Row(status, "a.txt")->status == "M");
	UAM_ASSERT(Row(status, "n.bin") && Row(status, "n.bin")->status == "M");
	UAM_ASSERT(Row(status, "new.txt") && Row(status, "new.txt")->status == "?");
	// The expanded keyword came back unchanged, so SVN counts only the edited line.
	const uam::VcsChangedFile* keyword = Row(status, "k.txt");
	UAM_ASSERT(keyword && keyword->status == "M" && keyword->additions == 1 && keyword->deletions == 1);
	const uam::VcsCommitResult committed = copies.vcs.Commit(copies.app, source, uam::VcsType::Svn, "ported", {"a.txt", "k.txt", "n.bin", "new.txt"});
	if (!committed.ok) throw TestFailure(committed.error);
	UAM_ASSERT_EQ(SvnOutput(copies.temp.root, {"cat", "--ignore-keywords", "--", copies.url + "/k.txt@HEAD"}), std::string("$Revision$\nbody ported"));
}

UAM_TEST(SvnRealSnapshotPortBackRefusesWhenTheWorkingCopyMovedOn)
{
	if (!RealSvnAvailable() || !GitForRealSvn()) return;
	// Another user changes only a property of the file the chat edited. Without keywords its text is unchanged, so the
	// port proceeds; with an expanded keyword the update rewrites the text, the source no longer matches the snapshot's
	// baseline and the whole port is refused.
	for (const bool keyword : {false, true})
	{
		SvnWorkingCopies copies;
		UAM_ASSERT(uam::io::WriteTextFile(copies.checkout / "k.txt", "$Revision$\nbody\n"));
		Svn(copies.checkout, {"add", "--", "k.txt"});
		if (keyword) Svn(copies.checkout, {"propset", "svn:keywords", "Revision", "k.txt"});
		Svn(copies.checkout, {"commit", "-m", "baseline"});
		Svn(copies.checkout, {"update"});
		const std::string before = keyword ? "$Revision: 1 $\nbody\n" : "$Revision$\nbody\n";
		UAM_ASSERT_EQ(ReadFile(copies.checkout / "k.txt"), before);
		SvnSnapshotChat snapshot(copies);
		const fs::path worktree = snapshot.Worktree();
		const std::string edited = keyword ? "$Revision: 1 $\nbody ported\n" : "$Revision$\nbody ported\n";
		UAM_ASSERT(uam::io::WriteTextFile(worktree / "k.txt", edited));

		Svn(copies.other, {"update"});
		Svn(copies.other, {"propset", "uam:fixture", "moved", "k.txt"});
		Svn(copies.other, {"commit", "-m", "property only"});
		Svn(copies.checkout, {"update"});
		const std::string updated = keyword ? "$Revision: 2 $\nbody\n" : before;
		UAM_ASSERT_EQ(ReadFile(copies.checkout / "k.txt"), updated);

		const uam::GitWorktreeOperationResult ported = uam::GitWorktreeService().PortChatChanges(copies.app, snapshot.Chat());
		if (!keyword)
		{
			if (!ported.ok) throw TestFailure("port: " + ported.message);
			UAM_ASSERT_EQ(ReadFile(copies.checkout / "k.txt"), edited);
			continue;
		}
		UAM_ASSERT(!ported.ok);
		UAM_ASSERT_EQ(ported.message, std::string("Source changed after the isolation baseline at k.txt; resolve that conflict before porting."));
		// Nothing was applied: the working copy is as the update left it and the chat keeps its snapshot and edit.
		UAM_ASSERT_EQ(ReadFile(copies.checkout / "k.txt"), updated);
		UAM_ASSERT(!snapshot.Chat().workspace_worktree_directory.empty());
		UAM_ASSERT_EQ(ReadFile(worktree / "k.txt"), edited);
		ChatSession source;
		source.workspace_directory = copies.checkout.string();
		UAM_ASSERT(copies.vcs.Status(copies.app, source, uam::VcsType::Svn, false).changed_files.empty());
	}
}

UAM_TEST(SvnRealUpdateBeforeNewChatRunsBeforeTheManagedSnapshot)
{
	if (!RealSvnAvailable() || !GitForRealSvn()) return;
	SvnWorkingCopies copies;
	copies.CommitFiles(copies.checkout, {{"b.txt", "b\n"}}, "baseline");
	Svn(copies.other, {"update"});
	copies.CommitFiles(copies.other, {{"b.txt", "b\nincoming\n"}}, "incoming edit");
	SvnLaunch launch(copies);
	copies.app.settings.git_enabled = true;
	launch.Chat().workspace_isolation_kind = uam::paths::kPendingGitWorktreeIsolationKind;
	const auto task = launch.Run();
	UAM_ASSERT(task->error.empty() && task->merged);
	// The update runs first, so the chat's snapshot starts from the newest revision.
	UAM_ASSERT_EQ(ReadFile(uam::paths::PathFromUtf8(launch.Chat().workspace_worktree_directory) / "b.txt"), std::string("b\nincoming\n"));
	launch.AwaitSave(task);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(copies.app, launch.Chat()).state, uam::WorktreeLaunchPreparationState::Ready);
	UAM_ASSERT(!ChatRepository::LoadLocalChat(copies.app.data_root, launch.chat_id)->svn_update_pending);
	UAM_ASSERT_EQ(ReadFile(copies.checkout / "b.txt"), std::string("b\nincoming\n"));
	UAM_ASSERT_EQ(copies.WorkingRevisions(), (std::set<std::string>{"2"}));
}
#endif
