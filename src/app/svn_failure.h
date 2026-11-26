#pragma once

#include "common/models/app_models.h"

#include <string>
#include <string_view>

namespace uam
{
	/// <summary>RunSvn's error when the svn client could not be started at all.</summary>
	inline constexpr std::string_view kSvnClientMissing =
	    "The svn command was not found; install Subversion and make sure svn is on PATH.";

	/// <summary>Bounded failure detail: the missing-client notice, or svn's most specific "E<code>: ..." line (the first line
	/// of the innermost error in svn's first chain, past lead lines that only introduce the next error), or the first
	/// "W<code>: ..." warning when there is no error or the error only says some targets failed; empty when none exists.
	/// A line that hands over to non-svn output, such as a hook's "with output:", carries that output's first line.</summary>
	std::string SvnFailureSummary(const ProcessExecutionResult& result);

	/// <summary>The one-line message a person sees for a failed svn command; it does not log. Canceled and timed-out
	/// commands, credentials svn could not get or the server refused (E215004, E170001 "Can't get..." or "Authentication
	/// error", or E170013 caused by ssh authentication) and an out-of-date commit (E155011, E160028, E170004) get plain
	/// wording; otherwise SvnFailureSummary with paths under `workspace` made relative, then UAM's own first error line,
	/// then `fallback`.</summary>
	std::string SvnFailureMessage(const ProcessExecutionResult& result, std::string_view fallback, std::string_view workspace = {});

	/// <summary>Logs a failure whose message is shown to a person. Only svn's own E/W lines (or UAM's first error line) are
	/// written, never command output (a line counts as svn's only when it starts with svn's prefix, and output cut at the
	/// capture limit is not read at all), with workspace and home paths shortened and URL user names removed, bounded, and
	/// once per workspace, `operation` and error code every five minutes.</summary>
	void LogSvnFailure(const ProcessExecutionResult& result, std::string_view operation, std::string_view workspace = {});
} // namespace uam
