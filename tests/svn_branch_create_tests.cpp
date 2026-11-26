// SVN branch creation (#520): a fake repository for the command-level cases, and `--integration` against a disposable
// file:// repository prepared by tests/svn_branch_create_integration.py.
#include "app/svn_branch_service.h"
#include "common/models/app_models.h"
#include "common/paths/path_utils.h"
#include "common/utils/shell_escape.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace
{
	void Check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
	bool Has(const std::vector<std::string>& args, const std::string& value) { return std::find(args.begin(), args.end(), value) != args.end(); }

	/// A repository at file:///repo whose working copy is checked out from `url` at `revision`. Commands are answered from
	/// `folders` (repository paths) so creation, discovery and the switch see one consistent repository.
	struct Repository
	{
		std::filesystem::path directory = std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("uam-svn-create-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		uam::SvnBranchScope scope;
		std::string root = "file:///repo", uuid = "uuid-1", url = "file:///repo/trunk", revision = "5";
		int head = 7;
		std::set<std::string> folders{"/trunk", "/branches", "/branches/existing", "/tags"};
		std::string status_entry;
		std::string mixed_revision;
		bool copy_ok = true, copy_records = true, log_matches = true, switch_ok = true, list_ok = true, commit_despite_failure = false;
		/// Repository paths that commits after the working copy's revision deleted.
		std::vector<std::string> later_deletions;
		std::string printed_revision;
		/// Repository paths whose svn:externals are set.
		std::set<std::string> externals;
		std::string committed_from, committed_from_revision, committed_path;
		int committed = 0, copies = 0, switches = 0;
		std::vector<std::string> copy_args;
		std::vector<std::vector<std::string>> commands;

		Repository()
		{
			std::filesystem::create_directories(directory / ".svn");
			scope = {directory, directory, {}, true};
		}
		~Repository() { std::error_code ec; std::filesystem::remove_all(directory, ec); }

		/// `file:///repo/a%20b@7` gives `/a b`; empty for anything outside the repository.
		std::string RepositoryPath(std::string target) const
		{
			if (const auto at = target.rfind('@'); at != std::string::npos && at >= root.size()) target.erase(at);
			if (target == root) return "/";
			if (!target.starts_with(root + "/")) return {};
			std::string decoded;
			for (std::size_t i = root.size(); i < target.size(); ++i)
			{
				if (target[i] == '%' && i + 2 < target.size()) { decoded += static_cast<char>(std::stoi(target.substr(i + 1, 2), nullptr, 16)); i += 2; }
				else decoded += target[i];
			}
			return decoded;
		}

		uam::SvnBranchContext Expected() const
		{
			return {uam::paths::Utf8PathString(directory), url, root, uuid, revision, "", ""};
		}

		uam::SvnBranchRunner Runner()
		{
			// The trailing pack accepts the failure line a newer runner signature adds.
			return [this](const std::vector<std::string>& args, int timeout, std::string& output, auto&&...)
			{
				commands.push_back(args);
				// Reads stay within a probe's 10 seconds; the commit, the switch and the main line's externals get longer.
				Check(timeout > 0 && timeout <= (args[0] == "copy" || args[0] == "switch" || args[0] == "propget" ? 600000 : 10000), "command timeout is bounded");
				output.clear();
				const std::string target = args.back();
				const bool local = !target.starts_with("file:");
				if (args[0] == "info" && args[2] == "--xml")
				{
					output = "<?xml version=\"1.0\"?><info><entry kind=\"dir\" path=\"copy\" revision=\"" + revision + "\"><wc-info><depth>infinity</depth></wc-info></entry>"
					    "<entry kind=\"file\" path=\"copy/file\" revision=\"" + (mixed_revision.empty() ? revision : mixed_revision) + "\"><wc-info><depth>infinity</depth></wc-info></entry></info>";
					return true;
				}
				if (args[0] == "info")
				{
					const std::string& field = args[3];
					if (local)
					{
						if (field == "wc-root") output = uam::paths::Utf8PathString(directory);
						else if (field == "depth") output = "infinity";
						else if (field == "url") output = url;
						else if (field == "repos-root-url") output = root;
						else if (field == "repos-uuid") output = uuid;
						else if (field == "revision") output = revision;
						else return false;
						output += '\n';
						return true;
					}
					const std::string path = RepositoryPath(target);
					if (path != "/" && !folders.contains(path)) return false;
					if (field == "revision") output = std::to_string(head);
					else if (field == "repos-uuid") output = uuid;
					else if (field == "kind") output = "dir";
					else if (field == "last-changed-revision") output = path == committed_path ? std::to_string(committed) : "1";
					else return false;
					output += '\n';
					return true;
				}
				if (args[0] == "status")
				{
					output = "<?xml version=\"1.0\"?><status><target path=\"copy\">" + status_entry + "</target></status>";
					return true;
				}
				if (args[0] == "propget")
				{
					const bool set = externals.contains(local ? std::string("local") : RepositoryPath(target));
					output = set ? "<?xml version=\"1.0\"?><properties><target><property>x</property></target></properties>" : "<?xml version=\"1.0\"?><properties></properties>";
					return true;
				}
				if (args[0] == "list")
				{
					if (!list_ok) return false;
					const std::string parent = RepositoryPath(target);
					const bool xml = Has(args, "--xml");
					std::string search;
					if (const auto at = std::find(args.begin(), args.end(), "--search"); at != args.end()) search = *(at + 1);
					const auto folded = [](std::string value) { for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return value; };
					if (xml) output = "<lists><list path=\"" + target + "\">";
					for (const auto& folder : folders)
						if (folder.starts_with(parent + "/") && folder.find('/', parent.size() + 1) == std::string::npos && (search.empty() || folded(folder.substr(parent.size() + 1)) == folded(search)))
							output += xml ? "<entry kind=\"dir\"><name>" + folder.substr(parent.size() + 1) + "</name></entry>" : folder.substr(parent.size() + 1) + "/\n";
					if (xml) output += "</list></lists>";
					return true;
				}
				if (args[0] == "copy")
				{
					++copies;
					copy_args = args;
					if (!copy_ok && !commit_despite_failure) return false;
					const std::string source = args[args.size() - 2];
					committed_from = RepositoryPath(source);
					committed_from_revision = source.substr(source.rfind('@') + 1);
					committed_path = RepositoryPath(target);
					// svn nests a copy made onto an existing folder inside it.
					if (folders.contains(committed_path)) committed_path += committed_from.substr(committed_from.rfind('/'));
					if (copy_records) folders.insert(committed_path);
					committed = ++head;
					output = "Committed revision " + (printed_revision.empty() ? std::to_string(committed) : printed_revision) + ".\n";
					return copy_ok;
				}
				if (args[0] == "log" && !Has(args, "--stop-on-copy"))
				{
					Check(Has(args, "--verbose") && Has(args, revision.empty() ? "" : std::to_string(std::stoi(revision) + 1) + ":" + std::to_string(head)), "deletions are read from the commits after the working copy's revision");
					output = "<?xml version=\"1.0\"?><log><logentry revision=\"6\"><paths>";
					for (const auto& path : later_deletions) output += "<path action=\"D\" kind=\"file\">" + path + "</path>";
					output += "</paths></logentry></log>";
					return true;
				}
				if (args[0] == "log")
				{
					Check(Has(args, "--verbose") && Has(args, "--stop-on-copy") && Has(args, "1:HEAD") && Has(args, "1"), "the confirming log reads the new path's oldest revision");
					output = "<?xml version=\"1.0\"?><log><logentry\n   revision=\"" + std::to_string(committed) + "\"><paths><path\n   action=\"A\" kind=\"dir\" copyfrom-path=\"" +
					    (log_matches ? committed_from : "/elsewhere") + "\" copyfrom-rev=\"" + committed_from_revision + "\">" + committed_path + "</path></paths><msg>m</msg></logentry></log>";
					return true;
				}
				if (args[0] == "switch")
				{
					++switches;
					if (!switch_ok) return false;
					url = args[args.size() - 2];
					url.erase(url.rfind('@'));
					// svn's canonical URLs leave + and @ unescaped.
					for (const auto& [escaped, plain] : {std::pair{"%2B", "+"}, std::pair{"%40", "@"}})
						for (auto at = url.find(escaped); at != std::string::npos; at = url.find(escaped)) url.replace(at, 3, plain);
					revision = std::to_string(head);
					return true;
				}
				return false;
			};
		}

		uam::SvnBranchCreateResult Create(const std::string& name, uam::SvnBranchSource source = uam::SvnBranchSource::WorkingCopy, const std::vector<SvnRepositoryLayout>& layouts = {}, const std::string& message = "Create branch")
		{
			return uam::CreateSvnBranch(scope, Expected(), {name, source, message}, layouts, Runner());
		}
	};

	SvnRepositoryLayout Layout(std::string repository, std::string trunk, std::string branches)
	{
		SvnRepositoryLayout layout;
		layout.repository = std::move(repository);
		layout.trunk = std::move(trunk);
		layout.branches = std::move(branches);
		return layout;
	}

	void LocationCases()
	{
		const auto at = [](std::string url, const std::vector<SvnRepositoryLayout>& layouts = {})
		{
			uam::SvnBranchContext context;
			context.repository = "file:///repo";
			context.uuid = "UUID-1";
			context.url = std::move(url);
			return uam::SvnBranchLocationsFor(context, layouts);
		};
		auto found = at("file:///repo/trunk");
		Check(found.ok && found.branches == "^/branches" && found.main_line == "^/trunk", "standard layout at the repository root");
		found = at("file:///repo/shop/trunk/src");
		Check(found.ok && found.branches == "^/shop/branches" && found.main_line == "^/shop/trunk", "a project folder carries over to both locations");
		found = at("file:///repo/shop/branches/feature");
		Check(found.ok && found.branches == "^/shop/branches", "a branch working copy places siblings beside it");
		found = at("file:///repo/odd/path");
		Check(found.ok && found.branches == "^/branches" && found.main_line == "^/trunk", "an unrecognised path falls back to the repository root");
		found = at("file:///repo");
		Check(found.ok && found.branches == "^/branches", "a repository-root working copy reads the root layout");
		found = at("file:///repo/my%20project/trunk");
		Check(found.ok && found.branches == "^/my project/branches", "encoded project folders are decoded");
		const std::vector<SvnRepositoryLayout> by_root{Layout("FILE:///repo/", "lines/main", "lines/dev")};
		found = at("file:///repo/lines/main", by_root);
		Check(found.ok && found.branches == "^/lines/dev" && found.main_line == "^/lines/main", "a layout matched by root URL ignores case and trailing slash");
		const std::vector<SvnRepositoryLayout> by_uuid{Layout("uuid-1", "main", "work")};
		found = at("file:///repo/main", by_uuid);
		Check(found.ok && found.branches == "^/work" && found.main_line == "^/main", "a layout matched by UUID ignores case");
		const std::vector<SvnRepositoryLayout> other{Layout("file:///elsewhere", "main", "work")};
		Check(at("file:///repo/trunk", other).branches == "^/branches", "another repository's layout is not used");
		Check(!at("file:///other/trunk").ok, "a URL outside the repository has no locations");
		Check(!at("file:///repo/a%2").ok && !at("file:///repo/a/../b").ok && !at("file:///repo/a//b").ok, "malformed or dot-segment URLs have no locations");
	}

	void ValidationCases()
	{
		std::string error;
		for (const std::string name : {"feature-1", "release_2.0", "A"})
			Check(uam::ValidSvnBranchCreate({name, uam::SvnBranchSource::WorkingCopy, "m"}, error), "valid name " + name);
		for (const std::string& name : std::vector<std::string>{"", ".hidden", "-flag", "trailing.", "a..b", "a/b", "a b", "a@b", "a+b", "a\\b", std::string(129, 'a')})
		{
			Check(!uam::ValidSvnBranchCreate({name, uam::SvnBranchSource::WorkingCopy, "m"}, error) && !error.empty() && error.find('\n') == std::string::npos, "invalid name refused: " + name);
		}
		for (const std::string message : {"", "   ", "two\nlines", "tab\there"})
			Check(!uam::ValidSvnBranchCreate({"ok", uam::SvnBranchSource::WorkingCopy, message}, error) && error.find("message") != std::string::npos, "message must be one non-empty line");
		Repository r;
		const auto refused = r.Create("a/b");
		Check(!refused.ok && r.commands.empty(), "an invalid name never reaches svn");
	}

	void CreationCases()
	{
		{
			Repository r;
			const auto created = r.Create("feature-1");
			Check(created.ok, "working-copy creation: " + created.error);
			Check(r.copies == 1 && r.switches == 1, "one copy commit, then one switch");
			const std::vector<std::string> expected{"copy", "--non-interactive", "--force-log", "-m", "Create branch", "--", "file:///repo/trunk@5", "file:///repo/branches/feature-1"};
			Check(r.copy_args == expected, "copy is a server-side URL copy of the working copy's URL pinned at its revision, without --parents");
			Check(r.url == "file:///repo/branches/feature-1" && created.context.url == r.url && created.context.revision == "8", "the working copy is switched to the new branch at its commit");
			const auto switched = std::ranges::find_if(r.commands, [](const std::vector<std::string>& command) { return command[0] == "switch"; });
			Check(switched != r.commands.end() && switched->back() == uam::paths::Utf8PathString(r.directory) + "/", "switch destination is a literal local path, not a peg target");
			Check(created.branches.size() == 1 && created.branches[0].path == "^/branches/feature-1" && created.branches[0].current, "the new branch is reported as current");
		}
		{
			Repository r;
			const auto created = r.Create("from-trunk", uam::SvnBranchSource::MainLine);
			Check(created.ok, "main-line creation: " + created.error);
			Check(r.copy_args[r.copy_args.size() - 2] == "file:///repo/trunk@7", "the main line is pinned to the newest revision read before the copy");
		}
		{
			Repository r;
			r.url = "file:///repo/shop/trunk";
			r.folders = {"/shop/trunk", "/shop/branches"};
			const auto created = r.Create("feature");
			Check(created.ok && r.copy_args.back() == "file:///repo/shop/branches/feature", "a project folder's branches folder: " + created.error);
		}
		{
			Repository r;
			r.url = "file:///repo/lines/main";
			r.folders = {"/lines/main", "/lines/dev"};
			const auto created = r.Create("feature", uam::SvnBranchSource::MainLine, {Layout("uuid-1", "lines/main", "lines/dev")});
			Check(created.ok && r.copy_args.back() == "file:///repo/lines/dev/feature" && r.copy_args[r.copy_args.size() - 2] == "file:///repo/lines/main@7", "an overridden layout places and starts the branch: " + created.error);
		}
		{
			Repository r;
			const auto refused = r.Create("existing");
			Check(!refused.ok && r.copies == 0 && refused.error.find("already exists") != std::string::npos, "an existing target is refused before any commit");
			const auto& search = r.commands[std::find_if(r.commands.begin(), r.commands.end(), [](const auto& c) { return c[0] == "list"; }) - r.commands.begin()];
			Check(Has(search, "--search") && Has(search, "existing"), "the name check asks svn for the one name, however large the folder");
		}
		{
			Repository r;
			r.folders.erase("/branches");
			r.folders.erase("/branches/existing");
			const auto refused = r.Create("feature");
			Check(!refused.ok && r.copies == 0 && refused.error.find("^/branches") != std::string::npos && refused.error.find("Settings") != std::string::npos, "a missing branches folder is never created");
		}
		{
			Repository r;
			r.folders.erase("/trunk");
			r.url = "file:///repo/branches/existing";
			const auto refused = r.Create("feature", uam::SvnBranchSource::MainLine);
			Check(!refused.ok && r.copies == 0 && refused.error.find("^/trunk") != std::string::npos, "a missing main line is refused");
		}
		{
			Repository r;
			r.later_deletions = {"/trunk/gone"};
			const auto refused = r.Create("feature");
			Check(!refused.ok && r.copies == 0 && refused.error.find("deleted files in a later commit") != std::string::npos, "a deletion committed from this working copy is not resurrected by copying URL@revision");
			std::ofstream(r.directory / "gone") << "still here";
			Check(r.Create("feature").ok, "someone else's later deletion, still on disk here, does not refuse");
		}
		{
			Repository r;
			r.later_deletions = {"/trunk/gone"};
			Check(r.Create("feature", uam::SvnBranchSource::MainLine).ok, "the main line copies the repository's newest state regardless");
		}
		{
			Repository r;
			const auto refused = r.Create("EXISTING");
			Check(!refused.ok && r.copies == 0 && refused.error.find("already exists") != std::string::npos && refused.outcome == uam::SvnBranchCreateOutcome::Refused, "a name differing only in letter case collides and is refused");
		}
		{
			Repository r;
			r.copy_ok = false;
			r.commit_despite_failure = true;
			const auto created = r.Create("feature");
			Check(created.ok && created.outcome == uam::SvnBranchCreateOutcome::CreatedAndSwitched, "a commit the server finished after the client failed is confirmed from the log, not reported as absent: " + created.error);
		}
		{
			Repository r;
			r.printed_revision = "99";
			const auto refused = r.Create("feature");
			Check(!refused.ok && r.switches == 0 && refused.outcome == uam::SvnBranchCreateOutcome::NeedsInspection, "the log's creation must be the copy svn printed, not another commit of the same name");
		}
		{
			Check(uam::SvnCommandChangesRepository({"copy", "a", "b"}) && uam::SvnCommandChangesRepository({"switch", "x"}), "copy and switch change the repository or working copy");
			for (const char* read : {"info", "list", "log", "status", "propget"}) Check(!uam::SvnCommandChangesRepository({read}), "reads can be stopped");
			Check(!uam::SvnCommandChangesRepository({}), "an empty command changes nothing");
		}
		{
			Repository r;
			r.mixed_revision = "6";
			const auto refused = r.Create("feature");
			Check(!refused.ok && r.copies == 0 && refused.error.find("other revisions") != std::string::npos, "a mixed-revision working copy cannot be copied as URL@revision");
			Check(r.Create("feature", uam::SvnBranchSource::MainLine).ok, "the main line can still be copied from a mixed-revision working copy");
		}
		{
			Repository r;
			r.status_entry = "<entry path=\"file\"><wc-status item=\"modified\"/></entry>";
			const auto refused = r.Create("feature");
			Check(!refused.ok && r.copies == 0 && r.switches == 0, "a dirty working copy refuses before the commit, since the switch would refuse after it");
		}
		{
			Repository r;
			auto expected = r.Expected();
			expected.revision = "4";
			const auto refused = uam::CreateSvnBranch(r.scope, expected, {"feature", uam::SvnBranchSource::WorkingCopy, "m"}, {}, r.Runner());
			Check(!refused.ok && r.copies == 0 && refused.error.find("changed") != std::string::npos, "a working copy that moved since the picker listed it is refused");
		}
		{
			Repository r;
			r.scope.other_workspaces.push_back(r.directory);
			Check(!r.Create("feature").ok && r.copies == 0, "a working copy another chat uses is refused");
		}
		{
			Repository r;
			r.url = r.root;
			r.folders.insert("/");
			const auto refused = r.Create("feature");
			Check(!refused.ok && r.copies == 0 && refused.error.find("root") != std::string::npos, "the repository root is never copied into its own branches folder");
		}
		{
			Repository r;
			r.copy_ok = false;
			const auto refused = r.Create("feature");
			Check(!refused.ok && r.switches == 0 && refused.error.find("did not create") != std::string::npos && refused.error.find("commit message") != std::string::npos, "a refused copy that left nothing behind says so");
			Check(refused.outcome == uam::SvnBranchCreateOutcome::NotCreated, "a confirmed-absent branch is NotCreated");
		}
		{
			Repository r;
			r.copy_ok = false;
			// The target appears only after the existence check: the copy failed, yet the folder is there now.
			int checks = 0;
			const uam::SvnBranchRunner base = r.Runner();
			const uam::SvnBranchRunner racing = [&](const std::vector<std::string>& args, int timeout, std::string& output, auto&&... rest)
			{
				if (args[0] == "copy") { ++checks; r.folders.insert("/branches/feature"); }
				return base(args, timeout, output, rest...);
			};
			const auto refused = uam::CreateSvnBranch(r.scope, r.Expected(), {"feature", uam::SvnBranchSource::WorkingCopy, "m"}, {}, racing);
			Check(checks == 1 && !refused.ok && refused.error.find("Inspect the repository") != std::string::npos && r.switches == 0, "an unconfirmed copy whose target now exists needs inspection");
		}
		{
			Repository r;
			r.log_matches = false;
			const auto refused = r.Create("feature");
			Check(!refused.ok && r.switches == 0 && refused.error.find("does not confirm") != std::string::npos, "a commit the log does not confirm is never switched to");
		}
		{
			Repository r;
			r.url = "file:///repo/p+q/trunk";
			r.folders = {"/p+q/trunk", "/p+q/branches"};
			const auto created = r.Create("feature");
			Check(created.ok && r.url == "file:///repo/p+q/branches/feature", "svn's unescaped + in the switched URL still confirms the switch: " + created.error);
		}
		{
			Repository r;
			r.externals.insert("/trunk");
			const auto refused = r.Create("feature", uam::SvnBranchSource::MainLine);
			Check(!refused.ok && r.copies == 0 && refused.error.find("external") != std::string::npos, "externals in the source refuse before the commit, since the switch would refuse after it");
		}
		{
			Repository r;
			for (int i = 0; i < 300; ++i) r.folders.insert("/branches/b" + std::to_string(i));
			const auto created = r.Create("feature");
			Check(created.ok && r.switches == 1, "a branches folder past the folder-selection listing limit still creates and switches: " + created.error);
		}
		{
			Repository r;
			r.list_ok = false;
			const auto refused = r.Create("feature");
			Check(!refused.ok && r.copies == 0 && refused.error.find("Could not check whether") != std::string::npos, "an unreadable branches folder never counts as the name being free");
		}
		{
			Repository r;
			const uam::SvnBranchRunner base = r.Runner();
			// Another creation of the same name lands between the check and the copy, so svn nests the copy inside it.
			const uam::SvnBranchRunner racing = [&](const std::vector<std::string>& args, int timeout, std::string& output, auto&&... rest)
			{
				if (args[0] == "copy") r.folders.insert("/branches/feature");
				return base(args, timeout, output, rest...);
			};
			const auto refused = uam::CreateSvnBranch(r.scope, r.Expected(), {"feature", uam::SvnBranchSource::WorkingCopy, "m"}, {}, racing);
			Check(!refused.ok && r.switches == 0 && refused.error.find("went inside it as ^/branches/feature/trunk") != std::string::npos, "a nested copy is reported as what happened: " + refused.error);
		}
		{
			Repository r;
			r.switch_ok = false;
			const auto refused = r.Create("feature");
			Check(!refused.ok && r.copies == 1 && refused.error.starts_with("Created ^/branches/feature r8 from ^/trunk r5, but the working copy was not switched."), "a failed switch after creation names the created branch: " + refused.error);
			Check(refused.branches.size() == 1 && !refused.branches[0].current && refused.outcome == uam::SvnBranchCreateOutcome::CreatedNotSwitched, "a committed but unswitched branch is reported as created, not current");
		}
	}

	/// Against the disposable repository from tests/svn_branch_create_integration.py: `mode` is `working-copy`, `main-line`
	/// or `existing`; prints the new branch's context for the script to check.
	void RealRepository(const std::filesystem::path& directory, const std::string& config, const std::string& mode, const std::string& name)
	{
		Check(directory.filename() == "copy @literal" && directory.parent_path().filename().string().starts_with("uam-svn-create-") && uam::paths::PathFromUtf8(config) == directory.parent_path() / "config", "integration requires the disposable harness working copy and config");
		uam::SvnBranchScope scope{directory, directory, {}, true};
		const uam::SvnBranchRunner run = [&](const std::vector<std::string>& args, int, std::string& output, auto&&...)
		{
			std::vector<std::string> command{"svn", "--config-dir", config, "--no-auth-cache"};
			command.insert(command.end(), args.begin(), args.end());
#ifdef _WIN32
			FILE* pipe = _popen(("\"" + uam::shell::JoinEscapedArgs(command) + "\"").c_str(), "r");
#else
			FILE* pipe = popen((uam::shell::JoinEscapedArgs(command) + " 2>/dev/null").c_str(), "r");
#endif
			if (!pipe) return false;
			char buffer[4096]; output.clear(); while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
#ifdef _WIN32
			return _pclose(pipe) == 0;
#else
			return pclose(pipe) == 0;
#endif
		};
		const auto listed = uam::DiscoverSvnBranches(scope, "", run);
		Check(listed.ok, "real SVN working copy: " + listed.error);
		const auto source = mode == "main-line" ? uam::SvnBranchSource::MainLine : uam::SvnBranchSource::WorkingCopy;
		const auto created = uam::CreateSvnBranch(scope, listed.context, {name, source, "UAM integration: create " + name}, {}, run);
		if (mode.starts_with("refuse:"))
		{
			Check(!created.ok && created.outcome == uam::SvnBranchCreateOutcome::Refused && created.error.find(mode.substr(7)) != std::string::npos, "real refusal: " + created.error);
			std::cout << "PASS: disposable real SVN refuses (" << mode.substr(7) << ") without committing\n";
			return;
		}
		Check(created.ok, "real SVN branch creation: " + created.error);
		std::cout << "CREATED " << created.context.url << ' ' << created.context.revision << '\n';
	}
}

int main(int argc, char** argv)
{
	try
	{
		if (argc == 6 && std::string(argv[1]) == "--integration") RealRepository(uam::paths::PathFromUtf8(argv[2]), argv[3], argv[4], argv[5]);
		else
		{
			LocationCases();
			ValidationCases();
			CreationCases();
			std::cout << "PASS: SVN branch creation locations, validation, pinned copy, refusals and the guarded switch\n";
		}
	}
	catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
	return 0;
}
