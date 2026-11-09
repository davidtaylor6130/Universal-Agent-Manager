#include "test_harness.h"
#include "app/vcs_provider.h"
#include "common/constants/app_constants.h"
#include "common/security/read_only_command.h"
#include "remote/runner_startup.h"

#include <algorithm>
#include <chrono>
#include <thread>

using namespace uam_test;

namespace
{
#ifndef _WIN32
	std::string ResolvedTool(const std::string& name)
	{
		return uam::strings::Trim(PlatformServicesFactory::Instance().process_service.ExecuteCommand("command -v " + name).output);
	}

	/// Installs a same-named wrapper on PATH that hides the tool's output for `commit` only.
	void WriteSilentCommitWrapper(const fs::path& directory, const std::string& tool, const std::string& real)
	{
		const fs::path wrapper = directory / tool;
		UAM_ASSERT(uam::io::WriteTextFile(wrapper, "#!/bin/sh\nfor a in \"$@\"; do [ \"$a\" = commit ] && exec \"" + real + "\" \"$@\" >/dev/null 2>&1; done\nexec \"" + real + "\" \"$@\"\n"));
		fs::permissions(wrapper, fs::perms::owner_all);
	}

	bool ChangedPathStatus(const uam::VcsCommitStatus& status, const std::string& path, const std::string& code)
	{
		return std::ranges::any_of(status.changed_files, [&](const uam::VcsChangedFile& file) { return file.path == path && file.status == code; });
	}

	bool SvnToolsAvailable()
	{
		if (!ResolvedTool("svn").empty() && !ResolvedTool("svnadmin").empty()) return true;
		std::fprintf(stderr, "SKIP real-svn case: svn or svnadmin is not installed\n");
		return false;
	}

	/// Real runner service reached through a fake `ssh`, so remote VCS commands execute against temporary repositories.
	struct RemoteVcsHost
	{
		TempDir temp{"uam-vcs-remote"};
		fs::path socket = temp.root / "r.sock";
		fs::path fake_ssh = temp.root / "ssh";
		uam::platform::StdioProcessPlatformFields process;
		std::optional<ScopedEnvVar> path;
		std::optional<ScopedEnvVar> runner_env;
		std::optional<ScopedEnvVar> socket_env;
		uam::AppState app;
		ChatSession chat;

		/// `tool_directory` goes first on PATH for the runner and for directly spawned tools.
		explicit RemoteVcsHost(const fs::path& tool_directory = {})
		{
			auto& service = PlatformServicesFactory::Instance().process_service;
			const fs::path runner = service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
			const std::string inherited_path = uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
			path.emplace("PATH", (tool_directory.empty() ? std::string() : tool_directory.string() + ":") + temp.root.string() + ":" + inherited_path);
			std::string error;
			UAM_ASSERT(service.StartStdioProcess(process, temp.root, {runner.string(), "serve", "--socket", socket.string()}, &error));
			for (int attempt = 0; attempt < 500 && !fs::exists(socket); ++attempt)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			UAM_ASSERT(fs::exists(socket));
			UAM_ASSERT(uam::io::WriteTextFile(fake_ssh, "#!/bin/sh\nexec \"$UAM_TEST_RUNNER\" bridge --socket \"$UAM_TEST_SOCKET\"\n"));
			fs::permissions(fake_ssh, fs::perms::owner_all);
			runner_env.emplace("UAM_TEST_RUNNER", runner.string());
			socket_env.emplace("UAM_TEST_SOCKET", socket.string());
			ExecutionHost host;
			host.id = "remote-vcs-provider";
			host.label = "Remote VCS provider";
			host.transport = "ssh";
			host.ssh_alias = "test-host";
			host.platform = "linux";
			host.architecture = "arm64";
			host.runner_version = std::string(uam::constants::kAppVersion).substr(1);
			host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
			host.runner_status = "ready";
			app.settings.execution_hosts.push_back(host);
			chat.id = "remote-vcs-provider-chat";
			chat.execution_host_id = host.id;
			app.data_root = temp.root / "data";
		}

		~RemoteVcsHost()
		{
			auto& service = PlatformServicesFactory::Instance().process_service;
			service.StopStdioProcess(process, true);
			service.CloseStdioProcessHandles(process);
		}
	};
#endif
}

UAM_TEST(VcsProvidersRegisterGitThenSvnWithTheirOwnDeclarations)
{
	const auto providers = uam::VcsProviders();
	UAM_ASSERT_EQ(providers.size(), std::size_t{2});
	UAM_ASSERT_EQ(providers[0]->Type(), uam::VcsType::Git);
	UAM_ASSERT_EQ(providers[1]->Type(), uam::VcsType::Svn);
	UAM_ASSERT_EQ(&uam::VcsProviderFor(uam::VcsType::Git), providers[0]);
	UAM_ASSERT_EQ(&uam::VcsProviderFor(uam::VcsType::Svn), providers[1]);
	UAM_ASSERT_EQ(std::string(uam::VcsProviderFor(uam::VcsType::Git).Declaration().display_name), std::string("Git"));
	UAM_ASSERT_EQ(std::string(uam::VcsProviderFor(uam::VcsType::Svn).Declaration().display_name), std::string("SVN"));
	for (std::size_t index = 0; index < providers.size(); ++index)
		UAM_ASSERT_EQ(&providers[index]->Declaration(), uam::vcs::kVcsProviderDeclarations[index]);

	UAM_ASSERT_EQ(uam::VcsTypeToString(uam::VcsType::Git), std::string("git"));
	UAM_ASSERT_EQ(uam::VcsTypeToString(uam::VcsType::Svn), std::string("svn"));

	const auto declares_branch_create = [](const uam::VcsType type)
	{
		return uam::ranges::Contains(uam::VcsProviderFor(type).Declaration().capabilities, uam::vcs::VcsCapability::BranchCreate);
	};
	UAM_ASSERT(declares_branch_create(uam::VcsType::Git));
	UAM_ASSERT(!declares_branch_create(uam::VcsType::Svn));
}

UAM_TEST(VcsSelectionFallsBackToTheSystemThatIsActuallyPresent)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-vcs-select");
	uam::AppState app;
	ChatSession chat;
	const uam::VcsCommitService service;
	const fs::path svn_only = temp.root / "svn";
	fs::create_directories(svn_only / ".svn");
	chat.workspace_directory = svn_only.string();
	UAM_ASSERT_EQ(service.Status(app, chat, uam::VcsType::Git, false, {}, false).active_vcs_type, uam::VcsType::Svn);
	const fs::path git_only = temp.root / "git";
	fs::create_directories(git_only);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(git_only.string())));
	chat.workspace_directory = git_only.string();
	UAM_ASSERT_EQ(service.Status(app, chat, uam::VcsType::Svn, false, {}, false).active_vcs_type, uam::VcsType::Git);
}

UAM_TEST(VcsProviderSettingsGateFollowsEachSystemsOwnSwitch)
{
	uam::AppState app;
	ChatSession chat;
	chat.execution_host_id = "missing-remote-host";
	const uam::VcsCommitService service;
	const auto gated = [&](const uam::VcsType type)
	{
		std::string diff_error;
		(void)service.Diff(app, chat, "file.cpp", type, &diff_error);
		return diff_error.find("disabled") != std::string::npos;
	};
	app.settings.git_enabled = false;
	app.settings.svn_enabled = true;
	UAM_ASSERT(gated(uam::VcsType::Git));
	UAM_ASSERT(!gated(uam::VcsType::Svn));
	UAM_ASSERT(service.Commit(app, chat, uam::VcsType::Git, "m", {"file.cpp"}).error.find("disabled") != std::string::npos);
	UAM_ASSERT(service.Commit(app, chat, uam::VcsType::Svn, "m", {"file.cpp"}).error.find("disabled") == std::string::npos);
	UAM_ASSERT(service.GenerateMessage(app, chat, uam::VcsType::Git, {"file.cpp"}).error.find("disabled") != std::string::npos);
	UAM_ASSERT(service.GenerateMessage(app, chat, uam::VcsType::Svn, {"file.cpp"}).error.find("disabled") == std::string::npos);
	app.settings.git_enabled = true;
	app.settings.svn_enabled = false;
	UAM_ASSERT(!gated(uam::VcsType::Git));
	UAM_ASSERT(gated(uam::VcsType::Svn));
	UAM_ASSERT(service.Commit(app, chat, uam::VcsType::Svn, "m", {"file.cpp"}).error.find("disabled") != std::string::npos);
}

UAM_TEST(VcsProviderReadOnlyListsApplyOnlyToTheirOwnExecutable)
{
	using uam::command_safety::IsReadOnlyCommand;
	UAM_ASSERT(IsReadOnlyCommand("git status --short"));
	UAM_ASSERT(IsReadOnlyCommand("git --no-pager status"));
	UAM_ASSERT(IsReadOnlyCommand("svn status --quiet"));
	UAM_ASSERT(IsReadOnlyCommand("svn log --limit 2"));
	// One system's actions and leading flag must not leak into the other's grammar.
	UAM_ASSERT(!IsReadOnlyCommand("svn show"));
	UAM_ASSERT(!IsReadOnlyCommand("git info"));
	UAM_ASSERT(!IsReadOnlyCommand("git st"));
	UAM_ASSERT(!IsReadOnlyCommand("svn --no-pager status"));
	UAM_ASSERT(!IsReadOnlyCommand("git"));
	UAM_ASSERT(!IsReadOnlyCommand("svn"));
	UAM_ASSERT(!IsReadOnlyCommand("hg status"));
}

#ifndef _WIN32
UAM_TEST(VcsCommitNoticeNamesGitWhenTheToolPrintsNothing)
{
	if (!GitAvailableForTests()) return;
	TempDir wrappers("uam-vcs-tools");
	WriteSilentCommitWrapper(wrappers.root, "git", ResolvedTool("git"));
	RemoteVcsHost host(wrappers.root);

	// Local Git cannot be covered here: its shell is a login shell, which reorders PATH ahead of the wrapper.
	const fs::path repo = host.temp.root / "git";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "a.txt", "a\n"));
	host.chat.workspace_directory = repo.string();
	const uam::VcsCommitResult git = uam::VcsCommitService().Commit(host.app, host.chat, uam::VcsType::Git, "first", {"a.txt"});
	if (!git.ok) throw TestFailure(git.error);
	UAM_ASSERT_EQ(git.message, std::string("Git commit created."));
}

UAM_TEST(VcsCommitNoticeNamesSvnWhenTheToolPrintsNothing)
{
	if (!SvnToolsAvailable()) return;
	TempDir wrappers("uam-vcs-tools");
	WriteSilentCommitWrapper(wrappers.root, "svn", ResolvedTool("svn"));
	RemoteVcsHost host(wrappers.root);
	const uam::VcsCommitService vcs;

	const fs::path svn_repository = host.temp.root / "repository";
	UAM_ASSERT(RunTestCommand("svnadmin create " + ShellQuoteForTest(svn_repository.string())));
	const fs::path checkout = host.temp.root / "checkout";
	UAM_ASSERT(uam::RunSvn(host.temp.root, {"checkout", "--non-interactive", "file://" + uam::paths::Utf8PathString(svn_repository), checkout.string()}).exit_code == 0);
	UAM_ASSERT(uam::io::WriteTextFile(checkout / "remote.txt", "a\n"));
	host.chat.workspace_directory = checkout.string();
	const uam::VcsCommitResult remote_svn = vcs.Commit(host.app, host.chat, uam::VcsType::Svn, "remote", {"remote.txt"});
	if (!remote_svn.ok) throw TestFailure(remote_svn.error);
	UAM_ASSERT_EQ(remote_svn.message, std::string("SVN commit created."));

	UAM_ASSERT(uam::io::WriteTextFile(checkout / "local.txt", "a\n"));
	ChatSession local_chat;
	local_chat.workspace_directory = checkout.string();
	const uam::VcsCommitResult local_svn = vcs.Commit(host.app, local_chat, uam::VcsType::Svn, "local", {"local.txt"});
	if (!local_svn.ok) throw TestFailure(local_svn.error);
	UAM_ASSERT_EQ(local_svn.message, std::string("SVN commit created."));
}

UAM_TEST(RemoteSvnStatusDiffAndCommitGoThroughTheSvnProvider)
{
	if (!GitAvailableForTests() || !SvnToolsAvailable()) return;
	RemoteVcsHost host;
	const fs::path repository = host.temp.root / "repository";
	const fs::path checkout = host.temp.root / "checkout";
	UAM_ASSERT(RunTestCommand("svnadmin create " + ShellQuoteForTest(repository.string())));
	const std::string url = "file://" + uam::paths::Utf8PathString(repository);
	UAM_ASSERT(uam::RunSvn(host.temp.root, {"checkout", "--non-interactive", url, checkout.string()}).exit_code == 0);
	UAM_ASSERT(uam::io::WriteTextFile(checkout / "tracked.txt", "v1\n"));
	UAM_ASSERT(uam::RunSvn(checkout, {"add", "--non-interactive", "--", "tracked.txt"}).exit_code == 0);
	UAM_ASSERT(uam::RunSvn(checkout, {"commit", "--non-interactive", "-m", "baseline"}).exit_code == 0);
	UAM_ASSERT(uam::RunSvn(checkout, {"update", "--non-interactive"}).exit_code == 0);
	UAM_ASSERT(uam::io::WriteTextFile(checkout / "tracked.txt", "v2\n"));
	UAM_ASSERT(uam::io::WriteTextFile(checkout / "new.txt", "brand new\n"));
	host.chat.workspace_directory = checkout.string();

	const uam::VcsCommitService vcs;
	const uam::VcsCommitStatus status = vcs.Status(host.app, host.chat, uam::VcsType::Svn);
	if (!status.error.empty()) throw TestFailure(status.error);
	UAM_ASSERT(status.vcs_types == std::vector<uam::VcsType>{uam::VcsType::Svn});
	UAM_ASSERT_EQ(status.active_vcs_type, uam::VcsType::Svn);
	UAM_ASSERT(!status.branch_or_revision.empty());
	UAM_ASSERT(ChangedPathStatus(status, "tracked.txt", "M"));
	UAM_ASSERT(ChangedPathStatus(status, "new.txt", "?"));

	std::string error;
	const std::string tracked_patch = vcs.Diff(host.app, host.chat, "tracked.txt", uam::VcsType::Svn, &error);
	if (!error.empty()) throw TestFailure(error);
	UAM_ASSERT(tracked_patch.find("+v2") != std::string::npos);
	const std::string new_patch = vcs.Diff(host.app, host.chat, "new.txt", uam::VcsType::Svn, &error);
	if (!error.empty()) throw TestFailure(error);
	UAM_ASSERT(new_patch.find("+brand new") != std::string::npos);

	const uam::VcsCommitResult committed = vcs.Commit(host.app, host.chat, uam::VcsType::Svn, "remote svn publish", {"tracked.txt"});
	if (!committed.ok) throw TestFailure(committed.error);
	UAM_ASSERT(committed.message.find("Committed revision") != std::string::npos);
	UAM_ASSERT(ChangedPathStatus(committed.status, "new.txt", "?"));
	UAM_ASSERT(!ChangedPathStatus(committed.status, "tracked.txt", "M"));
	UAM_ASSERT(uam::RunSvn(checkout, {"log", "--non-interactive", "-l", "1", url}).output.find("remote svn publish") != std::string::npos);
}

UAM_TEST(RemoteGitStatusDiffAndCommitGoThroughTheGitProvider)
{
	if (!GitAvailableForTests()) return;
	RemoteVcsHost host;
	const fs::path repository = host.temp.root / "repository";
	fs::create_directories(repository);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repository.string())));
	UAM_ASSERT(RunGitForTest(repository, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repository, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repository / "tracked.txt", "v1\n"));
	UAM_ASSERT(RunGitForTest(repository, "add tracked.txt"));
	UAM_ASSERT(RunGitForTest(repository, "commit -m baseline"));
	UAM_ASSERT(uam::io::WriteTextFile(repository / "tracked.txt", "v2\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repository / "new.txt", "brand new\n"));
	host.chat.workspace_directory = repository.string();

	const uam::VcsCommitService vcs;
	const uam::VcsCommitStatus status = vcs.Status(host.app, host.chat, uam::VcsType::Git);
	if (!status.error.empty()) throw TestFailure(status.error);
	UAM_ASSERT(status.vcs_types == std::vector<uam::VcsType>{uam::VcsType::Git});
	UAM_ASSERT(!status.branch_or_revision.empty());
	UAM_ASSERT(ChangedPathStatus(status, "tracked.txt", " M"));
	UAM_ASSERT(ChangedPathStatus(status, "new.txt", "??"));

	std::string error;
	UAM_ASSERT(vcs.Diff(host.app, host.chat, "tracked.txt", uam::VcsType::Git, &error).find("+v2") != std::string::npos);
	UAM_ASSERT(error.empty());
	UAM_ASSERT(vcs.Diff(host.app, host.chat, "new.txt", uam::VcsType::Git, &error).find("+brand new") != std::string::npos);
	UAM_ASSERT(error.empty());

	const uam::VcsCommitResult committed = vcs.Commit(host.app, host.chat, uam::VcsType::Git, "remote git publish", {"tracked.txt"});
	if (!committed.ok) throw TestFailure(committed.error);
	UAM_ASSERT(RunGitForTest(repository, "log -1 --format=%s | grep -qx 'remote git publish'"));
	UAM_ASSERT(ChangedPathStatus(committed.status, "new.txt", "??"));
	UAM_ASSERT(!ChangedPathStatus(committed.status, "tracked.txt", " M"));
}

UAM_TEST(RemoteGitStatusUsesTheWorkspaceWhenTheRootIsReportedEmpty)
{
	if (!GitAvailableForTests()) return;
	TempDir tools("uam-vcs-empty-root");
	const fs::path wrapper = tools.root / "git";
	UAM_ASSERT(uam::io::WriteTextFile(wrapper, "#!/bin/sh\ncase \" $* \" in *\" --show-toplevel \"*) exit 0;; esac\nexec \"" + ResolvedTool("git") + "\" \"$@\"\n"));
	fs::permissions(wrapper, fs::perms::owner_all);
	RemoteVcsHost host(tools.root);
	const fs::path repository = host.temp.root / "repository";
	fs::create_directories(repository);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repository.string())));
	UAM_ASSERT(uam::io::WriteTextFile(repository / "new.txt", "new\n"));
	host.chat.workspace_directory = repository.string();
	const uam::VcsCommitStatus status = uam::VcsCommitService().Status(host.app, host.chat, uam::VcsType::Git);
	if (!status.error.empty()) throw TestFailure(status.error);
	UAM_ASSERT(ChangedPathStatus(status, "new.txt", "??"));
}
#endif
