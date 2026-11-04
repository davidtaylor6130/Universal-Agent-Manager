#include "test_harness.h"
#include "app/workspace_vcs_context_service.h"
#include "app/chat_reference_context.h"

using namespace uam_test;

UAM_TEST(ChatReferenceValuesAreBoundedInBytesAndRejectControlCharacters)
{
	UAM_ASSERT(uam::BoundedChatReferenceValue("feature/context", 1024));
	UAM_ASSERT(!uam::BoundedChatReferenceValue(std::string(1025, 'x'), 1024));
	UAM_ASSERT(!uam::BoundedChatReferenceValue("/workspace\n/other", 4096));
	UAM_ASSERT(!uam::BoundedChatReferenceValue(std::string("a\0b", 3), 4096));
}

UAM_TEST(ChatReferenceContextRejectsChangedBirthHostAndWorktreeAssociation)
{
	ChatSession captured;
	captured.id = "reference-owner";
	captured.created_at = "2026-10-23 12:00:00";
	captured.execution_host_id = "local";
	captured.folder_id = "workspace";
	captured.workspace_directory = "/source";
	captured.workspace_isolation_kind = "gitWorktree";
	captured.workspace_source_directory = "/source";
	captured.workspace_worktree_directory = "/worktree";
	captured.workspace_branch_name = "feature/context";
	captured.workspace_base_ref = "main";
	UAM_ASSERT(uam::SameChatReferenceContext(captured, captured));
	for (std::string ChatSession::* field : {&ChatSession::id, &ChatSession::created_at, &ChatSession::execution_host_id,
	         &ChatSession::folder_id, &ChatSession::workspace_directory, &ChatSession::workspace_isolation_kind,
	         &ChatSession::workspace_source_directory, &ChatSession::workspace_worktree_directory,
	         &ChatSession::workspace_branch_name, &ChatSession::workspace_base_ref})
	{
		ChatSession current = captured;
		current.*field += "-changed";
		UAM_ASSERT(!uam::SameChatReferenceContext(current, captured));
	}
	ChatSession passive = captured;
	passive.imported_read_only = true;
	UAM_ASSERT(!uam::SameChatReferenceContext(passive, captured));
	ChatSession replacement = captured;
	replacement.metadata_incarnation = std::make_shared<const std::uint8_t>(0);
	UAM_ASSERT(!uam::SameChatReferenceContext(replacement, captured));
	ChatSession display_only = captured;
	display_only.title = "Renamed chat";
	UAM_ASSERT(uam::SameChatReferenceContext(display_only, captured));
}

UAM_TEST(WorkspaceVcsContextReadsOnlyLocalGitBranchesAndDetachedCommit)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-local-branch-context");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "source.txt", "source survivor\n"));
	UAM_ASSERT(RunGitForTest(repo, "add source.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m base"));
	UAM_ASSERT(RunGitForTest(repo, "branch feature/base"));
	UAM_ASSERT(RunGitForTest(repo, "tag excluded-tag"));
	UAM_ASSERT(RunGitForTest(repo, "update-ref refs/remotes/origin/excluded HEAD"));
	const uam::WorkspaceVcsContext attached = uam::ReadWorkspaceVcsContext(repo, true, false);
	UAM_ASSERT(attached.error.empty());
	UAM_ASSERT_EQ(attached.vcs, "git");
	UAM_ASSERT(!attached.detached);
	UAM_ASSERT(!attached.current_branch.empty());
	UAM_ASSERT_EQ(attached.local_branches.size(), 2u);
	bool found = false;
	for (const uam::WorkspaceGitBranch& branch : attached.local_branches)
	{
		UAM_ASSERT_EQ(branch.commit, attached.current_commit);
		if (branch.name == "feature/base") found = true;
		UAM_ASSERT(branch.name != "excluded-tag" && branch.name != "origin/excluded");
	}
	UAM_ASSERT(found);
	UAM_ASSERT(RunGitForTest(repo, "checkout --detach HEAD"));
	const uam::WorkspaceVcsContext detached = uam::ReadWorkspaceVcsContext(repo, true, false);
	UAM_ASSERT(detached.error.empty());
	UAM_ASSERT(detached.detached);
	UAM_ASSERT(detached.current_branch.empty());
	UAM_ASSERT_EQ(detached.current_commit, attached.current_commit);
	UAM_ASSERT_EQ(ReadFile(repo / "source.txt"), "source survivor\n");
	UAM_ASSERT(!fs::exists(temp.root / "worktrees"));
}

UAM_TEST(WorkspaceVcsContextListsNewestCommitBranchFirst)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-branch-commit-order");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "source.txt", "base\n"));
	UAM_ASSERT(RunGitForTest(repo, "add source.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m base"));
	UAM_ASSERT(RunGitForTest(repo, "branch aaa-older"));
	UAM_ASSERT(RunGitForTest(repo, "checkout -b zzz-newer"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "source.txt", "newer\n"));
	{
		ScopedEnvVar date("GIT_COMMITTER_DATE", "2090-01-01T00:00:00+0000");
		UAM_ASSERT(RunGitForTest(repo, "commit -am newer"));
	}
	UAM_ASSERT(RunGitForTest(repo, "checkout aaa-older"));
	const uam::WorkspaceVcsContext context = uam::ReadWorkspaceVcsContext(repo, true, false);
	UAM_ASSERT(context.error.empty());
	UAM_ASSERT_EQ(context.local_branches.size(), 3u);
	// The furthest-ahead branch leads even though it sorts last by name and is not checked out.
	UAM_ASSERT_EQ(context.local_branches.front().name, "zzz-newer");
}

UAM_TEST(WorkspaceVcsContextDisabledCanceledAndPlainDirectoryDoNotMutate)
{
	TempDir temp("uam-local-context-policies");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "keep.txt", "foreign survivor\n"));
	const uam::WorkspaceVcsContext disabled = uam::ReadWorkspaceVcsContext(temp.root, false, false);
	UAM_ASSERT(disabled.vcs.empty() && disabled.error.empty());
	std::stop_source cancel;
	cancel.request_stop();
	const uam::WorkspaceVcsContext canceled = uam::ReadWorkspaceVcsContext(temp.root, true, true, cancel.get_token());
	UAM_ASSERT(!canceled.error.empty());
	const uam::WorkspaceVcsContext plain = uam::ReadWorkspaceVcsContext(temp.root, true, true);
	UAM_ASSERT(plain.vcs.empty());
	UAM_ASSERT(!uam::ReadWorkspaceVcsContext("relative", true, true).error.empty());
	UAM_ASSERT_EQ(ReadFile(temp.root / "keep.txt"), "foreign survivor\n");
	UAM_ASSERT(!fs::exists(temp.root / ".git"));
	UAM_ASSERT(!fs::exists(temp.root / ".svn"));
}

UAM_TEST(WorkspaceVcsContextSvnReportsActualCheckoutWithoutBranchInference)
{
	if (!RunTestCommand("svn --version --quiet") || !RunTestCommand("svnadmin --version --quiet")) return;
	TempDir temp("uam-local-svn-context");
	const fs::path repository = temp.root / "repository";
	const fs::path checkout = temp.root / "checkout";
	UAM_ASSERT(RunTestCommand("svnadmin create " + ShellQuoteForTest(repository.string())));
	#ifdef _WIN32
	const std::string url = "file:///" + repository.generic_string();
#else
	const std::string url = "file://" + uam::paths::Utf8PathString(repository);
#endif
	UAM_ASSERT(RunTestCommand("svn checkout --non-interactive " + ShellQuoteForTest(url) + " " + ShellQuoteForTest(checkout.string())));
	UAM_ASSERT(uam::io::WriteTextFile(checkout / "keep.txt", "untracked survivor\n"));
	const uam::WorkspaceVcsContext context = uam::ReadWorkspaceVcsContext(checkout, false, true);
	UAM_ASSERT(context.error.empty());
	UAM_ASSERT_EQ(context.vcs, "svn");
	UAM_ASSERT_EQ(context.checkout_url, url);
	UAM_ASSERT_EQ(context.revision, "0");
	UAM_ASSERT(context.local_branches.empty() && context.current_branch.empty());
	UAM_ASSERT_EQ(ReadFile(checkout / "keep.txt"), "untracked survivor\n");
}

UAM_TEST(WorkspaceVcsContextProbeSlotsReleaseSkippedAndCompletedWorkers)
{
	const std::shared_ptr<std::atomic<unsigned>> pending = std::make_shared<std::atomic<unsigned>>(0);
	std::vector<std::shared_ptr<void>> workers;
	for (unsigned index = 0; index < 4; ++index)
	{
		workers.push_back(uam::AcquireWorkspaceVcsContextSlot(pending));
		UAM_ASSERT(workers.back() != nullptr);
	}
	UAM_ASSERT(!uam::AcquireWorkspaceVcsContextSlot(pending));
	workers.pop_back(); // A queued worker discarded before Execute releases its capture.
	UAM_ASSERT_EQ(pending->load(), 3u);
	std::shared_ptr<void> replacement = uam::AcquireWorkspaceVcsContextSlot(pending);
	UAM_ASSERT(replacement != nullptr);
	workers.clear();
	replacement.reset();
	UAM_ASSERT_EQ(pending->load(), 0u);
}
