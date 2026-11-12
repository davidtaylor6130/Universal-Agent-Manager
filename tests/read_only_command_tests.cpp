#include "test_harness.h"
#include "common/security/read_only_command.h"

#include <algorithm>
#include <fstream>

using namespace uam_test;

namespace
{
	// Workspace-relative operands stay inside this scope; the directory need not exist.
	const uam::command_safety::ReadScope& Scope()
	{
		static const uam::command_safety::ReadScope scope{.workspace = fs::temp_directory_path() / "uam-read-scope-fixture", .resolve_executables = false};
		return scope;
	}

	bool ReadOnly(std::string_view command)
	{
		return uam::command_safety::IsReadOnlyCommand(command, Scope());
	}
}

UAM_TEST(ReadOnlyCommandsAcceptSupportedLiteralReadersAndOptions)
{
	for (const std::string_view command : {
		"pwd", "pwd -P", "pwd -LP", "cat -- README.md", "cat -ns \"file with spaces.txt\"",
		"ls -lah -- src", "grep -rIn -e TODO -- src", "grep --fixed-strings --regexp=--compile -- src",
		"grep -e \"[a-z]+\" -- source.txt", "head -n20 README.md", "tail --lines=20 README.md",
		"wc -lw README.md", "stat README.md", "file -bi README.md", "file -c -m ./private.magic",
		"which -a git", "where git", "type README.md", "Get-Content -LiteralPath \"project/file with spaces.txt\" -TotalCount 10",
		"get-content -RAW README.md", "git status --short --branch --ignore-submodules", "git status --porcelain=2 -uno --ignore-submodules=all",
		"git --no-pager diff --no-ext-diff --no-textconv --stat", "git diff --no-ext-diff --no-textconv --name-only HEAD -- src", "git diff --no-ext-diff --no-textconv -U3 -- README.md",
		"git log --no-ext-diff --no-textconv --no-show-signature --oneline -n5", "git log --no-ext-diff --no-textconv --no-show-signature --format=oneline", "git show --no-ext-diff --no-textconv --no-show-signature --stat HEAD"})
	{
		UAM_ASSERT(ReadOnly(command));
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
		UAM_ASSERT(!ReadOnly(command));
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
		UAM_ASSERT(!ReadOnly(command));
	}
	UAM_ASSERT(!ReadOnly(std::string("cat file\0suffix", 15)));
	UAM_ASSERT(!ReadOnly(std::string(32769, 'x')));
}

UAM_TEST(ReadOnlyCommandsRespectLiteralPathsAndOptionValueBoundaries)
{
	for (const std::string_view command : {
		"file -- --compile", "git diff --no-ext-diff --no-textconv -- --output=private.txt", "git log --no-ext-diff --no-textconv --no-show-signature -- --output=private.txt",
		"cat -- -n", "grep --regexp=--output -- source.txt",
		"cat 'file.txt'", "cat \"file with spaces.txt\"", "grep -e \"[ab]*\" -- source.txt"})
	{
		UAM_ASSERT(ReadOnly(command));
	}
	UAM_ASSERT(!ReadOnly("file -C -- ./private.magic"));
	UAM_ASSERT(!ReadOnly("grep -RInQ TODO source.txt"));
	UAM_ASSERT(!ReadOnly("git log --no-ext-diff --no-textconv --max-count="));
	UAM_ASSERT(!ReadOnly("get-content -LiteralPath"));
	UAM_ASSERT(ReadOnly("cat \"caf\xC3\xA9.txt\""));
	UAM_ASSERT(!ReadOnly("file -m magic\xC2\xA0-C"));
}

UAM_TEST(ReadOnlyGitCommandsRequireExplicitNativeHelperSuppression)
{
	for (const std::string action : {"diff", "log", "show"})
	{
		const std::string command = "git " + action + (action == "diff" ? "" : " --no-show-signature");
		UAM_ASSERT(!ReadOnly(command));
		UAM_ASSERT(!ReadOnly(command + " --no-ext-diff"));
		UAM_ASSERT(!ReadOnly(command + " --no-textconv"));
		UAM_ASSERT(ReadOnly(command + " --no-ext-diff --no-textconv"));
		UAM_ASSERT(ReadOnly(command + " --no-textconv --no-ext-diff"));
		UAM_ASSERT(!ReadOnly(command + " -- --no-ext-diff --no-textconv"));
		UAM_ASSERT(!ReadOnly(command + " --no-ext-diff -- --no-textconv"));
		UAM_ASSERT(!ReadOnly(command + " --no-textconv -- --no-ext-diff"));
	}
	UAM_ASSERT(!ReadOnly("git log --no-ext-diff --format=--no-textconv"));
	UAM_ASSERT(!ReadOnly("git log --no-textconv --format=--no-ext-diff"));
	UAM_ASSERT(!ReadOnly("git show --no-textconv --pretty=--no-ext-diff"));
	UAM_ASSERT(!ReadOnly("git diff --no-textconv -O--no-ext-diff"));
	UAM_ASSERT(!ReadOnly("git diff --no-ext-diff --no-textconv --ext-diff"));
	UAM_ASSERT(!ReadOnly("git log --no-ext-diff --no-textconv --textconv"));
	UAM_ASSERT(ReadOnly("git status --short --ignore-submodules"));
}

UAM_TEST(ReadOnlyGitCommandsRequireSubmoduleAndSignatureSuppression)
{
	// Submodule status reads each submodule's configuration; signature display runs gpg.program.
	UAM_ASSERT(!ReadOnly("git status"));
	UAM_ASSERT(!ReadOnly("git status --short"));
	UAM_ASSERT(!ReadOnly("git status --ignore-submodules=none"));
	UAM_ASSERT(!ReadOnly("git status --ignore-submodules=dirty"));
	UAM_ASSERT(!ReadOnly("git status -- --ignore-submodules"));
	UAM_ASSERT(ReadOnly("git status --ignore-submodules"));
	UAM_ASSERT(ReadOnly("git --no-pager status --ignore-submodules=all --short"));
	for (const std::string action : {"log", "show"})
	{
		const std::string helpers = "git " + action + " --no-ext-diff --no-textconv";
		UAM_ASSERT(!ReadOnly(helpers));
		UAM_ASSERT(!ReadOnly(helpers + " --show-signature"));
		UAM_ASSERT(!ReadOnly(helpers + " --format=--no-show-signature"));
		UAM_ASSERT(!ReadOnly(helpers + " -- --no-show-signature"));
		UAM_ASSERT(ReadOnly(helpers + " --no-show-signature"));
	}
}

UAM_TEST(ReadOnlyVcsCommandsRejectNetworkOperands)
{
	for (const std::string_view command : {
		"svn info https://example.invalid/repo", "svn log svn+ssh://example.invalid/repo", "svn cat file:///etc/passwd",
		"svn list http://example.invalid/repo", "svn blame https://example.invalid/repo/file", "svn propget svn:ignore https://example.invalid/repo",
		"svn diff --internal-diff https://example.invalid/a https://example.invalid/b", "svn diff --internal-diff --old=https://example.invalid/a",
		"git log --no-ext-diff --no-textconv --no-show-signature -- https://example.invalid/x", "cat https://example.invalid/x"})
	{
		UAM_ASSERT(!ReadOnly(command));
	}
	UAM_ASSERT(ReadOnly("svn info"));
	UAM_ASSERT(ReadOnly("svn log -l 5 src"));
}

UAM_TEST(ReadOnlyCommandsStayInsideTheWorkspaceAndOutOfTheDataRoot)
{
	TempDir temp("uam-read-scope");
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace / "src");
	fs::create_directories(workspace / "Data");
	fs::create_directories(temp.root / "outside");
	std::ofstream(temp.root / "outside" / "secret.txt") << "secret";
	std::ofstream(workspace / "README.md") << "readme";
	uam::command_safety::ReadScope scope{.workspace = workspace, .data_root = workspace / "Data", .resolve_executables = false};
	const auto read = [&](std::string_view command) { return uam::command_safety::IsReadOnlyCommand(command, scope); };
	for (const std::string_view command : {
		"cat README.md", "cat ./src/../README.md", "ls", "ls -la src", "grep -rn TODO src", "grep -rn \"/api/v1\" src",
		"head -n5 README.md", "git diff --no-ext-diff --no-textconv --no-index README.md src/new.txt", "Get-Content -Path README.md", "stat -L README.md"})
	{
		UAM_ASSERT(read(command));
	}
	const std::string absolute_outside = "cat \"" + uam::paths::PortablePathString(temp.root / "outside" / "secret.txt") + "\"";
	const std::string absolute_inside = "cat \"" + uam::paths::PortablePathString(workspace / "README.md") + "\"";
	UAM_ASSERT(!read(absolute_outside));
	UAM_ASSERT(read(absolute_inside));
	for (const std::string_view command : {
		"cat /etc/passwd", "cat ../outside/secret.txt", "cat src/../../outside/secret.txt", "cat ~/.ssh/id_rsa", "cat ~", "head -n5 ~/x",
		"grep -r TODO /", "grep -e x ../outside", "grep -f ../outside/patterns src", "grep --file=~/patterns src", "grep -f~/patterns src",
		"file -f ../outside/list", "file -m ../outside/magic README.md", "git diff --no-ext-diff --no-textconv --no-index README.md ../outside/secret.txt",
		"git diff --no-ext-diff --no-textconv -O../outside/order", "git log --no-ext-diff --no-textconv --no-show-signature -- ../outside",
		"Get-Content -Path ../outside/secret.txt", "Get-Content -LiteralPath ~/secret.txt", "svn cat ../outside/secret.txt",
		"cat Data/companion.json", "ls Data", "grep -r token Data", "ls -L", "ls -RL src", "grep -R TODO src"})
	{
		UAM_ASSERT(!read(command));
	}
#if defined(_WIN32) || defined(__APPLE__)
	UAM_ASSERT(!read("cat data/companion.json")); // Case-insensitive volumes cannot dodge the data-root boundary.
#endif
#if !defined(_WIN32)
	fs::create_directory_symlink(temp.root / "outside", workspace / "link");
	UAM_ASSERT(!read("cat link/secret.txt"));
	UAM_ASSERT(!read("ls link"));
#endif
	// A data root that holds the workspace (a managed worktree) leaves the workspace itself readable.
	const uam::command_safety::ReadScope managed{.workspace = workspace, .data_root = temp.root, .resolve_executables = false};
	UAM_ASSERT(uam::command_safety::IsReadOnlyCommand("cat README.md", managed));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("cat ../outside/secret.txt", managed));
	// A provider-reported working directory must itself be inside, and anchors relative operands.
	uam::command_safety::ReadScope nested = scope;
	nested.cwd = "src";
	UAM_ASSERT(uam::command_safety::IsReadOnlyCommand("cat ../README.md", nested));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("cat ../../outside/secret.txt", nested));
	nested.cwd = temp.root / "outside";
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("ls", nested));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("pwd", nested));
	// No workspace admits nothing; a remote workspace is checked lexically.
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("pwd", uam::command_safety::ReadScope{}));
	const uam::command_safety::ReadScope remote{.workspace = "/srv/project", .local = false};
	UAM_ASSERT(uam::command_safety::IsReadOnlyCommand("cat src/../README.md", remote));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("cat ../other/secret.txt", remote));
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("cat /etc/passwd", remote));
}

UAM_TEST(ReadOnlyCommandsRequireTrustedExecutablesOutsideTheWorkspace)
{
	TempDir temp("uam-read-executables");
	const fs::path workspace = temp.root / "workspace";
	const fs::path trusted = temp.root / "trusted-bin";
	fs::create_directories(workspace / "bin");
	fs::create_directories(trusted);
	const auto make = [](const fs::path& file, bool executable)
	{
		std::ofstream(file) << "#!/bin/sh\n";
		if (executable) fs::permissions(file, fs::perms::owner_all, fs::perm_options::add);
	};
#if !defined(_WIN32) // POSIX search: ':' separators and execute bits.
	make(trusted / "cat", true);
	make(trusted / "git", true);
	uam::command_safety::ReadScope scope{.workspace = workspace, .search_path = uam::paths::PortablePathString(trusted), .windows_search = false};
	const auto read = [&](std::string_view command) { return uam::command_safety::IsReadOnlyCommand(command, scope); };
	UAM_ASSERT(read("cat README.md"));
	UAM_ASSERT(read("git status --ignore-submodules"));
	UAM_ASSERT(!read("head README.md")); // Resolves nowhere.
	UAM_ASSERT(read("pwd"));             // Builtins never search PATH.
	// A search entry inside the workspace, or relative to the working directory, that wins resolution is refused.
	make(workspace / "bin" / "cat", true);
	scope.search_path = uam::paths::PortablePathString(workspace / "bin") + ":" + uam::paths::PortablePathString(trusted);
	UAM_ASSERT(!read("cat README.md"));
	UAM_ASSERT(read("git status --ignore-submodules"));
	scope.search_path = "bin:" + uam::paths::PortablePathString(trusted);
	UAM_ASSERT(!read("cat README.md"));
	scope.search_path = ":" + uam::paths::PortablePathString(trusted); // An empty entry is the working directory.
	make(workspace / "cat", true);
	UAM_ASSERT(!read("cat README.md"));
	// A non-executable file is skipped, as the shell would.
	fs::remove(workspace / "cat");
	make(workspace / "bin" / "head", false);
	make(trusted / "head", true);
	scope.search_path = uam::paths::PortablePathString(workspace / "bin") + ":" + uam::paths::PortablePathString(trusted);
	UAM_ASSERT(read("head README.md"));
#endif

	// cmd.exe searches the working directory first and appends PATHEXT, so a workspace `ls.bat` beats the real `ls`.
	uam::command_safety::ReadScope windows{.workspace = workspace, .search_path = uam::paths::PortablePathString(trusted), .path_extensions = ".COM;.EXE;.BAT", .windows_search = true};
	make(trusted / "ls.EXE", true);
	UAM_ASSERT(uam::command_safety::IsReadOnlyCommand("ls src", windows));
	make(workspace / "ls.bat", false);
	UAM_ASSERT(!uam::command_safety::IsReadOnlyCommand("ls src", windows));
	UAM_ASSERT(uam::command_safety::IsReadOnlyCommand("Get-Content README.md", windows)); // A PowerShell cmdlet, not a PATH lookup.
	// Remote hosts are not resolved from the desktop.
	const uam::command_safety::ReadScope remote{.workspace = "/srv/project", .local = false, .search_path = ""};
	UAM_ASSERT(uam::command_safety::IsReadOnlyCommand("cat README.md", remote));
}

UAM_TEST(ReadCommandEnvironmentDisablesPagersAndAppendsFilesystemMonitorConfig)
{
	const auto value = [](const auto& environment, std::string_view key)
	{
		const auto found = std::ranges::find_if(environment, [&](const auto& entry) { return entry.first == key; });
		return found == environment.end() ? std::string("<unset>") : found->second;
	};
	const auto remote = uam::command_safety::ReadCommandEnvironment(false);
	UAM_ASSERT_EQ(value(remote, "GIT_PAGER"), std::string("cat"));
	UAM_ASSERT_EQ(value(remote, "PAGER"), std::string("cat"));
	UAM_ASSERT_EQ(value(remote, "GIT_CONFIG_COUNT"), std::string("<unset>"));
	const std::optional<std::string> previous = uam::env::GetNonEmptyString("GIT_CONFIG_COUNT");
	UAM_ASSERT(uam::env::SetString("GIT_CONFIG_COUNT", "2"));
	const auto local = uam::command_safety::ReadCommandEnvironment(true);
	(void)uam::env::SetString("GIT_CONFIG_COUNT", previous.value_or(""));
	UAM_ASSERT_EQ(value(local, "GIT_PAGER"), std::string("cat"));
	UAM_ASSERT_EQ(value(local, "GIT_CONFIG_KEY_2"), std::string("core.fsmonitor"));
	UAM_ASSERT_EQ(value(local, "GIT_CONFIG_VALUE_2"), std::string("false"));
	UAM_ASSERT_EQ(value(local, "GIT_CONFIG_COUNT"), std::string("3"));
}
