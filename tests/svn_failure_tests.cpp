#include "test_harness.h"
#include "app/svn_failure.h"
#include "common/vcs/svn_environment.h"

#include <algorithm>

using namespace uam_test;

namespace
{
	ProcessExecutionResult Failed(std::string output, std::string error = {})
	{
		ProcessExecutionResult result;
		result.ok = true;
		result.exit_code = 1;
		result.output = std::move(output);
		result.error = std::move(error);
		return result;
	}

	bool HasVariable(const std::vector<std::pair<std::string, std::string>>& environment, const std::string& name, const std::string& value)
	{
		return std::ranges::find(environment, std::pair<std::string, std::string>(name, value)) != environment.end();
	}

	bool HasName(const std::vector<std::pair<std::string, std::string>>& environment, const std::string& name)
	{
		return std::ranges::any_of(environment, [&](const auto& variable) { return variable.first == name; });
	}
} // namespace

UAM_TEST(SvnFailureSummaryChoosesTheInnermostCause)
{
	// svn prints its chain outermost first; the connection wrapper is not the reason.
	UAM_ASSERT_EQ(uam::SvnFailureSummary(Failed("svn: E170013: Unable to connect to a repository at URL 'svn://127.0.0.1:3690/repo'\nsvn: E000111: Can't connect to host '127.0.0.1': Connection refused\n")),
	    std::string("E000111: Can't connect to host '127.0.0.1': Connection refused"));
	// Within the innermost code, svn's own lead line comes first.
	UAM_ASSERT_EQ(uam::SvnFailureSummary(Failed("svn: E155004: Run 'svn cleanup' to remove locks (type 'svn help cleanup' for details)\nsvn: E155004: Working copy '/w' locked.\nsvn: E155004: '/w' is already locked.\n")),
	    std::string("E155004: Run 'svn cleanup' to remove locks (type 'svn help cleanup' for details)"));
	// "Some targets don't exist" only summarises the warnings above it.
	UAM_ASSERT_EQ(uam::SvnFailureSummary(Failed("svn: warning: W155010: The node '/w/gone' was not found.\n\nsvn: E200009: Could not display info for all targets because some targets don't exist\n")),
	    std::string("W155010: The node '/w/gone' was not found"));
	// "details follow" wrappers and the errors after "Additional errors:" are not the cause.
	UAM_ASSERT_EQ(uam::SvnFailureSummary(Failed("svn: E175002: Commit failed (details follow):\nsvn: E175002: Unexpected HTTP status 500 on '/repo'\nsvn: E200042: Additional errors:\nsvn: E175002: Unexpected HTTP status 409 on '/cleanup'\n")),
	    std::string("E175002: Unexpected HTTP status 500 on '/repo'"));
	// A line that continues on the next one stops at its own sentence; stderr in `error` counts too.
	UAM_ASSERT_EQ(uam::SvnFailureSummary(Failed({}, "svn: E000022: Can't convert string from native encoding to 'UTF-8':\nsvn: E000022: /w/wc?\\195?\\169\n")),
	    std::string("E000022: Can't convert string from native encoding to 'UTF-8'"));
	UAM_ASSERT_EQ(uam::SvnFailureSummary(Failed("Sending a.txt\n")), std::string());
	// svn's prefix inside a diff or log message is content, not svn's own error.
	UAM_ASSERT_EQ(uam::SvnFailureSummary(Failed("+ticket says: svn: E170001: Can't get password\nsvn: E155010: The node '/w/x' was not found.\n")),
	    std::string("E155010: The node '/w/x' was not found"));
	// A hook's reason follows svn's "with output:" line and is not svn's own line; it is the part the person needs.
	UAM_ASSERT_EQ(uam::SvnFailureSummary(Failed("Sending a.txt\nsvn: E165001: Commit failed (details follow):\nsvn: E165001: Commit blocked by pre-commit hook (exit code 1) with output:\n\nCommit message must reference a ticket (ABC-123)\nsecond hook line\n")),
	    std::string("E165001: Commit blocked by pre-commit hook (exit code 1) with output: Commit message must reference a ticket (ABC-123)"));
	// A translated lead line is recognised by its shape: it ends in ':' and the same code follows with a quoted path.
	UAM_ASSERT_EQ(uam::SvnFailureSummary(Failed("svn: E155004: Commit gescheitert (Details folgen):\nsvn: E155004: Arbeitskopie '/w/sub' gesperrt.\n")),
	    std::string("E155004: Arbeitskopie '/w/sub' gesperrt"));
}

UAM_TEST(SvnFailureMessageAsksForCredentialsInsteadOfConnectionFailure)
{
	const std::string missing = uam::SvnFailureMessage(Failed("svn: E170013: Unable to connect to a repository at URL 'svn://127.0.0.1:3690/repo'\nsvn: E170001: Can't get username or password\n"), "fallback");
	UAM_ASSERT_EQ(missing, std::string("SVN needs your credentials. Run svn info svn://127.0.0.1:3690/repo once in a terminal to save them, then retry."));
	UAM_ASSERT(uam::SvnFailureMessage(Failed("svn: E170013: Unable to connect to a repository at URL 'svn://h/r'\nsvn: E170001: Authentication error from server: Password incorrect\n"), {}).starts_with("SVN needs your credentials. Run svn info svn://h/r "));
	// HTTP gives up on its credential providers without naming the URL.
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed("svn: E215004: No more credentials or we tried too many times.\nAuthentication failed\n"), {}),
	    std::string("SVN needs your credentials. Run svn info -r HEAD once in a terminal in this working copy to save them, then retry."));
	// svn+ssh in batch mode: ssh's refusal sits beside svn's connection and tunnel errors.
	UAM_ASSERT(uam::SvnFailureMessage(Failed("Permission denied (publickey).\nsvn: E170013: Unable to connect to a repository at URL 'svn+ssh://h/r'\nsvn: E210002: To better debug SSH connection problems, remove the -q option from 'ssh' in the [tunnels] section of your Subversion configuration file.\nsvn: E210002: Network connection closed unexpectedly\n"), {}).starts_with("SVN needs your credentials. Run svn info svn+ssh://h/r "));
	UAM_ASSERT(uam::SvnFailureMessage(Failed("Host key verification failed.\nsvn: E170013: Unable to connect to a repository at URL 'svn+ssh://h/r'\nsvn: E210002: Network connection closed unexpectedly\n"), {}).starts_with("SVN needs your credentials."));
	// Signed in, but this account may not read the path: saving credentials cannot help, so svn's own line is shown.
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed("svn: E170013: Unable to connect to a repository at URL 'svn://h/r/secret'\nsvn: E170001: Authorization failed\n"), {}),
	    std::string("E170001: Authorization failed"));
	// A refused connection is not a credentials problem.
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed("svn: E170013: Unable to connect to a repository at URL 'svn://h/r'\nsvn: E000111: Can't connect to host 'h': Connection refused\n"), {}),
	    std::string("E000111: Can't connect to host 'h': Connection refused"));
}

UAM_TEST(SvnFailureMessageNamesTheOutOfDateFileOnOneLine)
{
	const ProcessExecutionResult commit = Failed("Sending        f.txt\nsvn: E155011: Commit failed (details follow):\nsvn: E155011: File '/work/copy/sub/f.txt' is out of date\nsvn: E160028: File '/trunk/sub/f.txt' is out of date\n");
	UAM_ASSERT_EQ(uam::SvnFailureMessage(commit, "fallback", "/work/copy"), std::string("sub/f.txt is out of date. Update, then commit again."));
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed("svn: E155011: Commit failed (details follow):\nsvn: E155011: Directory 'C:\\work\\copy\\docs' is out of date\n"), {}, "C:\\work\\copy\\"), std::string("docs is out of date. Update, then commit again."));
	// The codes alone mean "out of date", so a translated client gets the same wording.
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed("svn: E155011: Commit gescheitert (Details folgen):\nsvn: E155011: Datei '/work/copy/f.txt' ist veraltet\n"), {}, "/work/copy"), std::string("f.txt is out of date. Update, then commit again."));
	// Over HTTP only the repository path is named.
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed("svn: E175002: Commit failed (details follow):\nsvn: E170004: Item '/trunk/f.txt' is out of date\n"), {}, "/work/copy"), std::string("/trunk/f.txt is out of date. Update, then commit again."));
	// Other failures are svn's chosen line with workspace paths made relative, never the raw multi-line output.
	const std::string locked = uam::SvnFailureMessage(Failed("svn: E155004: Commit failed (details follow):\nsvn: E155004: Working copy '/work/copy/sub' locked.\n"), {}, "/work/copy/");
	UAM_ASSERT_EQ(locked, std::string("E155004: Working copy 'sub' locked"));
	UAM_ASSERT(locked.find('\n') == std::string::npos);
}

UAM_TEST(SvnFailureMessageCoversStopsTimeoutsAndUamErrors)
{
	ProcessExecutionResult canceled = Failed({});
	canceled.canceled = true;
	UAM_ASSERT_EQ(uam::SvnFailureMessage(canceled, "fallback"), std::string("Canceled."));
	ProcessExecutionResult timed_out = Failed("svn: E170013: Unable to connect to a repository at URL 'svn://h/r'\n");
	timed_out.timed_out = true;
	UAM_ASSERT_EQ(uam::SvnFailureMessage(timed_out, {}), std::string("SVN did not answer in time; check the connection to the repository."));
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed({}, std::string(uam::kSvnClientMissing)), {}), std::string(uam::kSvnClientMissing));
	// Output cut at the capture limit is never read as svn's errors, even when a cut line starts like one.
	ProcessExecutionResult limited = Failed("Index: notes.txt\nsvn: E170001: Can't get password\n", "Command output exceeded the 4 MiB safety limit.");
	limited.output_truncated = true;
	UAM_ASSERT_EQ(uam::SvnFailureMessage(limited, "fallback"), std::string("Command output exceeded the 4 MiB safety limit."));
		// UAM's own reason, first line only, when svn printed nothing recognisable.
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed({}, "The remote VCS helper is unavailable.\nsecond line"), "fallback"), std::string("The remote VCS helper is unavailable."));
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed("Sending a.txt\nTransmitting file data .\n"), "SVN did not commit the selection."), std::string("SVN did not commit the selection."));
}

UAM_TEST(SvnConfigSshTunnelCountsOnlyAUserChoice)
{
	UAM_ASSERT(!uam::vcs::SvnConfigDefinesSshTunnel(""));
	// The shipped config only has commented examples.
	UAM_ASSERT(!uam::vcs::SvnConfigDefinesSshTunnel("[tunnels]\n### ssh = $SVN_SSH ssh -q\n# rsh = rsh\n"));
	UAM_ASSERT(!uam::vcs::SvnConfigDefinesSshTunnel("[tunnels]\nssh = $SVN_SSH ssh -q\n"));
	UAM_ASSERT(!uam::vcs::SvnConfigDefinesSshTunnel("[auth]\nssh = something\n[tunnels]\nrsh = rsh\n"));
	UAM_ASSERT(uam::vcs::SvnConfigDefinesSshTunnel("[miscellany]\nglobal-ignores = *.o\n\n[tunnels]\nssh = $SVN_SSH ssh -i ~/.ssh/svnkey -l bob\n"));
	UAM_ASSERT(uam::vcs::SvnConfigDefinesSshTunnel("[tunnels]\r\nSSH: plink -batch\r\n"));
	// svn itself ignores these: a spaced section name, and an indented line, which continues the previous value.
	UAM_ASSERT(!uam::vcs::SvnConfigDefinesSshTunnel("[ tunnels ]\nssh = plink -batch\n"));
	UAM_ASSERT(!uam::vcs::SvnConfigDefinesSshTunnel("[tunnels]\nrsh = rsh\n  ssh = plink -batch\n"));
}

UAM_TEST(SvnEnvironmentSetsBatchSshOnlyWhenTheUserHasNotChosenOne)
{
	TempDir home{"uam-svn-environment-home"};
#if defined(_WIN32)
	ScopedEnvVar config_home("APPDATA", uam::paths::Utf8PathString(home.root));
#else
	ScopedEnvVar config_home("HOME", uam::paths::Utf8PathString(home.root));
#endif
	{
		ScopedEnvVar unset("SVN_SSH", "");
		const auto local = uam::vcs::SvnEnvironment({});
		// A machine-wide config may already choose a tunnel; then nothing is set, which the case below covers.
		if (!uam::vcs::SvnUserChoseSshTunnel()) UAM_ASSERT(HasVariable(local, "SVN_SSH", std::string(uam::vcs::kSvnBatchSsh)));
#if !defined(_WIN32)
		UAM_ASSERT(HasName(local, "LC_ALL"));
#endif
	}
	{
		ScopedEnvVar chosen("SVN_SSH", "ssh -i ~/.ssh/svn");
		UAM_ASSERT(!HasName(uam::vcs::SvnEnvironment({}), "SVN_SSH"));
	}
	{
		// A configured `$SVN_SSH ...` tunnel would be replaced whole by a set SVN_SSH, so the user's tunnel wins.
#if defined(_WIN32)
		const fs::path config = home.root / "Subversion";
#else
		const fs::path config = home.root / ".subversion";
#endif
		fs::create_directories(config);
		UAM_ASSERT(uam::io::WriteTextFile(config / "config", "[tunnels]\nssh = $SVN_SSH ssh -i ~/.ssh/svnkey\n"));
		ScopedEnvVar unset("SVN_SSH", "");
		UAM_ASSERT(uam::vcs::SvnUserChoseSshTunnel());
		UAM_ASSERT(!HasName(uam::vcs::SvnEnvironment({}), "SVN_SSH"));
	}
	// An SSH host's own environment decides its tunnel; only the locale is set there, and nothing for Windows hosts.
	UAM_ASSERT(HasVariable(uam::vcs::SvnEnvironment("linux"), "LC_ALL", "C.UTF-8"));
	UAM_ASSERT(!HasName(uam::vcs::SvnEnvironment("linux"), "SVN_SSH"));
	UAM_ASSERT(uam::vcs::SvnEnvironment("windows").empty());
}
