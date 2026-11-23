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
	// A refused connection is not a credentials problem.
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed("svn: E170013: Unable to connect to a repository at URL 'svn://h/r'\nsvn: E000111: Can't connect to host 'h': Connection refused\n"), {}),
	    std::string("E000111: Can't connect to host 'h': Connection refused"));
}

UAM_TEST(SvnFailureMessageNamesTheOutOfDateFileOnOneLine)
{
	const ProcessExecutionResult commit = Failed("Sending        f.txt\nsvn: E155011: Commit failed (details follow):\nsvn: E155011: File '/work/copy/sub/f.txt' is out of date\nsvn: E160028: File '/trunk/sub/f.txt' is out of date\n");
	UAM_ASSERT_EQ(uam::SvnFailureMessage(commit, "fallback", "/work/copy"), std::string("sub/f.txt is out of date. Update, then commit again."));
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed("svn: E155011: Commit failed (details follow):\nsvn: E155011: Directory 'C:\\work\\copy\\docs' is out of date\n"), {}, "C:\\work\\copy\\"), std::string("docs is out of date. Update, then commit again."));
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
	UAM_ASSERT_EQ(uam::SvnFailureMessage(timed_out, {}), std::string("SVN did not answer in time. Check the connection to the repository, then retry."));
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed({}, std::string(uam::kSvnClientMissing)), {}), std::string(uam::kSvnClientMissing));
	// UAM's own reason, first line only, when svn printed nothing recognisable.
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed({}, "The remote VCS helper is unavailable.\nsecond line"), "fallback"), std::string("The remote VCS helper is unavailable."));
	UAM_ASSERT_EQ(uam::SvnFailureMessage(Failed("Sending a.txt\nTransmitting file data .\n"), "SVN did not commit the selection."), std::string("SVN did not commit the selection."));
}

UAM_TEST(SvnEnvironmentSetsBatchSshOnlyWhenTheUserHasNotChosenOne)
{
	{
		ScopedEnvVar unset("SVN_SSH", "");
		const auto local = uam::vcs::SvnEnvironment({});
		UAM_ASSERT(HasVariable(local, "SVN_SSH", std::string(uam::vcs::kSvnBatchSsh)));
#if !defined(_WIN32)
		UAM_ASSERT(HasName(local, "LC_ALL"));
#endif
	}
	{
		ScopedEnvVar chosen("SVN_SSH", "ssh -i ~/.ssh/svn");
		UAM_ASSERT(!HasName(uam::vcs::SvnEnvironment({}), "SVN_SSH"));
	}
	// An SSH host's own environment decides its tunnel; only the locale is set there, and nothing for Windows hosts.
	UAM_ASSERT(HasVariable(uam::vcs::SvnEnvironment("linux"), "LC_ALL", "C.UTF-8"));
	UAM_ASSERT(!HasName(uam::vcs::SvnEnvironment("linux"), "SVN_SSH"));
	UAM_ASSERT(uam::vcs::SvnEnvironment("windows").empty());
}
