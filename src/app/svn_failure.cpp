#include "app/svn_failure.h"

#include "common/utils/diagnostic_log.h"
#include "common/utils/string_utils.h"

#include <array>
#include <chrono>
#include <cstddef>
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
		};

		/// Every `svn: E...` and `svn: warning: W...` line, in order.
		std::vector<SvnLine> ParseLines(const ProcessExecutionResult& result)
		{
			std::vector<SvnLine> lines;
			std::istringstream stream(result.output + "\n" + result.error);
			std::string line;
			while (std::getline(stream, line))
			{
				SvnLine parsed;
				std::size_t start = line.find("svn: E");
				if (start != std::string::npos) start += 5;
				else if ((start = line.find("svn: warning: W")) != std::string::npos) { start += 14; parsed.warning = true; }
				else continue;
				const std::size_t colon = line.find(':', start);
				if (colon == std::string::npos) continue;
				parsed.code = line.substr(start, colon - start);
				parsed.text = uam::strings::Trim(std::string_view(line).substr(colon + 1));
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

		const SvnLine* SelectLine(const std::vector<SvnLine>& lines)
		{
			const SvnLine* first_warning = nullptr;
			std::vector<const SvnLine*> errors;
			for (const SvnLine& line : lines)
			{
				if (line.warning) { if (first_warning == nullptr) first_warning = &line; continue; }
				// What follows "Additional errors:" belongs to cleanup after the first failure, not to its cause.
				if (line.code == "E200042") break;
				if (line.text.ends_with("(details follow):")) continue;
				errors.push_back(&line);
			}
			if (errors.empty()) return first_warning;
			// svn prints a chain outermost first. The innermost code is the cause; within that code the first line is svn's
			// own lead (E155004 leads with "Run 'svn cleanup'", then lists the locked paths).
			std::size_t chosen = errors.size() - 1;
			while (chosen > 0 && errors[chosen - 1]->code == errors[chosen]->code) --chosen;
			// "Could not display info for all targets" and the externals summary only count the warnings above them.
			const std::string& code = errors[chosen]->code;
			if ((code == "E200009" || code == "E205011") && first_warning != nullptr) return first_warning;
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
				if (line.code == "E170001" || line.code == "E215004") return true;
				connect = connect || line.code == "E170013";
			}
			if (!connect) return false;
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

		/// Empty unless svn refused a commit because a selected path is older than the repository's.
		std::string OutOfDateMessage(const std::vector<SvnLine>& lines, std::string_view workspace)
		{
			std::string path;
			bool out_of_date = false;
			for (const SvnLine& line : lines)
			{
				if (line.warning || line.text.find("out of date") == std::string::npos) continue;
				if (line.code != "E155011" && line.code != "E160028" && line.code != "E170004") continue;
				out_of_date = true;
				// The working-copy path (E155011) reads better than the repository path, when svn gave one.
				if (path.empty() || line.code == "E155011") path = Quoted(line.text);
				if (line.code == "E155011" && !path.empty()) break;
			}
			if (!out_of_date) return {};
			path = Bound(Relative(path, workspace), 300);
			return (path.empty() ? std::string("A selected file") : path) + " is out of date. Update, then commit again.";
		}

		void LogFailure(const ProcessExecutionResult& result, const std::string& message)
		{
			static std::mutex mutex;
			static std::string last;
			static std::chrono::steady_clock::time_point last_time;
			std::string raw = uam::strings::Trim(result.output + "\n" + result.error);
			for (char& c : raw) if (c == '\n' || c == '\r') c = ' ';
			const std::string entry = "SVN command failed (exit " + std::to_string(result.exit_code) + (result.timed_out ? ", timed out" : "") + "): " + message + " | " + Bound(raw, 2048);
			{
				const std::lock_guard lock(mutex);
				const auto now = std::chrono::steady_clock::now();
				if (entry == last && now - last_time < std::chrono::minutes(5)) return;
				last = entry;
				last_time = now;
			}
			uam::diagnostics::Write(entry);
		}
	} // namespace

	std::string SvnFailureSummary(const ProcessExecutionResult& result)
	{
		if (result.error == kSvnClientMissing) return result.error;
		const std::vector<SvnLine> lines = ParseLines(result);
		const SvnLine* line = SelectLine(lines);
		if (line == nullptr) return {};
		std::string summary = Bound(line->code + ": " + line->text, 240);
		// svn ends a line with ':' when it continues on the next one (often a path); the summary stops at the sentence.
		while (!summary.empty() && (summary.back() == '.' || summary.back() == ':')) summary.pop_back();
		return summary;
	}

	std::string SvnFailureMessage(const ProcessExecutionResult& result, std::string_view fallback, std::string_view workspace)
	{
		if (result.canceled) return "Canceled.";
		std::string message;
		const std::vector<SvnLine> lines = ParseLines(result);
		if (result.timed_out) message = "SVN did not answer in time. Check the connection to the repository, then retry.";
		else if (result.error == kSvnClientMissing) message = result.error;
		else if (NeedsCredentials(lines, result)) message = CredentialsMessage(lines);
		else if (message = OutOfDateMessage(lines, workspace); message.empty())
		{
			message = Relative(SvnFailureSummary(result), workspace);
			if (message.empty())
			{
				// UAM's own reason (the remote helper, an output limit) when svn printed nothing it recognises.
				const std::string error = uam::strings::Trim(result.error);
				message = Bound(error.substr(0, error.find('\n')), 240);
			}
			if (message.empty()) message = std::string(fallback);
		}
		LogFailure(result, message);
		return message;
	}
} // namespace uam
