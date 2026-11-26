#include "app/svn_branch_service.h"
#include "app/svn_failure.h"
#include "common/models/app_models.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_lease.h"
#include "common/utils/shell_escape.h"
#include "common/vcs/svn_environment.h"
#include "cef/branch_operation_dispatch.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#ifndef _WIN32
#include <sys/wait.h>
#endif

// This isolated program does not link the diagnostic writer; the stub keeps what SvnFailureMessage logs for the checks.
namespace uam::diagnostics
{
	std::string g_log;
	void Write(std::string_view message) { g_log.append(message).push_back('\n'); }
}

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
		/// Set to make one command fail the way RunSvn reports it: no output, and svn's reason as the failure line.
		std::function<bool(const std::vector<std::string>&)> fails;
		std::string failure_text;
		Fixture()
		{
			std::filesystem::create_directories(directory / ".svn");
			scope = {directory, directory, {}, true};
			status = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><status><target path=\"working-copy\"></target></status>";
		}
		~Fixture() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
		uam::SvnBranchRunner Runner()
		{
			return [this](const std::vector<std::string>& args, int timeout, std::string& output, std::string& failure)
			{
				++reads;
				Check(timeout > 0 && timeout <= 10000, "command timeout is bounded");
				if (fails && fails(args)) { failure = failure_text; return false; }
				if (args[0] == "info")
				{
					if (args[2] == "--xml")
					{
						++inventory_reads;
						const auto depth = sparse_inventory_read == inventory_reads ? "empty" : inventory_depth;
						output = inventory_xml.empty() ? "<?xml version=\"1.0\"?><info><entry kind=\"dir\" path=\"copy\"><wc-info><wcroot-abspath>" + uam::paths::Utf8PathString(directory) + "</wcroot-abspath><depth>infinity</depth></wc-info></entry><entry kind=\"dir\" path=\"copy/child\"><wc-info><depth>" + depth + "</depth></wc-info></entry></info>" : inventory_xml;
						return true;
					}
					const auto& field = args[3];
					if (field == "depth") output = "infinity";
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
	/// svn's own reason reaches the person instead of a bare "could not": the service adds the runner's failure line.
	void FailureCarryThroughCases()
	{
		const std::string credentials = "SVN needs your credentials. Run svn info svn://repository.invalid/lines once in a terminal to save them, then retry.";
		{
			Fixture f; f.failure_text = "E155007: '/private/copy' is not a working copy";
			f.fails = [](const std::vector<std::string>& args) { return args[0] == "info" && args[3] == "depth"; };
			const auto result = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			Check(!result.ok && result.error == "Could not verify the full SVN working-copy root. E155007: '/private/copy' is not a working copy.", "working-copy read failure carries svn's reason: " + result.error);
		}
		{
			Fixture f; f.failure_text = credentials;
			f.fails = [](const std::vector<std::string>& args) { return args[0] == "info" && args.back().starts_with("file:"); };
			const auto result = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			Check(!result.ok && result.error == "Could not discover directories in that SVN repository folder. " + credentials, "repository discovery carries the credentials message: " + result.error);
		}
		{
			Fixture f; const auto found = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			f.failure_text = "E155004: Run 'svn cleanup' to remove locks (type 'svn help cleanup' for details)";
			f.fails = [](const std::vector<std::string>& args) { return args[0] == "status"; };
			const auto result = uam::SwitchSvnBranch(f.scope, found.context, found.branches[1].path, f.Runner());
			Check(!result.ok && f.switches == 0 && result.error.starts_with("Could not read the working copy's status. E155004: Run 'svn cleanup'"), "a failed status read is not reported as a dirty working copy: " + result.error);
		}
		{
			Fixture f; const auto found = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			f.failure_text = credentials;
			f.fails = [](const std::vector<std::string>& args) { return args[0] == "propget"; };
			const auto result = uam::SwitchSvnBranch(f.scope, found.context, found.branches[1].path, f.Runner());
			Check(!result.ok && f.switches == 0 && result.error == "Could not check SVN external definitions. " + credentials, "a failed externals read is not reported as externals: " + result.error);
		}
		{
			Fixture f; const auto found = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			f.failure_text = "E155004: Run 'svn cleanup' to remove locks (type 'svn help cleanup' for details)";
			f.fails = [](const std::vector<std::string>& args) { return args[0] == "switch"; };
			const auto result = uam::SwitchSvnBranch(f.scope, found.context, found.branches[1].path, f.Runner());
			Check(!result.ok && result.error == "SVN did not confirm switching. E155004: Run 'svn cleanup' to remove locks (type 'svn help cleanup' for details). Inspect the working copy before retrying; no automatic rollback was attempted.", "switch failure keeps the inspection warning and adds svn's reason: " + result.error);
			Check(result.error.find('\n') == std::string::npos, "one line");
		}
		{
			Fixture f; f.fails = [](const std::vector<std::string>& args) { return args[0] == "list"; };
			const auto result = uam::DiscoverSvnBranches(f.scope, "^/lines", f.Runner());
			Check(!result.ok && result.error == "Could not discover directories in that SVN repository folder.", "a failure without a reason keeps the service's sentence");
		}
		std::cout << "PASS: SVN failure reasons carried into discovery and switch refusals\n";
	}
	/// The log keeps svn's own error lines only: no command output, workspace paths shortened, URL user names removed,
	/// and one entry per operation and code at a time.
	void LogSanitisingCases()
	{
		uam::diagnostics::g_log.clear();
		ProcessExecutionResult limited;
		limited.exit_code = 1;
		limited.output = "Index: secret.c\n+password = hunter2\n";
		limited.error = "Command output exceeded the 4 MiB safety limit.";
		uam::LogSvnFailure(limited, "diff", "/w");
		Check(uam::diagnostics::g_log.find("exceeded") != std::string::npos && uam::diagnostics::g_log.find("hunter2") == std::string::npos && uam::diagnostics::g_log.find("Index:") == std::string::npos, "an output-limited failure logs UAM's reason, never the output: " + uam::diagnostics::g_log);
		uam::diagnostics::g_log.clear();
		ProcessExecutionResult refused;
		refused.exit_code = 1;
		refused.output = "Sending        sub/file.txt\nsvn: E170013: Unable to connect to a repository at URL 'svn+ssh://alice@repository.invalid/r'\nsvn: E000013: Can't open file '/w/sub/.svn/wc.db': Permission denied\n";
		uam::LogSvnFailure(refused, "commit", "/w");
		const std::string first = uam::diagnostics::g_log;
		Check(first.find("svn+ssh://repository.invalid/r") != std::string::npos && first.find("alice") == std::string::npos, "URL user names are removed: " + first);
		Check(first.find("'sub/.svn/wc.db'") != std::string::npos && first.find("/w/") == std::string::npos && first.find("Sending") == std::string::npos, "workspace paths are relative and output lines are left out: " + first);
		uam::LogSvnFailure(refused, "commit", "/w");
		Check(uam::diagnostics::g_log == first, "the same failure is logged once in five minutes");
		uam::LogSvnFailure(refused, "update", "/w");
		Check(uam::diagnostics::g_log.size() > first.size(), "another operation's failure is logged");
		ProcessExecutionResult canceled = refused;
		canceled.canceled = true;
		const std::size_t before = uam::diagnostics::g_log.size();
		uam::LogSvnFailure(canceled, "status", "/w");
		Check(uam::diagnostics::g_log.size() == before, "a stop is not a failure");
		uam::diagnostics::g_log.clear();
		ProcessExecutionResult quoted;
		quoted.exit_code = 1;
		quoted.output = "+note: svn: E170001: Can't get password; token=sk-fixture\nsvn: E000013: Can't open file '/w2/x': Permission denied\n";
		uam::LogSvnFailure(quoted, "diff", "/w2");
		Check(uam::diagnostics::g_log.find("sk-fixture") == std::string::npos && uam::diagnostics::g_log.find("E000013") != std::string::npos, "svn's prefix inside content is not logged: " + uam::diagnostics::g_log);
#ifndef _WIN32
		uam::diagnostics::g_log.clear();
		const char* previous_home = std::getenv("HOME");
		const std::string saved_home = previous_home != nullptr ? previous_home : "";
		setenv("HOME", "/home/fixture/", 1);
		ProcessExecutionResult homed;
		homed.exit_code = 1;
		homed.output = "svn: E000013: Can't open file '/home/fixture/other/x' or '/home/fixturebeta/y': Permission denied\n";
		uam::LogSvnFailure(homed, "status", "/w3");
		if (previous_home != nullptr) setenv("HOME", saved_home.c_str(), 1); else unsetenv("HOME");
		Check(uam::diagnostics::g_log.find("'~/other/x'") != std::string::npos && uam::diagnostics::g_log.find("'/home/fixturebeta/y'") != std::string::npos, "only the whole home folder is shortened: " + uam::diagnostics::g_log);
#endif
		std::cout << "PASS: SVN failure log keeps svn's lines only, sanitised and rate limited\n";
	}
	void InjectedCases()
	{
		LogSanitisingCases();
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
			Check(f.switch_args.back() == uam::paths::Utf8PathString(f.directory) + "/", "switch destination is a literal local path, not a peg target");
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
			const auto refused = uam::SwitchSvnBranch(f.scope, result.context, result.branches[1].path, f.Runner());
			Check(!refused.ok && f.switches == 0, "external properties refuse before mutation");
			Check(refused.error == "Resolve SVN external definitions before switching folders.", "a refusal read from svn's output keeps its own wording");
		}
		FailureCarryThroughCases();
		std::cout << "PASS: original SVN scenarios, real scope-builder ownership, descendant inventory and folder-remap lease regressions\n";
	}
	/// Real svn with the app's SVN environment (unless `shared_environment` is false, as a launch without a locale would be) and
	/// the app's failure message. The test shell form stands in for RunSvn's argv launch; the variables are the same.
	uam::SvnBranchRunner RealRunner(const std::string& config, bool shared_environment, int* switches = nullptr)
	{
		return [=](const std::vector<std::string>& args, int, std::string& output, std::string& failure)
		{
			if (switches != nullptr && args[0] == "switch") ++*switches;
			std::vector<std::string> command{"svn", "--config-dir", config, "--no-auth-cache"};
			command.insert(command.end(), args.begin(), args.end());
			std::string line;
#ifdef _WIN32
			(void)shared_environment;
			// _popen runs `cmd /c` without /S: a line starting with a quote and holding more than two loses its first and
			// last quote. One extra outer pair is what cmd strips, leaving every argument's own quotes intact. svn's errors
			// are on stderr, which _popen does not capture, so they join the output as they do through popen below.
			line = "\"" + uam::shell::JoinEscapedArgs(command) + " 2>&1\"";
			FILE* pipe = _popen(line.c_str(), "r");
#else
			if (shared_environment)
				for (const auto& [variable, value] : uam::vcs::SvnEnvironment({})) line += variable + "=" + uam::shell::EscapeArg(value) + " ";
			line += uam::shell::JoinEscapedArgs(command) + " 2>&1";
			FILE* pipe = popen(line.c_str(), "r");
#endif
			if (!pipe) return false;
			char buffer[4096]; output.clear(); while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
			ProcessExecutionResult result;
#ifdef _WIN32
			result.exit_code = _pclose(pipe);
#else
			const int status = pclose(pipe);
			result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
			result.ok = result.exit_code == 0;
			result.output = output;
			failure = result.ok ? std::string{} : uam::SvnFailureMessage(result, {});
			if (!result.ok) uam::LogSvnFailure(result, args[0]);
			return result.ok;
		};
	}
	/// Without the shared environment a non-ASCII working copy cannot even be read, and svn's reason reaches the refusal.
	void LocaleOffWorkingCopy(const std::filesystem::path& directory, const std::string& config)
	{
		uam::SvnBranchScope scope{directory, directory, {}, true};
		const auto found = uam::DiscoverSvnBranches(scope, "^/lines", RealRunner(config, false));
		Check(!found.ok && found.error.starts_with("Could not verify the full SVN working-copy root. E"), "without a UTF-8 locale the read fails with svn's code: " + found.error);
		std::cout << "PASS: control run without the shared SVN environment fails on the non-ASCII working copy: " << found.error << "\n";
	}
	/// A repository that refuses anonymous access: the person is told to save credentials, svn's chain stays in the log.
	void CredentialsWorkingCopy(const std::filesystem::path& directory, const std::string& config)
	{
		uam::SvnBranchScope scope{directory, directory, {}, true};
		const auto found = uam::DiscoverSvnBranches(scope, "^/lines", RealRunner(config, true));
		Check(!found.ok && found.error.starts_with("Could not discover directories in that SVN repository folder. SVN needs your credentials. Run svn info svn://") && found.error.ends_with(" once in a terminal to save them, then retry."), "credentials message: " + found.error);
		Check(found.error.find("E170013") == std::string::npos && found.error.find('\n') == std::string::npos, "one line without the connection wrapper");
		Check(uam::diagnostics::g_log.find("E170001") != std::string::npos, "svn's own lines go to the log");
		std::cout << "PASS: real svnserve without anonymous access asks for credentials: " << found.error << "\n";
	}
	/// svn+ssh through the shared environment: ssh runs in batch mode and its refusal reads as missing credentials.
	void SshProbe(const std::string& config, const std::string& url)
	{
		std::string output, failure;
		const bool ok = RealRunner(config, true)({"info", "--non-interactive", "--", url}, 10000, output, failure);
		Check(!ok && failure.starts_with("SVN needs your credentials. Run svn info " + url), "batch ssh refusal asks for credentials: " + failure);
		std::cout << "PASS: svn+ssh refusal asks for credentials: " << failure << "\n";
	}
	void RealWorkingCopy(const std::filesystem::path& directory, const std::string& config, bool sparse = false)
	{
		const auto name = uam::paths::Utf8PathString(directory.filename());
		Check((name == "copy @literal" || name == "wc\xC3\xA9 @literal") && directory.parent_path().filename().string().starts_with("uam-svn-composer-") && uam::paths::PathFromUtf8(config) == directory.parent_path() / "config", "integration requires the disposable harness working copy and config");
		uam::SvnBranchScope scope{directory, directory, {}, true};
		int switches = 0;
		const uam::SvnBranchRunner run = RealRunner(config, true, &switches);
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
		else if (argc == 4 && std::string(argv[1]) == "--locale-off-integration") LocaleOffWorkingCopy(uam::paths::PathFromUtf8(argv[2]), argv[3]);
		else if (argc == 4 && std::string(argv[1]) == "--credentials-integration") CredentialsWorkingCopy(uam::paths::PathFromUtf8(argv[2]), argv[3]);
		else if (argc == 4 && std::string(argv[1]) == "--ssh-probe") SshProbe(argv[2], argv[3]);
		else if (argc == 2 && std::string(argv[1]) == "--ownership-regression") SourceOwnershipCases();
		else if (argc == 2 && std::string(argv[1]) == "--depth-regression") SparseDescendantCases();
		else if (argc == 2 && std::string(argv[1]) == "--dispatch-regression") BranchDispatchFailureCases();
		else InjectedCases();
	}
	catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
	return 0;
}
