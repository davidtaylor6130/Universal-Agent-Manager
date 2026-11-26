#include "app/svn_branch_service.h"
#include "common/config/svn_layout_lookup.h"
#include "common/models/app_models.h"
#include "common/paths/path_utils.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <regex>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace uam
{
	SvnBranchScope BuildSvnBranchScope(bool enabled, const ChatSession& chat, const std::vector<ChatFolder>& folders, const std::vector<ChatSession>& references)
	{
		SvnBranchScope scope;
		scope.editable = enabled && (chat.execution_host_id.empty() || chat.execution_host_id == "local") &&
		    !chat.imported_read_only && chat.agent_run_id.empty() && chat.goal_owner_chat_id.empty() && chat.temporary_parent_chat_id.empty() &&
		    chat.workspace_worktree_directory.empty() && chat.workspace_source_directory.empty() && chat.workspace_base_ref.empty() && chat.workspace_isolation_kind.empty();
		for (const auto& folder : folders)
			if (folder.id == chat.folder_id && (folder.execution_host_id.empty() || folder.execution_host_id == "local")) scope.folder = paths::PathFromUtf8(folder.directory);
		scope.directory = chat.workspace_directory.empty() ? scope.folder : paths::PathFromUtf8(chat.workspace_directory);
		for (const auto& other : references)
		{
			if (other.id == chat.id || (!other.execution_host_id.empty() && other.execution_host_id != "local")) continue;
			// A snapshot still owns its source even when its active worktree is elsewhere.
			for (const auto* directory : {&other.workspace_directory, &other.workspace_worktree_directory, &other.workspace_source_directory})
				if (!directory->empty()) scope.other_workspaces.push_back(paths::PathFromUtf8(*directory));
			if (other.workspace_directory.empty() && other.workspace_worktree_directory.empty())
				for (const auto& folder : folders)
					if (folder.id == other.folder_id && !folder.directory.empty()) scope.other_workspaces.push_back(paths::PathFromUtf8(folder.directory));
		}
		return scope;
	}
	namespace
	{
		constexpr std::size_t kOutputLimit = 128 * 1024;
		bool CleanText(const std::string& value)
		{
			return !value.empty() && value.size() <= 4096 && std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
		}
		bool RelativePath(const std::string& path)
		{
			if (path == "^/") return true;
			if (!CleanText(path) || !path.starts_with("^/") || path.size() < 2 || path.back() == '/' || path.find('\\') != std::string::npos) return false;
			std::istringstream parts(path.substr(2));
			std::string part;
			while (std::getline(parts, part, '/')) if (part.empty() || part == "." || part == "..") return false;
			return true;
		}
		std::string UrlPath(const std::string& path)
		{
			constexpr char hex[] = "0123456789ABCDEF";
			std::string encoded;
			for (unsigned char c : path)
			{
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') encoded += static_cast<char>(c);
				else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
			}
			return encoded;
		}
		bool Overlaps(const std::filesystem::path& a, const std::filesystem::path& b)
		{
			auto prefix = [](const auto& left, auto right)
			{
				// Filesystem identity also handles Windows path casing and existing aliases.
				for (; !right.empty(); right = right.parent_path())
				{
					std::error_code ec;
					if (std::filesystem::equivalent(left, right, ec) && !ec) return true;
					if (right == right.parent_path()) break;
				}
				return false;
			};
			return prefix(a, b) || prefix(b, a);
		}
		bool VerifyScope(const SvnBranchScope& scope, std::string& error)
		{
			error = "Choose an editable, exclusively used local SVN working-copy root.";
			std::error_code ec;
			if (!scope.editable || !scope.directory.is_absolute() || !scope.folder.is_absolute()) return false;
			const auto directory = std::filesystem::canonical(scope.directory, ec);
			if (ec || !std::filesystem::equivalent(directory, scope.folder, ec) || ec) return false;
			for (auto path = scope.directory.lexically_normal(); !path.empty(); path = path.parent_path())
			{
				if (paths::IsLinkOrReparsePointNoThrow(path)) return false;
				if (path == path.parent_path()) break;
			}
			const auto admin = std::filesystem::symlink_status(directory / ".svn", ec);
			if (ec || !std::filesystem::is_directory(admin) || paths::IsLinkOrReparsePointNoThrow(directory / ".svn")) return false;
			for (const auto& other : scope.other_workspaces)
			{
				if (!other.is_absolute()) return false;
				const auto resolved = std::filesystem::weakly_canonical(other, ec);
				if (ec || Overlaps(directory, resolved)) { error = "Another chat references this SVN working copy. Use a separate working copy before switching."; return false; }
			}
			error.clear();
			return true;
		}
		bool DecodeName(std::string text, std::string& name)
		{
			name.clear();
			for (std::size_t i = 0; i < text.size(); ++i)
			{
				if (text[i] != '&') { name += text[i]; continue; }
				const auto end = text.find(';', i);
				if (end == std::string::npos) return false;
				const auto entity = text.substr(i, end - i + 1);
				if (entity == "&amp;") name += '&'; else if (entity == "&lt;") name += '<'; else if (entity == "&gt;") name += '>';
				else if (entity == "&quot;") name += '"'; else if (entity == "&apos;") name += '\''; else return false;
				i = end;
			}
			return CleanText(name) && name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos;
		}
		bool ParseDirectories(const std::string& xml, std::vector<std::string>& names)
		{
			if (xml.size() > kOutputLimit || xml.find("<!") != std::string::npos || xml.find("<lists>") == std::string::npos || xml.find("</lists>") == std::string::npos) return false;
			std::size_t position = 0, count = 0;
			while ((position = xml.find("<entry", position)) != std::string::npos)
			{
				if (++count > 256) return false;
				const auto tag_end = xml.find('>', position), end = xml.find("</entry>", position);
				if (tag_end == std::string::npos || end == std::string::npos || tag_end >= end) return false;
				const auto next = xml.find("<entry", tag_end);
				if (next != std::string::npos && next < end) return false;
				const auto tag = xml.substr(position, tag_end - position + 1);
				static const std::regex directory_tag(R"(<entry\s+kind="dir"\s*>)");
				static const std::regex file_tag(R"(<entry\s+kind="file"\s*>)");
				if (!std::regex_match(tag, directory_tag) && !std::regex_match(tag, file_tag)) return false;
				const auto start_name = xml.find("<name>", tag_end), end_name = xml.find("</name>", tag_end);
				if (start_name == std::string::npos || end_name == std::string::npos || start_name + 6 >= end_name || end_name >= end) return false;
				std::string name;
				if (!DecodeName(xml.substr(start_name + 6, end_name - start_name - 6), name)) return false;
				if (std::regex_match(tag, directory_tag)) names.push_back(name);
				position = end + 8;
			}
			return true;
		}
		bool FullDepthInventory(std::string_view xml)
		{
			const auto trim = [](std::string_view value)
			{
				while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
				while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
				return value;
			};
			if (xml.size() > kOutputLimit || xml.find("<!") != std::string_view::npos) return false;
			xml = trim(xml);
			if (!xml.starts_with("<?xml")) return false;
			const auto declaration = xml.find("?>");
			if (declaration == std::string_view::npos) return false;
			xml = trim(xml.substr(declaration + 2));
			if (!xml.starts_with("<info>") || !xml.ends_with("</info>")) return false;
			xml = trim(xml.substr(6, xml.size() - 13));
			std::size_t count = 0;
			while (!xml.empty())
			{
				if (++count > 4096 || !xml.starts_with("<entry") || xml.size() < 7 || !std::isspace(static_cast<unsigned char>(xml[6]))) return false;
				const auto tag = xml.find('>'), end = xml.find("</entry>");
				if (tag == std::string_view::npos || end == std::string_view::npos || tag >= end) return false;
				const auto entry = xml.substr(tag + 1, end - tag - 1);
				if (entry.find("<entry") != std::string_view::npos) return false;
				const auto wc = entry.find("<wc-info>"), wc_end = entry.find("</wc-info>");
				if (wc == std::string_view::npos || wc_end == std::string_view::npos || wc + 9 > wc_end || entry.find("<wc-info>", wc + 9) != std::string_view::npos) return false;
				const auto info = entry.substr(wc + 9, wc_end - wc - 9);
				const auto depth = info.find("<depth>"), depth_end = info.find("</depth>");
				if (depth == std::string_view::npos || depth_end == std::string_view::npos || depth + 7 > depth_end || info.find("<depth>", depth + 7) != std::string_view::npos || trim(info.substr(depth + 7, depth_end - depth - 7)) != "infinity") return false;
				xml = trim(xml.substr(end + 8));
			}
			return count > 0;
		}
		class Probe
		{
		  public:
			explicit Probe(const SvnBranchRunner& runner) : m_runner(runner), m_deadline(std::chrono::steady_clock::now() + std::chrono::seconds(30)) {}
			bool Run(std::vector<std::string> args, std::string& output)
			{
				const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(m_deadline - std::chrono::steady_clock::now()).count();
				output.clear();
				m_failure.clear();
				if (left <= 0) { m_failure = "SVN did not answer in time. Check the connection to the repository, then retry."; return false; }
				return m_runner(args, static_cast<int>(std::min<long long>(left, 10000)), output, m_failure) && output.size() <= kOutputLimit;
			}
			/// Why the latest command failed, when svn said; empty after a success or a refusal based on its output.
			const std::string& Failure() const { return m_failure; }
			/// The refusal, or, when the command itself failed, `failed` followed by svn's reason.
			std::string Explain(std::string_view refused, std::string_view failed = {}) const
			{
				if (m_failure.empty()) return std::string(refused);
				std::string message(failed.empty() ? refused : failed);
				message += ' ';
				message += m_failure;
				if (message.back() != '.') message += '.';
				return message;
			}
			bool Info(const std::string& field, const std::string& target, std::string& output, const std::string& revision = {})
			{
				std::vector<std::string> args{"info", "--non-interactive", "--show-item", field};
				if (!revision.empty()) { args.push_back("-r"); args.push_back(revision); }
				args.insert(args.end(), {"--", target + "@" + revision});
				if (!Run(args, output)) return false;
				while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
				return CleanText(output);
			}
			bool Read(const SvnBranchScope& scope, SvnBranchContext& context)
			{
				context.directory = paths::Utf8PathString(scope.directory.lexically_normal());
				std::string root, depth;
				if (!Info("wc-root", context.directory, root) || !Info("depth", context.directory, depth) || depth != "infinity") return false;
				std::error_code ec;
				if (!std::filesystem::equivalent(paths::PathFromUtf8(root), scope.directory, ec) || ec) return false;
				// Recursive info includes excluded nodes; root depth/status alone miss sticky sparse children.
				std::string inventory;
				if (!Run({"info", "--non-interactive", "--xml", "--depth", "infinity", "--", context.directory + "@"}, inventory) || !FullDepthInventory(inventory)) return false;
				return Info("url", context.directory, context.url) && Info("repos-root-url", context.directory, context.repository) &&
				    Info("repos-uuid", context.directory, context.uuid) && Info("revision", context.directory, context.revision) && Numeric(context.revision) &&
				    (context.repository.starts_with("https://") || context.repository.starts_with("http://") || context.repository.starts_with("svn://") || context.repository.starts_with("svn+ssh://") || context.repository.starts_with("file://"));
			}
			bool Clean(const SvnBranchContext& context)
			{
				std::string output;
				if (!Run({"status", "--non-interactive", "--xml", "--no-ignore", "--", context.directory + "@"}, output)) return false;
				// Any tracked change, unversioned/ignored path, conflict, external or switched child refuses.
				static const std::regex empty_status(R"(\s*<\?xml[^?]*\?>\s*<status>\s*<target\s+path="[^"<>]*"\s*>\s*</target>\s*</status>\s*)");
				return std::regex_match(output, empty_status);
			}
			bool NoExternals(const std::string& target, const std::string& revision = {})
			{
				std::vector<std::string> args{"propget", "--non-interactive", "--xml", "--recursive", "svn:externals"};
				if (!revision.empty()) args.insert(args.end(), {"-r", revision});
				args.insert(args.end(), {"--", target + "@" + revision});
				std::string output;
				static const std::regex empty_properties(R"(\s*<\?xml[^?]*\?>\s*<properties>\s*</properties>\s*)");
				return Run(args, output) && std::regex_match(output, empty_properties);
			}
			static bool Numeric(const std::string& value) { return !value.empty() && value.size() <= 20 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }); }
		  private:
			const SvnBranchRunner& m_runner;
			std::string m_failure;
			std::chrono::steady_clock::time_point m_deadline;
		};
		SvnBranchResult Discover(const SvnBranchScope& scope, const std::string& parent, Probe& probe)
		{
			SvnBranchResult result;
			if (!VerifyScope(scope, result.error)) return result;
			if (!probe.Read(scope, result.context)) { result.error = probe.Explain("Could not verify the full SVN working-copy root."); return result; }
			if (parent.empty()) { result.ok = true; return result; }
			if (!RelativePath(parent)) { result.error = "Enter an explicit repository folder such as ^/project/lines, without dot segments."; return result; }
			result.context.parent = parent;
			const std::string url = result.context.repository + UrlPath(parent.substr(1));
			std::string uuid, kind, xml;
			if (!probe.Info("repos-uuid", url, uuid) || uuid != result.context.uuid || !probe.Info("kind", url, kind) || kind != "dir" ||
			    !probe.Info("revision", url, result.context.repository_revision, "HEAD") || !Probe::Numeric(result.context.repository_revision) ||
			    !probe.Run({"list", "--non-interactive", "--xml", "--depth", "immediates", "-r", result.context.repository_revision, "--", url + "@" + result.context.repository_revision}, xml))
			{ result.error = probe.Explain("Could not discover directories in that SVN repository folder."); return result; }
			std::vector<std::string> names;
			if (!ParseDirectories(xml, names)) { result.error = "SVN directory discovery exceeded its supported format or 256-entry limit."; return result; }
			for (const auto& name : names)
			{
				const auto path = parent + (parent.back() == '/' ? "" : "/") + name;
				if (!RelativePath(path)) { result.error = "An SVN directory has an unsupported path."; return result; }
				result.branches.push_back({name, path, result.context.url == result.context.repository + UrlPath(path.substr(1))});
			}
			result.ok = true;
			return result;
		}
	}
	SvnBranchResult DiscoverSvnBranches(const SvnBranchScope& scope, const std::string& parent, const SvnBranchRunner& runner)
	{
		Probe probe(runner);
		return Discover(scope, parent, probe);
	}
	SvnBranchResult SwitchSvnBranch(const SvnBranchScope& scope, const SvnBranchContext& expected, const std::string& path, const SvnBranchRunner& runner)
	{
		Probe probe(runner);
		auto result = Discover(scope, expected.parent, probe);
		if (!result.ok) return result;
		result.ok = false;
		if (expected.parent.empty() || result.context != expected) { result.error = "The SVN working copy or repository changed. Discover the folders again."; return result; }
		const auto choice = std::find_if(result.branches.begin(), result.branches.end(), [&](const auto& item) { return item.path == path; });
		if (choice == result.branches.end()) { result.error = "Choose a directory from the current SVN discovery."; return result; }
		if (!probe.Clean(result.context)) { result.error = probe.Explain("Resolve changes, ignored/untracked paths, conflicts, externals and switched children before switching SVN folders.", "Could not read the working copy's status."); return result; }
		if (choice->current) { result.ok = true; return result; }
		const auto target = result.context.repository + UrlPath(path.substr(1));
		std::string uuid, kind, output;
		if (!probe.NoExternals(expected.directory) || !probe.NoExternals(target, expected.repository_revision))
		{ result.error = probe.Explain("Resolve SVN external definitions before switching folders.", "Could not check SVN external definitions."); return result; }
		if (!probe.Info("repos-uuid", target, uuid, expected.repository_revision) || uuid != expected.uuid ||
		    !probe.Info("kind", target, kind, expected.repository_revision) || kind != "dir")
		{ result.error = probe.Explain("The selected SVN folder no longer belongs to this repository.", "Could not read the selected SVN folder."); return result; }
		SvnBranchContext current;
		if (!VerifyScope(scope, result.error) || !probe.Read(scope, current) || current.url != expected.url || current.revision != expected.revision ||
		    current.uuid != expected.uuid || current.repository != expected.repository || !probe.Clean(current))
		{ result.error = probe.Explain("The working copy changed before SVN switching. Discover the folders again.", "Could not recheck the working copy before SVN switching."); return result; }
		// Deliberate switch only: no checkout, --force, --ignore-ancestry, cleanup or rollback.
		if (!probe.Run({"switch", "--non-interactive", "--ignore-externals", "--accept", "postpone", "--depth", "infinity", "-r", expected.repository_revision, "--", target + "@" + expected.repository_revision, expected.directory}, output))
		{
			result.error = "SVN did not confirm switching.";
			if (!probe.Failure().empty()) result.error += " " + probe.Failure() + (probe.Failure().back() == '.' ? "" : ".");
			result.error += " Inspect the working copy before retrying; no automatic rollback was attempted.";
			return result;
		}
		if (!probe.Read(scope, current) || current.url != target || current.uuid != expected.uuid || current.repository != expected.repository ||
		    current.revision != expected.repository_revision || !probe.Clean(current))
		{ result.error = probe.Explain("SVN switching needs inspection: its final URL, revision or clean state was not confirmed."); return result; }
		result.context = current;
		result.ok = true;
		result.error.clear();
		return result;
	}

	namespace
	{
		std::vector<std::string> SplitFolder(std::string_view folder)
		{
			std::vector<std::string> parts;
			for (std::size_t start = 0; start < folder.size();)
			{
				const auto end = (std::min)(folder.find('/', start), folder.size()); // parenthesised: Windows headers may define min
				if (end > start) parts.emplace_back(folder.substr(start, end - start));
				start = end + 1;
			}
			return parts;
		}

		std::string JoinFolder(const std::vector<std::string>& parts)
		{
			std::string joined;
			for (const auto& part : parts) joined += "/" + part;
			return joined;
		}

		int HexValue(char c)
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		}

		/// The working copy's decoded repository path segments (`^/a/b` gives a, b); false for a URL outside the repository.
		bool RepositorySegments(const SvnBranchContext& context, std::vector<std::string>& segments)
		{
			segments.clear();
			if (context.repository.empty() || context.url.empty()) return false;
			if (context.url == context.repository) return true;
			if (!context.url.starts_with(context.repository + "/")) return false;
			const std::string_view encoded = std::string_view(context.url).substr(context.repository.size() + 1);
			std::string decoded;
			for (std::size_t i = 0; i < encoded.size(); ++i)
			{
				if (encoded[i] != '%') { decoded += encoded[i]; continue; }
				if (i + 2 >= encoded.size()) return false;
				const int high = HexValue(encoded[i + 1]), low = HexValue(encoded[i + 2]);
				if (high < 0 || low < 0) return false;
				decoded += static_cast<char>(high * 16 + low);
				i += 2;
			}
			segments = SplitFolder(decoded);
			return !segments.empty() && "^" + JoinFolder(segments) == "^/" + decoded && RelativePath("^" + JoinFolder(segments));
		}

		bool DecodeXmlText(std::string_view text, std::string& decoded)
		{
			decoded.clear();
			for (std::size_t i = 0; i < text.size(); ++i)
			{
				if (text[i] != '&') { decoded += text[i]; continue; }
				const auto end = text.find(';', i);
				if (end == std::string_view::npos) return false;
				const auto entity = text.substr(i, end - i + 1);
				if (entity == "&amp;") decoded += '&'; else if (entity == "&lt;") decoded += '<'; else if (entity == "&gt;") decoded += '>';
				else if (entity == "&quot;") decoded += '"'; else if (entity == "&apos;") decoded += '\''; else return false;
				i = end;
			}
			return true;
		}

		/// One attribute of a start tag, decoded; false when it is missing or malformed.
		bool Attribute(const std::string& tag, const std::string& name, std::string& value)
		{
			const std::string key = name + "=\"";
			for (std::size_t at = tag.find(key); at != std::string::npos; at = tag.find(key, at + 1))
			{
				if (at == 0 || !std::isspace(static_cast<unsigned char>(tag[at - 1]))) continue;
				const auto start = at + key.size(), end = tag.find('"', start);
				return end != std::string::npos && DecodeXmlText(std::string_view(tag).substr(start, end - start), value);
			}
			return false;
		}

		/// Every node of `svn info --xml --depth infinity` is at `revision`, so the working copy's URL@revision is what it holds.
		bool SingleRevision(const std::string& xml, const std::string& revision)
		{
			if (xml.size() > kOutputLimit || xml.find("<!") != std::string::npos) return false;
			std::size_t position = 0, count = 0;
			while ((position = xml.find("<entry", position)) != std::string::npos)
			{
				const auto end = xml.find('>', position);
				if (end == std::string::npos || ++count > 4096) return false;
				const std::string tag = xml.substr(position, end - position + 1);
				std::string value;
				if (!Attribute(tag, "revision", value) || value != revision) return false;
				position = end;
			}
			return count > 0;
		}

		/// A one-entry `svn log --xml --verbose` records `target` as added by copying `source` at `revision`; `committed` is that entry's revision.
		bool CopyCommitted(const std::string& xml, const std::string& target, const std::string& source, const std::string& revision, std::string& committed)
		{
			if (xml.size() > kOutputLimit || xml.find("<!") != std::string::npos) return false;
			const auto entry = xml.find("<logentry"), entry_end = entry == std::string::npos ? entry : xml.find('>', entry);
			if (entry_end == std::string::npos || xml.find("<logentry", entry_end) != std::string::npos ||
			    !Attribute(xml.substr(entry, entry_end - entry + 1), "revision", committed) || !Probe::Numeric(committed)) return false;
			std::size_t position = entry_end;
			while ((position = xml.find("<path", position)) != std::string::npos)
			{
				const auto tag_end = xml.find('>', position);
				if (tag_end == std::string::npos) return false;
				const char after = position + 5 < xml.size() ? xml[position + 5] : '\0';
				if (after != '>' && !std::isspace(static_cast<unsigned char>(after))) { position = tag_end; continue; } // <paths>
				const auto close = xml.find("</path>", tag_end);
				if (close == std::string::npos) return false;
				const std::string tag = xml.substr(position, tag_end - position + 1);
				std::string path, action, from_path, from_revision;
				if (DecodeXmlText(std::string_view(xml).substr(tag_end + 1, close - tag_end - 1), path) && path == target &&
				    Attribute(tag, "action", action) && action == "A" && Attribute(tag, "copyfrom-path", from_path) && from_path == source &&
				    Attribute(tag, "copyfrom-rev", from_revision) && from_revision == revision)
					return true;
				position = close;
			}
			return false;
		}

		bool ValidBranchName(const std::string& name)
		{
			if (name.empty() || name.size() > 128 || name.front() == '.' || name.front() == '-' || name.back() == '.' || name.find("..") != std::string::npos) return false;
			return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
		}

		std::string Revisioned(const std::string& path, const std::string& revision) { return path + " r" + revision; }
	}

	SvnBranchLocations SvnBranchLocationsFor(const SvnBranchContext& context, const std::vector<SvnRepositoryLayout>& layouts)
	{
		SvnBranchLocations locations;
		std::vector<std::string> segments;
		if (!RepositorySegments(context, segments)) return locations;
		SvnRepositoryLayout layout;
		if (const SvnRepositoryLayout* saved = svn_layout::Find(layouts, context.repository, context.uuid)) layout = *saved;
		const auto trunk = SplitFolder(layout.trunk), branches = SplitFolder(layout.branches), tags = SplitFolder(layout.tags);
		if (trunk.empty() || branches.empty()) return locations;
		const auto starts = [&](std::size_t index, const std::vector<std::string>& folder)
		{ return !folder.empty() && index + folder.size() <= segments.size() && std::equal(folder.begin(), folder.end(), segments.begin() + static_cast<std::ptrdiff_t>(index)); };
		// Same reading as the history view: the first trunk, or branches/tags folder followed by a name, ends the project folder.
		std::size_t project = 0;
		for (std::size_t index = 0; index < segments.size(); ++index)
			if (starts(index, trunk) || (starts(index, branches) && index + branches.size() < segments.size()) || (starts(index, tags) && index + tags.size() < segments.size()))
			{ project = index; break; }
		std::vector<std::string> base(segments.begin(), segments.begin() + static_cast<std::ptrdiff_t>(project));
		auto under = [&](const std::vector<std::string>& folder) { auto path = base; path.insert(path.end(), folder.begin(), folder.end()); return "^" + JoinFolder(path); };
		locations.branches = under(branches);
		locations.main_line = under(trunk);
		locations.ok = RelativePath(locations.branches) && RelativePath(locations.main_line);
		if (!locations.ok) locations.branches.clear(), locations.main_line.clear();
		return locations;
	}

	bool SameSvnWorkingCopy(const SvnBranchContext& a, const SvnBranchContext& b)
	{
		return a.directory == b.directory && a.url == b.url && a.repository == b.repository && a.uuid == b.uuid && a.revision == b.revision;
	}

	bool ValidSvnBranchCreate(const SvnBranchCreateRequest& request, std::string& error)
	{
		error.clear();
		if (!ValidBranchName(request.name))
			error = "Name the SVN branch with letters, digits, dots, dashes or underscores, starting with a letter or digit.";
		else if (!CleanText(request.message) || std::all_of(request.message.begin(), request.message.end(), [](unsigned char c) { return std::isspace(c); }))
			error = "Write a one-line commit message for the new SVN branch.";
		return error.empty();
	}

	bool SvnCommandChangesRepository(const std::vector<std::string>& args)
	{
		return !args.empty() && (args.front() == "copy" || args.front() == "switch");
	}

	namespace
	{
		constexpr int kCopyTimeoutMs = 120000;
		constexpr int kSwitchTimeoutMs = 600000;

		/// A command allowed longer than a probe's 10-second reads: the commit and the switch. Works with either runner shape
		/// (with or without the trailing failure line).
		template <typename Runner>
		bool RunLong(const Runner& runner, const std::vector<std::string>& args, const int timeout_ms, std::string& output)
		{
			output.clear();
			bool ok = false;
			if constexpr (std::is_invocable_v<const Runner&, const std::vector<std::string>&, int, std::string&>) ok = runner(args, timeout_ms, output);
			else
			{
				std::string failure;
				ok = runner(args, timeout_ms, output, failure);
			}
			return ok && output.size() <= kOutputLimit;
		}

		/// The revision svn printed for its commit ("Committed revision N."); empty when the client's language differs.
		std::string PrintedRevision(const std::string& output)
		{
			static const std::regex committed(R"(Committed revision (\d+)\.)");
			std::smatch match;
			return std::regex_search(output, match, committed) ? match[1].str() : std::string{};
		}

		/// Paths a later commit deleted or replaced under `base` (a decoded repository path), from `svn log --xml --verbose`.
		bool RemovedPaths(const std::string& xml, const std::string& base, std::vector<std::string>& relative)
		{
			if (xml.size() > kOutputLimit || xml.find("<!") != std::string::npos || xml.find("<log") == std::string::npos) return false;
			std::size_t position = 0;
			while ((position = xml.find("<path", position)) != std::string::npos)
			{
				const auto tag_end = xml.find('>', position);
				if (tag_end == std::string::npos) return false;
				const char after = position + 5 < xml.size() ? xml[position + 5] : '\0';
				if (after != '>' && !std::isspace(static_cast<unsigned char>(after))) { position = tag_end; continue; } // <paths>
				const auto close = xml.find("</path>", tag_end);
				if (close == std::string::npos) return false;
				std::string path, action;
				if (!DecodeXmlText(std::string_view(xml).substr(tag_end + 1, close - tag_end - 1), path) || !Attribute(xml.substr(position, tag_end - position + 1), "action", action)) return false;
				if ((action == "D" || action == "R") && path.starts_with(base + "/")) relative.push_back(path.substr(base.size() + 1));
				position = close;
			}
			return true;
		}
	}

	SvnBranchCreateResult CreateSvnBranch(const SvnBranchScope& scope, const SvnBranchContext& expected, const SvnBranchCreateRequest& request,
	    const std::vector<SvnRepositoryLayout>& layouts, const SvnBranchRunner& runner)
	{
		SvnBranchCreateResult result;
		const auto refuse = [&](std::string error) { result.error = std::move(error); return result; };
		if (!ValidSvnBranchCreate(request, result.error)) return result;
		// Each phase has its own time budget, so slow checks never leave the commit or its confirmation without time.
		Probe checks(runner);
		SvnBranchResult found = Discover(scope, {}, checks);
		if (!found.ok) return refuse(found.error);
		const SvnBranchContext working = found.context;
		if (!SameSvnWorkingCopy(working, expected)) return refuse("The SVN working copy changed. Open the branch picker again.");
		const SvnBranchLocations locations = SvnBranchLocationsFor(working, layouts);
		if (!locations.ok) return refuse("UAM cannot place a branch for this repository path. Check the repository's layout in Settings.");
		// The new branch is switched to straight after it is created, so anything that would refuse that switch refuses first.
		if (!checks.Clean(working)) return refuse("Resolve changes, ignored/untracked paths, conflicts, externals and switched children before creating an SVN branch.");
		if (!checks.NoExternals(working.directory)) return refuse("Resolve SVN external definitions before creating a branch.");
		std::string head, uuid, kind;
		if (!checks.Info("revision", working.repository, head, "HEAD") || !Probe::Numeric(head)) return refuse("Could not read the SVN repository's newest revision.");
		const std::string branches_url = working.repository + UrlPath(locations.branches.substr(1));
		if (!checks.Info("repos-uuid", branches_url, uuid, head) || uuid != working.uuid || !checks.Info("kind", branches_url, kind, head) || kind != "dir")
			return refuse("This repository has no " + locations.branches + " folder. Set its branches folder in Settings, under SVN repository layouts.");
		const std::string target = locations.branches + "/" + request.name;
		const std::string target_url = working.repository + UrlPath(target.substr(1));
		// Copying onto an existing folder would nest the copy inside it, so the name must be shown absent, not merely unreadable.
		// `--search` keeps the reply to the matching names however large the folder is; it matches regardless of letter case,
		// and a name differing only in case is refused too, since it collides in checkouts on macOS and Windows.
		const auto name_taken = [&](Probe& probe, const std::string& revision, bool& taken)
		{
			std::string listing;
			if (!probe.Run({"list", "--non-interactive", "--search", request.name, "--", branches_url + "@" + revision}, listing)) return false;
			taken = false;
			std::istringstream lines(listing);
			const auto folded = [](std::string value) { for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return value; };
			for (std::string line; std::getline(lines, line);)
			{
				while (!line.empty() && (line.back() == '\r' || line.back() == '/')) line.pop_back();
				taken = taken || folded(line) == folded(request.name);
			}
			return true;
		};
		bool taken = false;
		if (!name_taken(checks, head, taken)) return refuse("Could not check whether " + target + " exists. Check your access to the repository, then retry.");
		if (taken) return refuse("A branch named " + request.name + " already exists in " + locations.branches + ". Choose another name.");

		std::string source, source_url, source_revision;
		if (request.source == SvnBranchSource::WorkingCopy)
		{
			std::vector<std::string> segments;
			if (!RepositorySegments(working, segments) || segments.empty()) return refuse("A branch cannot be created from the repository root. Copy from the main line instead.");
			source = "^" + JoinFolder(segments);
			source_url = working.url;
			source_revision = working.revision;
			std::string inventory;
			if (!checks.Run({"info", "--non-interactive", "--xml", "--depth", "infinity", "--", working.directory + "@"}, inventory) || !SingleRevision(inventory, working.revision))
				return refuse("Parts of this working copy are at other revisions. Update it first, or copy from the main line.");
			// A deletion committed from this working copy leaves no trace in it, yet URL@revision still has the file. A later
			// commit's deletion that is also missing on disk is that case; someone else's deletion is still on disk here.
			if (working.revision != head)
			{
				std::string log;
				std::vector<std::string> removed;
				if (!checks.Run({"log", "--non-interactive", "--xml", "--verbose", "--quiet", "-r", std::to_string(std::stoull(working.revision) + 1) + ":" + head, "--", working.url + "@" + head}, log) ||
				    !RemovedPaths(log, source.substr(1), removed))
					return refuse("This working copy is far behind " + source + ". Update it first, or copy from the main line.");
				for (const std::string& path : removed)
				{
					std::error_code ec;
					if (!std::filesystem::exists(std::filesystem::symlink_status(paths::PathFromUtf8(working.directory) / paths::PathFromUtf8(path), ec)))
						return refuse("This working copy deleted files in a later commit than its revision. Update it first, or copy from the main line.");
				}
			}
		}
		else
		{
			source = locations.main_line;
			source_url = working.repository + UrlPath(locations.main_line.substr(1));
			source_revision = head;
			if (!checks.Info("repos-uuid", source_url, uuid, head) || uuid != working.uuid || !checks.Info("kind", source_url, kind, head) || kind != "dir")
				return refuse("This repository has no " + locations.main_line + " folder. Set its trunk folder in Settings, under SVN repository layouts.");
			// The working copy's own externals were checked locally; the main line is read from the server, which can be slow.
			std::string properties;
			if (!RunLong(runner, {"propget", "--non-interactive", "--xml", "--recursive", "svn:externals", "-r", head, "--", source_url + "@" + head}, kCopyTimeoutMs, properties))
				return refuse("Could not check external definitions in " + source + ". Check your access to the repository, then retry.");
			static const std::regex empty_properties(R"(\s*<\?xml[^?]*\?>\s*<properties>\s*</properties>\s*)");
			if (!std::regex_match(properties, empty_properties)) return refuse("Resolve SVN external definitions in " + source + " before branching from it.");
		}
		if (target.starts_with(source + "/")) return refuse("A branch cannot be created inside the folder it copies. Check the repository's layout in Settings.");

		// One server-side commit; no --parents, so a missing branches folder can never be created by accident. A stoppable
		// runner must let it finish (SvnCommandChangesRepository).
		std::string output;
		const bool copied = RunLong(runner, {"copy", "--non-interactive", "--force-log", "-m", request.message, "--", source_url + "@" + source_revision, target_url}, kCopyTimeoutMs, output);
		const std::string printed = copied ? PrintedRevision(output) : std::string{};
		// The oldest revision of the new path, however many commits follow it, is the copy that created it. This runs even
		// when the copy reported failure: a server can finish a commit after the client gave up.
		Probe confirm(runner);
		std::string log, committed;
		const bool confirmed = confirm.Run({"log", "--non-interactive", "--xml", "--verbose", "--stop-on-copy", "--limit", "1", "-r", "1:HEAD", "--", target_url + "@HEAD"}, log) &&
		    CopyCommitted(log, target.substr(1), source.substr(1), source_revision, committed) && (printed.empty() || printed == committed);
		if (!confirmed)
		{
			const std::string nested = target + "/" + SplitFolder(source).back();
			result.outcome = SvnBranchCreateOutcome::NeedsInspection;
			if (confirm.Info("kind", working.repository + UrlPath(nested.substr(1)), kind, "HEAD"))
				return refuse(target + " appeared while it was being created, so the copy went inside it as " + nested + ". Inspect the repository.");
			if (copied) return refuse("Created " + target + ", but the repository log does not confirm it was copied from " + Revisioned(source, source_revision) + ". Inspect it before switching.");
			bool exists = true;
			if (name_taken(confirm, "HEAD", exists) && !exists)
			{
				result.outcome = SvnBranchCreateOutcome::NotCreated;
				return refuse("SVN did not create " + target + ". Check the commit message and your access to the repository, then retry.");
			}
			return refuse("SVN did not confirm creating " + target + ". Inspect the repository before retrying.");
		}
		// From here the branch exists; a refusal says so and keeps the working copy where it was.
		result.outcome = SvnBranchCreateOutcome::CreatedNotSwitched;
		result.branches = {{request.name, target, false}};
		const std::string created = "Created " + Revisioned(target, committed) + " from " + Revisioned(source, source_revision) + ", but the working copy was not switched. ";

		// The same guarded switch as folder selection, aimed at the commit just made rather than a fresh listing.
		Probe settle(runner);
		SvnBranchContext current;
		std::string scope_error;
		if (!VerifyScope(scope, scope_error) || !settle.Read(scope, current) || !SameSvnWorkingCopy(current, working) || !settle.Clean(current))
			return refuse(created + "The working copy changed meanwhile. Switch to the branch from the picker once it is clean.");
		if (!RunLong(runner, {"switch", "--non-interactive", "--ignore-externals", "--accept", "postpone", "--depth", "infinity", "-r", committed, "--", target_url + "@" + committed, working.directory}, kSwitchTimeoutMs, output))
			return refuse(created + "SVN did not confirm switching. Inspect the working copy before retrying; no automatic rollback was attempted.");
		std::vector<std::string> segments;
		// svn leaves characters such as + and @ unescaped in its URLs, so the final URL is compared as a decoded path.
		Probe verify(runner);
		if (!verify.Read(scope, current) || !RepositorySegments(current, segments) || "^" + JoinFolder(segments) != target || current.uuid != working.uuid ||
		    current.repository != working.repository || current.revision != committed || !verify.Clean(current))
			return refuse(created + "SVN switching needs inspection: its final URL, revision or clean state was not confirmed.");
		result.ok = true;
		result.outcome = SvnBranchCreateOutcome::CreatedAndSwitched;
		result.context = current;
		result.branches = {{request.name, target, true}};
		return result;
	}
}
