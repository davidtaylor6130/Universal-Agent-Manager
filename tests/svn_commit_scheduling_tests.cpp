#include "test_harness.h"
#include "common/security/command_safety.h"
#include "common/security/read_only_command.h"

using namespace uam_test;

#ifndef _WIN32
namespace
{
	struct SvnSchedulingFixture
	{
		TempDir temporary{"uam-svn-scheduling"};
		fs::path workspace = temporary.root / "checkout with spaces";
		fs::path commands = temporary.root / "commands";
		uam::AppState app;
		ChatSession chat;
		std::unique_ptr<ScopedEnvVar> path;

		SvnSchedulingFixture()
		{
			fs::create_directories(workspace / ".svn");
			chat.workspace_directory = workspace.string();
			app.settings.svn_enabled = true;
			app.settings.git_enabled = false;
			const fs::path shim = temporary.root / "svn";
			const std::string script = "#!/bin/sh\n"
				"printf '%s\\n' \"$*\" >> " + ShellQuoteForTest(commands.string()) + "\n"
				"printf '\"%s\" ' \"$@\" >> " + ShellQuoteForTest((temporary.root / "argv").string()) + "\n"
				"printf '\\n' >> " + ShellQuoteForTest((temporary.root / "argv").string()) + "\n"
				"case \"$1\" in\n"
				"info) printf '1\\n';;\n"
				"status) for arg do target=\"$arg\"; done; case \"$*\" in *--depth*) target=${target%@}; if test -f " + ShellQuoteForTest((temporary.root / "retained").string()) + "; then case \"$target\" in *nested|*name@.txt) printf 'A       %s\\n' \"$target\"; exit 0;; esac; fi; case \"$target\" in *missing.txt) printf '!       %s\\n' \"$target\";; *new.txt|*name@.txt|*nested) printf '?       %s\\n' \"$target\";; *) printf 'M       %s\\n' \"$target\";; esac;;\n"
				// Like real svn, the panel query echoes paths in the form of its target (absolute targets print absolute paths).
				"*) case \"$target\" in .) cat " + ShellQuoteForTest((temporary.root / "noise").string()) + "; printf '?       new.txt\\n!       missing.txt\\n';; /*) cat " + ShellQuoteForTest((temporary.root / "noise").string()) + "; printf '?       %s/new.txt\\n!       %s/missing.txt\\n' \"$target\" \"$target\";; *) target=${target%@}; printf 'A       %s\\n' \"$target\"; if test -d \"$target\"; then for child in \"$target\"/*; do printf 'A       %s\\n' \"$child\"; done; fi;; esac;; esac;;\n"
				"add|delete) test ! -f " + ShellQuoteForTest((temporary.root / "fail").string()) + ";;\n"
				"commit) test ! -f " + ShellQuoteForTest((temporary.root / "fail-commit").string()) + " || exit 1; test ! -f " + ShellQuoteForTest((temporary.root / "retained").string()) + " || exit 0; grep -q '^add ' " + ShellQuoteForTest(commands.string()) + " && grep -q '^delete ' " + ShellQuoteForTest(commands.string()) + ";;\n"
				"diff) :;;\n"
				"*) exit 1;;\nesac\n";
			UAM_ASSERT(uam::io::WriteTextFile(shim, script));
			fs::permissions(shim, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
			path = std::make_unique<ScopedEnvVar>("PATH", temporary.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or(""));
			UAM_ASSERT(uam::io::WriteTextFile(workspace / "new.txt", "selected\n"));
			// Changelist, external and tree-conflict lines that real `svn status` interleaves with file rows.
			UAM_ASSERT(uam::io::WriteTextFile(temporary.root / "noise", "\n--- Changelist 'review':\nX       vendor\n\nPerforming status on external item at 'vendor':\n      >   local file edit, incoming file delete or move upon update\nSummary of conflicts:\n  Tree conflicts: 1\n"));
		}

		std::vector<std::vector<std::string>> Calls(const std::string& action) const
		{
			std::vector<std::vector<std::string>> calls;
			std::istringstream lines(uam::io::ReadTextFile(temporary.root / "argv"));
			std::string line;
			while (std::getline(lines, line))
			{
				std::vector<std::string> argv = uam::command_line::SplitWords(line);
				if (!argv.empty() && argv.front() == action) calls.push_back(std::move(argv));
			}
			return calls;
		}
	};
}

UAM_TEST(SvnExplicitCommitSchedulesSelectedAddedAndMissingPaths)
{
	SvnSchedulingFixture fixture;
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(fixture.app, fixture.chat, uam::VcsType::Svn, "private fixture", {"new.txt", "missing.txt"});
	UAM_ASSERT(result.ok);
	const std::string commands = uam::io::ReadTextFile(fixture.commands);
	// A selected new item is added with its contents (for a file this equals empty depth).
	UAM_ASSERT(commands.find("add --non-interactive --depth infinity -- new.txt@") != std::string::npos);
	UAM_ASSERT(commands.find("delete --non-interactive --keep-local -- ") != std::string::npos);
	UAM_ASSERT(commands.find("commit --non-interactive --depth empty ") != std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.workspace / "new.txt"), "selected\n");
}

UAM_TEST(SvnExplicitCommitRefusesEscapingSelectionBeforeScheduling)
{
	SvnSchedulingFixture fixture;
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(fixture.app, fixture.chat, uam::VcsType::Svn, "private fixture", {"../outside.txt"});
	UAM_ASSERT(!result.ok);
	const std::string commands = uam::io::ReadTextFile(fixture.commands);
	UAM_ASSERT(commands.find("\nadd ") == std::string::npos);
	UAM_ASSERT(commands.find("\ncommit ") == std::string::npos);
}

UAM_TEST(SvnSchedulingFailureRetainsFilesAndNeverPublishes)
{
	SvnSchedulingFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temporary.root / "fail", "fail\n"));
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(fixture.app, fixture.chat, uam::VcsType::Svn, "private fixture", {"new.txt"});
	UAM_ASSERT(!result.ok);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.workspace / "new.txt"), "selected\n");
	UAM_ASSERT(uam::io::ReadTextFile(fixture.commands).find("\ncommit ") == std::string::npos);
}
UAM_TEST(SvnNestedPegNamedAdditionSchedulesOnlyShallowParents)
{
	SvnSchedulingFixture fixture;
	fs::create_directories(fixture.workspace / "nested");
	UAM_ASSERT(uam::io::WriteTextFile(fixture.workspace / "nested" / "name@.txt", "nested selected\n"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.workspace / "nested" / "unselected.txt", "retain\n"));
	(void)uam::VcsCommitService().Commit(fixture.app, fixture.chat, uam::VcsType::Svn, "private fixture", {"nested/name@.txt"});
	const std::string commands = uam::io::ReadTextFile(fixture.commands);
	UAM_ASSERT(commands.find("name@.txt@") != std::string::npos);
	bool parent_added = false;
	bool leaf_added = false;
	for (const std::vector<std::string>& argv : fixture.Calls("add"))
	{
		UAM_ASSERT_EQ(argv.size(), 6U);
		UAM_ASSERT_EQ(argv[4], "--");
		// Workspace-relative peg targets; only the selected leaf brings its own contents.
		parent_added = parent_added || (argv.back() == "nested@" && argv[3] == "empty");
		leaf_added = leaf_added || (argv.back() == "nested/name@.txt@" && argv[3] == "infinity");
	}
	UAM_ASSERT(parent_added);
	UAM_ASSERT(leaf_added);
	UAM_ASSERT(commands.find("unselected.txt") == std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.workspace / "nested" / "unselected.txt"), "retain\n");
}

UAM_TEST(SvnPublicationFailureNeverRevertsOrRetriesLocalScheduling)
{
	SvnSchedulingFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temporary.root / "fail-commit", "fail\n"));
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(fixture.app, fixture.chat, uam::VcsType::Svn, "private fixture", {"new.txt", "missing.txt"});
	UAM_ASSERT(!result.ok);
	const std::string commands = uam::io::ReadTextFile(fixture.commands);
	const std::size_t commit = commands.find("\ncommit ");
	UAM_ASSERT(commit != std::string::npos);
	UAM_ASSERT(commands.find("\ncommit ", commit + 1) == std::string::npos);
	UAM_ASSERT(commands.find("revert") == std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.workspace / "new.txt"), "selected\n");
}
UAM_TEST(SvnRetainedSchedulingRetryIncludesAddedParentWithoutUnselectedChildren)
{
	SvnSchedulingFixture fixture;
	fs::create_directories(fixture.workspace / "nested");
	UAM_ASSERT(uam::io::WriteTextFile(fixture.workspace / "nested" / "name@.txt", "selected\n"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.workspace / "nested" / "unselected.txt", "retain\n"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temporary.root / "fail-commit", "fail\n"));
	UAM_ASSERT(!uam::VcsCommitService().Commit(fixture.app, fixture.chat, uam::VcsType::Svn, "first attempt", {"nested/name@.txt", "missing.txt"}).ok);
	fs::remove(fixture.temporary.root / "fail-commit");
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temporary.root / "retained", "scheduled\n"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.commands, ""));
	const uam::VcsCommitResult retry = uam::VcsCommitService().Commit(fixture.app, fixture.chat, uam::VcsType::Svn, "explicit retry", {"nested/name@.txt"});
	UAM_ASSERT(retry.ok);
	const std::string commands = uam::io::ReadTextFile(fixture.commands);
	const std::size_t commit = commands.find("commit --non-interactive --depth empty ");
	UAM_ASSERT(commit != std::string::npos);
	const std::string publication = commands.substr(commit);
	UAM_ASSERT(publication.find(" nested@ ") != std::string::npos);
	UAM_ASSERT(publication.find("name@.txt@") != std::string::npos);
	UAM_ASSERT(commands.find("add ") == std::string::npos);
	UAM_ASSERT(commands.find("unselected.txt") == std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.workspace / "nested" / "unselected.txt"), "retain\n");
}
#endif

#ifndef _WIN32
UAM_TEST(SvnPanelStatusPathsCommitWhenSvnEchoesAbsoluteTargets)
{
	// Regression: `svn status <absolute workspace>` printed absolute rows that the commit then rejected.
	SvnSchedulingFixture fixture;
	const uam::VcsCommitStatus status = uam::VcsCommitService().Status(fixture.app, fixture.chat, uam::VcsType::Svn, false);
	UAM_ASSERT(status.error.empty());
	UAM_ASSERT_EQ(status.changed_files.size(), 3U);
	std::vector<std::string> selected;
	for (const uam::VcsChangedFile& file : status.changed_files)
		if (file.status != "X") selected.push_back(file.path);
	UAM_ASSERT_EQ(selected.size(), 2U);
	UAM_ASSERT_EQ(selected[0], "new.txt");
	UAM_ASSERT_EQ(selected[1], "missing.txt");
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(fixture.app, fixture.chat, uam::VcsType::Svn, "private fixture", selected);
	UAM_ASSERT(result.ok);
	for (const std::vector<std::string>& argv : fixture.Calls("status"))
		UAM_ASSERT(!uam::paths::PathFromUtf8(argv.back()).is_absolute());
}
#endif

UAM_TEST(SvnStatusParserAcceptsOnlyValidStatusRows)
{
	const std::string output =
	    "M       plain.txt\n"
	    " M      props-only\n"
	    "A  +    copied.txt\n"
	    "      C tree.txt\n"
	    "      >   local file edit, incoming file delete or move upon update\n"
	    "\n--- Changelist 'review':\n"
	    "M       in-list.txt\n"
	    "X       vendor\n"
	    "\nPerforming status on external item at 'vendor':\n"
	    "M       vendor/lib.c\n"
	    "Summary of conflicts:\n"
	    "  Tree conflicts: 1\n"
	    "svn: warning: W155010: The node 'private' was not found.\n"
	    "?       caf\xC3\xA9 name.txt\r\n"
	    "?       /workspace/root/absolute.txt\n";
	const std::vector<uam::VcsChangedFile> files = uam::ParseSvnStatusOutput(output, "/workspace/root");
	const std::vector<std::pair<std::string, std::string>> expected = {
	    {"plain.txt", "M"}, {"props-only", " M"}, {"copied.txt", "A  +"}, {"tree.txt", "      C"}, {"in-list.txt", "M"},
	    {"vendor", "X"}, {"vendor/lib.c", "M"}, {"caf\xC3\xA9 name.txt", "?"}, {"absolute.txt", "?"}};
	UAM_ASSERT_EQ(files.size(), expected.size());
	for (std::size_t index = 0; index < expected.size(); ++index)
	{
		UAM_ASSERT_EQ(files[index].path, expected[index].first);
		UAM_ASSERT_EQ(files[index].status, expected[index].second);
	}
}

UAM_TEST(SvnRemoteSchedulingUsesPegTargetsEmptyDepthAndFolderContents)
{
	std::vector<std::vector<std::string>> calls;
	std::string kind = "file";
	const auto svn = [&](const std::vector<std::string>& args)
	{
		calls.push_back(args);
		ProcessExecutionResult result;
		result.ok = true;
		result.exit_code = 0;
		const std::string& target = args.back();
		if (args[0] == "status" && args[2] == "--depth")
			result.output = target == "dir@" ? "?       dir\n" : target == "gone.txt@" ? "!       gone.txt\n" : "M       " + target.substr(0, target.size() - 1) + "\n";
		else if (args[0] == "status") result.output = "A       dir\nA       dir/f.txt\n";
		else if (args[0] == "info") result.output = kind + "\n";
		return result;
	};
	const auto find = [&](const std::string& action)
	{
		const auto found = std::ranges::find_if(calls, [&](const std::vector<std::string>& call) { return call.front() == action; });
		return found == calls.end() ? std::vector<std::string>{} : *found;
	};
	std::string error;
	UAM_ASSERT(uam::VcsCommitService::CommitSvnSelectionForTests(svn, "-message", {"dir", "gone.txt", "-dash.txt", "a@b.txt"}, error));
	UAM_ASSERT_EQ(find("add"), (std::vector<std::string>{"add", "--non-interactive", "--depth", "infinity", "--", "dir@"}));
	UAM_ASSERT_EQ(find("delete"), (std::vector<std::string>{"delete", "--non-interactive", "--keep-local", "--", "gone.txt@"}));
	UAM_ASSERT_EQ(find("commit"), (std::vector<std::string>{"commit", "--non-interactive", "--depth", "empty", "--force-log", "-m", "-message", "--",
	                                                       "-dash.txt@", "a@b.txt@", "dir@", "gone.txt@", "dir/f.txt@"}));

	// Deleted directories and escaping selections are refused before any scheduling.
	for (const std::vector<std::string>& selection : std::vector<std::vector<std::string>>{{"gone.txt"}, {"../outside.txt"}, {"/absolute.txt"}, {"dir/.svn/entries"}})
	{
		calls.clear();
		kind = "dir";
		UAM_ASSERT(!uam::VcsCommitService::CommitSvnSelectionForTests(svn, "message", selection, error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT(find("add").empty() && find("delete").empty() && find("commit").empty());
	}
}

UAM_TEST(SvnLineStatsFallBackToEachFileAndOlderPegSpelling)
{
	// A failed workspace diff still counts each file; an older client that needs the peg '@' still diffs '@' names.
	std::vector<std::vector<std::string>> calls;
	const auto svn = [&](const std::vector<std::string>& args)
	{
		calls.push_back(args);
		ProcessExecutionResult result;
		result.ok = true;
		const std::string& target = args.back();
		result.exit_code = target == "edit.txt" || target == "a@1.txt@" || target == "a@1.txt" ? 0 : 1;
		if (target == "edit.txt") result.output = "Index: edit.txt\n--- edit.txt\n+++ edit.txt\n@@ -1 +1,2 @@\n-old\n+new\n+more\n";
		else if (target == "a@1.txt@") result.output = "Index: a@1.txt\n--- a@1.txt\n+++ a@1.txt\n@@ -1,2 +1 @@\n-x\n-y\n+z\n";
		// An older client reads the unescaped name as a peg revision of another path and succeeds with the wrong diff.
		else if (target == "a@1.txt") result.output = "Index: a\n--- a\n+++ a\n@@ -1 +1,9 @@\n+wrong\n";
		return result;
	};
	std::vector<uam::VcsChangedFile> files = {{"edit.txt", "M", true}, {"a@1.txt", "M", true}, {"new.txt", "?", true}};
	uam::VcsCommitService::ApplySvnLineStatsForTests(svn, files);
	UAM_ASSERT_EQ(files[0].additions, 2);
	UAM_ASSERT_EQ(files[0].deletions, 1);
	UAM_ASSERT_EQ(files[1].additions, 1);
	UAM_ASSERT_EQ(files[1].deletions, 2);
	UAM_ASSERT_EQ(files[2].additions, 0);
	for (const std::vector<std::string>& call : calls)
		UAM_ASSERT(call.back() != "new.txt" && call.back() != "new.txt@");
}

UAM_TEST(SvnReadOnlyCommandsAvoidPromptsWithoutAllowingWritesOrDiffHelpers)
{
	using uam::command_safety::ClassifyCommand;
	using uam::command_safety::RiskLevel;
	for (const std::string_view command : {"svn status", "svn st -u", "svn info --show-item url", "svn log -l 5 -v", "svn ls -R",
	         "svn cat -r 3 README.md", "svn blame README.md", "svn propget svn:ignore .", "svn proplist -v .", "svn diff --internal-diff -r 3 -- README.md"})
	{
		UAM_ASSERT(uam::command_safety::IsReadOnlyCommand(command));
		UAM_ASSERT_EQ(ClassifyCommand(command), RiskLevel::Allowed);
	}
	for (const std::string_view command : {"svn", "svn diff", "svn diff --diff-cmd writer --internal-diff", "svn log --diff", "svn commit -m change",
	         "svn update", "svn revert -R .", "svn propset svn:ignore x .", "svn cleanup", "svn status --config-option config:helpers:diff-cmd=writer",
	         "svn diff -- notes--internal-diff.txt", "svn diff x--internal-diff", "svn diff -- --internal-diff"})
	{
		UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(command));
		UAM_ASSERT_EQ(ClassifyCommand(command), RiskLevel::WarnHigh);
	}
}

UAM_TEST(NearestVcsRootLabelsGitNestedInsideSvnCheckoutAsGit)
{
	TempDir temp("uam-nearest-vcs-root");
	const fs::path outer = temp.root / "outer";
	const fs::path inner = outer / "inner";
	fs::create_directories(outer / ".svn");
	fs::create_directories(inner / ".git");
	fs::create_directories(inner / "src");
	fs::create_directories(outer / "plain");
	const uam::paths::NearestVcsRoot nested = uam::paths::FindNearestVcsRoot(inner / "src");
	UAM_ASSERT(nested.git && !nested.svn);
	UAM_ASSERT_EQ(nested.root, inner);
	const uam::paths::NearestVcsRoot svn = uam::paths::FindNearestVcsRoot(outer / "plain");
	UAM_ASSERT(svn.svn && !svn.git);
	UAM_ASSERT_EQ(svn.root, outer);

	uam::AppState app;
	app.settings.git_enabled = false;
	app.settings.svn_enabled = true;
	ChatSession chat;
	chat.workspace_directory = inner.string();
	UAM_ASSERT(!uam::VcsCommitService().Status(app, chat, uam::VcsType::Svn, false).available);
	std::vector<uam::GitChatBranch> branches;
	bool is_svn = true;
	std::string error;
	(void)uam::GitWorktreeService().ListChatBranches(app, chat, branches, is_svn, error);
	UAM_ASSERT(!is_svn);
	chat.workspace_directory = (outer / "plain").string();
	(void)uam::GitWorktreeService().ListChatBranches(app, chat, branches, is_svn, error);
	UAM_ASSERT(is_svn);
}

UAM_TEST(SvnCommitHoldsWorkspaceLeaseAndChatOperationGuard)
{
	const fs::path source_root = fs::path(__FILE__).parent_path().parent_path();
	const std::string source = ReadFile(source_root / "src/cef/vcs_handlers.cpp");
	const std::size_t begin = source.find("void UamQueryHandler::HandleCommitVcsChanges");
	UAM_ASSERT(begin != std::string::npos);
	const std::string body = source.substr(begin, source.find("void UamQueryHandler::", begin + 1) - begin);
	const std::size_t lease = body.find("worktree_retention_gate.TryAcquireMutation()");
	const std::size_t guard = body.find("worktree_operation_chat_ids.insert(chat_id)");
	UAM_ASSERT(lease != std::string::npos && guard != std::string::npos);
	UAM_ASSERT(lease < body.find("RunAsyncCefQuery") && guard < body.find("RunAsyncCefQuery"));
	UAM_ASSERT(body.find("files, lease, remote_vcs_lease]") != std::string::npos);
	UAM_ASSERT(body.find("live_app->worktree_operation_chat_ids.erase(chat_id)") != std::string::npos);
}

#ifndef _WIN32
namespace
{
	struct RealSvnCheckout
	{
		TempDir temporary{"uam-real-svn"};
		fs::path repository = temporary.root / "repository";
		fs::path workspace = temporary.root / "checkout";
		std::string url;
		uam::AppState app;
		ChatSession chat;
		bool ready = false;

		RealSvnCheckout()
		{
			if (!RunTestCommand("svn --version --quiet") || !RunTestCommand("svnadmin --version --quiet"))
			{
				std::fprintf(stderr, "SKIP real-svn case: svn or svnadmin is not installed\n");
				return;
			}
			UAM_ASSERT(RunTestCommand("svnadmin create " + ShellQuoteForTest(repository.string())));
			url = "file://" + uam::paths::Utf8PathString(repository);
			UAM_ASSERT(Svn(temporary.root, {"checkout", url, "checkout"}));
			chat.workspace_directory = workspace.string();
			app.settings.svn_enabled = true;
			app.settings.git_enabled = false;
			ready = true;
		}

		bool Svn(const fs::path& cwd, std::vector<std::string> args) const
		{
			args.insert(args.begin() + 1, "--non-interactive");
			const ProcessExecutionResult result = uam::RunSvn(cwd, args);
			return result.ok && result.exit_code == 0;
		}

		bool Svn(std::vector<std::string> args) const { return Svn(workspace, std::move(args)); }

		std::set<std::string> ChangedPaths() const
		{
			std::set<std::string> paths;
			const uam::VcsCommitStatus status = uam::VcsCommitService().Status(app, chat, uam::VcsType::Svn, false);
			UAM_ASSERT(status.error.empty());
			for (const uam::VcsChangedFile& file : status.changed_files) paths.insert(file.path);
			return paths;
		}
	};
}

UAM_TEST(SvnRealLocalCommitPublishesSelectedChangesWithUtf8Paths)
{
	RealSvnCheckout svn;
	if (!svn.ready) return;
	// A GUI launch can inherit the C locale; svn must still exchange UTF-8 paths with UAM.
	ScopedEnvVar locale("LC_ALL", "C");
	ScopedEnvVar language("LANG", "C");
	const std::string cafe = "caf\xC3\xA9.txt";
	for (const std::string& name : {std::string("tracked.txt"), std::string("-dash.txt"), std::string("at@name.txt"), cafe, std::string("doomed.txt"), std::string("unselected.txt")})
		UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / uam::paths::PathFromUtf8(name), "v1\n"));
	UAM_ASSERT(svn.Svn({"add", "--", "tracked.txt", "-dash.txt", "at@name.txt@", cafe, "doomed.txt", "unselected.txt"}));
	UAM_ASSERT(svn.Svn({"propset", "svn:global-ignores", "*.log", "."}));
	UAM_ASSERT(svn.Svn({"commit", "-m", "baseline"}));
	for (const std::string& name : {std::string("tracked.txt"), std::string("-dash.txt"), std::string("at@name.txt"), cafe, std::string("unselected.txt")})
		UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / uam::paths::PathFromUtf8(name), "v2\n"));
	fs::remove(svn.workspace / "doomed.txt");
	fs::create_directories(svn.workspace / "newdir" / "sub");
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "newdir" / "a.txt", "new\n"));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "newdir" / "sub" / "b.txt", "new\n"));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "newdir" / "skip.log", "ignored\n"));
	UAM_ASSERT(svn.Svn({"changelist", "review", "tracked.txt"}));

	const std::set<std::string> changed = svn.ChangedPaths();
	UAM_ASSERT_EQ(changed, (std::set<std::string>{"tracked.txt", "-dash.txt", "at@name.txt", cafe, "doomed.txt", "unselected.txt", "newdir"}));
	std::vector<std::string> selected;
	for (const std::string& path : changed)
		if (path != "unselected.txt") selected.push_back(path);
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(svn.app, svn.chat, uam::VcsType::Svn, "Publish selection\nwith 50% detail", selected);
	if (!result.ok) std::fprintf(stderr, "commit: %s\n", result.error.c_str());
	UAM_ASSERT(result.ok);
	UAM_ASSERT_EQ(svn.ChangedPaths(), (std::set<std::string>{"unselected.txt"}));
	const ProcessExecutionResult listed = uam::RunSvn(svn.workspace, {"list", "--non-interactive", "-R", "--", svn.url});
	UAM_ASSERT(listed.output.find("newdir/sub/b.txt") != std::string::npos);
	UAM_ASSERT(listed.output.find("newdir/a.txt") != std::string::npos);
	UAM_ASSERT(listed.output.find("skip.log") == std::string::npos);
	UAM_ASSERT(listed.output.find("doomed.txt") == std::string::npos);
	UAM_ASSERT(listed.output.find(cafe) != std::string::npos);
	const ProcessExecutionResult log = uam::RunSvn(svn.workspace, {"log", "--non-interactive", "-l", "1", "--", svn.url});
	UAM_ASSERT(log.output.find("Publish selection\nwith 50% detail") != std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(svn.workspace / "unselected.txt"), "v2\n");
}

UAM_TEST(SvnRealChatChangesCountAndOpenTrackedAndNewFiles)
{
	RealSvnCheckout svn;
	if (!svn.ready) return;
	for (const std::string name : {"tracked.txt", "at@name.txt"})
		UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / name, "one\ntwo\n"));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "image.bin", std::string("a\0b", 3)));
	UAM_ASSERT(svn.Svn({"add", "--", "tracked.txt", "at@name.txt@", "image.bin"}));
	UAM_ASSERT(svn.Svn({"commit", "-m", "baseline"}));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "image.bin", std::string("c\0d", 3)));
	fs::create_directories(svn.workspace / "added");
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "added" / "f.txt", "x\ny\n"));
	UAM_ASSERT(svn.Svn({"add", "--", "added"}));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "tracked.txt", "one\nchanged\nthree\n"));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "at@name.txt", "one\n"));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "new.txt", "a\nb\nc"));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "bin.dat", std::string("x\0y", 3)));
	fs::create_directories(svn.workspace / "newdir");
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "newdir" / "inner.txt", "inner\n"));

	const uam::VcsCommitStatus status = uam::VcsCommitService().Status(svn.app, svn.chat, uam::VcsType::Svn, true, "chat");
	UAM_ASSERT(status.error.empty());
	std::map<std::string, uam::VcsChangedFile> files;
	for (const uam::VcsChangedFile& file : status.changed_files) files[file.path] = file;
	UAM_ASSERT_EQ(files.size(), static_cast<std::size_t>(8));
	// The scheduled folder's own row must not repeat its child's lines.
	UAM_ASSERT_EQ(files["added"].additions, 0);
	UAM_ASSERT_EQ(files["added/f.txt"].additions, 2);
	UAM_ASSERT(files["image.bin"].binary);
	UAM_ASSERT_EQ(files["tracked.txt"].additions, 2);
	UAM_ASSERT_EQ(files["tracked.txt"].deletions, 1);
	UAM_ASSERT_EQ(files["at@name.txt"].additions, 0);
	UAM_ASSERT_EQ(files["at@name.txt"].deletions, 1);
	UAM_ASSERT_EQ(files["new.txt"].additions, 3);
	UAM_ASSERT(files["bin.dat"].binary);
	UAM_ASSERT_EQ(files["newdir"].additions, 0);

	const auto diff = [&](const std::string& path, std::string& error) { return uam::VcsCommitService().Diff(svn.app, svn.chat, path, uam::VcsType::Svn, &error, "chat"); };
	std::string error;
	UAM_ASSERT(diff("tracked.txt", error).find("+changed\n+three") != std::string::npos);
	UAM_ASSERT(error.empty());
	UAM_ASSERT(diff("at@name.txt", error).find("-two") != std::string::npos);
	UAM_ASSERT(error.empty());
	const std::string added = diff("new.txt", error);
	UAM_ASSERT(added.find("@@ -0,0 +1,3 @@\n+a\n+b\n+c\n\\ No newline at end of file") != std::string::npos);
	UAM_ASSERT(error.empty());
	UAM_ASSERT(diff("bin.dat", error).find("binary") != std::string::npos);
	UAM_ASSERT(diff("newdir", error).empty());
	UAM_ASSERT(!error.empty());
}

UAM_TEST(SvnRealStatusSkipsExternalHeadersAndRefusesExternals)
{
	RealSvnCheckout svn;
	if (!svn.ready) return;
	UAM_ASSERT(svn.Svn(svn.temporary.root, {"mkdir", "-m", "external", "--", svn.url + "/ext"}));
	UAM_ASSERT(svn.Svn({"update"}));
	UAM_ASSERT(svn.Svn({"propset", "svn:externals", "^/ext vendor", "."}));
	UAM_ASSERT(svn.Svn({"commit", "-m", "externals"}));
	UAM_ASSERT(svn.Svn({"update"}));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "vendor" / "lib.txt", "external change\n"));
	const std::set<std::string> changed = svn.ChangedPaths();
	UAM_ASSERT(changed.contains("vendor"));
	for (const std::string& path : changed)
		UAM_ASSERT(path == "vendor" || path.starts_with("vendor"));
	UAM_ASSERT(changed.contains("vendor/lib.txt"));
	for (const std::string& selection : {"vendor", "vendor/lib.txt"})
	{
		const uam::VcsCommitResult result = uam::VcsCommitService().Commit(svn.app, svn.chat, uam::VcsType::Svn, "external", {selection});
		UAM_ASSERT(!result.ok);
		UAM_ASSERT_EQ(result.error, "Resolve SVN conflicts, obstructions or external paths before committing.");
	}
	// Refusal happens before scheduling: the external file stays unversioned.
	UAM_ASSERT(svn.ChangedPaths().contains("vendor/lib.txt"));
}

UAM_TEST(SvnSnapshotSkipsSvnIgnoredItemsAndKeepsThemUntracked)
{
	RealSvnCheckout svn;
	if (!svn.ready || !GitAvailableForTests()) return;
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "app.txt", "tracked\n"));
	UAM_ASSERT(svn.Svn({"add", "--", "app.txt"}));
	UAM_ASSERT(svn.Svn({"propset", "svn:ignore", "Library\ntrail*", "."}));
	UAM_ASSERT(svn.Svn({"commit", "-m", "ignore"}));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "Library" / "cache.bin", "large generated cache\n"));
	// Git drops unescaped trailing spaces from exclude patterns, so this name needs escaping.
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "trail " / "cache.bin", "ignored\n"));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "draft.txt", "unversioned\n"));
	svn.app.settings.git_enabled = true;
	svn.app.data_root = svn.temporary.root / "data";
	svn.app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatFolder folder;
	folder.id = "folder";
	folder.directory = svn.workspace.string();
	svn.app.folders.push_back(folder);
	ChatSession chat = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
	chat.workspace_directory = svn.workspace.string();
	svn.app.chats.push_back(chat);
	const uam::GitWorktreeOperationResult created = uam::GitWorktreeService().CreateForChat(svn.app, svn.app.chats.front());
	if (!created.ok) std::fprintf(stderr, "snapshot: %s\n", created.message.c_str());
	UAM_ASSERT(created.ok && created.status.managed_repository && created.status.is_svn_workspace);
	const fs::path worktree = svn.app.chats.front().workspace_worktree_directory;
	UAM_ASSERT_EQ(ReadFile(worktree / "app.txt"), std::string("tracked\n"));
	UAM_ASSERT_EQ(ReadFile(worktree / "draft.txt"), std::string("unversioned\n"));
	UAM_ASSERT(!fs::exists(worktree / "Library"));
	UAM_ASSERT(!fs::exists(worktree / "trail "));
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "Library" / "rebuilt.bin", "agent output\n"));
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "trail " / "rebuilt.bin", "agent output\n"));
	const ProcessExecutionResult status = PlatformServicesFactory::Instance().process_service.ExecuteCommand("git -C " + ShellQuoteForTest(worktree.string()) + " status --porcelain", 120000);
	UAM_ASSERT(status.ok && status.exit_code == 0);
	UAM_ASSERT(uam::strings::IsBlank(status.output));
	UAM_ASSERT_EQ(ReadFile(svn.workspace / "Library" / "cache.bin"), std::string("large generated cache\n"));
}
#endif

#ifndef _WIN32
UAM_TEST(SvnRealNewFolderRetryRequiresScheduledContentsSelection)
{
	RealSvnCheckout svn;
	if (!svn.ready) return;
	// A pre-commit hook rejects the first publication, leaving the folder scheduled for addition.
	const fs::path refuse = svn.temporary.root / "refuse";
	UAM_ASSERT(uam::io::WriteTextFile(refuse, "refuse\n"));
	const fs::path hook = svn.repository / "hooks" / "pre-commit";
	UAM_ASSERT(uam::io::WriteTextFile(hook, "#!/bin/sh\ntest ! -f " + ShellQuoteForTest(refuse.string()) + "\n"));
	fs::permissions(hook, fs::perms::owner_all);
	fs::create_directories(svn.workspace / "newdir" / "sub");
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "newdir" / "a.txt", "new\n"));
	UAM_ASSERT(uam::io::WriteTextFile(svn.workspace / "newdir" / "sub" / "b.txt", "new\n"));
	UAM_ASSERT(!uam::VcsCommitService().Commit(svn.app, svn.chat, uam::VcsType::Svn, "first", {"newdir"}).ok);
	UAM_ASSERT(svn.ChangedPaths().contains("newdir/sub/b.txt"));
	fs::remove(refuse);
	// The scheduled contents are now their own rows: selecting only the folder is refused rather than publishing an
	// empty folder or rows the user left unticked.
	const uam::VcsCommitResult partial = uam::VcsCommitService().Commit(svn.app, svn.chat, uam::VcsType::Svn, "retry", {"newdir"});
	UAM_ASSERT(!partial.ok);
	UAM_ASSERT_EQ(partial.error, "Select the scheduled items inside this new SVN folder too; committing the folder alone would leave them unpublished.");
	const std::set<std::string> rows = svn.ChangedPaths();
	UAM_ASSERT_EQ(rows, (std::set<std::string>{"newdir", "newdir/a.txt", "newdir/sub", "newdir/sub/b.txt"}));
	const uam::VcsCommitResult retry = uam::VcsCommitService().Commit(svn.app, svn.chat, uam::VcsType::Svn, "retry", std::vector<std::string>(rows.begin(), rows.end()));
	if (!retry.ok) std::fprintf(stderr, "retry: %s\n", retry.error.c_str());
	UAM_ASSERT(retry.ok);
	UAM_ASSERT(svn.ChangedPaths().empty());
	const ProcessExecutionResult listed = uam::RunSvn(svn.workspace, {"list", "--non-interactive", "-R", "--", svn.url});
	UAM_ASSERT(listed.output.find("newdir/sub/b.txt") != std::string::npos);
	UAM_ASSERT(listed.output.find("newdir/a.txt") != std::string::npos);
}

UAM_TEST(SvnSnapshotStopsWhenIgnoreRulesCannotBeRead)
{
	if (!GitAvailableForTests()) return;
	// Real svn reports a non-checkout as a warning with exit 0; an error exit must fail the same way.
	for (const std::string& mode : {"error", "warning"})
	{
	TempDir temp("uam-svn-ignore-failure");
	const fs::path source = temp.root / "source";
	fs::create_directories(source / ".svn");
	UAM_ASSERT(uam::io::WriteTextFile(source / "app.txt", "tracked\n"));
	const fs::path shim = temp.root / "bin" / "svn";
	UAM_ASSERT(uam::io::WriteTextFile(shim, mode == "error" ? "#!/bin/sh\nprintf '%s\\n' \"svn: E155007: '$PWD' is not a working copy\" >&2\nexit 1\n" : "#!/bin/sh\nprintf '%s\\n' \"svn: warning: W155007: '$PWD' is not a working copy\" >&2\nexit 0\n"));
	fs::permissions(shim, fs::perms::owner_all);
	ScopedEnvVar path("PATH", shim.parent_path().string() + ":" + uam::env::GetNonEmptyString("PATH").value_or(""));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatFolder folder;
	folder.id = "folder";
	folder.directory = source.string();
	app.folders.push_back(folder);
	ChatSession chat = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
	chat.workspace_directory = source.string();
	app.chats.push_back(chat);
	const uam::GitWorktreeOperationResult created = uam::GitWorktreeService().CreateForChat(app, app.chats.front());
	UAM_ASSERT(!created.ok);
	UAM_ASSERT(created.message.find("Could not read the SVN ignore rules") != std::string::npos);
	UAM_ASSERT(created.message.find(mode == "error" ? "E155007" : "W155007") != std::string::npos);
	UAM_ASSERT(app.chats.front().workspace_worktree_directory.empty());
	UAM_ASSERT_EQ(ReadFile(source / "app.txt"), std::string("tracked\n"));
	}
}
#endif
