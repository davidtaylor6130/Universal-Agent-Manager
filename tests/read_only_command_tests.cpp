#include "test_harness.h"
#include "common/security/read_only_command.h"

using namespace uam_test;

UAM_TEST(ReadOnlyCommandsAcceptSupportedLiteralReadersAndOptions)
{
	for (const std::string_view command : {
		"pwd", "pwd -P", "pwd -LP", "cat -- README.md", "cat -ns \"file with spaces.txt\"",
		"ls -lah -- src", "grep -RIn -e TODO -- src", "grep --fixed-strings --regexp=--compile -- src",
		"grep -e \"[a-z]+\" -- source.txt", "head -n20 README.md", "tail --lines=20 README.md",
		"wc -lw README.md", "stat README.md", "file -bi README.md", "file -c -m ./private.magic",
		"which -a git", "where git", "type README.md", "Get-Content -LiteralPath \"C:/project/file with spaces.txt\" -TotalCount 10",
		"get-content -RAW README.md", "git status --short --branch", "git status --porcelain=2 -uno",
		"git --no-pager diff --no-ext-diff --no-textconv --stat", "git diff --no-ext-diff --no-textconv --name-only HEAD -- src", "git diff --no-ext-diff --no-textconv -U3 -- README.md",
		"git log --no-ext-diff --no-textconv --oneline -n5", "git log --no-ext-diff --no-textconv --format=oneline", "git show --no-ext-diff --no-textconv --stat HEAD"})
	{
		UAM_ASSERT(uam::command_safety::IsReadOnlyCommand(command));
	}
}

UAM_TEST(ReadOnlyCommandsRejectMutatingOptionsAndTheirAbbreviations)
{
	for (const std::string_view command : {
		"git diff --no-ext-diff --no-textconv --output=private.txt", "git log --no-ext-diff --no-textconv --output private.txt", "git show --no-ext-diff --no-textconv --output=private.txt",
		"git diff --no-ext-diff --no-textconv --out=private.txt", "git log --no-ext-diff --no-textconv --out private.txt", "git diff --no-ext-diff --no-textconv -o private.txt",
		"file -C -m ./private.magic", "file --compile --magic-file=./private.magic", "file --comp -m ./private.magic",
		"file -bC -m ./private.magic", "file -cbC -m ./private.magic", "git diff --no-ext-diff --no-textconv --ext-diff", "git log --no-ext-diff --no-textconv --textconv",
		"git -c diff.external=writer diff", "git --paginate log", "git status --unknown", "cat --unknown file.txt",
		"Get-Content (New-Item private.txt -ItemType File)", "Get-Content -Unknown file.txt",
		"git diff --no-ext-diff --no-textconv --relative --output=private.txt", "git log --no-ext-diff --no-textconv --pretty --output=private.txt", "git log --no-ext-diff --no-textconv --format --output=private.txt",
		"file -m 'magic -C other'", "git log --no-ext-diff --no-textconv --format 'short --output=private.txt'"})
	{
		UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(command));
	}
}

UAM_TEST(ReadOnlyCommandsRejectShellEvaluationAndIncompleteLiteralGrammar)
{
	for (const std::string_view command : {
		"", "  ", "cat 'unterminated", "cat \"unterminated", "cat file\\ name", "cat \"C:\\project\\file\"",
		"c'at' file.txt", "cat \"file\"suffix", "cat file.txt; pwd", "cat file.txt && pwd", "cat file.txt | head",
		"cat file.txt > output.txt", "cat < input.txt", "cat $(writer)", "cat `writer`", "cat $INPUT",
		"cat \"$INPUT\"", "cat %INPUT%", "cat !INPUT!", "cat ^INPUT", "Get-Content @(writer)",
		"Get-Content { writer }", "Get-Content [IO.File]::WriteAllText", "ls *", "cat foo?", "cat [ab]",
		"pwd\nwriter", "pwd\rwriter", "cat file.txt # ignored", "cat 'file with spaces.txt'", "grep -e --output -- source.txt", "env cat file.txt", "sh -c pwd",
		"/bin/cat file.txt", "unknown-command file.txt", "git", "git --no-pager", "pwd unexpected", "head -n", "file -m"})
	{
		UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(command));
	}
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(std::string("cat file\0suffix", 15)));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(std::string(32769, 'x')));
}

UAM_TEST(ReadOnlyCommandsRespectLiteralPathsAndOptionValueBoundaries)
{
	for (const std::string_view command : {
		"file -- --compile", "git diff --no-ext-diff --no-textconv -- --output=private.txt", "git log --no-ext-diff --no-textconv -- --output=private.txt",
		"cat -- -n", "grep --regexp=--output -- source.txt",
		"cat 'file.txt'", "cat \"file with spaces.txt\"", "grep -e \"[ab]*\" -- source.txt"})
	{
		UAM_ASSERT(uam::command_safety::IsReadOnlyCommand(command));
	}
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("file -C -- ./private.magic"));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("grep -RInQ TODO source.txt"));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("git log --no-ext-diff --no-textconv --max-count="));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("get-content -LiteralPath"));
	UAM_ASSERT(uam::command_safety::IsReadOnlyCommand("cat \"caf\xC3\xA9.txt\""));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("file -m magic\xC2\xA0-C"));
}

UAM_TEST(ReadOnlyGitCommandsRequireExplicitNativeHelperSuppression)
{
	for (const std::string action : {"diff", "log", "show"})
	{
		const std::string command = "git " + action;
		UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(command));
		UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(command + " --no-ext-diff"));
		UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(command + " --no-textconv"));
		UAM_ASSERT(uam::command_safety::IsReadOnlyCommand(command + " --no-ext-diff --no-textconv"));
		UAM_ASSERT(uam::command_safety::IsReadOnlyCommand(command + " --no-textconv --no-ext-diff"));
		UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(command + " -- --no-ext-diff --no-textconv"));
		UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(command + " --no-ext-diff -- --no-textconv"));
		UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand(command + " --no-textconv -- --no-ext-diff"));
	}
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("git log --no-ext-diff --format=--no-textconv"));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("git log --no-textconv --format=--no-ext-diff"));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("git show --no-textconv --pretty=--no-ext-diff"));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("git diff --no-textconv -O--no-ext-diff"));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("git diff --no-ext-diff --no-textconv --ext-diff"));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("git log --no-ext-diff --no-textconv --textconv"));
	UAM_ASSERT(uam::command_safety::IsReadOnlyCommand("git status --short"));
}
