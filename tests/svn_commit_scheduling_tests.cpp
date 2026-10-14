#include "test_harness.h"

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
				"status) case \"$*\" in *--depth*) for arg do target=\"$arg\"; done; target=${target%@}; if test -f " + ShellQuoteForTest((temporary.root / "retained").string()) + "; then case \"$target\" in *nested|*name@.txt) printf 'A       %s\\n' \"$target\"; exit 0;; esac; fi; case \"$target\" in *missing.txt) printf '!       %s\\n' \"$target\";; *new.txt|*name@.txt|*nested) printf '?       %s\\n' \"$target\";; *) printf 'M       %s\\n' \"$target\";; esac;; *) printf '?       new.txt\\n!       missing.txt\\n';; esac;;\n"
				"add|delete) test ! -f " + ShellQuoteForTest((temporary.root / "fail").string()) + ";;\n"
				"commit) test ! -f " + ShellQuoteForTest((temporary.root / "fail-commit").string()) + " || exit 1; test ! -f " + ShellQuoteForTest((temporary.root / "retained").string()) + " || exit 0; grep -q '^add ' " + ShellQuoteForTest(commands.string()) + " && grep -q '^delete ' " + ShellQuoteForTest(commands.string()) + ";;\n"
				"diff) :;;\n"
				"*) exit 1;;\nesac\n";
			UAM_ASSERT(uam::io::WriteTextFile(shim, script));
			fs::permissions(shim, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
			path = std::make_unique<ScopedEnvVar>("PATH", temporary.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or(""));
			UAM_ASSERT(uam::io::WriteTextFile(workspace / "new.txt", "selected\n"));
		}
	};
}

UAM_TEST(SvnExplicitCommitSchedulesSelectedAddedAndMissingPaths)
{
	SvnSchedulingFixture fixture;
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(fixture.app, fixture.chat, uam::VcsType::Svn, "private fixture", {"new.txt", "missing.txt"});
	UAM_ASSERT(result.ok);
	const std::string commands = uam::io::ReadTextFile(fixture.commands);
	UAM_ASSERT(commands.find("add --non-interactive --depth empty -- ") != std::string::npos);
	UAM_ASSERT(commands.find("delete --non-interactive -- ") != std::string::npos);
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
	std::istringstream argv_lines(uam::io::ReadTextFile(fixture.temporary.root / "argv"));
	std::string argv_line;
	bool parent_added = false;
	bool leaf_added = false;
	const std::string parent_target = fs::weakly_canonical(fixture.workspace / "nested").string() + "@";
	const std::string leaf_target = fs::weakly_canonical(fixture.workspace / "nested" / "name@.txt").string() + "@";
	while (std::getline(argv_lines, argv_line))
	{
		const std::vector<std::string> argv = uam::command_line::SplitWords(argv_line);
		if (argv.empty() || argv.front() != "add") continue;
		UAM_ASSERT_EQ(argv.size(), 6U);
		UAM_ASSERT_EQ(argv[3], "empty");
		UAM_ASSERT_EQ(argv[4], "--");
		parent_added = parent_added || argv.back() == parent_target;
		leaf_added = leaf_added || argv.back() == leaf_target;
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
	UAM_ASSERT(publication.find(fs::weakly_canonical(fixture.workspace / "nested").string() + "@") != std::string::npos);
	UAM_ASSERT(publication.find("name@.txt@") != std::string::npos);
	UAM_ASSERT(commands.find("add ") == std::string::npos);
	UAM_ASSERT(commands.find("unselected.txt") == std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.workspace / "nested" / "unselected.txt"), "retain\n");
}
#endif
