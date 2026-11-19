#include "app/svn_history.h"

#include "common/platform/platform_services.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <system_error>

namespace uam::svn_history
{
	namespace
	{
		constexpr int kReadTimeoutMs = 120000;
		// A large checkout can take minutes to update; the caller's stop token still ends it early.
		constexpr int kUpdateTimeoutMs = 15 * 60 * 1000;
		constexpr std::size_t kMaxPathBytes = 4096;

		bool ParseRevision(std::string_view text, long long& revision)
		{
			const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), revision);
			return code == std::errc{} && end == text.data() + text.size() && revision > 0;
		}

		void AppendUtf8(std::string& out, const std::uint32_t code_point)
		{
			if (code_point < 0x80) out.push_back(static_cast<char>(code_point));
			else if (code_point < 0x800)
			{
				out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
				out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
			}
			else if (code_point < 0x10000)
			{
				out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
				out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
			}
			else if (code_point < 0x110000)
			{
				out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
				out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
			}
		}

		/// svn escapes only the five predefined entities and numeric character references.
		std::string Unescape(std::string_view text)
		{
			std::string out;
			out.reserve(text.size());
			for (std::size_t index = 0; index < text.size();)
			{
				const std::size_t semicolon = text[index] == '&' ? text.find(';', index) : std::string_view::npos;
				if (semicolon == std::string_view::npos || semicolon - index > 10)
				{
					out.push_back(text[index++]);
					continue;
				}
				const std::string_view name = text.substr(index + 1, semicolon - index - 1);
				std::uint32_t code_point = 0;
				bool known = true;
				if (name == "lt") out.push_back('<');
				else if (name == "gt") out.push_back('>');
				else if (name == "amp") out.push_back('&');
				else if (name == "quot") out.push_back('"');
				else if (name == "apos") out.push_back('\'');
				else if (name.size() > 1 && name[0] == '#')
				{
					const bool hex = name[1] == 'x' || name[1] == 'X';
					const std::string_view digits = name.substr(hex ? 2 : 1);
					const auto [end, code] = std::from_chars(digits.data(), digits.data() + digits.size(), code_point, hex ? 16 : 10);
					known = !digits.empty() && code == std::errc{} && end == digits.data() + digits.size();
					if (known) AppendUtf8(out, code_point);
				}
				else known = false;
				if (!known)
				{
					out.push_back(text[index++]);
					continue;
				}
				index = semicolon + 1;
			}
			return out;
		}

		/// Value of `name="..."` inside one start tag; empty when absent.
		std::string Attribute(std::string_view tag, std::string_view name)
		{
			for (std::size_t at = tag.find(name); at != std::string_view::npos; at = tag.find(name, at + 1))
			{
				const std::size_t value = at + name.size() + 2;
				if (at == 0 || (tag[at - 1] != ' ' && tag[at - 1] != '\n' && tag[at - 1] != '\t' && tag[at - 1] != '\r') || tag.substr(at + name.size(), 2) != "=\"") continue;
				const std::size_t end = tag.find('"', value);
				return end == std::string_view::npos ? std::string() : Unescape(tag.substr(value, end - value));
			}
			return {};
		}

		/// Text of the first `<name>...</name>` child; empty when absent.
		std::string ElementText(std::string_view block, std::string_view name)
		{
			const std::string open = "<" + std::string(name) + ">";
			const std::size_t start = block.find(open);
			if (start == std::string_view::npos) return {};
			const std::size_t end = block.find("</" + std::string(name) + ">", start);
			if (end == std::string_view::npos) return {};
			// Windows svn writes its XML through a text-mode stream, so a message's line breaks arrive as CRLF.
			std::string text;
			for (const char c : block.substr(start + open.size(), end - start - open.size()))
				if (c != '\r') text.push_back(c);
			return Unescape(text);
		}

		bool OutputLimited(const ProcessExecutionResult& result)
		{
			return result.output_truncated || result.error.find(uam::platform::kCapturedCommandOutputLimitError) != std::string::npos;
		}

		std::string Failure(const ProcessExecutionResult& result, std::string_view fallback)
		{
			if (result.canceled) return "Canceled.";
			if (result.timed_out) return "SVN did not answer in time. Check the connection to the repository, then retry.";
			const std::string summary = SvnFailureSummary(result);
			if (!summary.empty()) return summary;
			const std::string detail = uam::strings::Trim(result.error.empty() ? result.output : result.error);
			return detail.empty() ? std::string(fallback) : detail.substr(0, 400);
		}

		bool Succeeded(const ProcessExecutionResult& result)
		{
			return result.ok && !result.timed_out && !result.canceled && result.exit_code == 0;
		}

		std::string PercentDecode(std::string_view text)
		{
			const auto hex = [](const char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
			std::string out;
			for (std::size_t index = 0; index < text.size(); ++index)
			{
				if (text[index] == '%' && index + 2 < text.size() && hex(text[index + 1]) >= 0 && hex(text[index + 2]) >= 0)
				{
					out.push_back(static_cast<char>(hex(text[index + 1]) * 16 + hex(text[index + 2])));
					index += 2;
				}
				else out.push_back(text[index]);
			}
			return out;
		}

		struct WorkingCopy
		{
			std::string revision;
			/// Repository path of the working copy, such as `/branches/feat`; `/` at the repository root.
			std::string path;
			std::string root;
			std::string uuid;
		};

		/// One `svn info --xml` for everything the views need about the working copy; empty fields when it fails.
		WorkingCopy ReadWorkingCopy(const VcsCommandRunner& svn)
		{
			WorkingCopy copy;
			const ProcessExecutionResult result = svn({"info", "--xml", "--non-interactive", "--", "."}, kReadTimeoutMs);
			if (!Succeeded(result)) return copy;
			const std::string_view xml = result.output;
			const std::size_t entry = xml.find("<entry");
			const std::size_t entry_end = entry == std::string_view::npos ? entry : xml.find('>', entry);
			if (entry_end != std::string_view::npos) copy.revision = Attribute(xml.substr(entry, entry_end - entry), "revision");
			const std::string relative = ElementText(xml, "relative-url");
			if (relative.starts_with("^/")) copy.path = PercentDecode(std::string_view(relative).substr(1));
			copy.root = ElementText(xml, "root");
			copy.uuid = ElementText(xml, "uuid");
			return copy;
		}

		/// Percent-encodes every byte outside the URI unreserved set so svn reads the path literally, `@` and `%` included.
		std::string EncodeRepositoryPath(std::string_view path)
		{
			static constexpr char hex[] = "0123456789ABCDEF";
			std::string out;
			for (const char raw : path)
			{
				const unsigned char byte = static_cast<unsigned char>(raw);
				const bool plain = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~' || byte == '/';
				if (plain) out.push_back(raw);
				else
				{
					out.push_back('%');
					out.push_back(hex[byte >> 4]);
					out.push_back(hex[byte & 0x0F]);
				}
			}
			return out;
		}

		/// An update line is four status columns, a space and the path; `C` in any column is a conflict. The columns
		/// are the same in every locale, unlike the "Summary of conflicts" text.
		bool ReportsConflict(std::string_view output)
		{
			for (std::size_t start = 0; start < output.size();)
			{
				std::size_t end = output.find('\n', start);
				if (end == std::string_view::npos) end = output.size();
				const std::string_view line = output.substr(start, end - start);
				start = end + 1;
				if (line.size() < 6 || line[4] != ' ') continue;
				const std::string_view columns = line.substr(0, 4);
				if (columns.find_first_not_of(" ADUCGERB") == std::string_view::npos && columns.find('C') != std::string_view::npos) return true;
			}
			return false;
		}
	} // namespace

	std::vector<VcsHistoryEntry> ParseLogXml(std::string_view xml, const std::size_t max_paths)
	{
		std::vector<VcsHistoryEntry> entries;
		constexpr std::string_view kEntryEnd = "</logentry>";
		for (std::size_t start = xml.find("<logentry"); start != std::string_view::npos;)
		{
			const std::size_t end = xml.find(kEntryEnd, start);
			if (end == std::string_view::npos) break;
			const std::string_view block = xml.substr(start, end - start);
			start = xml.find("<logentry", end);
			const std::size_t head_end = block.find('>');
			if (head_end == std::string_view::npos) continue;
			VcsHistoryEntry entry;
			entry.revision = Attribute(block.substr(0, head_end), "revision");
			long long revision = 0;
			if (!ParseRevision(entry.revision, revision)) continue;
			entry.author = ElementText(block, "author");
			entry.date = ElementText(block, "date");
			entry.message = ElementText(block, "msg");
			for (std::size_t at = block.find("<path"); at != std::string_view::npos; at = block.find("<path", at + 1))
			{
				const std::size_t tag_end = block.find('>', at);
				// `<paths>` shares the prefix.
				if (tag_end == std::string_view::npos || at + 5 >= block.size() || block[at + 5] == 's') continue;
				const std::size_t close = block.find("</path>", tag_end);
				if (close == std::string_view::npos) break;
				if (entry.paths.size() >= max_paths)
				{
					++entry.paths_omitted;
					continue;
				}
				const std::string_view tag = block.substr(at, tag_end - at);
				entry.paths.push_back({Attribute(tag, "action"), Unescape(block.substr(tag_end + 1, close - tag_end - 1)), Attribute(tag, "kind"), Attribute(tag, "copyfrom-path"), Attribute(tag, "copyfrom-rev")});
			}
			entries.push_back(std::move(entry));
		}
		return entries;
	}

	VcsHistoryPage Log(const VcsCommandRunner& svn, const VcsHistoryRequest& request)
	{
		VcsHistoryPage page;
		const bool newest = request.before.empty();
		long long before = 0;
		if (!newest && !ParseRevision(request.before, before))
		{
			page.error = "The history position is not a revision number.";
			return page;
		}
		const int limit = std::clamp(request.limit, 1, kMaxPageSize);
		const std::string search = uam::strings::Trim(request.search);
		const auto run = [&](const std::string& from, const bool verbose, const int count)
		{
			// One extra entry tells whether another page exists. Revision 0 never changes a path, so it only keeps an empty repository from being an error.
			std::vector<std::string> args = {"log", "--xml", "--non-interactive", "--limit", std::to_string(count + 1), "--revision", from + ":0"};
			if (verbose) args.push_back("--verbose");
			if (!search.empty()) args.insert(args.end(), {"--search", search});
			args.insert(args.end(), {"--", "."});
			return svn(args, kReadTimeoutMs);
		};
		std::string from = newest ? "HEAD" : request.before;
		ProcessExecutionResult result = run(from, true, limit);
		if (newest && !Succeeded(result) && !OutputLimited(result) && !result.canceled && !result.timed_out)
		{
			// The working copy's path may no longer exist at HEAD (a deleted or moved branch); its own history still does.
			const ProcessExecutionResult base = run("BASE", true, limit);
			if (Succeeded(base) || OutputLimited(base))
			{
				from = "BASE";
				result = base;
			}
		}
		const bool limited = OutputLimited(result);
		if (!Succeeded(result) && !limited)
		{
			page.error = Failure(result, "SVN could not read this working copy's history.");
			return page;
		}
		page.entries = ParseLogXml(result.output);
		bool more = page.entries.size() > static_cast<std::size_t>(limit);
		if (limited)
		{
			more = true;
			if (page.entries.empty())
			{
				// One revision's changed paths alone exceed the capture limit: list it without them.
				const ProcessExecutionResult brief = run(from, false, 1);
				if (!Succeeded(brief))
				{
					page.error = Failure(brief, "SVN could not read this working copy's history.");
					return page;
				}
				page.entries = ParseLogXml(brief.output);
				more = page.entries.size() > 1;
				if (more) page.entries.resize(1);
				for (VcsHistoryEntry& entry : page.entries) entry.paths_omitted = 1;
			}
		}
		else if (more) page.entries.resize(static_cast<std::size_t>(limit));
		long long last = 0;
		if (more && !page.entries.empty() && ParseRevision(page.entries.back().revision, last) && last > 1) page.next = std::to_string(last - 1);
		if (newest)
		{
			const WorkingCopy copy = ReadWorkingCopy(svn);
			page.working_revision = copy.revision;
			page.location = copy.path.empty() ? std::string() : "^" + copy.path;
			page.repository_root = copy.root;
			page.repository_uuid = copy.uuid;
		}
		return page;
	}

	std::string RevisionDiff(const VcsCommandRunner& svn, const std::string_view revision, const std::string_view path, std::string& error)
	{
		long long number = 0;
		const bool unsafe_path = path.size() < 2 || path.size() > kMaxPathBytes || path.front() != '/' ||
		    std::ranges::any_of(path, [](const char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
		if (!ParseRevision(revision, number) || unsafe_path)
		{
			error = "Select a changed path from a listed revision.";
			return {};
		}
		const std::string target = "^" + EncodeRepositoryPath(path) + "@";
		// A path the revision deleted exists only at the revision before it. `--depth empty` keeps a folder to its own property changes.
		ProcessExecutionResult first;
		for (const long long peg : {number, number - 1})
		{
			if (peg < 1) break;
			const ProcessExecutionResult result = svn({"diff", "--internal-diff", "--non-interactive", "--depth", "empty", "--change", std::to_string(number), "--", target + std::to_string(peg)}, kReadTimeoutMs);
			if (OutputLimited(result))
			{
				error = "This change is too large to show here.";
				return {};
			}
			if (Succeeded(result)) return result.output;
			if (result.canceled || result.timed_out)
			{
				error = Failure(result, "");
				return {};
			}
			if (peg == number) first = result;
		}
		error = Failure(first, "SVN could not produce this change.");
		return {};
	}

	VcsUpdateResult Update(const VcsCommandRunner& svn)
	{
		VcsUpdateResult update;
		const ProcessExecutionResult result = svn({"update", "--non-interactive", "--accept", "postpone", "--", "."}, kUpdateTimeoutMs);
		update.output = uam::strings::Trim(result.output);
		if (!Succeeded(result))
		{
			update.error = result.canceled ? "The update was stopped. Run it again to finish; a locked working copy needs svn cleanup first."
			    : Failure(result, "SVN could not update this working copy.");
			return update;
		}
		update.ok = true;
		update.conflicts = ReportsConflict(result.output);
		update.revision = ReadWorkingCopy(svn).revision;
		return update;
	}

	namespace
	{
		constexpr std::size_t kMaxCopyEntryPaths = 5000;

		struct OriginPoint
		{
			std::string branch_path;
			std::string origin_path;
			std::string copy_revision;
			std::string origin_revision;
		};

		/// A log range whose newer end is past the newest revision: there is simply nothing in it yet.
		bool RangeStartsPastHead(const ProcessExecutionResult& result)
		{
			return SvnFailureSummary(result).find("E160006") != std::string::npos;
		}

		/// `^/path@HEAD`, encoded so svn reads every byte of the path literally.
		std::string HeadTarget(std::string_view repository_path)
		{
			return "^" + EncodeRepositoryPath(repository_path) + "@HEAD";
		}

		/// The copy that created the line of history at `target`, when there was one: the oldest revision before a copy.
		/// `moved` says the same revision deleted the copy's source, so the "copy" was a rename.
		bool FindCopy(const VcsCommandRunner& svn, const std::string& target, const std::vector<const char*>& ranges, const std::string& path, OriginPoint& point, bool& moved, std::string& error)
		{
			ProcessExecutionResult result;
			for (const char* range : ranges)
			{
				result = svn({"log", "--xml", "--verbose", "--non-interactive", "--stop-on-copy", "--limit", "1", "--revision", range, "--", target}, kReadTimeoutMs);
				if (Succeeded(result) || OutputLimited(result) || result.canceled || result.timed_out) break;
			}
			if (!Succeeded(result) && !OutputLimited(result))
			{
				error = Failure(result, "SVN could not read where this working copy was copied from.");
				return false;
			}
			const std::vector<VcsHistoryEntry> entries = ParseLogXml(result.output, kMaxCopyEntryPaths);
			if (entries.empty()) return true;
			std::size_t best = 0;
			std::string source;
			for (const VcsHistoryPath& changed : entries.front().paths)
			{
				const bool covers = path == changed.path || (path.starts_with(changed.path) && path.size() > changed.path.size() && path[changed.path.size()] == '/');
				// A path copied onto itself (a deleted line restored from its own past) is the same line, not a branch of it.
				if (changed.copy_from_path.empty() || changed.copy_from_path == changed.path || !covers || changed.path.size() < best) continue;
				best = changed.path.size();
				source = changed.copy_from_path;
				// A working copy of a folder inside the branch came from the same folder inside the origin.
				point.origin_path = changed.copy_from_path + path.substr(changed.path.size());
				point.copy_revision = entries.front().revision;
				point.origin_revision = changed.copy_from_revision;
			}
			moved = !source.empty() && std::ranges::any_of(entries.front().paths, [&](const VcsHistoryPath& changed) { return changed.action == "D" && changed.path == source; });
			return true;
		}

		/// Where this line of history was copied from. A rename is the same line under a new name, so the search
		/// continues through renames to the copy that started the line.
		bool FindOrigin(const VcsCommandRunner& svn, const WorkingCopy& copy, OriginPoint& point, std::string& error)
		{
			if (copy.path.empty())
			{
				error = "SVN could not read this working copy.";
				return false;
			}
			point.branch_path = copy.path;
			bool moved = false;
			if (!FindCopy(svn, ".", {"0:HEAD", "0:BASE"}, copy.path, point, moved, error)) return false;
			// ponytail: eight renames is plenty; a longer chain shows the last name found as the origin.
			for (int renames = 0; moved && renames < 8; ++renames)
			{
				OriginPoint earlier;
				const std::string range = "0:" + point.origin_revision;
				const std::string target = "^" + EncodeRepositoryPath(point.origin_path) + "@" + point.origin_revision;
				if (!FindCopy(svn, target, {range.c_str()}, point.origin_path, earlier, moved, error)) return false;
				if (earlier.origin_path.empty())
				{
					// The renamed line was never copied from anywhere: it is a main line under a new name.
					point.origin_path.clear();
					point.copy_revision.clear();
					point.origin_revision.clear();
					break;
				}
				point.origin_path = earlier.origin_path;
				point.copy_revision = earlier.copy_revision;
				point.origin_revision = earlier.origin_revision;
			}
			return true;
		}

		std::vector<std::string> ParseEligibleRevisions(std::string_view output)
		{
			std::vector<long long> numbers;
			for (std::size_t start = 0; start < output.size();)
			{
				std::size_t end = output.find('\n', start);
				if (end == std::string_view::npos) end = output.size();
				std::string_view line = output.substr(start, end - start);
				start = end + 1;
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '*')) line.remove_suffix(1);
				long long number = 0;
				if (line.size() > 1 && line.front() == 'r' && ParseRevision(line.substr(1), number)) numbers.push_back(number);
			}
			std::ranges::sort(numbers, std::greater<>());
			numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
			std::vector<std::string> revisions;
			for (const long long number : numbers) revisions.push_back(std::to_string(number));
			return revisions;
		}

	} // namespace

	VcsOriginReply Origin(const VcsCommandRunner& svn, const VcsOriginRequest& request)
	{
		VcsOriginReply reply;
		// Anything the request names is checked before svn is asked anything.
		if (request.view != "summary" && request.view != "ahead" && request.view != "behind")
		{
			reply.error = "Unknown comparison view.";
			return reply;
		}
		std::vector<std::string> wanted;
		for (const std::string& revision : request.revisions)
		{
			long long number = 0;
			if (!ParseRevision(revision, number) || wanted.size() >= static_cast<std::size_t>(kMaxPageSize))
			{
				reply.error = "Select revisions from the list of origin revisions.";
				return reply;
			}
			wanted.push_back(std::to_string(number));
		}
		const WorkingCopy copy = ReadWorkingCopy(svn);
		OriginPoint point;
		if (!FindOrigin(svn, copy, point, reply.error)) return reply;
		reply.branch_path = point.branch_path;
		reply.origin_path = point.origin_path;
		reply.copy_revision = point.copy_revision;
		reply.origin_revision = point.origin_revision;
		if (point.origin_path.empty())
		{
			if (request.view != "summary") reply.error = "This working copy was not copied from another path.";
			return reply;
		}
		const std::string origin = HeadTarget(point.origin_path);
		const std::string branch = HeadTarget(point.branch_path);

		if (request.view == "summary")
		{
			// Merge tracking knows which origin revisions the branch already has; without it every origin revision since the copy counts.
			const ProcessExecutionResult eligible = svn({"mergeinfo", "--show-revs", "eligible", "--non-interactive", "--", origin, branch}, kReadTimeoutMs);
			if (eligible.canceled) { reply.error = "Canceled."; return reply; }
			if (Succeeded(eligible)) reply.behind_revisions = ParseEligibleRevisions(eligible.output);
			else
			{
				long long from = 0;
				const ProcessExecutionResult logged = ParseRevision(point.origin_revision, from)
				    ? svn({"log", "--xml", "--quiet", "--non-interactive", "--limit", std::to_string(kMaxBehindRevisions + 1), "--revision", "HEAD:" + std::to_string(from + 1), "--", origin}, kReadTimeoutMs)
				    : eligible;
				if (logged.canceled) { reply.error = "Canceled."; return reply; }
				if (Succeeded(logged) || OutputLimited(logged) || RangeStartsPastHead(logged))
				{
					reply.behind_from_log = true;
					for (const VcsHistoryEntry& entry : ParseLogXml(logged.output, 0)) reply.behind_revisions.push_back(entry.revision);
				}
			}
			// ponytail: the graph pages these 50 at a time; past this many the lane simply ends early.
			if (reply.behind_revisions.size() > kMaxBehindRevisions) reply.behind_revisions.resize(kMaxBehindRevisions);
			// Only a record that names the origin says its revisions were merged; a branch also inherits its origin's own records.
			const ProcessExecutionResult recorded = svn({"propget", "svn:mergeinfo", "--non-interactive", "--", branch}, kReadTimeoutMs);
			const std::string named = point.origin_path + ":";
			reply.merges_recorded = Succeeded(recorded) && (recorded.output.starts_with(named) || recorded.output.find("\n" + named) != std::string::npos);
			return reply;
		}

		if (request.view == "ahead")
		{
			VcsHistoryRequest history;
			history.before = request.before;
			history.limit = request.limit;
			reply.page = Log(svn, history);
			reply.error = reply.page.error;
			long long copied = 0;
			(void)ParseRevision(point.copy_revision, copied);
			// The line's own revisions end at the copy; whatever follows in its log belongs to the origin.
			const std::size_t before = reply.page.entries.size();
			std::erase_if(reply.page.entries, [&](const VcsHistoryEntry& entry) { long long revision = 0; return !ParseRevision(entry.revision, revision) || revision <= copied; });
			if (reply.page.entries.size() != before) reply.page.next.clear();
			return reply;
		}

		if (request.view == "behind")
		{
			if (wanted.empty()) return reply;
			// One `-r` each: the revisions the branch lacks can be far apart, and a range would read everything between them.
			std::vector<std::string> args = {"log", "--xml", "--verbose", "--non-interactive"};
			for (const std::string& revision : wanted) args.insert(args.end(), {"--revision", revision});
			args.insert(args.end(), {"--", origin});
			const ProcessExecutionResult result = svn(args, kReadTimeoutMs);
			if (OutputLimited(result))
			{
				reply.error = "These revisions are too large to list together.";
				return reply;
			}
			if (!Succeeded(result))
			{
				reply.error = Failure(result, "SVN could not read the origin's history.");
				return reply;
			}
			reply.page.entries = ParseLogXml(result.output);
			std::ranges::sort(reply.page.entries, [](const VcsHistoryEntry& left, const VcsHistoryEntry& right) { return std::stoll(left.revision) > std::stoll(right.revision); });
			return reply;
		}

		reply.error = "Unknown comparison view.";
		return reply;
	}
} // namespace uam::svn_history
