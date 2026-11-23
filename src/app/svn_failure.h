#pragma once

#include "common/models/app_models.h"

#include <string>
#include <string_view>

namespace uam
{
	/// <summary>RunSvn's error when the svn client could not be started at all.</summary>
	inline constexpr std::string_view kSvnClientMissing =
	    "The svn command-line client was not found. Install Subversion and make sure svn is on PATH.";

	/// <summary>Bounded failure detail: the missing-client notice, or svn's most specific "E<code>: ..." line (the first line
	/// of the innermost error in svn's first chain, past "details follow" wrappers), or the first "W<code>: ..." warning when
	/// there is no error or the error only says some targets failed; empty when none exists.</summary>
	std::string SvnFailureSummary(const ProcessExecutionResult& result);

	/// <summary>The one-line message a person sees for a failed svn command. Canceled and timed-out commands, credentials
	/// svn could not get or that were refused (E170001, E215004, or E170013 caused by ssh authentication), and an out-of-date
	/// commit (E155011, E160028, E170004) get plain wording; otherwise SvnFailureSummary with paths under `workspace` made
	/// relative, then UAM's own first error line, then `fallback`. The raw output goes only to the diagnostic log, bounded
	/// and with a repeated identical failure logged once per five minutes.</summary>
	std::string SvnFailureMessage(const ProcessExecutionResult& result, std::string_view fallback, std::string_view workspace = {});
} // namespace uam
