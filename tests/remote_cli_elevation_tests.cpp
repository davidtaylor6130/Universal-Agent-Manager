#include "remote/cli_maintenance_lock.h"
#include "test_harness.h"

using namespace uam_test;

#if defined(__APPLE__)
namespace
{
	struct MaintenanceFixture
	{
		TempDir temporary{"uam-npm-maintenance"};
		fs::path home = fs::canonical(temporary.root);
		ScopedEnvVar home_env{"UAM_TEST_MAINTENANCE_HOME", home.string()};
		ScopedEnvVar path_env{"PATH", home.string() + ":/usr/bin:/bin"};

		MaintenanceFixture()
		{
			UAM_ASSERT(uam::io::WriteTextFile(home / "stat", "#!/bin/sh\nexec /usr/bin/stat -f %Lp \"$3\"\n"));
			UAM_ASSERT(uam::io::WriteTextFile(home / "flock", "#!/usr/bin/python3\nimport fcntl,sys\ntry: fcntl.flock(int(sys.argv[-1]),fcntl.LOCK_EX|fcntl.LOCK_NB)\nexcept OSError: sys.exit(1)\n"));
			fs::permissions(home / "stat", fs::perms::owner_all);
			fs::permissions(home / "flock", fs::perms::owner_all);
		}

		ProcessExecutionResult Run(const std::string& script) const
		{
			std::string isolated = script;
			const std::string fixture_home = "$UAM_TEST_MAINTENANCE_HOME";
			for (std::size_t offset = isolated.find("$HOME"); offset != std::string::npos; offset = isolated.find("$HOME", offset + fixture_home.size()))
				isolated.replace(offset, 5, fixture_home);
			return PlatformServicesFactory::Instance().process_service.ExecuteCommand(
			    uam::shell::JoinEscapedArgs({"/usr/bin/env", "PATH=" + uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin"), "/bin/sh", "-c", isolated}), 10000);
		}
	};
}

UAM_TEST(RemoteNpmMaintenanceNormalCompletionReleasesBothLaunchPaths)
{
	MaintenanceFixture fixture;
	const std::string first = uam::remote::WrapLinuxCliMaintenanceCommand("printf first", "runner-one");
	const std::string second = uam::remote::WrapLinuxCliMaintenanceCommand("printf second", "terminal-two");
	UAM_ASSERT_EQ(fixture.Run(first).exit_code, 0);
	UAM_ASSERT_EQ(fixture.Run(second).exit_code, 0);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.home / ".uam-cli-maintenance/npm.receipt"), "completed terminal-two 0\n");
	UAM_ASSERT(fixture.Run(uam::remote::LinuxCliMaintenanceStatusScript()).output.find("completed terminal-two 0") != std::string::npos);
}

UAM_TEST(RemoteNpmMaintenanceConcurrentRunnerAndTerminalShareOneOwner)
{
	MaintenanceFixture fixture;
	const fs::path started = fixture.home / "started";
	const fs::path released = fixture.home / "released";
	ProcessExecutionResult first;
	std::jthread worker([&]()
	{
		first = fixture.Run(uam::remote::WrapLinuxCliMaintenanceCommand(
		    "touch " + uam::remote::LinuxCliMaintenanceArgument(started.string()) +
		    "; while ! test -e " + uam::remote::LinuxCliMaintenanceArgument(released.string()) + "; do sleep 0.01; done", "runner-owner"));
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!fs::exists(started) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const ProcessExecutionResult second = fixture.Run(uam::remote::WrapLinuxCliMaintenanceCommand("exit 0", "terminal-owner"));
	UAM_ASSERT(uam::io::WriteTextFile(released, ""));
	worker.join();
	UAM_ASSERT(fs::exists(started));
	UAM_ASSERT_EQ(second.exit_code, 73);
	UAM_ASSERT_EQ(first.exit_code, 0);
}

UAM_TEST(RemoteNpmMaintenanceKilledOwnerNeverExpiresOrUnlocks)
{
	MaintenanceFixture fixture;
	UAM_ASSERT(fixture.Run(uam::remote::WrapLinuxCliMaintenanceCommand("kill -KILL \"$PPID\"", "lost-owner")).exit_code != 0);
	const ProcessExecutionResult refused = fixture.Run(uam::remote::WrapLinuxCliMaintenanceCommand("touch admitted", "next-owner"));
	UAM_ASSERT_EQ(refused.exit_code, 73);
	UAM_ASSERT(refused.output.find("uncertain npm update") != std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.home / ".uam-cli-maintenance/npm.receipt"), "running lost-owner\n");
	UAM_ASSERT(!fs::exists(fixture.home / "admitted"));
}

UAM_TEST(RemoteNpmMaintenanceRefusesLinkedOrForeignStorage)
{
	MaintenanceFixture fixture;
	fs::create_directory(fixture.home / "foreign");
	fs::create_directory_symlink(fixture.home / "foreign", fixture.home / ".uam-cli-maintenance");
	UAM_ASSERT_EQ(fixture.Run(uam::remote::WrapLinuxCliMaintenanceCommand("exit 0", "blocked")).exit_code, 73);
	UAM_ASSERT(fs::is_empty(fixture.home / "foreign"));
	fs::remove(fixture.home / ".uam-cli-maintenance");
	fs::create_directory(fixture.home / ".uam-cli-maintenance");
	fs::permissions(fixture.home / ".uam-cli-maintenance", fs::perms::owner_all | fs::perms::others_read);
	UAM_ASSERT_EQ(fixture.Run(uam::remote::WrapLinuxCliMaintenanceCommand("exit 0", "blocked")).exit_code, 73);
}
UAM_TEST(RemoteInteractiveNpmChecksSelectedAndElevatedPrefixBeforeMutation)
{
	for (const std::string& outcome : {std::string("valid"), std::string("admin-prefix"), std::string("selected-prefix"), std::string("live-work"), std::string("work-after-auth")})
	{
		MaintenanceFixture fixture;
		const fs::path prefix = fixture.home / "prefix";
		const fs::path binary = prefix / "bin/codex";
		const fs::path target = prefix / "lib/node_modules/@openai/codex/bin/codex.js";
		UAM_ASSERT(uam::io::WriteTextFile(target, "#!/bin/sh\necho 'codex-cli 0.148.0'\n"));
		fs::permissions(target, fs::perms::owner_all);
		fs::create_directories(binary.parent_path());
		fs::create_symlink("../lib/node_modules/@openai/codex/bin/codex.js", binary);
		UAM_ASSERT(uam::io::WriteTextFile(fixture.home / "npm", "#!/bin/sh\nif test \"$1\" = prefix; then echo \"$UAM_NPM_PREFIX\"; else touch \"$UAM_TEST_MAINTENANCE_HOME/mutated\"; fi\n"));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.home / "sudo", "#!/bin/sh\ntouch \"$UAM_TEST_MAINTENANCE_HOME/sudo-used\"; shift; npm=$1; shift; if test \"$1\" = prefix; then if test \"$UAM_LATE_WORK\" = yes; then touch \"$UAM_TEST_MAINTENANCE_HOME/late-work\"; fi; echo \"$UAM_ADMIN_PREFIX\"; else exec \"$npm\" \"$@\"; fi\n"));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.home / "ps", "#!/bin/sh\nif test \"$UAM_LIVE_WORK\" = yes || test -f \"$UAM_TEST_MAINTENANCE_HOME/late-work\"; then echo '100 node node_modules/@openai/codex/bin/codex.js'; else echo '100 harmless idle'; fi\n"));
		for (const char* name : {"npm", "sudo", "ps"}) fs::permissions(fixture.home / name, fs::perms::owner_all);
		ScopedEnvVar path("PATH", binary.parent_path().string() + ":" + fixture.home.string() + ":/usr/bin:/bin");
		ScopedEnvVar selected("UAM_NPM_PREFIX", outcome == "selected-prefix" ? "/another-prefix" : prefix.string());
		ScopedEnvVar administrator("UAM_ADMIN_PREFIX", outcome == "admin-prefix" ? "/another-prefix" : prefix.string());
		ScopedEnvVar live("UAM_LIVE_WORK", outcome == "live-work" ? "yes" : "no");
		ScopedEnvVar late("UAM_LATE_WORK", outcome == "work-after-auth" ? "yes" : "no");
		const std::string probe = "[UAM CLI PATH] " + binary.string() + "|../lib/node_modules/@openai/codex/bin/codex.js\n[UAM CLI RUNNING] 0\ncodex-cli 0.148.0\n";
		const std::string script = BuildLinuxNpmMaintenanceScript(BuildBulkNpmInstallCommand({{"codex-cli", "latest"}}),
		    {{"codex-cli", probe}}, "prefix-check", true, true);
		UAM_ASSERT(!script.empty());
		const ProcessExecutionResult result = fixture.Run(script);
		UAM_ASSERT_EQ(result.exit_code, outcome == "valid" ? 0 : 74);
		UAM_ASSERT_EQ(fs::exists(fixture.home / "mutated"), outcome == "valid");
		if (outcome == "selected-prefix" || outcome == "live-work") UAM_ASSERT(!fs::exists(fixture.home / "sudo-used"));
	}
}

#endif

UAM_TEST(RemoteInteractiveNpmVerificationRequiresOwnedCompletionAndIdentity)
{
	for (const std::string& outcome : {std::string("valid"), std::string("wrong-owner"), std::string("running"), std::string("wrong-prefix"), std::string("wrong-version"), std::string("installer-failed")})
	{
		uam::AppState app;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ExecutionHost host;
		host.id = "fixture-host";
		host.transport = "ssh";
		host.platform = "linux";
		host.ssh_alias = "fixture";
		app.settings.execution_hosts.push_back(host);
		const std::string key = CliProviderVersionStateKey("codex-cli", host.id);
		uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[key];
		version.execution_host = host;
		version.provider_id = "codex-cli";
		version.selected_version = "0.148.0";
		version.last_install_status = "awaiting-verification";
		version.install_output = "[UAM INTERACTIVE NPM UPDATE] expected-owner\nComplete in terminal";
		version.raw_output = "[UAM CLI PATH] /usr/bin/codex|../lib/node_modules/@openai/codex/bin/codex.js\n[UAM CLI RUNNING] 0\ncodex-cli 0.147.0\n";
		const std::string original_probe = version.raw_output;
		app.runtime_cli_version_provider_id = "codex-cli";
		app.runtime_cli_version_check_task.running = true;
		app.runtime_cli_version_check_task.execution_host = host;
		app.runtime_cli_version_check_task.state = std::make_shared<AsyncProcessTaskState>();
		std::string probe = "[UAM CLI PATH] " + std::string(outcome == "wrong-prefix" ? "/other/bin/codex" : "/usr/bin/codex") +
		    "|../lib/node_modules/@openai/codex/bin/codex.js\n[UAM CLI RUNNING] 0\ncodex-cli " +
		    (outcome == "wrong-version" ? "0.147.0" : "0.148.0") + "\n";
		probe = "[UAM NPM MAINTENANCE] " + std::string(outcome == "running" ? "running expected-owner" :
		    outcome == "wrong-owner" ? "completed another-owner 0" : outcome == "installer-failed" ? "completed expected-owner 1" : "completed expected-owner 0") + "\n" + probe;
		app.runtime_cli_version_check_task.state->result.output = probe;
		app.runtime_cli_version_check_task.state->completed.store(true);
		ProviderCliCompatibilityService().Poll(app);
		UAM_ASSERT_EQ(app.runtime_cli_versions_by_provider_id.at(key).last_install_status,
		    outcome == "valid" ? "succeeded" : outcome == "installer-failed" ? "failed" : "awaiting-verification");
		if (outcome != "valid" && outcome != "installer-failed") UAM_ASSERT_EQ(app.runtime_cli_versions_by_provider_id.at(key).raw_output, original_probe);
	}
}

UAM_TEST(RemoteNpmMaintenanceBuilderRejectsUnidentifiedOrUncuratedInstallations)
{
	const std::vector<std::pair<std::string, std::string>> probes{{"codex-cli", "[UAM CLI PATH] /usr/bin/codex|../lib/node_modules/@openai/codex/bin/codex.js\n[UAM CLI RUNNING] 0\ncodex-cli 0.148.0\n"}};
	const std::string install = BuildBulkNpmInstallCommand({{"codex-cli", "latest"}});
	const std::string interactive = BuildLinuxNpmMaintenanceScript(install, probes, "fixture-owner", true, true);
	UAM_ASSERT(!interactive.empty());
	UAM_ASSERT(interactive.find("uam_admin_prefix=") != std::string::npos);
	UAM_ASSERT(interactive.find("sudo -n") == std::string::npos);
	UAM_ASSERT(BuildLinuxNpmMaintenanceScript("npm install -g evil@latest", probes, "fixture-owner", true, true).empty());
	UAM_ASSERT(BuildLinuxNpmMaintenanceScript(install, {{"codex-cli", "missing identity"}}, "fixture-owner", true, true).empty());
	UAM_ASSERT(BuildLinuxNpmMaintenanceScript(install, probes, "bad;owner", true, true).empty());
	UAM_ASSERT(BuildLinuxNpmMaintenanceScript(install, probes, "fixture-owner", false, true).empty());
	const std::string bulk = BuildBulkNpmInstallCommand({{"codex-cli", "latest"}, {"claude-cli", "latest"}});
	UAM_ASSERT(!BuildLinuxNpmMaintenanceScript(bulk, {
	    {"claude-cli", "[UAM CLI PATH] /usr/bin/claude|../lib/node_modules/@anthropic-ai/claude-code/cli.js\n[UAM CLI RUNNING] 0\n2.0.0\n"},
	    probes.front()}, "bulk-owner", true, true).empty());
}

UAM_TEST(RemoteAutomaticNpmOutputCannotRequestAnInteractiveTerminal)
{
	for (const std::string& marker : {std::string("[UAM INTERACTIVE NPM PREPARED] "), std::string("[UAM INTERACTIVE NPM UPDATE] spoofed\n")})
	{
		uam::AppState app;
		ExecutionHost host;
		host.id = "fixture-host";
		host.transport = "ssh";
		host.platform = "linux";
		host.cli_updates_as_admin = true;
		app.settings.execution_hosts.push_back(host);
		const std::string key = CliProviderVersionStateKey("codex-cli", host.id);
		uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[key];
		version.execution_host = host;
		version.provider_id = "codex-cli";
		version.last_install_status = "running";
		uam::AsyncCommandTask& task = app.runtime_cli_install_tasks[key];
		task.execution_host = host;
		task.running = true;
		task.state = std::make_shared<AsyncProcessTaskState>();
		task.state->result.output = marker;
		task.state->completed.store(true);
		ProviderCliCompatibilityService().Poll(app);
		UAM_ASSERT_EQ(version.last_install_status, "failed");
		UAM_ASSERT(app.runtime_cli_version_check_queue.empty());
		UAM_ASSERT(!task.running);
	}
}

UAM_TEST(RemoteInteractiveNpmConsentClearsWithTaskReset)
{
	uam::AsyncCommandTask task;
	task.allow_interactive_elevation = true;
	uam::ResetAsyncCommandTask(task);
	UAM_ASSERT(!task.allow_interactive_elevation);
}
