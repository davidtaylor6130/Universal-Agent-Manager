#include "app/svn_failure.h"

#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/env_utils.h"
#include "common/utils/string_utils.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace uam
{
	namespace
	{
		struct SvnLine
		{
			bool warning = false;
			std::string code; ///< "E170001" or "W155010".
			std::string text; ///< svn's message after the code.
			std::string next; ///< The first non-empty line after it, when that line is not svn's own (hook output, a path).
		};

		/// Only a line that starts with svn's prefix is svn's own; the same text inside a diff or log message is content.
		bool IsSvnLine(const std::string& line)
		{
			return line.starts_with("svn: E") || line.starts_with("svn: warning: W");
		}

		/// Output cut at the capture limit can end anywhere in a diff or listing, so none of it is read as svn's errors.
		bool OutputLimited(const ProcessExecutionResult& result)
		{
			return result.output_truncated || result.error.find(uam::platform::kCapturedCommandOutputLimitError) != std::string::npos;
		}

		/// Every `svn: E...` and `svn: warning: W...` line, in order.
		std::vector<SvnLine> ParseLines(const ProcessExecutionResult& result)
		{
			if (OutputLimited(result)) return {};
			std::vector<std::string> raw;
			std::istringstream stream(result.output + "\n" + result.error);
			for (std::string line; std::getline(stream, line);) raw.push_back(uam::strings::Trim(line));
			std::vector<SvnLine> lines;
			for (std::size_t index = 0; index < raw.size(); ++index)
			{
				const std::string& line = raw[index];
				if (!IsSvnLine(line)) continue;
				SvnLine parsed;
				parsed.warning = line.starts_with("svn: warning: W");
				const std::size_t start = parsed.warning ? 14 : 5;
				const std::size_t colon = line.find(':', start);
				if (colon == std::string::npos) continue;
				parsed.code = line.substr(start, colon - start);
				parsed.text = uam::strings::Trim(std::string_view(line).substr(colon + 1));
				for (std::size_t after = index + 1; after < raw.size() && !IsSvnLine(raw[after]); ++after)
					if (!raw[after].empty()) { parsed.next = raw[after]; break; }
				lines.push_back(std::move(parsed));
			}
			return lines;
		}

		/// Cuts at a UTF-8 character boundary.
		std::string Bound(std::string text, const std::size_t limit)
		{
			if (text.size() <= limit) return text;
			std::size_t end = limit;
			while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
			text.resize(end);
			return text;
		}

		/// A lead line such as "Commit failed (details follow):" that only introduces the next error. The second test does
		/// not depend on svn's language: a line ending in ':' followed by the same code naming a quoted path.
		bool IsWrapper(const std::vector<const SvnLine*>& errors, const std::size_t index)
		{
			const SvnLine& line = *errors[index];
			if (line.text.ends_with("(details follow):")) return true;
			return line.text.ends_with(':') && index + 1 < errors.size() && errors[index + 1]->code == line.code && errors[index + 1]->text.find('\'') != std::string::npos;
		}

		const SvnLine* SelectLine(const std::vector<SvnLine>& lines)
		{
			const SvnLine* first_warning = nullptr;
			std::vector<const SvnLine*> all_errors;
			for (const SvnLine& line : lines)
			{
				if (line.warning) { if (first_warning == nullptr) first_warning = &line; continue; }
				// What follows "Additional errors:" belongs to cleanup after the first failure, not to its cause.
				if (line.code == "E200042") break;
				all_errors.push_back(&line);
			}
			std::vector<const SvnLine*> errors;
			for (std::size_t index = 0; index < all_errors.size(); ++index)
				if (!IsWrapper(all_errors, index)) errors.push_back(all_errors[index]);
			if (errors.empty()) return first_warning;
			// svn prints a chain outermost first. The innermost code is the cause; within that code the first line is svn's
			// own lead (E155004 leads with "Run 'svn cleanup'", then lists the locked paths).
			std::size_t chosen = errors.size() - 1;
			while (chosen > 0 && errors[chosen - 1]->code == errors[chosen]->code) --chosen;
			// "Could not display info for all targets" only counts the warnings above it. The externals summary (E205011) is
			// the error that ended the command, so it is named rather than the warning printed on the way.
			if (errors[chosen]->code == "E200009" && first_warning != nullptr) return first_warning;
			return errors[chosen];
		}

		std::string Relative(std::string text, std::string_view workspace)
		{
			while (workspace.size() > 1 && (workspace.back() == '/' || workspace.back() == '\\')) workspace.remove_suffix(1);
			if (workspace.size() < 2) return text;
			for (const char separator : {'/', '\\'})
			{
				const std::string prefix = std::string(workspace) + separator;
				for (std::size_t at = text.find(prefix); at != std::string::npos; at = text.find(prefix, at)) text.erase(at, prefix.size());
			}
			return text;
		}

		std::string Quoted(const std::string& text)
		{
			const std::size_t open = text.find('\'');
			const std::size_t close = text.rfind('\'');
			return open == std::string::npos || close <= open ? std::string{} : text.substr(open + 1, close - open - 1);
		}

		bool NeedsCredentials(const std::vector<SvnLine>& lines, const ProcessExecutionResult& result)
		{
			bool connect = false;
			for (const SvnLine& line : lines)
			{
				if (line.code == "E215004") return true;
				// E170001 also means "Authorization failed": signed in, but this account may not read the path. Saving
				// credentials cannot help then, so only svn's "could not get" and "server refused" wordings count.
				if (line.code == "E170001" && (line.text.starts_with("Can't get") || line.text.starts_with("Authentication error"))) return true;
				connect = connect || line.code == "E170013";
			}
			if (!connect || OutputLimited(result)) return false;
			// With SVN_SSH in batch mode, ssh reports why it gave up on the lines around svn's own.
			const std::string raw = result.output + "\n" + result.error;
			for (const std::string_view cause : {"Permission denied (", "Host key verification failed", "Too many authentication failures", "Authentication failed"})
				if (raw.find(cause) != std::string::npos) return true;
			return false;
		}

		std::string CredentialsMessage(const std::vector<SvnLine>& lines)
		{
			for (const SvnLine& line : lines)
			{
				const std::size_t at = line.text.find("URL '");
				const std::size_t end = at == std::string::npos ? std::string::npos : line.text.find('\'', at + 5);
				if (end != std::string::npos && end > at + 5)
					return "SVN needs your credentials. Run svn info " + Bound(line.text.substr(at + 5, end - at - 5), 300) + " once in a terminal to save them, then retry.";
			}
			return "SVN needs your credentials. Run svn info -r HEAD once in a terminal in this working copy to save them, then retry.";
		}

		/// Empty unless svn refused a commit because a selected path is older than the repository's. Keyed on the codes,
		/// which mean nothing else, so a localised client still gets the plain wording.
		std::string OutOfDateMessage(const std::vector<SvnLine>& lines, std::string_view workspace)
		{
			std::string path;
			bool out_of_date = false;
			for (const SvnLine& line : lines)
			{
				if (line.warning || (line.code != "E155011" && line.code != "E160028" && line.code != "E170004")) continue;
				out_of_date = true;
				// The working-copy path (E155011) reads better than the repository path, when svn gave one.
				const std::string quoted = Quoted(line.text);
				if (quoted.empty()) continue;
				if (path.empty() || line.code == "E155011") path = quoted;
				if (line.code == "E155011") break;
			}
			if (!out_of_date) return {};
			path = Bound(Relative(path, workspace), 300);
			return (path.empty() ? std::string("A selected file") : path) + " is out of date. Update, then commit again.";
		}

		/// Log text keeps svn's own message, never file contents: workspace and home paths shortened, URL user names removed.
		std::string Sanitize(std::string text, std::string_view workspace)
		{
			text = Relative(std::move(text), workspace);
			if (const std::optional<std::filesystem::path> home = uam::env::GetUserHomePath())
			{
				std::string prefix = uam::paths::Utf8PathString(*home);
				while (prefix.size() > 1 && (prefix.back() == '/' || prefix.back() == '\\')) prefix.pop_back();
				// Only the whole home folder: `/Users/bob` must not shorten `/Users/bobby`.
				const auto boundary = [&](const std::size_t end) { return end == text.size() || std::string_view("/\\'\" ").find(text[end]) != std::string_view::npos; };
				if (prefix.size() > 1)
					for (std::size_t at = text.find(prefix); at != std::string::npos; at = text.find(prefix, at + 1))
						if (boundary(at + prefix.size())) text.replace(at, prefix.size(), "~");
			}
			for (std::size_t scheme = text.find("://"); scheme != std::string::npos; scheme = text.find("://", scheme + 3))
			{
				const std::size_t host = scheme + 3;
				const std::size_t end = text.find_first_of("/'\" ", host);
				const std::size_t user = text.rfind('@', end == std::string::npos ? text.size() : end);
				if (user != std::string::npos && user >= host && (end == std::string::npos || user < end)) text.erase(host, user + 1 - host);
			}
			return text;
		}
	} // namespace

	std::string SvnFailureSummary(const ProcessExecutionResult& result)
	{
		if (result.error == kSvnClientMissing) return result.error;
		const std::vector<SvnLine> lines = ParseLines(result);
		const SvnLine* line = SelectLine(lines);
		if (line == nullptr) return {};
		std::string text = line->text;
		// svn ends a line with ':' when it continues on the next one. A hook's own reason follows its "with output:" line;
		// a continuation that is svn's own (often a path) is left out and the summary stops at the sentence.
		if (text.ends_with(':') && !line->next.empty()) text += ' ' + Bound(line->next, 200);
		std::string summary = Bound(line->code + ": " + text, 400);
		while (!summary.empty() && (summary.back() == '.' || summary.back() == ':')) summary.pop_back();
		return summary;
	}

	std::string SvnFailureMessage(const ProcessExecutionResult& result, std::string_view fallback, std::string_view workspace)
	{
		if (result.canceled) return "Canceled.";
		if (result.timed_out) return "SVN did not answer in time. Check the connection to the repository, then retry.";
		if (result.error == kSvnClientMissing) return result.error;
		const std::vector<SvnLine> lines = ParseLines(result);
		if (NeedsCredentials(lines, result)) return CredentialsMessage(lines);
		if (std::string message = OutOfDateMessage(lines, workspace); !message.empty()) return message;
		if (std::string summary = Relative(SvnFailureSummary(result), workspace); !summary.empty()) return summary;
		// UAM's own reason (the remote helper, an output limit) when svn printed nothing it recognises.
		const std::string error = uam::strings::Trim(result.error);
		if (std::string first = Bound(error.substr(0, error.find('\n')), 240); !first.empty()) return first;
		return std::string(fallback);
	}

	void LogSvnFailure(const ProcessExecutionResult& result, std::string_view operation, std::string_view workspace)
	{
		if (result.canceled) return;
		const std::vector<SvnLine> lines = ParseLines(result);
		const SvnLine* selected = SelectLine(lines);
		std::string detail;
		if (result.timed_out) detail = "timed out";
		else if (!lines.empty())
		{
			for (const SvnLine& line : lines)
			{
				if (!detail.empty()) detail += " | ";
				detail += line.code + ": " + line.text;
				if (detail.size() > 1024) break;
			}
			detail = Bound(Sanitize(std::move(detail), workspace), 1024);
		}
		else
		{
			// No svn diagnostics: only UAM's own one-line reason, never the command's output (a diff, a listing).
			const std::string error = uam::strings::Trim(result.error);
			detail = Bound(Sanitize(error.substr(0, error.find('\n')), workspace), 240);
		}
		// Per working copy too: another checkout failing the same way is a separate event.
		const std::string key = std::string(workspace) + '\n' + std::string(operation) + ':' + (result.timed_out ? std::string("timeout") : selected != nullptr ? selected->code : detail);
		{
			static std::mutex mutex;
			static std::map<std::string, std::chrono::steady_clock::time_point> logged;
			const std::lock_guard lock(mutex);
			const auto now = std::chrono::steady_clock::now();
			const auto found = logged.find(key);
			if (found != logged.end() && now - found->second < std::chrono::minutes(5)) return;
			if (logged.size() >= 64) logged.clear();
			logged[key] = now;
		}
		uam::diagnostics::Write("SVN " + std::string(operation) + " failed (exit " + std::to_string(result.exit_code) + "): " + (detail.empty() ? std::string("no detail") : detail));
	}
} // namespace uam
