#include "test_harness.h"
#include "app/workspace_vcs_context_service.h"

using namespace uam_test;

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
