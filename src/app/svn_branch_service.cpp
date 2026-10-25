#include "app/svn_branch_service.h"
#include "common/models/app_models.h"
#include "common/paths/path_utils.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <regex>
#include <sstream>
#include <string_view>

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
				return left > 0 && m_runner(args, static_cast<int>(std::min<long long>(left, 10000)), output) && output.size() <= kOutputLimit;
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
			std::chrono::steady_clock::time_point m_deadline;
		};
		SvnBranchResult Discover(const SvnBranchScope& scope, const std::string& parent, Probe& probe)
		{
			SvnBranchResult result;
			if (!VerifyScope(scope, result.error)) return result;
			if (!probe.Read(scope, result.context)) { result.error = "Could not verify the full SVN working-copy root."; return result; }
			if (parent.empty()) { result.ok = true; return result; }
			if (!RelativePath(parent)) { result.error = "Enter an explicit repository folder such as ^/project/lines, without dot segments."; return result; }
			result.context.parent = parent;
			const std::string url = result.context.repository + UrlPath(parent.substr(1));
			std::string uuid, kind, xml;
			if (!probe.Info("repos-uuid", url, uuid) || uuid != result.context.uuid || !probe.Info("kind", url, kind) || kind != "dir" ||
			    !probe.Info("revision", url, result.context.repository_revision, "HEAD") || !Probe::Numeric(result.context.repository_revision) ||
			    !probe.Run({"list", "--non-interactive", "--xml", "--depth", "immediates", "-r", result.context.repository_revision, "--", url + "@" + result.context.repository_revision}, xml))
			{ result.error = "Could not discover directories in that SVN repository folder."; return result; }
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
		if (!probe.Clean(result.context)) { result.error = "Resolve changes, ignored/untracked paths, conflicts, externals and switched children before switching SVN folders."; return result; }
		if (choice->current) { result.ok = true; return result; }
		const auto target = result.context.repository + UrlPath(path.substr(1));
		std::string uuid, kind, output;
		if (!probe.NoExternals(expected.directory) || !probe.NoExternals(target, expected.repository_revision))
		{ result.error = "Resolve SVN external definitions before switching folders."; return result; }
		if (!probe.Info("repos-uuid", target, uuid, expected.repository_revision) || uuid != expected.uuid ||
		    !probe.Info("kind", target, kind, expected.repository_revision) || kind != "dir")
		{ result.error = "The selected SVN folder no longer belongs to this repository."; return result; }
		SvnBranchContext current;
		if (!VerifyScope(scope, result.error) || !probe.Read(scope, current) || current.url != expected.url || current.revision != expected.revision ||
		    current.uuid != expected.uuid || current.repository != expected.repository || !probe.Clean(current))
		{ result.error = "The working copy changed before SVN switching. Discover the folders again."; return result; }
		// Deliberate switch only: no checkout, --force, --ignore-ancestry, cleanup or rollback.
		if (!probe.Run({"switch", "--non-interactive", "--ignore-externals", "--accept", "postpone", "--depth", "infinity", "-r", expected.repository_revision, "--", target + "@" + expected.repository_revision, expected.directory}, output))
		{ result.error = "SVN did not confirm switching. Inspect the working copy before retrying; no automatic rollback was attempted."; return result; }
		if (!probe.Read(scope, current) || current.url != target || current.uuid != expected.uuid || current.repository != expected.repository ||
		    current.revision != expected.repository_revision || !probe.Clean(current))
		{ result.error = "SVN switching needs inspection: its final URL, revision or clean state was not confirmed."; return result; }
		result.context = current;
		result.ok = true;
		result.error.clear();
		return result;
	}
}
