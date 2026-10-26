#include "app/svn_branch_service.h"
#include "common/models/app_models.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_lease.h"
#include "common/utils/shell_escape.h"
#include "cef/branch_operation_dispatch.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
	void Check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
	void WorkspaceRemapLeaseCase()
	{
		uam::WorkspaceLeaseGate gate;
		auto exclusive = gate.TryAcquireCleanup();
		Check(static_cast<bool>(exclusive), "switch owns an exclusive workspace lease");
		bool remapped = false;
		// Same dispatch guard as the CEF router; the directory mutation is injected.
		const auto dispatch = [&](std::string_view action)
		{
			std::shared_ptr<uam::WorkspaceLeaseGate::Lease> lease;
			if (uam::RequiresWorkspaceMutationLease(action))
			{
				lease = gate.TryAcquireMutation();
				if (!lease) return false;
			}
			remapped = true;
			return true;
		};
		Check(!dispatch("createSession") && !remapped, "new workspace users wait for the switch lease");
		Check(!dispatch("renameFolder") && !remapped, "folder remapping cannot add a foreign working-copy user during switch");
		exclusive.reset();
		Check(dispatch("renameFolder") && remapped, "folder remapping resumes after the switch lease releases");
	}
	void BranchDispatchFailureCases()
	{
		for (int failed_stage : {1, 2})
		{
			uam::WorkspaceLeaseGate gate;
			std::unordered_set<std::string> operations;
			int responses = 0, switches = 0;
			const auto attempt = [&](int failure)
			{
				if (operations.contains("chat")) return false;
				auto lease = gate.TryAcquireCleanup();
				if (!lease) return false;
				operations.insert("chat");
				const auto post = [&](int stage)
				{
					if (failure == stage) { ++responses; return false; }
					return true;
				};
				if (!uam::query_handler_async::DispatchBranchOperation(operations, "chat", [&]() { return post(1); })) return false;
				if (!uam::query_handler_async::DispatchBranchOperation(operations, "chat", [&]() { return post(2); })) return false;
				++switches;
				operations.erase("chat");
				return true;
			};
			Check(!attempt(failed_stage), "injected first/second branch post failure returns failure");
			Check(responses == 1 && switches == 0, "dispatcher sends only its single failure, no switch command");
			Check(operations.empty(), "failed branch dispatch clears the chat operation marker");
			{ auto next_lease = gate.TryAcquireCleanup(); Check(static_cast<bool>(next_lease), "failed request releases its existing exclusive lease"); }
			Check(attempt(0) && switches == 1 && operations.empty(), "later request remains eligible after either post failure");
		}
	}
	struct Fixture
	{
		std::filesystem::path directory = std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("uam-svn-branch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		uam::SvnBranchScope scope;
		std::string url = "file:///private/repository/lines/main", revision = "2", head = "3", uuid = "same-repository";
		std::string listing = "<lists><list path=\"file:///private/repository/lines\"><entry kind=\"dir\"><name>main</name></entry><entry kind=\"dir\"><name>feature &amp; fix</name></entry><entry kind=\"file\"><name>readme</name></entry></list></lists>";
		std::string status;
		std::string inventory_depth = "infinity";
		std::string inventory_xml;
		int inventory_reads = 0, sparse_inventory_read = 0;
		bool externals = false;
		bool switch_ok = true, verify_switch = true;
		int switches = 0, reads = 0;
		std::vector<std::string> switch_args;
		Fixture()
		{
			std::filesystem::create_directories(directory / ".svn");
			scope = {directory, directory, {}, true};
			status = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><status><target path=\"working-copy\"></target></status>";
		}
		~Fixture() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
		uam::SvnBranchRunner Runner()
		{
			return [this](const std::vector<std::string>& args, int timeout, std::string& output)
			{
				++reads;
				Check(timeout > 0 && timeout <= 10000, "command timeout is bounded");
				if (args[0] == "info")
				{
					if (args[2] == "--xml")
					{
						++inventory_reads;
						const auto depth = sparse_inventory_read == inventory_reads ? "empty" : inventory_depth;
						output = inventory_xml.empty() ? "<?xml version=\"1.0\"?><info><entry kind=\"dir\" path=\"copy\"><wc-info><depth>infinity</depth></wc-info></entry><entry kind=\"dir\" path=\"copy/child\"><wc-info><depth>" + depth + "</depth></wc-info></entry></info>" : inventory_xml;
						return true;
					}
					const auto& field = args[3];
					if (field == "wc-root") output = uam::paths::Utf8PathString(directory);
					else if (field == "depth") output = "infinity";
					else if (field == "url") output = url;
					else if (field == "repos-root-url") output = "file:///private/repository";
					else if (field == "repos-uuid") output = uuid;
					else if (field == "kind") output = "dir";
					else if (field == "revision") output = args.back().starts_with("file:") ? head : revision;
					else return false;
					output += '\n';
					return true;
				}
				if (args[0] == "list") { output = listing; return true; }
				if (args[0] == "status") { output = status; return true; }
				if (args[0] == "propget") { output = externals ? "<?xml version=\"1.0\"?><properties><target><property>external</property></target></properties>" : "<?xml version=\"1.0\"?><properties></properties>"; return true; }
				if (args[0] == "switch")
				{
					++switches; switch_args = args;
					Check(std::find(args.begin(), args.end(), "--force") == args.end(), "no force");
					Check(std::find(args.begin(), args.end(), "--ignore-ancestry") == args.end(), "ancestry is required");
					if (switch_ok && verify_switch) { url = args[args.size() - 2]; url.erase(url.rfind('@')); revision = head; }
					return switch_ok;
				}
				return false;
			};
		}
	};
	void SourceOwnershipCases()
	{
		for (int field = 0; field < 4; ++field)
		for (int relationship = 0; relationship < 4; ++relationship)
		{
			Fixture f;
			ChatSession selected; selected.id = "selected"; selected.folder_id = "folder"; selected.workspace_directory = uam::paths::Utf8PathString(f.directory);
			ChatFolder folder; folder.id = "folder"; folder.directory = selected.workspace_directory;
			ChatSession other; other.id = "snapshot"; other.workspace_worktree_directory = uam::paths::Utf8PathString(f.directory.parent_path() / "separate-snapshot");
			const auto source = relationship == 0 ? f.directory : relationship == 1 ? f.directory.parent_path() : relationship == 2 ? f.directory / "child" : f.directory.parent_path() / "unrelated-source";
			ChatFolder fallback; fallback.id = "other-folder"; fallback.directory = uam::paths::Utf8PathString(source);
			if (field == 0) other.workspace_source_directory = fallback.directory;
			else if (field == 1) other.workspace_directory = fallback.directory;
			else if (field == 2) other.workspace_worktree_directory = fallback.directory;
			else { other.workspace_worktree_directory.clear(); other.folder_id = fallback.id; }
			f.scope = uam::BuildSvnBranchScope(true, selected, {folder, fallback}, {selected, other});
			const auto found = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			if (relationship < 3) Check(!found.ok && f.reads == 0 && f.switches == 0, "real scope builder refuses another snapshot's equal/ancestor/descendant source before any SVN operation");
			else Check(found.ok, "unrelated snapshot source remains eligible");
		}
		{
			Fixture f; ChatSession selected; selected.id = "selected"; selected.folder_id = "folder";
			ChatFolder folder; folder.id = "folder"; folder.directory = uam::paths::Utf8PathString(f.directory);
			ChatSession remote; remote.id = "remote"; remote.execution_host_id = "remote"; remote.workspace_source_directory = folder.directory;
			const auto scope = uam::BuildSvnBranchScope(true, selected, {folder}, {selected, remote});
			Check(uam::DiscoverSvnBranches(scope, "^/lines", f.Runner()).ok, "same path on a remote host is a different workspace owner");
		}
	}
	void SparseDescendantCases()
	{
		for (const auto& depth : {"empty", "files", "immediates", "exclude"})
		{
			Fixture f; const auto found = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			Check(found.ok, "full copy initially discoverable");
			f.inventory_depth = depth;
			Check(!uam::SwitchSvnBranch(f.scope, found.context, "^/lines/feature & fix", f.Runner()).ok && f.switches == 0, "sparse/excluded descendant refuses without switch or expansion");
		}
		for (const auto& xml : {"<?xml version=\"1.0\"?><info></info>", "<?xml version=\"1.0\"?><info><entry kind=\"dir\"><wc-info></wc-info></entry></info>", "<!DOCTYPE info><info></info>", "<?xml version=\"1.0\"?><info><entry><wc-info><depth>infinity</depth></wc-info></info>"})
		{
			Fixture f; f.inventory_xml = xml;
			Check(!uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner()).ok && f.switches == 0, "unverified/malformed inventory fails closed");
		}
		{
			Fixture f; f.inventory_xml = std::string(128 * 1024 + 1, 'x');
			Check(!uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner()).ok && f.switches == 0, "inventory output bound refuses without mutation");
		}
		for (int changed_read : {2, 3})
		{
			Fixture f; const auto found = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			f.sparse_inventory_read = f.inventory_reads + changed_read;
			const auto result = uam::SwitchSvnBranch(f.scope, found.context, "^/lines/feature & fix", f.Runner());
			Check(!result.ok && f.switches == (changed_read == 2 ? 0 : 1), "inventory rechecked before mutation and before confirmed success");
			if (changed_read == 3) Check(result.error.find("inspection") != std::string::npos, "post-switch sparsity remains uncertain and requires inspection");
		}
	}
	void InjectedCases()
	{
		BranchDispatchFailureCases();
		SourceOwnershipCases();
		SparseDescendantCases();
		WorkspaceRemapLeaseCase();
		{
			Fixture f; const auto result = uam::DiscoverSvnBranches(f.scope, "", f.Runner());
			Check(result.ok && result.error.empty() && result.branches.empty() && result.context.parent.empty(), "no implicit layout discovery");
			Check(f.switches == 0, "discovery never switches");
		}
		{
			Fixture f; const auto result = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			Check(result.ok && result.branches.size() == 2 && result.branches[0].current, "explicit parent directories");
			Check(result.branches[1].path == "^/lines/feature & fix", "decoded name retains path identity");
			const auto switched = uam::SwitchSvnBranch(f.scope, result.context, result.branches[1].path, f.Runner());
			Check(switched.ok && f.switches == 1 && switched.context.revision == "3", "verified pinned switch");
			Check(f.url.ends_with("feature%20%26%20fix"), "URL encoding is per path component");
			Check(f.switch_args[f.switch_args.size() - 2].ends_with("@3"), "target peg revision is pinned with its operative revision");
			Check(f.switch_args.back() == uam::paths::Utf8PathString(f.directory), "switch destination is a literal local path, not a peg target");
		}
		for (const std::string& path : {"branches", "^/../lines", "^/lines/./bad", "^/lines//bad", "^/lines\\bad", "^/lines\n"})
		{
			Fixture f; Check(!uam::DiscoverSvnBranches(f.scope, path, f.Runner()).ok && f.switches == 0, "unsafe mapping refused");
		}
		{
			Fixture f; Check(uam::DiscoverSvnBranches(f.scope, "^/", f.Runner()).ok, "explicit repository-root mapping supported");
		}
		for (const auto& entry : {"modified", "unversioned", "ignored", "conflicted", "external", "switched"})
		{
			Fixture f; const auto result = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			f.status = "<?xml version=\"1.0\"?><status><target path=\"copy\"><entry path=\"file\"><wc-status item=\"" + std::string(entry) + "\"/></entry></target></status>";
			Check(!uam::SwitchSvnBranch(f.scope, result.context, result.branches[1].path, f.Runner()).ok && f.switches == 0, "dirty and foreign subtree refusal");
		}
		{
			Fixture f; const auto result = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			f.scope.other_workspaces.push_back(f.directory / "child");
			Check(!uam::SwitchSvnBranch(f.scope, result.context, result.branches[1].path, f.Runner()).ok && f.switches == 0, "foreign chat refusal");
		}
		for (int change = 0; change < 4; ++change)
		{
			Fixture f; const auto result = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			if (change == 0) f.url += "-changed"; else if (change == 1) f.revision = "5"; else if (change == 2) f.head = "6"; else f.uuid = "foreign";
			Check(!uam::SwitchSvnBranch(f.scope, result.context, result.branches[1].path, f.Runner()).ok && f.switches == 0, "stale URL/revision/head/UUID refused");
		}
		for (const auto& xml : {"<lists><entry kind=\"dir\"><name>../bad</name></entry></lists>", "<lists><entry kind=\"dir\"><name>&unknown;</name></entry></lists>", "<!DOCTYPE bad><lists></lists>", "<lists><entry kind=\"dir\"><name>broken</lists>"})
		{
			Fixture f; f.listing = xml; Check(!uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner()).ok, "malformed listing refused");
		}
		{
			Fixture f; f.listing = std::string(128 * 1024 + 1, 'x'); Check(!uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner()).ok, "output bound");
		}
		for (bool failure : {false, true})
		{
			Fixture f; const auto result = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			f.switch_ok = failure; f.verify_switch = false;
			Check(!uam::SwitchSvnBranch(f.scope, result.context, result.branches[1].path, f.Runner()).ok && f.switches == 1, "failed/unconfirmed switch does not report success or rollback");
		}
		{
			Fixture f; f.scope.editable = false; Check(!uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner()).ok && f.reads == 0, "ineligible scope never probes");
		}
		{
			Fixture f; const auto result = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner()); f.externals = true;
			Check(!uam::SwitchSvnBranch(f.scope, result.context, result.branches[1].path, f.Runner()).ok && f.switches == 0, "external properties refuse before mutation");
		}
		std::cout << "PASS: original SVN scenarios, real scope-builder ownership, descendant inventory and folder-remap lease regressions\n";
	}
	void RealWorkingCopy(const std::filesystem::path& directory, const std::string& config, bool sparse = false)
	{
		Check(directory.filename() == "copy @literal" && directory.parent_path().filename().string().starts_with("uam-svn-composer-") && uam::paths::PathFromUtf8(config) == directory.parent_path() / "config", "integration requires the disposable harness working copy and config");
		uam::SvnBranchScope scope{directory, directory, {}, true};
		int switches = 0;
		const uam::SvnBranchRunner run = [&](const std::vector<std::string>& args, int, std::string& output)
		{
			if (args[0] == "switch") ++switches;
			std::vector<std::string> command{"svn", "--config-dir", config, "--no-auth-cache"};
			command.insert(command.end(), args.begin(), args.end());
#ifdef _WIN32
			FILE* pipe = _popen(uam::shell::JoinEscapedArgs(command).c_str(), "r");
#else
			FILE* pipe = popen(uam::shell::JoinEscapedArgs(command).c_str(), "r");
#endif
			if (!pipe) return false;
			char buffer[4096]; output.clear(); while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
#ifdef _WIN32
			return _pclose(pipe) == 0;
#else
			return pclose(pipe) == 0;
#endif
		};
		auto found = uam::DiscoverSvnBranches(scope, "^/lines", run);
		if (sparse)
		{
			const auto attempted = uam::SwitchSvnBranch(scope, found.context, "^/lines/feature a", run);
			Check(!found.ok && !attempted.ok && switches == 0, "real sparse/excluded child refuses discovery and switch without mutation");
			std::cout << "PASS: disposable real SVN sparse/excluded descendant refusal without switch\n";
			return;
		}
		Check(found.ok && found.branches.size() == 2, "real SVN discovery: " + found.error);
		const auto choice = std::find_if(found.branches.begin(), found.branches.end(), [](const auto& c) { return c.name == "feature a"; });
		Check(choice != found.branches.end(), "real SVN encoded path choice");
		auto changed = uam::SwitchSvnBranch(scope, found.context, choice->path, run);
		Check(changed.ok && changed.context.url.ends_with("feature%20a"), "real SVN switch: " + changed.error);
		std::ifstream selected_file(directory / "tracked.txt");
		const std::string selected_text((std::istreambuf_iterator<char>(selected_file)), std::istreambuf_iterator<char>());
		Check(selected_text == "branch content\n", "real branch files match the explicitly selected target");
		found = uam::DiscoverSvnBranches(scope, "^/lines", run);
		std::ofstream(directory / "untracked.txt") << "retained user work";
		Check(!uam::SwitchSvnBranch(scope, found.context, "^/lines/main", run).ok, "real untracked refusal");
		Check(std::filesystem::exists(directory / "untracked.txt"), "real user file preserved");
		std::cout << "PASS: disposable real SVN discovery, encoded-path switch, selected file content and untracked-file preservation\n";
	}
}
int main(int argc, char** argv)
{
	try
	{
		if (argc == 4 && std::string(argv[1]) == "--integration") RealWorkingCopy(uam::paths::PathFromUtf8(argv[2]), argv[3]);
		else if (argc == 4 && std::string(argv[1]) == "--sparse-integration") RealWorkingCopy(uam::paths::PathFromUtf8(argv[2]), argv[3], true);
		else if (argc == 2 && std::string(argv[1]) == "--ownership-regression") SourceOwnershipCases();
		else if (argc == 2 && std::string(argv[1]) == "--depth-regression") SparseDescendantCases();
		else if (argc == 2 && std::string(argv[1]) == "--dispatch-regression") BranchDispatchFailureCases();
		else InjectedCases();
	}
	catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
	return 0;
}
